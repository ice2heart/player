#include "core/app.h"
#include "core/queue_file.h"
#include "core/track_cache.h"
#include "fakes.h"
#include "frontend/threaded_scanner.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <memory>
#include <new>
#include <thread>

using namespace oap;
using namespace oap::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
RemoteSource albumSource(const std::string& id) { return {RemoteSource::Kind::Album, id}; }
RemoteSource playlistSource(const std::string& id) { return {RemoteSource::Kind::Playlist, id}; }
// The album Weather by Band with these tracks, as the manifest of a download keeps it.
CachedList cachedAlbum(const std::string& id, std::vector<CachedTrack> tracks) {
    return {albumSource(id), "Weather", "Band", std::move(tracks)};
}
void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}
std::string valueOf(const MenuItem& item) { return item.value ? item.value() : std::string(); }
} // namespace

// Library used by every test:
//   Alpha/01 - Storm.mp3, Alpha/02 - Calm.mp3   (two-part book)
//   Beta.m4b                                    (single file, embedded chapters if the test sets them)
// Music:
//   Lullaby.mp3                                 (loose track)
//   Road Trip/01 - Sunrise.mp3, 02 - Highway.mp3 (album)
class AppTest : public testing::Test {
protected:
    void SetUp() override {
        base = testTempDir();
        fs::remove_all(base);
        queueFile = files.queueFile = base / "config" / "queue.tsv";
        scrobbleFile = base / "config" / "scrobbles.tsv";
        root = base / "books";
        music = base / "music";
        touch(alpha() / "01 - Storm.mp3");
        touch(alpha() / "02 - Calm.mp3");
        touch(beta());
        touch(music / "Lullaby.mp3");
        touch(roadTrip() / "01 - Sunrise.mp3");
        touch(roadTrip() / "02 - Highway.mp3");
    }
    void TearDown() override { fs::remove_all(base); }

    fs::path alpha() const { return root / "Alpha"; }
    fs::path beta() const { return root / "Beta.m4b"; }
    fs::path roadTrip() const { return music / "Road Trip"; }
    std::string alphaFile(int n) const {
        return (alpha() / (n == 1 ? "01 - Storm.mp3" : "02 - Calm.mp3")).string();
    }

    // The files the App is given: the real ones, which count what is written to queue.tsv.
    class QueueWatchingFiles : public TextFiles {
    public:
        std::optional<std::string> read(const fs::path& file) override {
            if (file == queueFile) ++queueReads;
            return diskFiles().read(file);
        }
        bool writeAtomically(const fs::path& file, const std::string& contents) override {
            if (file == queueFile) {
                if (failQueueWrites) return false;
                ++queueWrites;
            }
            return diskFiles().writeAtomically(file, contents);
        }
        fs::path queueFile;
        int queueWrites = 0;
        int queueReads = 0;
        bool failQueueWrites = false; // e.g. the card is read-only for a moment
    };
    QueueWatchingFiles files;
    fs::path queueFile; // queue.tsv, next to state.tsv
    fs::path scrobbleFile; // scrobbles.tsv, next to state.tsv
    TextFiles* textFiles = &files; // what start() gives the App
    // What the previous version left in place of queue.tsv: the place of the list that was playing in
    // state.tsv (`last_stream.*`; `kind` and `id` say which album or playlist it was).
    void olderState(const std::string& kind = "album", const std::string& id = "al1") {
        auto text = diskFiles().read(queueFile);
        auto saved = text ? parseQueue(*text) : std::nullopt;
        ASSERT_TRUE(saved) << "something was playing";
        const QueueEntry& e = *saved->queue.current();
        char at[32];
        std::snprintf(at, sizeof at, "%.1f", saved->position);
        stateFile.file["last_stream.server"] = e.serverUrl;
        stateFile.file["last_stream.user"] = e.user;
        stateFile.file["last_stream.kind"] = kind;
        stateFile.file["last_stream.id"] = id;
        stateFile.file["last_stream.title"] = saved->title;
        stateFile.file["last_stream.track"] = std::to_string(saved->queue.playOrder()[saved->queue.position()]);
        stateFile.file["last_stream.track_id"] = e.trackId;
        stateFile.file["last_stream.pos"] = at;
        fs::remove(queueFile);
    }

    // The list of servers start() gives the App: the fakes, then `moreLinks`.
    std::vector<ServerLink> serverLinks() {
        if (noServerLinks) return {};
        std::vector<ServerLink> links = linkTo(settings, server, downloader);
        for (const ServerLink& more : moreLinks) links.push_back(more);
        return links;
    }

    // Starts the App and, unless `scan` is false, lets its background scan finish.
    void start(bool scan = true) {
        app = std::make_unique<App>(Services{speech, player, sounds, volume, serverLinks(), indexStore, scanner, settings, state, clock, textFiles, nullptr, nullptr, nullptr, localPlaylists}, root, music, cache, queueFile, scrobbleFile);
        app->start();
        if (scan) ASSERT_NO_FATAL_FAILURE(deliverScan());
    }

    // Runs a real scan of the folders on disk, as the request of the App's last scan says
    // (tags from `tags`), and hands it to the App as the background scan would.
    void deliverScan() {
        ASSERT_GT(scanner.pendingCount(), 0u) << "the App asked for no scan";
        const auto request = scanner.lastRequest();
        std::vector<IndexItem> items;
        bool done = false;
        ThreadedScanner real(tags);
        real.scan(request.request, [&](std::vector<IndexItem> found) {
            items = std::move(found);
            done = true;
        });
        for (int i = 0; i < 20000 && !done; ++i) { // up to 20 s: only a stalled worker gets there
            real.poll();
            if (!done) std::this_thread::sleep_for(1ms);
        }
        ASSERT_TRUE(done);
        ASSERT_TRUE(scanner.finish(request.id, std::move(items)));
        app->tick();
    }

    void press(Button b) {
        app->keyDown(b);
        clock.advance(50ms);
        app->tick();
        app->keyUp(b);
    }

    void deep(Button b) {
        const Button second = b == Button::Back ? Button::BackDeep : Button::ForwardDeep;
        app->keyDown(b);
        app->keyDown(second);
        app->keyUp(second);
        app->keyUp(b);
    }

    void hold(Button b) {
        app->keyDown(b);
        clock.advance(700ms);
        app->tick();
        app->keyUp(b);
    }

    void idle(std::chrono::milliseconds ms) {
        clock.advance(ms);
        app->tick();
    }

    // The server answers the ping App sent last (Play on a track whose server is marked down, or the
    // background check); the answer reaches App on the next tick.
    void answerPing() {
        ASSERT_EQ(server.lastRequest().kind, FakeMusicServer::Kind::Ping) << "App asked the server";
        ASSERT_TRUE(server.replyPing(server.lastRequest().id, {"navidrome", "0.64.2"}));
        app->tick();
    }

    // Home > Library > Alpha: starts playing the first file.
    void playAlpha() {
        start();
        press(Button::Ok);
        press(Button::Ok);
    }

    // Moves down the current menu until `label` is selected, then presses OK.
    void open(const std::string& label) {
        for (size_t i = 0; i < level().items.size() && level().items[level().index].label != label; ++i)
            press(Button::Forward);
        ASSERT_EQ(level().items[level().index].label, label);
        press(Button::Ok);
    }

    std::vector<std::string> labels() const {
        std::vector<std::string> out;
        for (const MenuItem& item : level().items) out.push_back(item.label);
        return out;
    }

    bool hasLabel(const std::string& label) const {
        const auto all = labels();
        return std::find(all.begin(), all.end(), label) != all.end();
    }

    // The labels of a Queue list without what follows its entries (Save as playlist, Previous queue, Clear queue).
    std::vector<std::string> queueEntries() const {
        std::vector<std::string> out = labels();
        out.resize(size_t(std::find(out.begin(), out.end(), "Save as playlist") - out.begin()));
        return out;
    }

    const Screen& level() const { return app->screens().back(); }

    // The requests the App made for what the user did: the catalogue check at start-up is not one.
    size_t listRequests() const {
        size_t n = 0;
        for (const auto& r : server.requests)
            if (r.kind != FakeMusicServer::Kind::CatalogueVersion) ++n;
        return n;
    }

    FakeSpeech speech;
    FakePlayer player;
    FakeSounds sounds;
    FakeVolume volume;
    FakeMusicServer server;
    bool noServerLinks = false;         // start() gives the App an empty list of servers
    std::vector<ServerLink> moreLinks;  // servers after the first, for start()
    FakeDownloader downloader;
    FakeLocalPlaylists* localPlaylists = nullptr;
    FakeLibraryStore indexStore;    // library.db
    FakeLibraryScanner scanner; // the background scan: the test delivers it (deliverScan())
    FakeTagReader tags;         // what the real scan reads from the files
    FakeClock clock;
    FakeStore configFile; // config.yaml
    FakeStore stateFile;  // state.tsv
    Settings settings{&configFile};
    Settings state{&stateFile};
    fs::path base;
    fs::path cache; // empty: no cache folder
    fs::path root;
    fs::path music;
    std::unique_ptr<App> app;
};

// ---------------------------------------------------------------- start up

TEST_F(AppTest, StartsAtHomeWhenNoBookWasPlaying) {
    start();
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    ASSERT_EQ(app->screens().size(), 1u);
    EXPECT_EQ(level().title, "Home");
    EXPECT_TRUE(speech.heard("Ready."));
    EXPECT_EQ(speech.last(), "Home. Library, 1 of 7");
    EXPECT_EQ(sounds.played.front(), Earcon::Boot);
}

TEST_F(AppTest, StartAppliesStoredSettings) {
    settings.setInt("volume", 40);
    settings.setInt("speech.rate", 310);
    start();
    EXPECT_FLOAT_EQ(volume.volume, 0.4f);
    EXPECT_EQ(speech.rate, 310);
}

TEST_F(AppTest, ResumesLastBookAtSavedPosition) {
    state.setString("last_book", alpha().string());
    state.setString("pos:" + alpha().string(), "1:42.5");
    start();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_DOUBLE_EQ(player.pos, 42.5);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "Alpha, paused. Press play to continue.");
}

TEST_F(AppTest, DoesNotResumeABookInsideTheCacheFolder) {
    cache = root / "cache";
    touch(cache / "server-1" / "al1" / "01.flac");
    state.setString("last_book", cache.string());
    start();
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(player.file, "");
}

TEST_F(AppTest, StartsAtHomeWhenTheLastBookCannotBeOpened) {
    state.setString("last_book", alpha().string());
    player.openSucceeds = false;
    start();
    EXPECT_TRUE(speech.heard("Cannot open 01 - Storm.mp3"));
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(speech.last(), "Home. Library, 1 of 7");
}

TEST_F(AppTest, InvalidSavedPositionStartsFromTheBeginning) {
    state.setString("last_book", alpha().string());
    state.setString("pos:" + alpha().string(), "7:99.0"); // the book has only two files
    start();
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

// ---------------------------------------------------------------- menus

TEST_F(AppTest, BackAndForwardMoveThroughTheMenuAndWrap) {
    start();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Music, 2 of 7");
    EXPECT_EQ(sounds.last(), Earcon::Move);
    press(Button::Back);
    press(Button::Back);
    EXPECT_EQ(speech.last(), "Help, 7 of 7");
    EXPECT_EQ(sounds.last(), Earcon::Wrap);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Library, 1 of 7");
    EXPECT_EQ(sounds.last(), Earcon::Wrap);
}

TEST_F(AppTest, NoWrapWhenWrapAroundIsOff) {
    settings.setInt("menu.wrap", 0);
    start();
    press(Button::Back);
    EXPECT_EQ(level().index, 0u);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Library, 1 of 7");
    hold(Button::Forward);
    press(Button::Forward);
    EXPECT_EQ(level().index, 6u);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Help, 7 of 7");
}

TEST_F(AppTest, LongPressForwardJumpsToLastItemOnce) {
    start();
    hold(Button::Forward);
    EXPECT_EQ(level().index, 6u);
    EXPECT_EQ(speech.last(), "Help, 7 of 7");
    const auto& usage = app->buttonUsage()[size_t(Button::Forward)];
    EXPECT_EQ(usage[size_t(Gesture::Long)], 1);
    EXPECT_EQ(usage[size_t(Gesture::Short)], 0);
}

TEST_F(AppTest, EveryKeyInterruptsSpeech) {
    start();
    int before = speech.stops;
    press(Button::Forward);
    EXPECT_EQ(speech.stops, before + 1);
}

TEST_F(AppTest, ExpertVerbosityLeavesOutThePosition) {
    settings.setInt("verbosity", 2);
    start();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Music");
}

TEST_F(AppTest, BeginnerVerbosityAddsAHint) {
    settings.setInt("verbosity", 0);
    start();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Music, 2 of 7. Press OK to open.");
}

TEST_F(AppTest, OkOpensSubmenuAndLongOkOffersOptions) {
    start();
    press(Button::Ok);
    EXPECT_EQ(level().title, "Library");
    EXPECT_EQ(speech.last(), "Library. Alpha, 2 parts, 1 of 2");
    hold(Button::Ok);
    EXPECT_EQ(level().title, "Library");
    EXPECT_EQ(speech.last(), "No options.");
    EXPECT_TRUE(player.opened.empty()) << "held OK never activates Alpha";
    EXPECT_EQ(sounds.last(), Earcon::Edge);
}

TEST_F(AppTest, ASettingOpensOnItsCurrentValue) {
    start();
    open("Settings");
    open("Volume");
    EXPECT_EQ(level().title, "Volume");
    EXPECT_EQ(speech.last(), "Volume. 70 percent, current, 15 of 21");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "75 percent, 16 of 21");
}

TEST_F(AppTest, ChoosingAValueFromTheListSavesItAppliesItAndGoesBack) {
    start();
    open("Settings");
    open("Speech rate");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(settings.getInt("speech.rate", 0), 250);
    EXPECT_EQ(configFile.onDisk("speech.rate"), "250") << "written at once";
    EXPECT_EQ(speech.rate, 250);
    EXPECT_EQ(level().title, "Settings");
    EXPECT_EQ(speech.last(), "Speech rate, 250 words per minute, 1 of 12");
}

TEST_F(AppTest, LeavingAValueListChangesNothing) {
    start();
    open("Settings");
    open("Volume");
    press(Button::Forward);
    press(Button::Home);
    EXPECT_EQ(level().title, "Settings");
    EXPECT_EQ(settings.getInt("volume", 70), 70);
    EXPECT_EQ(speech.last(), "Volume, 70 percent, 2 of 12");
}

TEST_F(AppTest, RightInSettingsChangesNoSetting) {
    start();
    open("Settings"); // Speech rate, then Volume, ... 12 items in all
    press(Button::Forward);
    press(Button::Forward);
    press(Button::Back);
    press(Button::Forward);
    EXPECT_EQ(settings.getInt("speech.rate", 220), 220);
    EXPECT_EQ(settings.getInt("volume", 70), 70);
    EXPECT_EQ(speech.rate, 220);
    EXPECT_FLOAT_EQ(volume.volume, 0.7f);
    press(Button::Forward);
    EXPECT_EQ(configFile.saves, 0) << "nothing was changed, so nothing is written";
    EXPECT_EQ(stateFile.saves, 0);
}

TEST_F(AppTest, ASettingIsOnlyChangedInItsList) {
    start();
    open("Settings"); // Speech rate
    press(Button::Forward);
    press(Button::Back);
    ASSERT_EQ(level().index, 0u) << "Back returns to the first setting";
    press(Button::Ok);
    EXPECT_EQ(level().title, "Speech rate");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(settings.getInt("speech.rate", 0), 250);
    EXPECT_EQ(speech.rate, 250);
    EXPECT_EQ(configFile.onDisk("speech.rate"), "250");
}

TEST_F(AppTest, RightInAListThatIsNotSortedByNameMovesTenItems) {
    start();
    open("Settings"); // 14 items
    deep(Button::Forward);
    EXPECT_EQ(level().index, 10u);
    EXPECT_EQ(sounds.last(), Earcon::Move);
    EXPECT_EQ(speech.last(), "Replace cached copies, off, 11 of 12") << "no letter: the list is not sorted by name";
    deep(Button::Forward);
    EXPECT_EQ(level().index, 11u) << "stops at the last item";
    EXPECT_EQ(sounds.last(), Earcon::Move);
    deep(Button::Back);
    EXPECT_EQ(level().index, 1u);
    deep(Button::Back);
    EXPECT_EQ(level().index, 0u) << "stops at the first item";
    EXPECT_EQ(sounds.last(), Earcon::Move);
}

TEST_F(AppTest, TenItemAndEndJumpsClampRegardlessOfWrap) {
    for (int wrap : {0, 1}) {
        settings.setInt("menu.wrap", wrap);
        start();
        open("Settings");
        deep(Button::Back);
        EXPECT_EQ(level().index, 0u);
        EXPECT_EQ(sounds.last(), Earcon::Edge);
        hold(Button::Forward);
        EXPECT_EQ(level().index, 11u);
        deep(Button::Forward);
        EXPECT_EQ(level().index, 11u);
        EXPECT_EQ(sounds.last(), Earcon::Edge);
        hold(Button::Forward);
        EXPECT_EQ(level().index, 11u);
        EXPECT_EQ(sounds.last(), Earcon::Edge);
        hold(Button::Back);
        EXPECT_EQ(level().index, 0u);
        hold(Button::Back);
        EXPECT_EQ(level().index, 0u);
        EXPECT_EQ(sounds.last(), Earcon::Edge);
    }
}

TEST_F(AppTest, LeftAndRightAlwaysMakeASound) {
    start();
    open("Podcasts"); // one item that does nothing but speak
    ASSERT_EQ(level().items.size(), 1u);
    for (Button b : {Button::Back, Button::Forward}) {
        size_t played = sounds.played.size();
        press(b);
        EXPECT_EQ(level().title, "Podcasts");
        EXPECT_EQ(sounds.played.size(), played + 1) << "no silent key";
        EXPECT_EQ(sounds.last(), Earcon::Edge);
        EXPECT_EQ(speech.last(), "No podcasts yet, 1 of 1");
    }
}

TEST_F(AppTest, HomeGoesBackToTheScreenYouCameFrom) {
    playAlpha(); // Home > Library > player
    press(Button::Home);
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(level().title, "Library");
    EXPECT_EQ(speech.last(), "Alpha, 2 parts, 1 of 2");
    EXPECT_TRUE(player.playing()) << "leaving the player does not stop it";
    press(Button::Home);
    EXPECT_EQ(labels().front(), "Now playing") << "Home is rebuilt when returned to";
    EXPECT_EQ(speech.last(), "Library, 2 of 8") << "and stays on the same item";
    press(Button::Home);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, HeldOkInABookOffersOptionsWithoutLeaving) {
    playAlpha();
    const auto before = player.file;
    hold(Button::Ok);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(speech.last(), "No options.");
    EXPECT_EQ(player.file, before);
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, ReturningToAListWithRepeatedLabelsKeepsThePlace) {
    touch(music / "Twins" / "01 - Song.mp3");
    touch(music / "Twins" / "02 - Song.mp3");
    start();
    open("Music");
    open("Albums A to Z");
    open("Twins");
    press(Button::Forward);
    press(Button::Ok); // the second "Song"
    ASSERT_EQ(player.file, (music / "Twins" / "02 - Song.mp3").string());
    press(Button::Home);
    EXPECT_EQ(level().index, 1u);
    EXPECT_EQ(speech.last(), "Song, 2 of 2");
}

TEST_F(AppTest, AStoredValueBetweenOptionsIsSpokenAndItsListHasNoCurrentOne) {
    settings.setInt("speech.rate", 225); // between 220 and 250
    start();
    open("Settings");
    EXPECT_EQ(speech.last(), "Settings. Speech rate, 225, 1 of 12");
    press(Button::Ok);
    EXPECT_EQ(level().index, 0u);
    for (const MenuItem& item : level().items) EXPECT_FALSE(item.value) << item.label << " is not the current value";
}

TEST_F(AppTest, ThereIsOnlyOnePlayerScreen) {
    playAlpha();
    press(Button::Ok); // Player menu
    open("Chapters");
    open("Calm");
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(app->screens().size(), 3u) << "back to the player, not a second one on top";
    hold(Button::Home);
    open("Now playing");
    EXPECT_EQ(app->screens().size(), 2u);
}

TEST_F(AppTest, KeyDescriberDescribesInsteadOfActing) {
    start();
    open("Help");
    open("Key describer");
    ASSERT_TRUE(app->keyDescriber());
    size_t index = level().index;
    press(Button::Up);
    EXPECT_EQ(level().index, index);
    EXPECT_EQ(speech.last().rfind("Up.", 0), 0u) << speech.last();
    hold(Button::Home);
    EXPECT_FALSE(app->keyDescriber());
    EXPECT_EQ(speech.last(), "Key describer off.");
}

TEST_F(AppTest, HomeMenuLayout) {
    start();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Library", "Music", "Server", "Podcasts", "Settings", "System", "Help"}));
}

// ---------------------------------------------------------------- help

TEST_F(AppTest, HelpGettingStartedExplainsTheBasics) {
    start();
    open("Help");
    open("Getting started");
    EXPECT_EQ(speech.last().rfind("This player has no screen. Left and Right move one item through a menu.", 0), 0u)
        << speech.last();
    EXPECT_NE(speech.last().find("Press deeper to jump ten items, or hold to reach the first or last item."), std::string::npos)
        << speech.last();
    EXPECT_EQ(speech.last().find("change a setting"), std::string::npos) << "Left and Right no longer change settings";
}

TEST_F(AppTest, HelpButtonsDescribesEachButton) {
    start();
    open("Help");
    open("Buttons");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play pause", "Left", "Right", "Up", "Down", "OK", "Esc",
                                                  "Volume up", "Volume down", "Options", "Talk", "Power"}));
    open("Left");
    EXPECT_EQ(speech.last(), "Left. In menus, previous item. Press deeper to go back ten items; hold for the first item. In a player, previous chapter or track, or restart after three seconds. Hold to seek back; press deeper to seek faster. Release to stop seeking.");
    open("Right");
    EXPECT_EQ(speech.last(), "Right. In menus, next item. Press deeper to go forward ten items; hold for the last item. In a player, next chapter or track. Hold to seek forward; press deeper to seek faster. Release to stop seeking.");
    EXPECT_FALSE(app->keyDescriber());
}


// ---------------------------------------------------------------- playback

TEST_F(AppTest, PlayingABookFromTheLibrary) {
    playAlpha();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(state.getString("last_book"), alpha().string());
    EXPECT_EQ(speech.last(), "Now playing. Alpha.");
}

TEST_F(AppTest, PlayWithNothingLoadedExplains) {
    start();
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Nothing to play. Choose a book in the library.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
}

TEST_F(AppTest, ABookThatCannotBeOpenedIsReportedAndYouStayInTheLibrary) {
    player.openSucceeds = false;
    playAlpha();
    EXPECT_TRUE(speech.heard("Cannot open 01 - Storm.mp3"));
    EXPECT_EQ(speech.last(), "Cannot open 02 - Calm.mp3") << "no \"Now playing\" after it";
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(level().title, "Library");
    EXPECT_EQ(state.getString("last_book"), "") << "not resumed at the next start";
    press(Button::Home);
    EXPECT_EQ(labels().front(), "Library") << "nothing loaded";
}

TEST_F(AppTest, ATrackThatCannotBeOpenedLeavesWhatWasPlaying) {
    playAlpha();
    player.pos = 20;
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    player.unopenable = {(music / "Lullaby.mp3").string()};
    open("Lullaby");
    EXPECT_EQ(speech.last(), "Cannot open Lullaby.mp3");
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(level().title, "Albums A to Z");
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_DOUBLE_EQ(player.pos, 20);
    EXPECT_TRUE(player.playing());
    hold(Button::Home);
    open("Now playing");
    EXPECT_EQ(speech.last(), "Now playing. Alpha.");
}

TEST_F(AppTest, ATrackThatCannotBeOpenedIsSkipped) {
    player.unopenable = {(roadTrip() / "01 - Sunrise.mp3").string()};
    start();
    open("Music");
    open("Play all");
    player.atEnd = true;
    app->tick(); // Lullaby ends, Sunrise cannot be opened
    EXPECT_TRUE(speech.heard("Cannot open 01 - Sunrise.mp3"));
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Highway");
}

TEST_F(AppTest, AnUnopenableLastTrackEndsTheAlbum) {
    player.unopenable = {(roadTrip() / "02 - Highway.mp3").string()};
    start();
    open("Music");
    open("Albums A to Z");
    open("Road Trip");
    open("Sunrise");
    player.atEnd = true;
    app->tick();
    EXPECT_TRUE(speech.heard("Cannot open 02 - Highway.mp3"));
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    EXPECT_TRUE(player.isOpen());
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "End of queue. Press play to start again.");
}

TEST_F(AppTest, ShortForwardAndBackMoveFilesWithTheThreeSecondRestartRule) {
    playAlpha();
    for (double seconds : {2.99, 3.0, 3.01}) {
        press(Button::Forward);
        ASSERT_EQ(player.file, alphaFile(2));
        player.pos = seconds;
        press(Button::Back);
        EXPECT_EQ(player.file, alphaFile(seconds > 3 ? 2 : 1));
        EXPECT_DOUBLE_EQ(player.pos, 0);
        if (seconds > 3) press(Button::Back);
    }
}

TEST_F(AppTest, ShortBackAtTheFirstChapterGivesBoundaryFeedback) {
    playAlpha();
    player.pos = 3;
    press(Button::Back);
    EXPECT_DOUBLE_EQ(player.pos, 3);
    EXPECT_EQ(speech.last(), "First chapter.");
    EXPECT_EQ(sounds.last(), Earcon::Edge);
}

TEST_F(AppTest, ShortChapterMovementKeepsPausedPlaybackPaused) {
    playAlpha();
    press(Button::PlayPause);
    press(Button::Forward);
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_EQ(speech.last(), "Calm");
    EXPECT_FALSE(player.playing());
}

TEST_F(AppTest, VolumeKeysChangeVolumeInPlayer) {
    playAlpha();
    press(Button::VolumeUp);
    EXPECT_EQ(settings.getInt("volume", 0), 75);
    EXPECT_FLOAT_EQ(volume.volume, 0.75f);
    press(Button::VolumeDown);
    press(Button::VolumeDown);
    EXPECT_EQ(settings.getInt("volume", 0), 65);
}

TEST_F(AppTest, PlayerVolumeIsWrittenOnlyOnExit) {
    playAlpha();
    press(Button::VolumeUp);
    press(Button::VolumeUp);
    press(Button::PlayPause);
    press(Button::VolumeDown);
    EXPECT_EQ(configFile.saves, 0) << "no config.yaml write per press";
    app->shutdown();
    EXPECT_EQ(configFile.saves, 1);
    EXPECT_EQ(configFile.onDisk("volume"), "75");
}

TEST_F(AppTest, VolumeStopsAtMaximum) {
    settings.setInt("volume", 100);
    playAlpha();
    press(Button::VolumeUp);
    EXPECT_EQ(settings.getInt("volume", 0), 100);
    EXPECT_EQ(speech.last(), "Maximum volume.");
}

TEST_F(AppTest, HelpDescribesInactiveKeysAndHeldOkAndEsc) {
    start();
    open("Help");
    open("Buttons");
    open("Down");
    EXPECT_EQ(speech.last(), "Down. Unused during normal use.");
    open("Up");
    EXPECT_EQ(speech.last(), "Up. Unused during normal use.");
    open("Talk");
    EXPECT_EQ(speech.last(), "Talk. Unused during normal use.");
    open("OK");
    EXPECT_NE(speech.last().find("Hold for options"), std::string::npos);
    open("Esc");
    EXPECT_NE(speech.last().find("without resuming"), std::string::npos);
}

// ---------------------------------------------------------------- volume and options keys

TEST_F(AppTest, VolumeKeysChangeTheVolumeInAMenuAndWriteNothingUntilExit) {
    start();
    ASSERT_EQ(app->mode(), App::Mode::Menu);
    press(Button::VolumeUp);
    EXPECT_EQ(settings.getInt("volume", 0), 75);
    EXPECT_FLOAT_EQ(volume.volume, 0.75f);
    EXPECT_EQ(speech.last(), "Volume 75 percent.");
    EXPECT_EQ(sounds.last(), Earcon::Volume);
    press(Button::VolumeDown);
    press(Button::VolumeDown);
    EXPECT_EQ(settings.getInt("volume", 0), 65);
    EXPECT_FLOAT_EQ(volume.volume, 0.65f);
    EXPECT_EQ(level().title, "Home") << "the menu did not move";
    EXPECT_EQ(configFile.saves, 0);
    app->shutdown();
    EXPECT_EQ(configFile.saves, 1);
    EXPECT_EQ(configFile.onDisk("volume"), "65");
}

TEST_F(AppTest, VolumeKeysChangeTheVolumeInThePlayerAndWriteNothingUntilExit) {
    playAlpha();
    ASSERT_EQ(app->mode(), App::Mode::Player);
    press(Button::VolumeUp);
    press(Button::VolumeUp);
    press(Button::VolumeDown);
    EXPECT_EQ(settings.getInt("volume", 0), 75);
    EXPECT_FLOAT_EQ(volume.volume, 0.75f);
    EXPECT_TRUE(player.playing()) << "volume does not pause";
    EXPECT_EQ(configFile.saves, 0);
    app->shutdown();
    EXPECT_EQ(configFile.onDisk("volume"), "75");
}

TEST_F(AppTest, HoldingAVolumeKeyRepeatsAndLettingGoStops) {
    start();
    app->keyDown(Button::VolumeUp);
    EXPECT_EQ(settings.getInt("volume", 0), 75) << "the first step is at once";
    idle(GestureDetector::kRepeatDelay - 10ms);
    EXPECT_EQ(settings.getInt("volume", 0), 75);
    int stops = speech.stops;
    idle(10ms);
    EXPECT_EQ(settings.getInt("volume", 0), 80);
    EXPECT_EQ(speech.last(), "Volume 80 percent.");
    EXPECT_GT(speech.stops, stops) << "the new volume replaces the old announcement";
    idle(GestureDetector::kRepeatInterval);
    idle(GestureDetector::kRepeatInterval);
    EXPECT_EQ(settings.getInt("volume", 0), 90);
    app->keyUp(Button::VolumeUp);
    idle(1s);
    EXPECT_EQ(settings.getInt("volume", 0), 90) << "no step after the key is let go";
    EXPECT_FLOAT_EQ(volume.volume, 0.9f);
}

TEST_F(AppTest, HoldingVolumeDownRepeatsDownwards) {
    playAlpha();
    app->keyDown(Button::VolumeDown);
    idle(GestureDetector::kRepeatDelay);
    idle(GestureDetector::kRepeatInterval);
    app->keyUp(Button::VolumeDown);
    EXPECT_EQ(settings.getInt("volume", 0), 55);
}

TEST_F(AppTest, VolumeAtMaximumPlaysTheEdgeSoundOncePerPress) {
    settings.setInt("volume", 100);
    start();
    sounds.played.clear();
    press(Button::VolumeUp);
    EXPECT_EQ(sounds.played, (std::vector<Earcon>{Earcon::Edge}));
    EXPECT_EQ(settings.getInt("volume", 0), 100);
    EXPECT_EQ(speech.last(), "Maximum volume.");
    sounds.played.clear();
    speech.said.clear();
    app->keyDown(Button::VolumeUp);
    for (int i = 0; i < 5; ++i) idle(GestureDetector::kRepeatInterval + GestureDetector::kRepeatDelay);
    app->keyUp(Button::VolumeUp);
    EXPECT_EQ(std::count(sounds.played.begin(), sounds.played.end(), Earcon::Edge), 1) << "not on each repeat";
    EXPECT_EQ(std::count(speech.said.begin(), speech.said.end(), "Maximum volume."), 1) << "not on each repeat";
}

TEST_F(AppTest, VolumeAtZeroPlaysTheEdgeSoundOncePerPress) {
    settings.setInt("volume", 0);
    start();
    sounds.played.clear();
    press(Button::VolumeDown);
    EXPECT_EQ(sounds.played, (std::vector<Earcon>{Earcon::Edge}));
    EXPECT_EQ(speech.last(), "Muted.");
    sounds.played.clear();
    app->keyDown(Button::VolumeDown);
    for (int i = 0; i < 5; ++i) idle(GestureDetector::kRepeatInterval + GestureDetector::kRepeatDelay);
    app->keyUp(Button::VolumeDown);
    EXPECT_EQ(std::count(sounds.played.begin(), sounds.played.end(), Earcon::Edge), 1);
}

TEST_F(AppTest, HoldingVolumeUpToTheLimitStepsThenPlaysTheEdgeSoundOnce) {
    settings.setInt("volume", 90);
    start();
    sounds.played.clear();
    app->keyDown(Button::VolumeUp); // 95
    idle(GestureDetector::kRepeatDelay); // 100
    for (int i = 0; i < 4; ++i) idle(GestureDetector::kRepeatInterval);
    app->keyUp(Button::VolumeUp);
    EXPECT_EQ(settings.getInt("volume", 0), 100);
    EXPECT_EQ(std::count(sounds.played.begin(), sounds.played.end(), Earcon::Volume), 2);
    EXPECT_EQ(std::count(sounds.played.begin(), sounds.played.end(), Earcon::Edge), 1);
    sounds.played.clear();
    press(Button::VolumeUp); // a new press at the limit sounds again
    EXPECT_EQ(sounds.played, (std::vector<Earcon>{Earcon::Edge}));
}

TEST_F(AppTest, ARepeatAtTheLimitDoesNotCutOffMaximumVolume) {
    settings.setInt("volume", 95);
    start();
    app->keyDown(Button::VolumeUp); // 100: "Maximum volume."
    int stops = speech.stops;
    for (int i = 0; i < 4; ++i) idle(GestureDetector::kRepeatDelay);
    app->keyUp(Button::VolumeUp);
    EXPECT_EQ(speech.stops, stops) << "a repeat that changes nothing leaves the speech alone";
    EXPECT_EQ(speech.last(), "Maximum volume.");
}

TEST_F(AppTest, AKeyThatWokeTheDeviceStaysSuppressedUntilItComesUp) {
    start();
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    app->keyDown(Button::VolumeUp); // wakes; the release is lost
    app->keyDown(Button::VolumeUp); // a duplicate cannot end suppression
    EXPECT_EQ(settings.getInt("volume", 70), 70);
    app->keyUp(Button::VolumeUp);
    press(Button::VolumeUp);
    EXPECT_EQ(settings.getInt("volume", 70), 75);
}

TEST_F(AppTest, EdgeSoundOfAHeldKeyIsHeardAgainAfterTheVolumeMoved) {
    settings.setInt("volume", 100);
    start();
    press(Button::VolumeUp);   // the edge sound
    press(Button::VolumeDown); // 95
    sounds.played.clear();
    app->keyDown(Button::VolumeUp); // 100
    for (int i = 0; i < 4; ++i) idle(GestureDetector::kRepeatDelay);
    app->keyUp(Button::VolumeUp);
    EXPECT_EQ(std::count(sounds.played.begin(), sounds.played.end(), Earcon::Edge), 1);
}

TEST_F(AppTest, AHeldVolumeKeyDoesNotWakeTheDeviceWhenItSleepsMeanwhile) {
    start();
    app->keyDown(Button::VolumeUp);
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    for (int i = 0; i < 4; ++i) idle(GestureDetector::kRepeatDelay);
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    app->keyUp(Button::VolumeUp);
    EXPECT_EQ(settings.getInt("volume", 70), 75) << "only the first step";
}

TEST_F(AppTest, OptionsSaysNoOptionsWithTheEdgeSoundInAMenuAndInThePlayer) {
    start();
    size_t index = level().index;
    press(Button::Options);
    EXPECT_EQ(speech.last(), "No options.");
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(level().index, index);
    EXPECT_EQ(level().title, "Home");

    playAlpha();
    ASSERT_EQ(app->mode(), App::Mode::Player);
    speech.said.clear();
    press(Button::Options);
    EXPECT_EQ(speech.last(), "No options.");
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, KeyDescriberNamesTheVolumeAndOptionsKeys) {
    start();
    open("Help");
    open("Key describer");
    ASSERT_TRUE(app->keyDescriber());
    press(Button::VolumeUp);
    EXPECT_EQ(speech.last(), "Volume up. Raises the volume, in menus and in the player. Hold to keep raising it.");
    press(Button::VolumeDown);
    EXPECT_EQ(speech.last(), "Volume down. Lowers the volume, in menus and in the player. Hold to keep lowering it.");
    press(Button::Options);
    EXPECT_EQ(speech.last(), "Options. Actions for the selected item. Says when there are none.");
    EXPECT_EQ(settings.getInt("volume", 70), 70) << "described, not acted on";
}

TEST_F(AppTest, HeldVolumeKeyIsDescribedOnceInTheKeyDescriber) {
    start();
    open("Help");
    open("Key describer");
    speech.said.clear();
    app->keyDown(Button::VolumeUp);
    for (int i = 0; i < 4; ++i) idle(GestureDetector::kRepeatDelay);
    app->keyUp(Button::VolumeUp);
    EXPECT_EQ(speech.said.size(), 1u);
    EXPECT_EQ(settings.getInt("volume", 70), 70);
}

TEST_F(AppTest, HeldVolumeKeyThatWokeTheDeviceDoesNotChangeTheVolume) {
    start();
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    app->keyDown(Button::VolumeUp);
    for (int i = 0; i < 4; ++i) idle(GestureDetector::kRepeatDelay);
    app->keyUp(Button::VolumeUp);
    EXPECT_NE(app->mode(), App::Mode::Sleeping);
    EXPECT_EQ(settings.getInt("volume", 70), 70);
    press(Button::VolumeUp);
    EXPECT_EQ(settings.getInt("volume", 70), 75) << "the next press works";
}

TEST_F(AppTest, HelpGettingStartedMentionsTheVolumeKeysAndOptions) {
    start();
    open("Help");
    open("Getting started");
    EXPECT_NE(speech.last().find("The volume keys change the volume everywhere"), std::string::npos) << speech.last();
    EXPECT_NE(speech.last().find("hold OK for options"), std::string::npos) << speech.last();
}

TEST_F(AppTest, PauseRemembersThePositionWithoutWritingIt) {
    playAlpha();
    player.pos = 77.3;
    press(Button::PlayPause);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(state.getString("pos:" + alpha().string()), "0:77.3");
    EXPECT_EQ(stateFile.saves, 0);
    EXPECT_EQ(configFile.saves, 0);
}

TEST_F(AppTest, ResumeAfterLongPauseRewinds) {
    playAlpha();
    player.pos = 50;
    press(Button::PlayPause);
    idle(11s);
    press(Button::PlayPause);
    EXPECT_TRUE(player.playing());
    EXPECT_DOUBLE_EQ(player.pos, 48);
}

TEST_F(AppTest, ResumeAfterShortPauseDoesNotRewind) {
    playAlpha();
    player.pos = 50;
    press(Button::PlayPause);
    idle(5s);
    press(Button::PlayPause);
    EXPECT_DOUBLE_EQ(player.pos, 50);
}

TEST_F(AppTest, MinutesOfPlaybackWriteNothing) {
    playAlpha();
    for (int i = 0; i < 10; ++i) {
        player.pos += 30;
        idle(31s);
    }
    EXPECT_EQ(stateFile.saves, 0);
    EXPECT_EQ(configFile.saves, 0);
}

TEST_F(AppTest, ChangingBooksWritesNothingYetSwitchingBackResumes) {
    playAlpha();
    player.pos = 20;
    hold(Button::Home);
    open("Library");
    open("Beta");
    EXPECT_EQ(player.file, beta().string());
    hold(Button::Home);
    open("Library");
    open("Alpha");
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_DOUBLE_EQ(player.pos, 20) << "the place was kept in memory";
    EXPECT_EQ(stateFile.saves, 0);
    EXPECT_EQ(configFile.saves, 0);
}

TEST_F(AppTest, ShutdownWritesThePositionToTheStateOnly) {
    playAlpha();
    player.pos = 33;
    app->shutdown();
    EXPECT_EQ(stateFile.saves, 1);
    EXPECT_EQ(stateFile.onDisk("pos:" + alpha().string()), "0:33.0");
    EXPECT_EQ(stateFile.onDisk("last_book"), alpha().string());
    EXPECT_EQ(configFile.saves, 0) << "config.yaml is not touched";
}

TEST_F(AppTest, SleepWritesThePositionToTheStateOnly) {
    playAlpha();
    player.pos = 44;
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_EQ(stateFile.saves, 1);
    EXPECT_EQ(stateFile.onDisk("pos:" + alpha().string()), "0:44.0");
    EXPECT_EQ(configFile.saves, 0);
}

TEST_F(AppTest, NextFileStartsWhenAFileFinishes) {
    playAlpha();
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, EndOfBookGoesBackToStartPaused) {
    playAlpha();
    player.atEnd = true;
    app->tick();
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "End of book. Press play to start again.");
}

TEST_F(AppTest, DeepPressSeeksImmediatelyAndReleaseDoesNotChangeTracks) {
    playAlpha();
    player.pos = 42;
    app->keyDown(Button::Forward);
    app->keyDown(Button::ForwardDeep);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_EQ(player.pos, 72);
    idle(700ms);
    EXPECT_EQ(player.pos, 102);
    app->keyUp(Button::ForwardDeep);
    app->keyUp(Button::Forward);
    EXPECT_EQ(player.pos, 102);
    EXPECT_EQ(speech.last(), "Position, 1 minute 42 seconds.");
    idle(1s);
    EXPECT_EQ(player.pos, 102);
    EXPECT_EQ(player.file, alphaFile(1));
}

TEST_F(AppTest, PushingDeeperAfterAnEndJumpStaysClamped) {
    start();
    open("Settings");
    app->keyDown(Button::Forward);
    clock.advance(700ms);
    app->tick();
    ASSERT_EQ(level().index, 11u);
    app->keyDown(Button::ForwardDeep);
    EXPECT_EQ(level().index, 11u);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    app->keyUp(Button::ForwardDeep);
    app->keyUp(Button::Forward);
}

TEST_F(AppTest, DeepPressInAMenuJumpsOnce) {
    start();
    open("Settings");
    app->keyDown(Button::Forward);
    app->keyDown(Button::ForwardDeep);
    app->keyUp(Button::ForwardDeep);
    app->keyUp(Button::Forward);
    EXPECT_EQ(level().index, 10u);
}

TEST_F(AppTest, HoldingRightInAMenuJumpsOnceWithNoRepeat) {
    start();
    open("Settings");
    app->keyDown(Button::Forward);
    clock.advance(700ms);
    app->tick();
    clock.advance(5s); // still held
    app->tick();
    app->keyUp(Button::Forward);
    EXPECT_EQ(level().index, 11u);
}

TEST_F(AppTest, ChaptersListOpensOnTheCurrentChapter) {
    playAlpha();
    player.atEnd = true;
    app->tick(); // now in file 2, "Calm"
    press(Button::Ok); // Player menu
    open("Chapters");
    EXPECT_EQ(speech.last(), "Chapters. Calm, 2 of 2");
}

TEST_F(AppTest, PreviousChapterGoesToChapterStartFirst) {
    playAlpha();
    player.atEnd = true;
    app->tick(); // now in file 2, "Calm"
    player.pos = 10;
    press(Button::Back);
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_EQ(speech.last(), "Calm");
    press(Button::Back);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_EQ(speech.last(), "Storm");
    press(Button::Back);
    EXPECT_EQ(speech.last(), "First chapter.");
    EXPECT_EQ(sounds.last(), Earcon::Edge);
}

TEST_F(AppTest, EmbeddedChaptersInASingleFileBook) {
    player.embedded = {{"Intro", 0}, {"Middle", 100}, {"End", 200}};
    start();
    press(Button::Ok);   // Library
    press(Button::Forward); // Beta
    press(Button::Ok);
    ASSERT_EQ(player.file, beta().string());
    player.pos = 150;
    press(Button::Forward);
    EXPECT_DOUBLE_EQ(player.pos, 200);
    EXPECT_EQ(speech.last(), "End");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Last chapter.");
    EXPECT_EQ(app->nowPlaying().chapterIndex, 2);
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
}

TEST_F(AppTest, HomeFromThePlayerMenuReturnsToThePlayer) {
    playAlpha();
    press(Button::Ok);
    EXPECT_EQ(level().title, "Player menu");
    press(Button::Home);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(speech.last(), "Now playing. Alpha.");
}

// ---------------------------------------------------------------- music

TEST_F(AppTest, MusicMenuIsPlayAllArtistsAlbumsAToZAndRefresh) {
    start();
    open("Music");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play all", "Artists", "Albums A to Z", "Playlists", "Genres", "Recently added", "Recently played", "Refresh library"}));
    EXPECT_EQ(speech.last(), "Music. Play all, 3 tracks, 1 of 8");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip"}));
    EXPECT_EQ(speech.last(), "Albums A to Z. Lullaby, 1 of 2");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Road Trip, 2 tracks, 2 of 2");
    press(Button::Back);
    press(Button::Ok); // a loose track plays straight away
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, (music / "Lullaby.mp3").string());
    EXPECT_EQ(speech.last(), "Now playing. Lullaby.");
}

TEST_F(AppTest, EmptyMusicFolderExplainsWhereMusicGoes) {
    fs::remove_all(music);
    start();
    open("Music");
    open("No music");
    EXPECT_EQ(speech.last(), "Copy music into " + music.string() + ". One folder per album.");
}

TEST_F(AppTest, CacheFolderInsideLibraryOrMusicIsNotListed) {
    cache = root / "cache";
    touch(cache / "server-1" / "al1" / "01.flac");
    start();
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta"}));
    press(Button::Home);
    app.reset();
    cache = music / "downloads";
    touch(cache / "server-1" / "al1" / "01.flac");
    start();
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip"}));
}

TEST_F(AppTest, ChoosingATrackPlaysTheRestOfTheAlbum) {
    start();
    open("Music");
    open("Albums A to Z");
    open("Road Trip");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Sunrise", "Highway"}));
    open("Highway");
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(speech.last(), "Now playing. Road Trip, Highway.");
}

TEST_F(AppTest, PlayAllPlaysEveryTrackInOrder) {
    start();
    open("Music");
    open("Play all");
    EXPECT_EQ(player.file, (music / "Lullaby.mp3").string());
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    EXPECT_TRUE(player.playing());
    player.atEnd = true;
    app->tick();
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "End of queue. Press play to start again.");
}

TEST_F(AppTest, MusicTalksAboutTracksNotChapters) {
    start();
    open("Music");
    open("Albums A to Z");
    open("Road Trip");
    open("Highway");
    player.pos = 100;
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Music player. Road Trip. Track 2 of 2, Highway. 8 minutes 20 seconds left in track. Playing.");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Last track.");
    press(Button::Ok);
    EXPECT_EQ(labels()[0], "Queue");
}

TEST_F(AppTest, SleepTimerAtEndOfTrack) {
    start();
    open("Music");
    open("Albums A to Z");
    open("Road Trip");
    open("Sunrise");
    press(Button::Ok);    // Player menu
    open("Sleep timer");
    open("End of track");
    EXPECT_TRUE(speech.heard("Sleep timer set to end of track."));
    player.pos = 599;
    idle(1s);
    EXPECT_NE(app->mode(), App::Mode::Sleeping) << "waits for the end of the track";
    player.atEnd = true;
    app->tick();          // Highway starts
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_TRUE(speech.heard("End of track. Sleep timer ended."));
}

TEST_F(AppTest, SleepTimerAtEndOfALooseTrack) {
    start();
    open("Music");
    open("Albums A to Z");
    open("Lullaby"); // one file, no chapters: the next "track" is the same one again
    press(Button::Ok); // Player menu
    open("Sleep timer");
    open("End of track");
    idle(1s);
    EXPECT_NE(app->mode(), App::Mode::Sleeping);
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_TRUE(speech.heard("End of track. Sleep timer ended."));
    press(Button::Ok); // wake
    hold(Button::Home);
    open("Settings");
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "Sleep timer, off, 12 of 12");
}

TEST_F(AppTest, MusicDoesNotTakeOverTheBookToResume) {
    playAlpha();
    player.pos = 20;
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    open("Road Trip");
    open("Sunrise");
    EXPECT_EQ(state.getString("pos:" + alpha().string()), "0:20.0") << "the book's place is kept";
    press(Button::PlayPause);
    EXPECT_EQ(state.getString("pos:" + roadTrip().string()), "");
    EXPECT_EQ(state.getString("last_book"), alpha().string());
}

TEST_F(AppTest, RestartAfterLocalMusicResumesTheMusicQueueAndKeepsTheBook) {
    playAlpha();
    player.pos = 20;
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    open("Road Trip");
    open("Sunrise");
    hold(Button::Power);
    open("Restart");
    ASSERT_EQ(app->mode(), App::Mode::Menu) << "nothing plays until it is chosen";
    ASSERT_GE(level().items.size(), 2u);
    EXPECT_EQ(labels()[0], "Now playing");
    EXPECT_EQ(valueOf(level().items[0]), "Road Trip");
    EXPECT_EQ(labels()[1], "Continue");
    EXPECT_EQ(valueOf(level().items[1]), "Alpha");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_DOUBLE_EQ(player.pos, 20) << "the book kept its place";
}

// ---------------------------------------------------------------- two players

TEST_F(AppTest, ABookThenMusicThenTheBookAgainEachResumeWhereTheyWere) {
    playAlpha();
    player.pos = 20;
    EXPECT_EQ(app->screens().back().kind, Screen::Kind::BookPlayer);
    hold(Button::Home);
    open("Music");
    open("Play all");
    EXPECT_EQ(app->screens().back().kind, Screen::Kind::MusicPlayer);
    const std::string musicFile = player.file;
    ASSERT_NE(musicFile, alphaFile(1));
    player.pos = 33;
    hold(Button::Home);
    ASSERT_EQ(labels()[0], "Now playing");
    ASSERT_EQ(labels()[1], "Continue");
    EXPECT_EQ(valueOf(level().items[1]), "Alpha") << "the book is the other player";
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(app->screens().back().kind, Screen::Kind::BookPlayer);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_DOUBLE_EQ(player.pos, 20);
    EXPECT_TRUE(player.playing());
    hold(Button::Home);
    EXPECT_EQ(valueOf(level().items[0]), "Alpha") << "the player used last comes first";
    EXPECT_EQ(labels()[1], "Continue");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(app->screens().back().kind, Screen::Kind::MusicPlayer);
    EXPECT_EQ(player.file, musicFile);
    EXPECT_DOUBLE_EQ(player.pos, 33);
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, HomeHasNoContinueWhileOnlyOnePlayerWasUsed) {
    playAlpha();
    hold(Button::Home);
    EXPECT_EQ(labels()[0], "Now playing");
    EXPECT_EQ(labels()[1], "Library");
}

TEST_F(AppTest, NowPlayingOnHomeEntersThePlayerThatIsLoaded) {
    playAlpha();
    hold(Button::Home);
    open("Music");
    open("Play all");
    const std::string musicFile = player.file;
    hold(Button::Home);
    press(Button::Ok); // Now playing: the music, which is loaded
    EXPECT_EQ(app->screens().back().kind, Screen::Kind::MusicPlayer);
    EXPECT_EQ(player.file, musicFile);
    EXPECT_EQ(player.opened.size(), 2u) << "entering the loaded player opens nothing";
}

TEST_F(AppTest, EachPlayerHasItsOwnMenu) {
    playAlpha();
    press(Button::Ok);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Chapters", "Sleep timer", "Book info", "Go to start of book", "Speed",
                                                  "Add bookmark"}));
    hold(Button::Home);
    open("Music");
    open("Play all");
    press(Button::Ok);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Queue", "Shuffle", "Repeat", "Go to first track",
                                                  "Sleep timer"}));
}

TEST_F(AppTest, ContinueThatCannotOpenKeepsThePlayerThatWasLoaded) {
    playAlpha();
    player.pos = 20;
    hold(Button::Home);
    open("Music");
    open("Play all");
    const std::string musicFile = player.file;
    hold(Button::Home);
    player.unopenable = {alphaFile(1), alphaFile(2)};
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_TRUE(speech.heard("Cannot open 01 - Storm.mp3"));
    EXPECT_EQ(player.file, musicFile) << "the music is loaded again";
    EXPECT_EQ(labels()[0], "Now playing");
    EXPECT_EQ(valueOf(level().items[0]), "All music") << "still the music player";
    EXPECT_EQ(labels()[1], "Continue");
    EXPECT_EQ(valueOf(level().items[1]), "Alpha") << "and the book still parked";
}

TEST_F(AppTest, RestartKeepsThePlayerThatWasParked) {
    start();
    open("Music");
    open("Play all");
    hold(Button::Home);
    open("Library");
    open("Alpha");
    player.pos = 20;
    hold(Button::Home);
    ASSERT_EQ(labels()[1], "Continue") << "the music is parked";
    hold(Button::Power);
    open("Restart");
    ASSERT_EQ(app->mode(), App::Mode::Player);
    hold(Button::Home);
    ASSERT_GE(level().items.size(), 2u);
    EXPECT_EQ(labels()[0], "Now playing");
    EXPECT_EQ(valueOf(level().items[0]), "Alpha");
    EXPECT_EQ(labels()[1], "Continue") << "the music queue is still parked";
    EXPECT_EQ(valueOf(level().items[1]), "All music");
}

TEST_F(AppTest, LocalMusicPlayedLastIsThePlayerOfTheRestart) {
    playAlpha();
    hold(Button::Home);
    open("Music");
    open("Play all");
    press(Button::Power);
    EXPECT_EQ(stateFile.onDisk("last_player"), "music");
}

// ---------------------------------------------------------------- library index

namespace {
// One local item as a scan leaves it.
IndexItem savedItem(const std::string& id, IndexItem::Kind kind, const std::string& title, std::vector<std::string> files) {
    IndexItem item;
    item.id = id;
    item.kind = kind;
    item.title = title;
    item.firstSeen = 100;
    for (const std::string& f : files) {
        IndexTrack t;
        t.location = f;
        t.title = fs::path(f).stem().string();
        item.tracks.push_back(t);
    }
    return item;
}
} // namespace

TEST_F(AppTest, LibraryAndMusicListTheSavedIndexBeforeTheScanIsDelivered) {
    auto saved = [this](const std::string& name) { // a file that is there: an album track that is gone is skipped
        touch(base / "saved" / (name + ".mp3"));
        return (base / "saved" / (name + ".mp3")).string();
    };
    indexStore.items["local:book:/saved/Old Book"] =
        savedItem("local:book:/saved/Old Book", IndexItem::Kind::Book, "Old Book", {"/saved/Old Book/1.mp3", "/saved/Old Book/2.mp3"});
    indexStore.items["local:album:Band\x1fOld Album"] =
        savedItem("local:album:Band\x1fOld Album", IndexItem::Kind::Music, "Old Album", {saved("a"), saved("b"), saved("c")});
    start(false);
    EXPECT_EQ(scanner.pendingCount(), 1u) << "the scan runs in the background";
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Old Book"}));
    EXPECT_EQ(speech.last(), "Library. Old Book, 2 parts, 1 of 1");
    press(Button::Home);
    open("Music");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play all", "Artists", "Albums A to Z", "Playlists", "Genres", "Recently added", "Recently played", "Refresh library"}));
    EXPECT_EQ(speech.last(), "Music. Play all, 3 tracks, 1 of 8");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Old Album"}));
    press(Button::Ok);
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(player.file, (base / "saved" / "b.mp3").string()) << "the album plays the files of the index";
}

TEST_F(AppTest, WithNoIndexYetTheListsSayScanningNotEmptyUntilTheScanIsDone) {
    start(false);
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Scanning"}));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Looking for books. Try again in a moment.");
    press(Button::Home);
    open("Music");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Scanning", "Refresh library"}));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Looking for music. Try again in a moment.");
    press(Button::Home);
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta"}));
}

TEST_F(AppTest, TheScanStartsFromTheRootsTheCacheAndTheLocalItemsOfTheIndex) {
    cache = base / "cache";
    indexStore.items["local:book:/saved/Old"] = savedItem("local:book:/saved/Old", IndexItem::Kind::Book, "Old", {"/saved/Old/1.mp3"});
    IndexItem server = savedItem("srv:album:1", IndexItem::Kind::Music, "On the server", {});
    server.source = IndexItem::Source::Server;
    indexStore.items[server.id] = server;
    start(false);
    ASSERT_EQ(scanner.requests.size(), 1u);
    const ScanRequest& request = scanner.lastRequest().request;
    EXPECT_EQ(request.libraryRoot, root);
    EXPECT_EQ(request.musicRoot, music);
    EXPECT_EQ(request.skip, cache);
    ASSERT_EQ(request.previous.size(), 1u) << "only the local items are handed back";
    EXPECT_EQ(request.previous[0].id, "local:book:/saved/Old");
    // The server item is not a local one: a scan does not remove it.
    deliverScan();
    EXPECT_TRUE(indexStore.items.count("srv:album:1"));
    EXPECT_FALSE(indexStore.items.count("local:book:/saved/Old")) << "it is no longer on disk";
}

TEST_F(AppTest, ABookAddedOnDiskAppearsAfterTheScanIsDelivered) {
    start();
    app->shutdown();
    app.reset();
    touch(root / "Gamma" / "01.mp3");
    start(false);
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta"})) << "the saved index, not a rescan";
    press(Button::Home);
    deliverScan();
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta", "Gamma"}));
    press(Button::Forward);
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(player.file, (root / "Gamma" / "01.mp3").string());
    EXPECT_TRUE(indexStore.items.count("local:book:" + (root / "Gamma").string())) << "and it is saved";
}

TEST_F(AppTest, ABookRemovedFromDiskLeavesTheLibraryAfterTheScan) {
    start();
    app->shutdown();
    app.reset();
    fs::remove_all(alpha());
    start();
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Beta"}));
    EXPECT_FALSE(indexStore.items.count("local:book:" + alpha().string()));
}

TEST_F(AppTest, TwoFoldersTaggedAsOneAlbumGiveOneAlbum) {
    touch(music / "Big Album CD1" / "a.mp3");
    touch(music / "Big Album CD2" / "b.mp3");
    for (const char* f : {"Big Album CD1/a.mp3", "Big Album CD2/b.mp3"}) {
        TrackTags t;
        t.album = "Big Album";
        t.albumArtist = "The Band";
        t.title = "Name";
        tags.tags[music / f] = t;
    }
    start();
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Big Album", "Lullaby", "Road Trip"}));
    EXPECT_EQ(speech.last(), "Albums A to Z. Big Album, 2 tracks, 1 of 3");
    press(Button::Ok);
    EXPECT_EQ(labels(), (std::vector<std::string>{"a", "b"}));
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(player.file, (music / "Big Album CD2" / "b.mp3").string());
}

// One album whose files have no album artist and a guest on one track, and an album tag spelled
// with other case on another: one entry, under the artist with the most tracks.
TEST_F(AppTest, TheFilesOfOneAlbumWithDifferentArtistsGiveOneAlbum) {
    const char* files[] = {"01.mp3", "02.mp3", "03.mp3"};
    const char* artists[] = {"Seatbelts", "Seatbelts feat. Mai Yamane", "seatbelts"};
    const char* albums[] = {"Cowboy Bebop", "Cowboy Bebop", "COWBOY  BEBOP"};
    for (int i = 0; i < 3; ++i) {
        touch(music / "Bebop" / files[i]);
        TrackTags t;
        t.album = albums[i];
        t.artist = artists[i]; // no album artist: the track artist stands in for it
        t.track = i + 1;
        tags.tags[music / "Bebop" / files[i]] = t;
    }
    start();
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Cowboy Bebop", "Lullaby", "Road Trip"}));
    EXPECT_EQ(speech.last(), "Albums A to Z. Cowboy Bebop, 3 tracks, 1 of 3");
    press(Button::Ok);
    EXPECT_EQ(labels(), (std::vector<std::string>{"01", "02", "03"})) << "every file, in order";
    hold(Button::Home);
    open("Music");
    open("Artists");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Seatbelts", "Unknown artist"})) << "no artist of one guest track";
}

// The same album name by other artists in other folders is two albums; one artist's album in
// two folders, spelled with other case, is one.
TEST_F(AppTest, AnAlbumNameOfTwoArtistsIsTwoAlbumsAndOneArtistsSpellingsAreOne) {
    auto tagged = [&](const fs::path& file, const std::string& albumArtist, const std::string& album) {
        touch(file);
        TrackTags t;
        t.album = album;
        t.albumArtist = albumArtist;
        tags.tags[file] = t;
    };
    tagged(music / "A Hits" / "a.mp3", "Alpha", "Greatest Hits");
    tagged(music / "B Hits" / "b.mp3", "Beta", "Greatest Hits");
    tagged(music / "Live CD1" / "c.mp3", "The Band", "Live");
    tagged(music / "Live CD2" / "d.mp3", "the band", "LIVE");
    start();
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Greatest Hits", "Greatest Hits", "Live", "Lullaby", "Road Trip"}));
    press(Button::Forward);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Live, 2 tracks, 3 of 5");
}

// Albums with no artist at all are joined only through a folder: two folders tagged
// "Greatest Hits" and nothing else are two albums.
TEST_F(AppTest, AlbumsWithNoArtistInTwoFoldersAreTwoAlbums) {
    for (const char* f : {"X/a.mp3", "Y/b.mp3"}) {
        touch(music / f);
        TrackTags t;
        t.album = "Greatest Hits";
        tags.tags[music / f] = t;
    }
    start();
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Greatest Hits", "Greatest Hits", "Lullaby", "Road Trip"}));
}

// One folder of singles: the same album name by two album artists is two albums.
TEST_F(AppTest, TheSameAlbumNameOfTwoAlbumArtistsInOneFolderIsTwoAlbums) {
    const std::pair<const char*, const char*> files[] = {{"a.mp3", "Alpha"}, {"b.mp3", "Beta"}};
    for (const auto& [f, artist] : files) {
        touch(music / "Singles" / f);
        TrackTags t;
        t.album = "Greatest Hits";
        t.albumArtist = t.artist = artist;
        tags.tags[music / "Singles" / f] = t;
    }
    start();
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Greatest Hits", "Greatest Hits", "Lullaby", "Road Trip"}));
}

TEST_F(AppTest, AFolderOfUntaggedFilesIsOneAlbumWithTheNameOfTheFolder) {
    touch(music / "Mix" / "disc 1" / "x.mp3");
    touch(music / "Mix" / "disc 2" / "y.mp3");
    touch(music / "Mix" / "z.flac");
    start();
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Mix", "Road Trip"}));
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Mix, 3 tracks, 2 of 3");
    press(Button::Ok);
    EXPECT_EQ(labels(), (std::vector<std::string>{"z", "x", "y"}));
}

TEST_F(AppTest, LibraryAndMusicAreSortedByTitleWithNumbersAsNumbers) {
    touch(root / "Part 10" / "1.mp3");
    touch(root / "Part 2" / "1.mp3");
    touch(music / "Vol 10" / "1.mp3");
    touch(music / "Vol 2" / "1.mp3");
    start();
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta", "Part 2", "Part 10"}));
    press(Button::Home);
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip", "Vol 2", "Vol 10"}));
}

TEST_F(AppTest, AScanThatChangesNothingWritesNoIndex) {
    start();
    EXPECT_GT(indexStore.saves, 0) << "the first scan fills the index";
    app->shutdown();
    app.reset();
    const int saves = indexStore.saves;
    clock.setWallTime(1'700'100'000); // a day later: times are not what a scan changes
    start();
    EXPECT_EQ(indexStore.saves, saves);
    EXPECT_TRUE(indexStore.removedIds.empty());
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta"}));
}

TEST_F(AppTest, ABookPlaysAndKeepsItsPlaceLikeBeforeTheIndex) {
    start();
    open("Library");
    press(Button::Ok);
    EXPECT_EQ(player.file, alphaFile(1));
    player.pos = 12;
    press(Button::PlayPause);
    app->shutdown();
    EXPECT_EQ(state.getString("last_book"), alpha().string());
    EXPECT_EQ(state.getString("pos:" + alpha().string()), "0:12.0");
}

TEST_F(AppTest, TheBookToResumeComesFromTheSavedIndexBeforeTheScanIsDelivered) {
    playAlpha();
    player.pos = 30;
    app->shutdown();
    app.reset();
    player.file.clear();
    // The index says what the book is made of: here its first part has gone from the list.
    auto& alphaItem = indexStore.items.at("local:book:" + alpha().string());
    alphaItem.tracks.erase(alphaItem.tracks.begin());
    start(false);
    EXPECT_EQ(speech.last(), "Alpha, paused. Press play to continue.");
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_DOUBLE_EQ(player.pos, 30);
}

TEST_F(AppTest, ABookToResumeThatIsGoneFromTheDiskIsNotResumed) {
    playAlpha();
    app->shutdown();
    app.reset();
    fs::remove_all(alpha());
    speech.said.clear();
    start(false); // the saved index still lists it
    for (const std::string& said : speech.said) EXPECT_EQ(said.find("paused"), std::string::npos) << said;
    EXPECT_EQ(app->mode(), App::Mode::Menu);
}

TEST_F(AppTest, ShuttingDownCancelsTheScan) {
    start(false);
    const auto id = scanner.lastRequest().id;
    app->shutdown();
    EXPECT_EQ(scanner.cancelled, (std::vector<LibraryScanner::RequestId>{id}));
    EXPECT_EQ(scanner.pendingCount(), 0u);
}

// ---------------------------------------------------------------- streaming

// An album as the server lists it. The Streaming menu passes such a list to
// playRemote(); these tests call it directly.
namespace {
std::vector<RemoteTrack> weather() {
    return {{"t1", "Storm Front", "Band", "Weather", 200, 1},
            {"t2", "Rain", "Band", "Weather", 180, 2},
            {"t3", "Sunshine", "Band", "Weather", 240, 3}};
}

// A stream URL holds the server login: it must never be spoken.
void expectNoUrlSpoken(const FakeSpeech& speech) {
    for (const std::string& text : speech.said) EXPECT_EQ(text.find("fake://"), std::string::npos) << text;
}
} // namespace

TEST_F(AppTest, ChoosingARemoteTrackPlaysItsStreamAndSaysTheServerTitle) {
    start();
    EXPECT_TRUE(app->playRemote("Weather", weather(), 1));
    EXPECT_EQ(server.requests.size(), 0u) << "the list is already there: no request";
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t2"}));
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(speech.last(), "Now playing. Weather, Rain.");
    player.atEnd = true;
    app->tick(); // the rest of the album follows
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_EQ(app->nowPlaying().chapter, "Sunshine");
    expectNoUrlSpoken(speech);
}

// A stream takes a moment to open, so the user hears why nothing plays yet:
// before every open they asked for.
TEST_F(AppTest, ARemoteTrackSaysLoadingBeforeItIsOpened) {
    start();
    std::vector<std::string> lastSaidAtOpen;
    player.beforeOpen = [&](const std::string&) { lastSaidAtOpen.push_back(speech.last()); };
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    EXPECT_EQ(lastSaidAtOpen, (std::vector<std::string>{"Loading."}));
    EXPECT_EQ(speech.last(), "Now playing. Weather, Storm Front.");
    press(Button::Forward); // next track
    EXPECT_EQ(lastSaidAtOpen.back(), "Loading.");
    EXPECT_EQ(speech.last(), "Rain");
    player.unopenable = {"fake://stream/t3"};
    press(Button::Forward); // fails: Rain is opened again on Play
    player.unopenable.clear();
    lastSaidAtOpen.clear();
    press(Button::PlayPause);
    answerPing(); // the failure marked the server down: Play asks it first
    EXPECT_EQ(lastSaidAtOpen, (std::vector<std::string>{"Loading."}));
    EXPECT_EQ(player.file, "fake://stream/t2");
    expectNoUrlSpoken(speech);
}

TEST_F(AppTest, TheNextRemoteTrackAtTheEndOfOneOpensWithoutLoading) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    speech.said.clear();
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_FALSE(speech.heard("Loading.")) << "not between every two tracks";
}

TEST_F(AppTest, ALocalBookOpensWithoutLoading) {
    playAlpha();
    press(Button::Forward);
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_FALSE(speech.heard("Loading."));
}

TEST_F(AppTest, ASingleRemoteTrackIsCalledByItsServerTitle) {
    start();
    std::vector<RemoteTrack> one = {weather()[0]};
    ASSERT_TRUE(app->playRemote("Weather", one, 0));
    EXPECT_EQ(speech.last(), "Now playing. Weather, Storm Front.");
    EXPECT_EQ(app->nowPlaying().chapter, "Storm Front");
}

TEST_F(AppTest, ARemoteTrackWithoutATitleIsCalledByItsNumber) {
    start();
    auto album = weather();
    album[1].title = "";
    ASSERT_TRUE(app->playRemote("Weather", album, 1));
    EXPECT_EQ(speech.last(), "Now playing. Weather, Track 2.");
    expectNoUrlSpoken(speech);
}

TEST_F(AppTest, AnEmptyRemoteListSaysSo) {
    start();
    EXPECT_FALSE(app->playRemote("Weather", {}, 0));
    EXPECT_EQ(speech.last(), "Nothing to play.");
    speech.said.clear();
    EXPECT_FALSE(app->playRemote("Weather", weather(), 3));
    EXPECT_EQ(speech.last(), "Nothing to play.");
    EXPECT_TRUE(player.opened.empty());
}

TEST_F(AppTest, RemoteTracksSaveNoPositionAndKeepTheBookToResume) {
    playAlpha();
    player.pos = 20;
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.pos = 50;
    press(Button::PlayPause); // pause
    press(Button::Power);     // sleep writes the state
    press(Button::Ok);        // wake
    app->shutdown();
    EXPECT_EQ(stateFile.onDisk("last_book"), alpha().string());
    for (const auto& [key, value] : stateFile.file) {
        if (key.rfind("pos:", 0) == 0) EXPECT_EQ(key, "pos:" + alpha().string());
        EXPECT_EQ(key.find("fake://"), std::string::npos) << key;
        EXPECT_EQ(value.find("fake://"), std::string::npos) << value;
    }
    EXPECT_EQ(stateFile.onDisk("pos:" + alpha().string()), "0:20.0") << "the book's place is kept";
}

TEST_F(AppTest, RemoteTracksAloneWriteTheQueueOnExitAndNothingElseNew) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.pos = 50;
    hold(Button::Power);
    open("Power off");
    app->shutdown();
    EXPECT_EQ(stateFile.saves, 1) << "the player used last";
    EXPECT_EQ(files.queueWrites, 1);
    for (const auto& [key, value] : stateFile.file) EXPECT_EQ(key.find("pos:"), std::string::npos) << key;
}

TEST_F(AppTest, ARemoteTrackThatFailsToOpenTriesNoOtherAndKeepsWhatWasLoaded) {
    playAlpha();
    player.pos = 20;
    player.unopenable = {"fake://stream/t2"};
    EXPECT_FALSE(app->playRemote("Weather", weather(), 1));
    EXPECT_TRUE(speech.heard("Cannot play Rain. Server not reachable."));
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t3"), 0) << "no other track tried";
    EXPECT_EQ(player.file, alphaFile(1)) << "the book stays loaded";
    EXPECT_DOUBLE_EQ(player.pos, 20);
    EXPECT_FALSE(player.playing()) << "paused";
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last().rfind("Book player. Alpha. Chapter 1 of 2, Storm.", 0), 0u) << speech.last();
    expectNoUrlSpoken(speech);
}

TEST_F(AppTest, ARemoteTrackThatFailsWhenTheOneBeforeEndsStopsThere) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.unopenable = {"fake://stream/t2"};
    player.pos = 200;
    player.atEnd = true;
    app->tick(); // Storm Front ends, Rain cannot be opened
    EXPECT_EQ(speech.last(), "Server not reachable. Press play to try again.") << "the queue stops at Rain";
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1", "fake://stream/t2"}))
        << "no other track tried, and no second wait on the server";
    EXPECT_FALSE(player.playing());
    // Storm Front was heard to the end: the track that failed is the current one,
    // so Play does not open the finished one again (a transcode cannot seek to its end).
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
    player.unopenable.clear(); // the server is back
    press(Button::PlayPause);
    answerPing();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_TRUE(player.playing());
    expectNoUrlSpoken(speech);
}

// Wi-Fi off during play: what was buffered runs out, then the stream fails.
TEST_F(AppTest, AStreamThatBreaksOffSaysServerNotReachableAndPlayResumesThere) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.pos = 120;
    player.broken = true;
    app->tick();
    EXPECT_EQ(speech.last(), "Server not reachable. Press play to try again.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1"})) << "no wait on the server for Rain";
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Storm Front");
    press(Button::PlayPause);
    answerPing();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 120);
    EXPECT_TRUE(player.playing());
    expectNoUrlSpoken(speech);
}

TEST_F(AppTest, TheLastStreamBreakingOffIsNotTheEndOfTheMusic) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 2));
    player.pos = 100;
    player.broken = true;
    app->tick();
    EXPECT_FALSE(speech.heard("End of queue. Press play to start again."));
    EXPECT_EQ(speech.last(), "Server not reachable. Press play to try again.");
    press(Button::PlayPause);
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_DOUBLE_EQ(player.pos, 100);
}

// ---------------------------------------------------------------- buffering

namespace {
long countOf(const std::vector<Earcon>& played, Earcon earcon) { return std::count(played.begin(), played.end(), earcon); }
long countOf(const std::vector<std::string>& said, const std::string& text) {
    return std::count(said.begin(), said.end(), text);
}
} // namespace

// A short dropout: the earcon, and the same track goes on. Nothing is said.
TEST_F(AppTest, AStreamDryForLessThanThreeSecondsGetsTheEarconOnly) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    speech.said.clear();
    sounds.played.clear();
    player.pos = 50;
    player.dry = true;
    app->tick(); // App sees it at once (it ticks every few ms)
    idle(300ms);
    EXPECT_EQ(countOf(sounds.played, Earcon::Buffering), 0) << "a moment after a seek is not a dropout";
    idle(300ms);
    EXPECT_EQ(countOf(sounds.played, Earcon::Buffering), 1);
    idle(2300ms); // 2.9 s dry
    player.dry = false;
    for (int i = 0; i < 50; ++i) idle(100ms);
    EXPECT_EQ(countOf(sounds.played, Earcon::Buffering), 1);
    EXPECT_TRUE(speech.said.empty()) << speech.last();
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1"})) << "the same track, not opened again";
    EXPECT_TRUE(player.playing());
    EXPECT_DOUBLE_EQ(player.pos, 50);
    EXPECT_EQ(app->nowPlaying().chapter, "Storm Front");
}

TEST_F(AppTest, AStreamDryForMoreThanThreeSecondsSaysWaitingForNetworkOnce) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    speech.said.clear();
    player.dry = true;
    app->tick(); // App sees it at once (it ticks every few ms)
    for (int i = 0; i < 29; ++i) idle(100ms);
    EXPECT_FALSE(speech.heard("Waiting for network.")) << "not before 3 s";
    idle(100ms);
    EXPECT_EQ(speech.last(), "Waiting for network.");
    for (int i = 0; i < 100; ++i) idle(100ms);
    EXPECT_EQ(countOf(speech.said, "Waiting for network."), 1);
    EXPECT_EQ(countOf(sounds.played, Earcon::Buffering), 1);
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1"}));
}

// Each dropout is new: after the stream has played for a while again, the next
// one gets the earcon and the phrase again.
TEST_F(AppTest, EachDropoutGetsItsOwnFeedback) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.dry = true;
    app->tick(); // App sees it at once (it ticks every few ms)
    idle(3s);
    idle(100ms);
    player.dry = false;
    for (int i = 0; i < 25; ++i) idle(100ms); // 2.4 s of audio
    player.dry = true;
    app->tick(); // App sees it at once (it ticks every few ms)
    idle(600ms);
    EXPECT_EQ(countOf(sounds.played, Earcon::Buffering), 2);
    idle(3s);
    EXPECT_EQ(countOf(speech.said, "Waiting for network."), 2);
}

// A slow link: the audio comes in bits with gaps between. That is one dropout:
// one earcon, then the phrase once it has gone on for 3 s, not an earcon per gap.
TEST_F(AppTest, AStutteringStreamIsOneDropout) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    speech.said.clear();
    sounds.played.clear();
    for (int cycle = 0; cycle < 8; ++cycle) {
        player.dry = true;
        for (int i = 0; i < 7; ++i) idle(100ms); // 0.7 s dry
        player.dry = false;
        for (int i = 0; i < 3; ++i) idle(100ms); // 0.3 s of audio
    }
    EXPECT_EQ(countOf(sounds.played, Earcon::Buffering), 1);
    EXPECT_EQ(speech.said, (std::vector<std::string>{"Waiting for network."}));
}

// Paused, nobody waits for the stream. Play during a dropout gets the feedback again.
TEST_F(AppTest, PlayDuringADropoutGetsTheFeedbackAgain) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.dry = true;
    app->tick();
    idle(600ms);
    press(Button::PlayPause); // pause
    press(Button::PlayPause); // play: still dry
    app->tick();
    sounds.played.clear();
    speech.said.clear();
    idle(600ms);
    EXPECT_EQ(countOf(sounds.played, Earcon::Buffering), 1);
    for (int i = 0; i < 30; ++i) idle(100ms);
    EXPECT_EQ(speech.said, (std::vector<std::string>{"Waiting for network."}));
}

// The phrase does not cut into other speech, such as a menu item being read: it
// comes once that has been said.
TEST_F(AppTest, WaitingForNetworkWaitsUntilOtherSpeechHasEnded) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    speech.said.clear();
    player.dry = true;
    app->tick(); // App sees it at once (it ticks every few ms)
    speech.isSpeaking = true;
    for (int i = 0; i < 50; ++i) idle(100ms);
    EXPECT_FALSE(speech.heard("Waiting for network."));
    speech.isSpeaking = false;
    idle(100ms);
    EXPECT_EQ(speech.said, (std::vector<std::string>{"Waiting for network."}));
}

// Player's retries give up (failed()): phase 1's message, and Play opens the
// track again where it stopped.
TEST_F(AppTest, ADropoutThatLastsEndsInServerNotReachable) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.pos = 80;
    player.dry = true;
    app->tick(); // App sees it at once (it ticks every few ms)
    for (int i = 0; i < 40; ++i) idle(100ms);
    ASSERT_EQ(speech.last(), "Waiting for network.");
    player.broken = true;
    idle(100ms);
    EXPECT_EQ(speech.last(), "Server not reachable. Press play to try again.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    for (int i = 0; i < 40; ++i) idle(100ms);
    EXPECT_EQ(countOf(speech.said, "Waiting for network."), 1);
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1"})) << "no other track";
    press(Button::PlayPause);
    answerPing();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 80);
    EXPECT_TRUE(player.playing());
}

// "Where am I?" in the music player: the player's name, then the album and artist of the current entry.
TEST_F(AppTest, WhereAmIInAQueueOfTwoAlbumsNamesTheAlbumAndArtistOfTheCurrentTrack) {
    start();
    std::vector<RemoteTrack> mixed = {{"t1", "Storm Front", "Band", "Weather", 200, 1},
                                      {"t2", "Blue", "Singer", "Ocean", 180, 1}};
    ASSERT_TRUE(app->playRemote("Mix", mixed, 0)); // a list that is not an album: the queue's title is "Mix"
    press(Button::Forward);
    player.pos = 0;
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Music player. Ocean, by Singer. Track 2 of 2, Blue. 10 minutes left in track. Playing.");
    EXPECT_EQ(speech.last().find("Mix"), std::string::npos) << "the queue's title is not spoken";
}

TEST_F(AppTest, WhereAmIInTheMusicPlayerLeavesOutTheByWhenTheArtistIsEmpty) {
    start();
    std::vector<RemoteTrack> tracks = {{"t1", "Storm Front", "", "Weather", 200, 1}, {"t2", "Rain", "", "Weather", 180, 2}};
    ASSERT_TRUE(app->playRemote("Weather", tracks, 0));
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last().rfind("Music player. Weather. Track 1 of 2, Storm Front.", 0), 0u) << speech.last();
}

TEST_F(AppTest, WhereAmIInTheMusicPlayerGivesTheArtistAloneWhenThereIsNoAlbum) {
    start();
    std::vector<RemoteTrack> tracks = {{"t1", "Storm Front", "Band", "", 200, 1}, {"t2", "Rain", "Band", "", 180, 2}};
    ASSERT_TRUE(app->playRemote("", tracks, 0)); // no list name either
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last().rfind("Music player. By Band. Track 1 of 2, Storm Front.", 0), 0u) << speech.last();
}

TEST_F(AppTest, WhereAmIInTheMusicPlayerSaysNothingOfAlbumAndArtistWhenNeitherIsKnown) {
    start();
    std::vector<RemoteTrack> tracks = {{"t1", "Storm Front", "", "", 200, 1}, {"t2", "Rain", "", "", 180, 2}};
    ASSERT_TRUE(app->playRemote("", tracks, 0));
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last().rfind("Music player. Track 1 of 2, Storm Front.", 0), 0u) << speech.last();
}

TEST_F(AppTest, WhereAmIInAMenuStillSpeaksThePathNotThePlayerName) {
    playAlpha();
    press(Button::Home); // the book plays on; the menu is Library
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last().find("Book player"), std::string::npos) << speech.last();
    EXPECT_EQ(speech.last().find("Music player"), std::string::npos) << speech.last();
    EXPECT_EQ(speech.last().rfind("Home, Library", 0), 0u) << speech.last();
}

// The player is closed until Play: it knows neither the place nor the length.
TEST_F(AppTest, WhereAmIAfterAFailedRemoteTrackSaysNoTimeLeft) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.pos = 42;
    player.unopenable = {"fake://stream/t2"};
    press(Button::Forward); // fails: Storm Front stays current, closed
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Music player. Weather, by Band. Track 1 of 3, Storm Front. Paused.");
    EXPECT_DOUBLE_EQ(app->nowPlaying().position, 42) << "where Play opens it";
}

TEST_F(AppTest, ALocalBookThatCannotBeOpenedLeavesTheRemoteTrackPlaying) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.pos = 42;
    player.unopenable = {alphaFile(1), alphaFile(2)};
    hold(Button::Home);
    open("Library");
    open("Alpha");
    EXPECT_TRUE(speech.heard("Cannot open 02 - Calm.mp3"));
    EXPECT_EQ(player.file, "fake://stream/t1") << "its server had nothing to do with it";
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, NextAndPreviousRemoteTrackAndWhereAmIUseServerTitles) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Rain");
    EXPECT_EQ(player.file, "fake://stream/t2");
    player.pos = 100;
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Music player. Weather, by Band. Track 2 of 3, Rain. 8 minutes 20 seconds left in track. Playing.");
    player.pos = 1;
    press(Button::Back);
    EXPECT_EQ(speech.last(), "Storm Front");
    EXPECT_EQ(player.file, "fake://stream/t1");
    press(Button::Ok); // Player menu
    open("Queue");
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"Storm Front", "Rain", "Sunshine"}));
    expectNoUrlSpoken(speech);
}

TEST_F(AppTest, SkippingToAnUnreachableRemoteTrackStaysOnThisOne) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.unopenable = {"fake://stream/t2"};
    player.pos = 42;
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Cannot play Rain. Server not reachable.");
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1", "fake://stream/t2"}));
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Storm Front");
    press(Button::PlayPause); // Storm Front again, where it was
    answerPing();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
    expectNoUrlSpoken(speech);
}

TEST_F(AppTest, ARemoteListThatFailsKeepsTheRemoteListBeforeItWithoutOpeningItAgain) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.pos = 42;
    player.unopenable = {"fake://stream/x9"};
    EXPECT_FALSE(app->playRemote("Gone", {{"x9", "Lost", "Band", "Gone", 100, 1}}, 0));
    EXPECT_TRUE(speech.heard("Cannot play Lost. Server not reachable."));
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1", "fake://stream/x9"}))
        << "no second wait on the server";
    EXPECT_EQ(app->nowPlaying().title, "Weather");
    EXPECT_EQ(app->nowPlaying().chapter, "Storm Front");
    press(Button::PlayPause);
    answerPing();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, TheEndOfARemoteListOpensTheFirstTrackOnlyOnPlay) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 2));
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(speech.last(), "End of queue. Press play to start again.");
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t3"})) << "no network until Play";
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Storm Front");
    player.unopenable = {"fake://stream/t1"};
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Cannot play Storm Front. Server not reachable.");
    player.unopenable.clear();
    press(Button::PlayPause);
    answerPing();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, GoToFirstRemoteTrackThatFailsStaysInThePlayerMenu) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 1));
    player.unopenable = {"fake://stream/t1"};
    press(Button::Ok); // Player menu
    open("Go to first track");
    EXPECT_EQ(speech.last(), "Cannot play Storm Front. Server not reachable.");
    EXPECT_EQ(level().title, "Player menu");
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
}

TEST_F(AppTest, AFailedRemoteTrackIsForgottenOnceABookPlays) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.pos = 42;
    player.unopenable = {"fake://stream/t2"};
    press(Button::Forward); // fails: Storm Front is to be opened again on Play
    hold(Button::Home);
    open("Library");
    open("Alpha");
    ASSERT_EQ(player.file, alphaFile(1));
    player.unopenable = {alphaFile(2)};
    press(Button::Forward); // Calm cannot be opened: nothing is open now
    ASSERT_EQ(speech.last(), "Cannot open 02 - Calm.mp3");
    player.opened.clear();
    press(Button::PlayPause);
    EXPECT_TRUE(player.opened.empty()) << "no retry at the remote track's place";
}

TEST_F(AppTest, ChoosingTheCurrentRemoteTrackAfterAFailureOpensIt) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.unopenable = {"fake://stream/t2"};
    press(Button::Forward); // fails: Storm Front stays the current track, closed
    ASSERT_FALSE(player.isOpen()) << "the chosen track failed";
    press(Button::Ok);     // Player menu
    open("Queue");
    open("Storm Front");
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_TRUE(player.isOpen());
    EXPECT_EQ(app->mode(), App::Mode::Player);
}

TEST_F(AppTest, ChoosingAnUnreachableTrackFromTheQueueListStaysInTheList) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.unopenable = {"fake://stream/t3"};
    press(Button::Ok); // Player menu
    open("Queue");
    open("Sunshine");
    EXPECT_EQ(speech.last(), "Cannot play Sunshine. Server not reachable.");
    EXPECT_EQ(level().title, "Queue");
    EXPECT_EQ(app->nowPlaying().chapter, "Storm Front");
    expectNoUrlSpoken(speech);
}

// ---------------------------------------------------------------- asynchronous open

// Streams open in the background, as on the real Player: App gets the result
// in tick() once the test has called player.finishOpen().
class AsyncOpenTest : public AppTest {
protected:
    // Weather plays from `index`, its stream opened.
    void playWeather(size_t index = 0) {
        start();
        player.holdStreams = true;
        ASSERT_TRUE(app->playRemote("Weather", weather(), index));
        ASSERT_TRUE(player.finishOpen());
        app->tick();
        ASSERT_TRUE(player.playing());
    }
    // Ends the pending open (it fails if its path is unopenable) and lets App see it.
    void finishOpen() {
        ASSERT_TRUE(player.finishOpen()) << "no open pending";
        app->tick();
    }
};

TEST_F(AsyncOpenTest, AFailedOpenSaysServerNotReachableAndKeepsWhatWasLoaded) {
    playAlpha();
    player.pos = 20;
    player.holdStreams = true;
    EXPECT_TRUE(app->playRemote("Weather", weather(), 1)) << "it has not failed yet";
    EXPECT_EQ(speech.last(), "Loading.");
    player.unopenable = {"fake://stream/t2"};
    finishOpen();
    EXPECT_EQ(speech.last(), "Cannot play Rain. Server not reachable.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t3"), 0) << "no other track tried";
    EXPECT_EQ(player.file, alphaFile(1)) << "the book is loaded again";
    EXPECT_DOUBLE_EQ(player.pos, 20);
    EXPECT_FALSE(player.playing()) << "paused";
    EXPECT_EQ(app->nowPlaying().title, "Alpha");
    expectNoUrlSpoken(speech);
}

// Each choice replaces the open before it, and a failure goes back to what
// played before the first.
TEST_F(AsyncOpenTest, ATrackChosenWhileAnotherOpensFallsBackToWhatPlayedBefore) {
    playWeather();
    player.pos = 42;
    EXPECT_TRUE(app->playRemote("Gone", {{"x9", "Lost", "Band", "Gone", 100, 1}}, 0));
    EXPECT_TRUE(app->playRemote("Away", {{"x8", "Far", "Band", "Away", 100, 1}}, 0));
    player.unopenable = {"fake://stream/x8"};
    finishOpen();
    EXPECT_EQ(speech.last(), "Cannot play Far. Server not reachable.");
    EXPECT_FALSE(speech.heard("Cannot play Lost. Server not reachable.")) << "its open was dropped, not failed";
    EXPECT_EQ(app->nowPlaying().title, "Weather");
    EXPECT_EQ(app->nowPlaying().chapter, "Storm Front");
    EXPECT_FALSE(player.playing());
    press(Button::PlayPause); // opens Storm Front again, where it was
    answerPing();
    EXPECT_EQ(speech.last(), "Loading.");
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(sounds.last(), Earcon::Play);
}

TEST_F(AsyncOpenTest, TheNextTrackOpensInTheBackgroundEvenWhenTheUserLeavesThePlayer) {
    playWeather();
    speech.said.clear();
    player.atEnd = true;
    app->tick();
    EXPECT_TRUE(player.opening());
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_FALSE(speech.heard("Loading."));
    press(Button::Home); // back to Home, while it opens
    ASSERT_TRUE(player.finishOpen()) << "not dropped: the user did not ask for it on that screen";
    app->tick();
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
    EXPECT_EQ(level().title, "Home") << "the user is not pulled back into the player";
}

TEST_F(AsyncOpenTest, TheNextTrackFailingInTheBackgroundStopsThere) {
    playWeather();
    player.atEnd = true;
    app->tick();
    player.unopenable = {"fake://stream/t2"};
    finishOpen();
    EXPECT_EQ(speech.last(), "Server not reachable. Press play to try again.") << "nothing else is left to play";
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Rain") << "Storm Front was heard to the end";
    player.unopenable.clear();
    press(Button::PlayPause);
    answerPing();
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_TRUE(player.playing());
}

TEST_F(AsyncOpenTest, SleepWhileATrackOpensDropsIt) {
    start();
    player.holdStreams = true;
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    press(Button::Power);
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_FALSE(player.finishOpen()) << "dropped: nothing may start playing while the device sleeps";
    app->tick();
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "Going to sleep.");
}

TEST_F(AsyncOpenTest, SkippingAgainWhileATrackOpensKeepsPlaying) {
    playWeather();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Rain");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Sunshine");
    finishOpen();
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1", "fake://stream/t2", "fake://stream/t3"}));
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_TRUE(player.playing()) << "it was playing before the first skip";
}

TEST_F(AsyncOpenTest, PauseWhileATrackOpensKeepsItPausedOnceOpen) {
    playWeather();
    press(Button::Forward); // Rain opens, to play
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Paused.");
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_TRUE(player.isOpen());
    EXPECT_FALSE(player.playing());
    press(Button::PlayPause);
    EXPECT_TRUE(player.playing());
}

TEST_F(AsyncOpenTest, PlayWhileAPausedTrackOpensPlaysItOnceOpen) {
    playWeather(1);
    press(Button::PlayPause); // paused
    press(Button::Back);       // Storm Front opens, paused like Rain
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Loading.");
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_TRUE(player.playing());
}

// A skip or Play belongs to no menu: the user may go on browsing while it opens.
TEST_F(AsyncOpenTest, ASkipStillOpensWhenTheUserLeavesThePlayer) {
    playWeather();
    press(Button::Forward);
    press(Button::Home); // back to Home
    ASSERT_TRUE(player.finishOpen()) << "not dropped";
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(level().title, "Home");
}

TEST_F(AsyncOpenTest, PowerOffWhileATrackOpensDropsIt) {
    start();
    player.holdStreams = true;
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    hold(Button::Power);
    open("Power off");
    EXPECT_TRUE(app->quitRequested());
    EXPECT_FALSE(player.finishOpen()) << "dropped: nothing may start while the device shuts down";
    app->tick(); // the front end goes on ticking while "Goodbye" is spoken
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "Powering off. Goodbye.");
}

// Player gives up after 10 s, but not during a DNS lookup: App gives up then.
TEST_F(AsyncOpenTest, AnOpenThatNeverEndsIsGivenUp) {
    start();
    player.holdStreams = true;
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    idle(14s);
    EXPECT_TRUE(player.opening());
    EXPECT_EQ(speech.last(), "Loading.");
    idle(2s);
    EXPECT_EQ(speech.last(), "Cannot play Storm Front. Server not reachable.");
    EXPECT_FALSE(player.finishOpen()) << "the player was closed: a result now never shows";
    app->tick();
    EXPECT_FALSE(player.playing());
}

TEST_F(AsyncOpenTest, AFailedChoiceKeepsTheSleepTimerOfTheBookBefore) {
    playAlpha();
    press(Button::Ok); // Player menu
    open("Sleep timer");
    open("End of chapter");
    player.holdStreams = true;
    player.unopenable = {"fake://stream/t1"};
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    finishOpen();
    ASSERT_EQ(app->nowPlaying().title, "Alpha");
    hold(Button::Home);
    open("Now playing");
    press(Button::PlayPause);
    ASSERT_TRUE(player.playing());
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_TRUE(speech.heard("End of chapter. Sleep timer ended."));
}

TEST_F(AsyncOpenTest, PlayOpensAFailedTrackAgainRewoundAfterALongPause) {
    playWeather();
    player.pos = 50;
    press(Button::PlayPause); // paused at 50
    idle(20s);
    player.unopenable = {"fake://stream/t2"};
    press(Button::Forward); // Rain fails: Storm Front is opened again on Play
    finishOpen();
    player.unopenable.clear();
    press(Button::PlayPause);
    answerPing();
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 48) << "2 s back, as after any long pause";
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(sounds.last(), Earcon::Play);
}

TEST_F(AsyncOpenTest, NoAutomaticSleepWhileTheNextTrackOpens) {
    playWeather();
    idle(6min); // no key pressed for longer than the 5 min of automatic sleep
    player.atEnd = true;
    app->tick();
    ASSERT_TRUE(player.opening());
    idle(1s);
    EXPECT_NE(app->mode(), App::Mode::Sleeping);
    finishOpen();
    EXPECT_TRUE(player.playing());
}

TEST_F(AsyncOpenTest, SleepTimerAtEndOfTrackWaitsForTheTrackSkippedTo) {
    playWeather();
    press(Button::Ok); // Player menu
    open("Sleep timer");
    open("End of track"); // back in the Player menu
    press(Button::Home);  // the player
    press(Button::Forward); // Rain opens
    idle(1s);
    EXPECT_NE(app->mode(), App::Mode::Sleeping) << "Storm Front did not end";
    finishOpen();
    idle(1s);
    EXPECT_NE(app->mode(), App::Mode::Sleeping) << "the timer waits for the end of Rain";
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_TRUE(speech.heard("End of track. Sleep timer ended."));
}

TEST_F(AsyncOpenTest, WhereAmIInThePlayerWhileATrackOpensSaysLoading) {
    playWeather();
    press(Button::Forward);
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Music player. Weather, by Band. Track 2 of 3, Rain. Loading.");
    press(Button::Ok);   // Player menu
    press(Button::Home); // back to the player
    EXPECT_EQ(speech.last(), "Now playing. Weather, Rain, loading.");
}

TEST_F(AsyncOpenTest, GoToFirstTrackEntersThePlayerOnceItHasOpened) {
    playWeather(1);
    press(Button::Ok); // Player menu
    open("Go to first track");
    EXPECT_EQ(speech.last(), "Loading.");
    EXPECT_EQ(level().title, "Player menu");
    finishOpen();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_TRUE(player.playing());
}

TEST_F(AsyncOpenTest, ALocalBookThatCannotBeOpenedOpensTheRemoteTrackAgainInTheBackground) {
    playWeather();
    player.pos = 42;
    player.unopenable = {alphaFile(1), alphaFile(2)};
    speech.said.clear();
    hold(Button::Home);
    open("Library");
    open("Alpha");
    EXPECT_TRUE(speech.heard("Cannot open 02 - Calm.mp3"));
    EXPECT_FALSE(speech.heard("Loading.")) << "the user did not ask for it";
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
}

// Only the book was asked for: the track behind it failing again is not news.
TEST_F(AsyncOpenTest, TheRemoteTrackBehindAFailedLocalBookFailsQuietly) {
    playWeather();
    player.pos = 42;
    player.unopenable = {alphaFile(1), alphaFile(2), "fake://stream/t1"};
    hold(Button::Home);
    open("Library");
    open("Alpha");
    speech.said.clear();
    sounds.played.clear();
    finishOpen();
    EXPECT_TRUE(speech.said.empty());
    EXPECT_TRUE(sounds.played.empty());
    player.unopenable.clear();
    press(Button::PlayPause); // Play tries it again, where it was
    answerPing();
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
}

// ---------------------------------------------------------------- read-ahead

// The next track's stream opens in the background near the end of a track, so
// it starts at once. Streams open only on finishOpen() here, so a track that
// plays without it was pre-opened.
class ReadAheadTest : public AsyncOpenTest {
protected:
    // Plays on, as the audio device would, for `ms` of track and clock time.
    void playFor(std::chrono::milliseconds ms) {
        player.pos += std::chrono::duration<double>(ms).count();
        idle(ms);
    }
    // Storm Front plays its last 5 s: Rain is pre-opened.
    void nearTheEnd() {
        playWeather();
        player.pos = player.length - 5;
        app->tick();
        ASSERT_EQ(player.preloaded, "fake://stream/t2");
    }
};

TEST_F(ReadAheadTest, TheNextTrackIsPreOpenedTenSecondsBeforeTheEndAndStartsAtOnce) {
    playWeather();
    player.pos = player.length - 12;
    playFor(1500ms); // 10.5 s left
    EXPECT_TRUE(player.preloads.empty()) << "not yet";
    playFor(500ms); // 10 s left
    EXPECT_EQ(player.preloads, (std::vector<std::string>{"fake://stream/t2"}));
    for (int i = 0; i < 9; ++i) playFor(1s);
    EXPECT_EQ(player.preloads.size(), 1u) << "opened once";
    speech.said.clear();
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_EQ(player.adopted, 1);
    EXPECT_FALSE(player.opening()) << "no wait for an open";
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
    EXPECT_FALSE(speech.heard("Loading."));
    EXPECT_FALSE(player.preloaded) << "taken over";
    expectNoUrlSpoken(speech);
}

TEST_F(ReadAheadTest, APreOpenThatFailedFallsBackToANormalOpen) {
    playWeather();
    player.preloadSucceeds = false; // the server did not answer in time
    player.pos = player.length - 5;
    app->tick();
    ASSERT_EQ(player.preloaded, "fake://stream/t2");
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_EQ(player.adopted, 0);
    EXPECT_TRUE(player.opening()) << "opened again, as without read-ahead";
    finishOpen();
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
    EXPECT_FALSE(speech.heard("Cannot play Rain. Server not reachable."));
}

TEST_F(ReadAheadTest, LeavingTheAlbumDropsThePreOpen) {
    nearTheEnd();
    hold(Button::Home);
    open("Library");
    open("Alpha");
    EXPECT_EQ(player.file, alphaFile(1));
    app->tick(); // the front end ticks every few ms
    EXPECT_FALSE(player.preloaded);
    player.pos = player.length - 5;
    app->tick();
    EXPECT_EQ(player.preloads, (std::vector<std::string>{"fake://stream/t2"})) << "a local book is not pre-opened";
}

TEST_F(ReadAheadTest, ChoosingAnotherRemoteListDropsThePreOpen) {
    nearTheEnd();
    std::vector<RemoteTrack> other = {{"o1", "Other", "Band", "Other", 100, 1}, {"o2", "Next", "Band", "Other", 100, 2}};
    ASSERT_TRUE(app->playRemote("Other", other, 0));
    app->tick();
    EXPECT_FALSE(player.preloaded) << "while the new list opens";
    finishOpen();
    EXPECT_FALSE(player.preloaded) << "and once it plays";
}

TEST_F(ReadAheadTest, ASeekBackDropsThePreOpenUntilTheEndComesAgain) {
    nearTheEnd();
    player.pos = 100; // e.g. a chapter list jump; skips seek the same way
    app->tick();
    EXPECT_FALSE(player.preloaded);
    player.pos = player.length - 5;
    app->tick();
    EXPECT_EQ(player.preloaded, "fake://stream/t2");
}

// A connection kept waiting may be closed by the server: a pause drops it, and Play opens it again.
TEST_F(ReadAheadTest, PausingDropsThePreOpenAndPlayOpensItAgain) {
    nearTheEnd();
    press(Button::PlayPause);
    ASSERT_FALSE(player.playing());
    app->tick(); // the front end ticks every few ms
    EXPECT_FALSE(player.preloaded);
    press(Button::PlayPause);
    ASSERT_TRUE(player.playing());
    app->tick();
    EXPECT_EQ(player.preloaded, "fake://stream/t2");
    EXPECT_EQ(player.preloads.size(), 2u);
}

// A track shorter than the read-ahead is in its last seconds as it starts.
TEST_F(ReadAheadTest, AShortTrackPreOpensTheNextAtOnce) {
    player.length = 8;
    playWeather();
    EXPECT_EQ(player.preloaded, "fake://stream/t2");
    player.atEnd = true;
    app->tick();
    ASSERT_EQ(player.file, "fake://stream/t2");
    EXPECT_EQ(player.preloaded, "fake://stream/t3") << "pre-opened as Rain starts";
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.adopted, 2);
    EXPECT_TRUE(player.playing());
}

TEST_F(ReadAheadTest, SleepDropsThePreOpen) {
    nearTheEnd();
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    app->tick(); // the front end ticks every few ms
    EXPECT_FALSE(player.preloaded);
    idle(1min);
    EXPECT_FALSE(player.preloaded) << "not pre-opened again while asleep";
}

TEST_F(ReadAheadTest, PowerOffDropsThePreOpen) {
    nearTheEnd();
    hold(Button::Power);
    open("Power off");
    ASSERT_TRUE(app->quitRequested());
    app->tick(); // the front end goes on ticking while "Goodbye" is spoken
    EXPECT_FALSE(player.preloaded);
}

TEST_F(ReadAheadTest, ShutdownDropsThePreOpen) {
    nearTheEnd();
    app->shutdown();
    EXPECT_FALSE(player.preloaded);
}

TEST_F(ReadAheadTest, TheLastTrackPreOpensNothing) {
    playWeather(1);
    player.pos = player.length - 5;
    app->tick();
    EXPECT_EQ(player.preloads, (std::vector<std::string>{"fake://stream/t3"})) << "the last track is pre-opened";
    player.atEnd = true;
    app->tick();
    ASSERT_EQ(player.file, "fake://stream/t3");
    player.pos = player.length - 5;
    app->tick();
    EXPECT_EQ(player.preloads.size(), 1u) << "nothing follows it";
}

TEST_F(ReadAheadTest, ATrackOfUnknownLengthPreOpensNothing) {
    player.length = 0; // a transcoded stream may not say
    playWeather();
    player.pos = 300;
    app->tick();
    EXPECT_TRUE(player.preloads.empty());
}

TEST_F(ReadAheadTest, ALocalBookIsNotPreOpened) {
    playAlpha();
    player.pos = player.length - 5;
    app->tick();
    EXPECT_TRUE(player.preloads.empty());
}

// ---------------------------------------------------------------- scrobble

// The server hears "now playing" when a remote track starts to play, and that
// it was played once it has been heard for half its length or 4 minutes.
class ScrobbleTest : public AsyncOpenTest {
protected:
    void SetUp() override {
        AsyncOpenTest::SetUp();
        settings.setString("server.url", "https://music.example"); // a submission is kept for the server set up
        settings.setString("server.user", "ice");
    }
    using Scrobble = FakeMusicServer::Scrobble;
    static Scrobble nowPlaying(const std::string& id) { return {id, false}; }
    static Scrobble played(const std::string& id) { return {id, true}; }
    // Ticks through `ms` of playback, a second at a time, as the device's loop would.
    void listen(std::chrono::milliseconds ms) {
        for (; ms >= 1s; ms -= 1s) idle(1s);
        if (ms > 0ms) idle(ms);
    }
};

TEST_F(ScrobbleTest, NowPlayingIsSentOnceTheTrackHasOpenedAndPlays) {
    start();
    player.holdStreams = true;
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    idle(1s);
    EXPECT_TRUE(server.scrobbles.empty()) << "chosen, not playing yet";
    finishOpen();
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1")}));
    listen(30s);
    EXPECT_EQ(server.scrobbles.size(), 1u) << "once per play";
    expectNoUrlSpoken(speech);
}

TEST_F(ScrobbleTest, ATrackThatFailsToOpenIsNotScrobbled) {
    start();
    player.holdStreams = true;
    player.unopenable = {"fake://stream/t1"};
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    finishOpen();
    ASSERT_EQ(speech.last(), "Cannot play Storm Front. Server not reachable.");
    listen(5min);
    EXPECT_TRUE(server.scrobbles.empty());
}

TEST_F(ScrobbleTest, ATrackOpenedPausedSaysNowPlayingOnlyOnPlay) {
    playWeather();
    press(Button::PlayPause); // paused
    press(Button::Forward);    // Rain opens, paused
    finishOpen();
    listen(10s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1")}));
    press(Button::PlayPause);
    idle(10ms); // the next tick
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1"), nowPlaying("t2")}));
}

TEST_F(ScrobbleTest, ATrackIsPlayedOnceHalfOfItHasBeenHeard) {
    player.length = 200;
    playWeather();
    listen(99s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1")})) << "not half yet";
    listen(1s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1"), played("t1")}));
    listen(90s);
    EXPECT_EQ(server.scrobbles.size(), 2u) << "once per play";
}

TEST_F(ScrobbleTest, ALongTrackIsPlayedAfterFourMinutes) {
    player.length = 3600; // half of it would be 30 minutes
    playWeather();
    listen(239s);
    EXPECT_EQ(server.scrobbles.size(), 1u);
    listen(1s);
    EXPECT_EQ(server.scrobbles.back(), played("t1"));
}

// A transcode (or a stream that does not say) has the length the server lists.
TEST_F(ScrobbleTest, WithoutALengthFromThePlayerTheServersLengthCounts) {
    player.length = 0;
    playWeather(); // Storm Front: 200 s on the server
    listen(99s);
    EXPECT_EQ(server.scrobbles.size(), 1u);
    listen(1s);
    EXPECT_EQ(server.scrobbles.back(), played("t1"));
}

TEST_F(ScrobbleTest, ATrackOfUnknownLengthIsPlayedAfterFourMinutes) {
    player.length = 0;
    start();
    std::vector<RemoteTrack> tracks = {{"t9", "Drone", "Band", "Weather", 0, 1}};
    ASSERT_TRUE(app->playRemote("Weather", tracks, 0));
    app->tick(); // it plays from here
    listen(239s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t9")}));
    listen(1s);
    EXPECT_EQ(server.scrobbles.back(), played("t9"));
}

TEST_F(ScrobbleTest, ASkipBeforeHalfSendsNoSubmission) {
    player.length = 200;
    playWeather();
    listen(60s);
    press(Button::Forward); // Rain
    finishOpen();
    listen(60s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1"), nowPlaying("t2")}));
}

TEST_F(ScrobbleTest, TheNextTrackAtTheEndOfOneIsScrobbledToo) {
    player.length = 200;
    playWeather();
    listen(100s);
    player.pos = 195; // pre-opens Rain
    app->tick();
    player.atEnd = true;
    app->tick();
    ASSERT_EQ(player.file, "fake://stream/t2");
    ASSERT_EQ(player.adopted, 1);
    EXPECT_EQ(server.scrobbles,
              (std::vector<Scrobble>{nowPlaying("t1"), played("t1"), nowPlaying("t2")}));
}

TEST_F(ScrobbleTest, PausedTimeDoesNotCount) {
    player.length = 200;
    playWeather();
    listen(50s);
    press(Button::PlayPause); // pause
    listen(4min);
    press(Button::PlayPause); // play
    listen(45s);
    EXPECT_EQ(server.scrobbles.size(), 1u) << "about 95 s heard";
    listen(10s);
    EXPECT_EQ(server.scrobbles.back(), played("t1"));
}

// Heard time, not the place reached: jumping to the middle is not listening.
TEST_F(ScrobbleTest, ASeekForwardDoesNotCount) {
    player.length = 200;
    playWeather();
    for (int i = 0; i < 5; ++i) deep(Button::Forward); // 150 s on
    ASSERT_GE(player.pos, 150);
    listen(30s);
    EXPECT_EQ(server.scrobbles.size(), 1u);
}

TEST_F(ScrobbleTest, TimeTheStreamIsDryDoesNotCount) {
    player.length = 200;
    playWeather();
    listen(50s);
    player.dry = true;
    listen(2min);
    player.dry = false;
    listen(45s);
    EXPECT_EQ(server.scrobbles.size(), 1u) << "about 95 s heard";
    listen(10s);
    EXPECT_EQ(server.scrobbles.back(), played("t1"));
}

TEST_F(ScrobbleTest, RestartingATrackCountsItAgain) {
    player.length = 200;
    playWeather();
    listen(100s);
    player.pos = 100;
    press(Button::Back); // more than 3 s in: back to its start
    ASSERT_EQ(player.file, "fake://stream/t1");
    ASSERT_DOUBLE_EQ(player.pos, 0);
    idle(0ms); // observe the new play after the short release action
    listen(100s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1"), played("t1"), nowPlaying("t1"),
                                                       played("t1")}));
}

TEST_F(ScrobbleTest, PlayingTheListAgainAfterItsEndCountsAgain) {
    player.length = 200;
    start();
    std::vector<RemoteTrack> one = {weather()[0]};
    ASSERT_TRUE(app->playRemote("Weather", one, 0));
    app->tick(); // it plays from here
    listen(100s);
    player.atEnd = true;
    app->tick();
    ASSERT_TRUE(speech.heard("End of queue. Press play to start again."));
    player.atEnd = false;
    press(Button::PlayPause);
    listen(1s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1"), played("t1"), nowPlaying("t1")}));
}

// Play after the stream broke off opens it again where it was: the same play goes on.
TEST_F(ScrobbleTest, ATrackOpenedAgainAfterItBrokeOffGoesOnWithItsPlay) {
    player.length = 200;
    playWeather();
    listen(60s);
    player.pos = 60;
    player.broken = true;
    app->tick();
    ASSERT_EQ(speech.last(), "Server not reachable. Press play to try again.");
    listen(1min);
    press(Button::PlayPause);
    answerPing();
    finishOpen();
    ASSERT_DOUBLE_EQ(player.pos, 60);
    listen(35s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1")})) << "about 95 s heard";
    listen(10s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1"), played("t1")}));
}

// The place it opens at is rewound to 0 after a long pause, yet it is the
// same play: what is said and counted does not depend on that place.
TEST_F(ScrobbleTest, ATrackOpenedAgainAtItsStartAfterAFailedChoiceGoesOnWithItsPlay) {
    player.length = 200;
    playWeather();
    listen(98s);
    player.pos = 1;
    press(Button::PlayPause); // paused at 1 s
    listen(20s);
    player.unopenable = {"fake://stream/t2"};
    press(Button::Forward); // Rain fails: Storm Front opens again on Play
    finishOpen();
    player.unopenable.clear();
    press(Button::PlayPause);
    answerPing();
    finishOpen();
    ASSERT_EQ(player.file, "fake://stream/t1");
    ASSERT_DOUBLE_EQ(player.pos, 0) << "rewound 2 s";
    listen(5s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1"), played("t1")}));
}

// Going back to the first chapter inside a track is a seek, like a skip back.
TEST_F(ScrobbleTest, AChapterInsideATrackIsNotARestart) {
    player.length = 200;
    player.embedded = {{"Intro", 0}, {"Song", 50}};
    start();
    std::vector<RemoteTrack> one = {weather()[0]};
    ASSERT_TRUE(app->playRemote("Weather", one, 0));
    app->tick();
    listen(20s);
    player.pos = 20;
    press(Button::Back); // the start of Intro, the first chapter
    ASSERT_DOUBLE_EQ(player.pos, 0);
    listen(80s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1"), played("t1")}));
}

// A local book that cannot be opened: the track behind it opens again where it was.
TEST_F(ScrobbleTest, ATrackBackBehindAFailedBookGoesOnWithItsPlay) {
    player.length = 200;
    playWeather();
    listen(60s);
    player.pos = 60;
    player.unopenable = {alphaFile(1), alphaFile(2)};
    hold(Button::Home);
    open("Library");
    open("Alpha");
    finishOpen();
    ASSERT_EQ(player.file, "fake://stream/t1");
    ASSERT_TRUE(player.playing());
    listen(45s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1"), played("t1")}));
}

TEST_F(ScrobbleTest, RestartingTheOnlyTrackCountsItAgain) {
    player.length = 200;
    start();
    std::vector<RemoteTrack> one = {weather()[0]};
    ASSERT_TRUE(app->playRemote("Weather", one, 0));
    app->tick();
    listen(30s);
    player.pos = 30;
    press(Button::Back); // back to its start
    ASSERT_DOUBLE_EQ(player.pos, 0);
    listen(1s);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1"), nowPlaying("t1")}));
}

TEST_F(ScrobbleTest, LocalFilesAreNeverScrobbled) {
    playAlpha();
    listen(10min);
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    open("Lullaby");
    listen(10min);
    EXPECT_TRUE(server.scrobbles.empty());
}

// A local book chosen while a remote track plays stops its count.
TEST_F(ScrobbleTest, ALocalBookAfterARemoteTrackIsNotCountedForIt) {
    player.length = 200;
    playWeather();
    listen(60s);
    hold(Button::Home);
    open("Library");
    open("Alpha");
    ASSERT_EQ(player.file, alphaFile(1));
    listen(5min);
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t1")}));
}

// ---------------------------------------------------------------- streaming menu

namespace {
std::vector<RemoteAlbum> someAlbums(const std::string& prefix, int n) {
    std::vector<RemoteAlbum> out;
    for (int i = 1; i <= n; ++i) out.push_back({prefix + std::to_string(i), prefix + " " + std::to_string(i), "Band", 3});
    return out;
}
using Kind = FakeMusicServer::Kind;
} // namespace

class StreamingTest : public AppTest {
protected:
    // Home > Server
    void openServer() {
        start();
        open("Server");
    }
    // Puts a playlist in the index of the server the App is set up for. Call it before start().
    void indexPlaylist(const std::string& id, const std::string& name, int songs) {
        if (settings.getString("server.url").empty()) settings.setString("server.url", "https://music.example");
        indexStore.playlists.push_back({TrackCache::serverKey(settings.getString("server.url"), settings.getString("server.user")),
                                        id, name, songs, 0});
    }
    // Home > Music > Playlists > `name`; its tracks are loading.
    void openPlaylist(const std::string& name) {
        start();
        open("Music");
        open("Playlists");
        open(name);
    }
    // Home > Server > `list`; the list is loading.
    void openList(const std::string& list) {
        openServer();
        open(list);
    }
    // Answers the request App made last and lets App see it.
    template <class T> void answer(std::vector<T> items, bool more = false) {
        ASSERT_TRUE(server.reply(server.lastRequest().id, std::move(items), more));
        app->tick();
    }
    std::size_t requestsOf(Kind kind) const {
        return size_t(std::count_if(server.requests.begin(), server.requests.end(),
                                    [kind](const FakeMusicServer::Request& r) { return r.kind == kind; }));
    }
};

TEST_F(StreamingTest, StreamingIsOnHomeUnderTheServerName) {
    settings.setString("server.name", "Home server");
    settings.setString("server.url", "https://music.example"); // a name without a url is no server
    start();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Library", "Music", "Home server", "Podcasts", "Settings", "System",
                                                  "Help"}));
    const size_t asked = server.requests.size(); // the catalogue sync of a server that is set up
    open("Home server");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Recently added", "Albums A to Z", "Artists",
                                                  "Random albums", "Starred", "Server status"}));
    EXPECT_EQ(server.requests.size(), asked) << "nothing is asked before a list is opened";
}

TEST_F(StreamingTest, TheNameOfAServerInTheServersListIsOnHome) {
    settings.setString("servers.0.name", "Audio shelf");
    settings.setString("servers.0.url", "https://music.example");
    start();
    EXPECT_EQ(labels()[2], "Audio shelf");
}

TEST_F(StreamingTest, AnEmptyServerNameIsCalledServer) {
    settings.setString("server.name", "");
    start();
    EXPECT_EQ(labels()[2], "Server");
}

TEST_F(StreamingTest, AListSaysLoadingThenFillsAndSpeaksTheFirstItem) {
    openList("Recently added");
    EXPECT_EQ(speech.last(), "Recently added. Loading, 1 of 1");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Loading"}));
    const auto& asked = server.lastRequest();
    EXPECT_EQ(asked.kind, Kind::Albums);
    EXPECT_EQ(asked.arg, "newest");
    EXPECT_EQ(asked.offset, 0);
    EXPECT_EQ(asked.size, 500);
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Still loading.");
    answer(someAlbums("Storm", 2));
    EXPECT_EQ(labels(), (std::vector<std::string>{"Storm 1", "Storm 2"}));
    EXPECT_EQ(speech.last(), "Storm 1, Band, 1 of 2");
    EXPECT_EQ(server.requests.size(), 1u);
}

TEST_F(StreamingTest, HomeWhileLoadingDropsTheLateAnswer) {
    openList("Recently added");
    auto id = server.lastRequest().id;
    press(Button::Home);
    EXPECT_EQ(level().title, "Server");
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), id), server.cancelled.end());
    EXPECT_FALSE(server.reply(id, someAlbums("Late", 1))) << "the answer is dropped";
    speech.said.clear();
    app->tick();
    EXPECT_TRUE(speech.said.empty());
    EXPECT_EQ(labels().front(), "Recently added");
}

TEST_F(StreamingTest, HoldingHomeWhileLoadingCancelsToo) {
    openList("Albums A to Z");
    auto id = server.lastRequest().id;
    hold(Button::Home);
    EXPECT_EQ(app->screens().size(), 1u);
    EXPECT_FALSE(server.reply(id, someAlbums("Late", 1)));
    EXPECT_EQ(server.pendingCount(), 0u);
}

TEST_F(StreamingTest, AnAnswerForArtistsDoesNotFillRandomAlbumsOpenedAfterIt) {
    openList("Artists");
    auto artists = server.lastRequest().id;
    press(Button::Home);
    open("Random albums");
    auto random = server.lastRequest().id;
    EXPECT_FALSE(server.reply(artists, std::vector<RemoteArtist>{{"a1", "Band", 2}}));
    app->tick();
    EXPECT_EQ(level().title, "Random albums");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Loading"}));
    ASSERT_TRUE(server.reply(random, std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}}));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Weather"}));
    EXPECT_EQ(speech.last(), "Weather, Band, 1 of 1");
}

TEST_F(StreamingTest, AlbumsAToZWaitsForLoadMoreAndAnnouncesTheLoadedBoundary) {
    openList("Albums A to Z");
    answer(someAlbums("A", 500), true);
    ASSERT_EQ(requestsOf(Kind::Albums), 1u);
    EXPECT_EQ(labels().back(), "Load more");
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "Load more, 501 of 501. More items are available. Load more to continue.");
    deep(Button::Forward);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(requestsOf(Kind::Albums), 1u) << "end jumps never request a page";
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Loading, 501 of 501");
    ASSERT_EQ(requestsOf(Kind::Albums), 2u);
    EXPECT_EQ(server.lastRequest().offset, 500);
    EXPECT_EQ(server.lastRequest().size, 500);
    EXPECT_EQ(server.lastRequest().arg, "alphabeticalByName");
    answer(someAlbums("B", 1), false);
    EXPECT_EQ(speech.last(), "B 1, Band, 501 of 501");
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "B 1, Band, 501 of 501");
    EXPECT_EQ(requestsOf(Kind::Albums), 2u);
    hold(Button::Back);
    EXPECT_EQ(speech.last(), "A 1, Band, 1 of 501");
}

TEST_F(StreamingTest, TheNextPageStartsAfterWhatCame) {
    openList("Albums A to Z");
    answer(someAlbums("A", 2), /*more=*/true); // a server that sends shorter pages
    open("Load more");
    EXPECT_EQ(server.lastRequest().offset, 2) << "nothing is skipped";
}

TEST_F(StreamingTest, AFirstPageWithoutMoreLoadsNoOther) {
    openList("Albums A to Z");
    answer(someAlbums("A", 2), /*more=*/false);
    EXPECT_EQ(requestsOf(Kind::Albums), 1u);
}

TEST_F(StreamingTest, ALaterPageFillsTheListSilentlyWhenTheUserHasGoneDeeper) {
    openList("Albums A to Z");
    answer(someAlbums("A", 1), /*more=*/true);
    open("Load more");
    auto page2 = server.lastRequest().id;
    hold(Button::Back);
    open("A 1"); // the album's tracks are loading
    ASSERT_TRUE(server.reply(page2, someAlbums("B", 1)));
    speech.said.clear();
    app->tick();
    EXPECT_TRUE(speech.said.empty());
    EXPECT_EQ(level().title, "A 1");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Loading"}));
    press(Button::Home);
    EXPECT_EQ(labels(), (std::vector<std::string>{"A 1", "B 1"}));
    EXPECT_EQ(speech.last(), "A 1, Band, 1 of 2");
}

TEST_F(StreamingTest, RandomAlbumsIsOnePageOfFifty) {
    openList("Random albums");
    EXPECT_EQ(server.lastRequest().kind, Kind::Albums);
    EXPECT_EQ(server.lastRequest().arg, "random");
    EXPECT_EQ(server.lastRequest().offset, 0);
    EXPECT_EQ(server.lastRequest().size, 50);
    answer(someAlbums("R", 3), /*more=*/true);
    EXPECT_EQ(requestsOf(Kind::Albums), 1u) << "another page would be a new draw";
    EXPECT_EQ(labels().size(), 3u);
}

TEST_F(StreamingTest, RecentlyAddedIsOnePage) {
    openList("Recently added");
    answer(someAlbums("N", 3), /*more=*/true);
    EXPECT_EQ(requestsOf(Kind::Albums), 1u);
}

TEST_F(StreamingTest, ReturningToALoadedListKeepsItWithoutAskingAgain) {
    openList("Recently added");
    answer(someAlbums("N", 3));
    press(Button::Forward);
    open("N 2");
    press(Button::Home);
    EXPECT_EQ(labels(), (std::vector<std::string>{"N 1", "N 2", "N 3"}));
    EXPECT_EQ(speech.last(), "N 2, Band, 2 of 3");
    EXPECT_EQ(requestsOf(Kind::Albums), 1u);
}

TEST_F(StreamingTest, AnErrorIsAListWithTryAgain) {
    openList("Recently added");
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not reachable", "Try again"}));
    EXPECT_EQ(speech.last(), "Server not reachable, 1 of 2");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Check that Wi-Fi is on and the server is running, then choose Try again.");
    open("Try again");
    EXPECT_EQ(speech.last(), "Loading, 1 of 1");
    ASSERT_EQ(requestsOf(Kind::Albums), 2u);
    EXPECT_EQ(server.lastRequest().arg, "newest");
    EXPECT_EQ(server.lastRequest().offset, 0);
    answer(someAlbums("N", 2));
    EXPECT_EQ(labels(), (std::vector<std::string>{"N 1", "N 2"}));
    EXPECT_EQ(speech.last(), "N 1, Band, 1 of 2");
}

TEST_F(StreamingTest, AFailedLaterPageCanBeTriedAgain) {
    openList("Albums A to Z");
    answer(someAlbums("A", 2), /*more=*/true);
    open("Load more");
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Server));
    hold(Button::Back);
    speech.said.clear();
    app->tick();
    EXPECT_TRUE(speech.said.empty()) << "the user is on a loaded item";
    EXPECT_EQ(labels(), (std::vector<std::string>{"A 1", "A 2", "Server error", "Try again"}));
    hold(Button::Forward);
    press(Button::Ok); // Try again
    EXPECT_EQ(speech.last(), "Loading, 3 of 3");
    EXPECT_EQ(server.lastRequest().offset, 2);
    answer(someAlbums("B", 1));
    EXPECT_EQ(labels(), (std::vector<std::string>{"A 1", "A 2", "B 1"}));
    EXPECT_EQ(speech.last(), "B 1, Band, 3 of 3");
}

TEST_F(StreamingTest, ABadLoginSaysWhatToCheck) {
    openList("Artists");
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::BadLogin));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Wrong user name or password", "Try again"}));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Check the user and password in the server section of config.yaml.");
}

TEST_F(StreamingTest, AnAnswerWhileAsleepFillsTheListSilently) {
    openList("Recently added");
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    speech.said.clear();
    answer(someAlbums("N", 1));
    EXPECT_TRUE(speech.said.empty());
    press(Button::Ok); // wake
    EXPECT_EQ(speech.last(), "Recently added. N 1, Band, 1 of 1");
}

TEST_F(StreamingTest, AnAnswerUnderThePowerMenuFillsTheListSilently) {
    openList("Recently added");
    hold(Button::Power);
    ASSERT_EQ(level().title, "Power");
    speech.said.clear();
    answer(someAlbums("N", 1));
    EXPECT_TRUE(speech.said.empty());
    press(Button::Home);
    EXPECT_EQ(speech.last(), "N 1, Band, 1 of 1");
}

TEST_F(StreamingTest, WithoutAServerTheMenuSaysHowToSetOneUp) {
    server.isConfigured = false;
    openServer();
    EXPECT_EQ(labels(), (std::vector<std::string>{"No server set up"}));
    EXPECT_EQ(speech.last(), "Server. No server set up, 1 of 1");
    press(Button::Ok);
    EXPECT_NE(speech.last().find("add a server section"), std::string::npos) << speech.last();
    EXPECT_NE(speech.last().find("config.yaml"), std::string::npos) << speech.last();
    EXPECT_TRUE(server.requests.empty());
}

TEST_F(StreamingTest, AnEmptyListOfServersSaysNoServerSetUp) {
    noServerLinks = true;
    openServer();
    EXPECT_EQ(labels(), (std::vector<std::string>{"No server set up"}));
    press(Button::Ok);
    EXPECT_NE(speech.last().find("add a server section"), std::string::npos) << speech.last();
    EXPECT_TRUE(server.requests.empty()) << "the fake is not on the list";
    idle(1s);
}

TEST_F(StreamingTest, AnEmptyListWithAServerInTheConfigSaysItIsNotSupported) {
    noServerLinks = true; // the front end made no link for it
    settings.setString("server.type", "audiobookshelf");
    settings.setString("server.url", "https://books.example");
    openServer();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not supported"}));
}

TEST_F(StreamingTest, ALoadingListOfTheFirstServerAsksNothingOfTheOthers) {
    FakeMusicServer second;
    FakeDownloader secondDownloader;
    moreLinks.push_back(ServerLink{ServerConfig{}, "other-1", "Server 2", second, secondDownloader});
    openServer();
    open("Recently added");
    EXPECT_EQ(server.requests.size(), 1u);
    answer(someAlbums("N", 1));
    idle(1s);
    for (const auto& r : second.requests) EXPECT_EQ(r.kind, FakeMusicServer::Kind::CatalogueVersion) << "only its own sync, no list";
}

// Two servers: the first is the fixture's `server` ("Music"), the second a fake of its own ("Audiobooks").
class TwoServersTest : public StreamingTest {
protected:
    FakeMusicServer second;
    FakeDownloader secondDownloader;
    void SetUp() override {
        StreamingTest::SetUp();
        settings.setString("servers.0.name", "Navidrome");
        settings.setString("servers.0.url", "https://music.example");
        ServerConfig config;
        config.name = "Audiobooks";
        config.url = "https://books.example";
        moreLinks.push_back(ServerLink{config, "books-1", "Audiobooks", second, secondDownloader});
    }
    // Home > `name`
    void openLink(const std::string& name) {
        start();
        open(name);
    }
};

TEST_F(TwoServersTest, HomeHasAnEntryForEachServerBetweenMusicAndPodcasts) {
    settings.setString("servers.0.name", "Music");
    start();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Library", "Music", "Music", "Audiobooks", "Podcasts", "Settings",
                                                  "System", "Help"}));
}

TEST_F(TwoServersTest, HomeSpeaksTheNameOfEachLink) {
    moreLinks.clear();
    moreLinks.push_back(ServerLink{ServerConfig{}, "other-1", "Server 2", second, secondDownloader});
    settings.setString("servers.0.name", "");
    start();
    EXPECT_EQ(labels().at(2), "Server") << "one server, no name";
    app.reset();
    // makeServers names a link of several "Server N" by its place, and App speaks what it is given.
    auto links = serverLinks();
    links[0].name = "Server 1";
    app = std::make_unique<App>(Services{speech, player, sounds, volume, std::move(links), indexStore, scanner, settings, state, clock, textFiles}, root, music, cache, queueFile, scrobbleFile);
    app->start();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    EXPECT_EQ(labels().at(2), "Server 1");
    EXPECT_EQ(labels().at(3), "Server 2");
}

TEST_F(TwoServersTest, EachEntryOpensTheStreamingMenuOfItsServer) {
    openLink("Audiobooks");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Recently added", "Albums A to Z", "Artists", "Random albums",
                                                  "Starred", "Server status"}));
    EXPECT_EQ(level().title, "Audiobooks");
}

TEST_F(TwoServersTest, RecentlyAddedOnTheSecondServerAsksOnlyTheSecond) {
    start();
    const size_t asked = server.requests.size(); // the catalogue sync of the first
    open("Audiobooks");
    open("Recently added");
    EXPECT_EQ(server.requests.size(), asked);
    ASSERT_EQ(second.requests.size(), 1u);
    EXPECT_EQ(second.lastRequest().kind, Kind::Albums);
    EXPECT_EQ(second.lastRequest().arg, "newest");
    EXPECT_EQ(speech.last(), "Recently added. Loading, 1 of 1");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"b1", "Dune", "Herbert", 3}}, false));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Dune"}));
    open("Dune"); // the album's tracks come from the same server
    EXPECT_EQ(second.lastRequest().kind, Kind::AlbumTracks);
    EXPECT_EQ(second.lastRequest().arg, "b1");
    EXPECT_EQ(server.requests.size(), asked);
}

TEST_F(TwoServersTest, TheFirstServersListsStillAskTheFirst) {
    start();
    open("Navidrome");
    open("Random albums");
    EXPECT_EQ(server.lastRequest().kind, Kind::Albums);
    EXPECT_EQ(server.lastRequest().arg, "random");
    EXPECT_TRUE(second.requests.empty());
}

TEST_F(TwoServersTest, ArtistsOfTheSecondServerOpenTheirAlbumsFromTheSecond) {
    openLink("Audiobooks");
    open("Artists");
    ASSERT_EQ(second.lastRequest().kind, Kind::Artists);
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteArtist>{{"ar1", "Herbert", 2}}, false));
    app->tick();
    open("Herbert");
    EXPECT_EQ(second.lastRequest().kind, Kind::ArtistAlbums);
    EXPECT_EQ(second.lastRequest().arg, "ar1");
}

TEST_F(TwoServersTest, OptionsOfAnAlbumOnTheSecondServerLoadItFromTheSecond) {
    openLink("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"b1", "Dune", "Herbert", 3}}, false));
    app->tick();
    press(Button::Options);
    open("Play now");
    EXPECT_EQ(speech.last(), "Loading.");
    EXPECT_EQ(second.lastRequest().kind, Kind::AlbumTracks);
    EXPECT_EQ(second.lastRequest().arg, "b1");
    for (const auto& r : server.requests) EXPECT_NE(r.arg, "b1");
}

TEST_F(TwoServersTest, OptionsOfAnArtistOnTheSecondServerLoadTheirAlbumsAndTracksFromTheSecond) {
    openLink("Audiobooks");
    open("Artists");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteArtist>{{"ar1", "Herbert", 1}}, false));
    app->tick();
    press(Button::Options);
    open("Play now");
    ASSERT_EQ(second.lastRequest().kind, Kind::ArtistAlbums);
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"b1", "Dune", "Herbert", 3}}, false));
    app->tick();
    EXPECT_EQ(second.lastRequest().kind, Kind::AlbumTracks);
    EXPECT_EQ(second.lastRequest().arg, "b1");
    for (const auto& r : server.requests) EXPECT_NE(r.arg, "b1");
}

TEST_F(TwoServersTest, ServerStatusNamesTheTypeAndVersionOfThatServer) {
    openLink("Audiobooks");
    open("Server status");
    EXPECT_EQ(speech.last(), "Checking the server.");
    EXPECT_EQ(second.lastRequest().kind, Kind::Ping);
    for (const auto& r : server.requests) EXPECT_NE(r.kind, Kind::Ping) << "the first server is not asked";
    ASSERT_TRUE(second.replyPing(second.lastRequest().id, {"audiobookshelf", "2.37.2"}));
    app->tick();
    EXPECT_EQ(speech.last(), "Connected to Audiobookshelf 2.37.2.");
}

// The "marked down" state belongs to the server that plays (the first, until step 1.5).
TEST_F(TwoServersTest, TheSecondServersStatusIsNotMarkedDownWithTheFirstAndItsAnswerDoesNotClearTheFirst) {
    start();
    player.unopenable = {"fake://stream/t1"};
    EXPECT_FALSE(app->playRemote("Weather", weather(), 0));
    ASSERT_TRUE(speech.heard("Cannot play Storm Front. Server not reachable."));
    press(Button::Home);
    open("Audiobooks");
    for (int i = 0; i < 5; ++i) press(Button::Forward);
    EXPECT_EQ(speech.last(), "Server status, 6 of 6");
    press(Button::Ok);
    ASSERT_TRUE(second.replyPing(second.lastRequest().id, {"audiobookshelf", "2.37.2"}));
    app->tick();
    press(Button::Home);
    hold(Button::Home);
    open("Navidrome");
    for (int i = 0; i < 5; ++i) press(Button::Forward);
    EXPECT_EQ(speech.last(), "Server status, not reachable, 6 of 6");
}

TEST_F(TwoServersTest, LeavingAListOfTheSecondServerCancelsOnlyItsRequest) {
    openLink("Audiobooks");
    open("Recently added");
    const auto id = second.lastRequest().id;
    const size_t firstCancelled = server.cancelled.size();
    press(Button::Home);
    EXPECT_EQ(second.cancelled, (std::vector<MusicServer::RequestId>{id}));
    EXPECT_EQ(server.cancelled.size(), firstCancelled);
}

TEST_F(TwoServersTest, LeavingServerStatusOfTheSecondServerCancelsOnlyItsPing) {
    openLink("Audiobooks");
    open("Server status");
    const auto id = second.lastRequest().id;
    const size_t firstCancelled = server.cancelled.size();
    press(Button::Home);
    EXPECT_EQ(second.cancelled, (std::vector<MusicServer::RequestId>{id}));
    EXPECT_EQ(server.cancelled.size(), firstCancelled);
}

TEST_F(TwoServersTest, PlayingWhileTheSecondServersStatusWaitsCancelsOnlyThatPing) {
    openLink("Audiobooks");
    open("Server status");
    const auto id = second.lastRequest().id;
    const size_t firstCancelled = server.cancelled.size();
    EXPECT_TRUE(app->playRemote("Weather", weather(), 0)); // the player opens over Server status: the user has moved on
    EXPECT_EQ(second.cancelled, (std::vector<MusicServer::RequestId>{id}));
    EXPECT_EQ(server.cancelled.size(), firstCancelled);
}

TEST_F(TwoServersTest, LeavingAnAlbumOptionsThatLoadsFromTheSecondServerCancelsOnlyThatRequest) {
    openLink("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"b1", "Dune", "Herbert", 3}}, false));
    app->tick();
    press(Button::Options);
    open("Play now");
    const auto id = second.lastRequest().id;
    ASSERT_EQ(second.lastRequest().kind, Kind::AlbumTracks);
    const size_t firstCancelled = server.cancelled.size();
    press(Button::Home);
    EXPECT_EQ(second.cancelled, (std::vector<MusicServer::RequestId>{id}));
    EXPECT_EQ(server.cancelled.size(), firstCancelled);
}

TEST_F(TwoServersTest, AListOfTheFirstServerLeftForTheSecondIsDropped) {
    start();
    open("Navidrome");
    open("Random albums");
    const auto firstId = server.lastRequest().id;
    hold(Button::Home);
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"b1", "Dune", "Herbert", 3}}, false));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Dune"}));
    EXPECT_FALSE(server.reply(firstId, std::vector<RemoteAlbum>{{"a1", "Late", "X", 1}}, false)) << "its screen is gone";
}

TEST_F(TwoServersTest, AnUnusableSecondServerIsNotAskedAndSaysItIsNotSupported) {
    second.isConfigured = false;
    openLink("Audiobooks");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not supported"}));
    EXPECT_TRUE(second.requests.empty());
}

TEST_F(StreamingTest, AServerThisPlayerCannotUseSaysSo) {
    server.isConfigured = false; // the front end could not make a server from it
    settings.setString("server.type", "audiobookshelf");
    settings.setString("server.url", "https://books.example");
    openServer();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not supported"}));
    press(Button::Ok);
    EXPECT_NE(speech.last().find("Subsonic"), std::string::npos) << speech.last();
    EXPECT_NE(speech.last().find("Audiobookshelf"), std::string::npos) << "the help names both types: " << speech.last();
    // Also what a build without network support says: it must not blame the type alone.
    EXPECT_NE(speech.last().find("network support"), std::string::npos) << speech.last();
    EXPECT_EQ(speech.last().find("https://"), std::string::npos) << "the address is not spoken";
}

TEST_F(StreamingTest, ServerStatusOnABooksServerNamesAudiobookshelfAndItsVersion) {
    server.catalogueKind = MusicServer::Catalogue::Books;
    settings.setString("server.type", "audiobookshelf");
    settings.setString("server.url", "https://books.example");
    openServer();
    open("Server status");
    EXPECT_EQ(speech.last(), "Checking the server.");
    ASSERT_EQ(server.lastRequest().kind, Kind::Ping);
    ASSERT_TRUE(server.replyPing(server.lastRequest().id, {"Audiobookshelf", "2.37.2"}));
    app->tick();
    EXPECT_EQ(speech.last(), "Connected to Audiobookshelf 2.37.2.");
}

TEST_F(StreamingTest, AnEmptyListSaysWhyAndWhatToDo) {
    openList("Random albums");
    answer(std::vector<RemoteAlbum>{});
    EXPECT_EQ(labels(), (std::vector<std::string>{"No albums"}));
    EXPECT_EQ(speech.last(), "No albums, 1 of 1");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "The server has no music yet. Add music to the server, then come back.");
}

TEST_F(StreamingTest, ServerStatusSaysTheServerTypeAndVersion) {
    openServer();
    open("Server status");
    EXPECT_EQ(speech.last(), "Checking the server.");
    EXPECT_EQ(server.lastRequest().kind, Kind::Ping);
    ASSERT_TRUE(server.replyPing(server.lastRequest().id, {"navidrome", "0.64.2"}));
    app->tick();
    EXPECT_EQ(speech.last(), "Connected to Navidrome 0.64.2.");
    EXPECT_EQ(level().title, "Server") << "a report stays where it is";
}

TEST_F(StreamingTest, ServerStatusSaysTheError) {
    openServer();
    open("Server status");
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(speech.last(), "Server not reachable.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
}

TEST_F(StreamingTest, ServerStatusAfterLeavingSaysNothing) {
    openServer();
    open("Server status");
    auto id = server.lastRequest().id;
    press(Button::Home);
    EXPECT_FALSE(server.replyPing(id, {"navidrome", "0.64.2"})) << "cancelled";
    app->tick();
    EXPECT_FALSE(speech.heard("Connected to Navidrome 0.64.2."));
}

TEST_F(StreamingTest, ServerStatusAfterGoingDeeperAndBackSaysNothing) {
    openServer();
    open("Server status");
    auto id = server.lastRequest().id;
    open("Recently added");
    press(Button::Home);
    EXPECT_FALSE(server.replyPing(id, {"navidrome", "0.64.2"})) << "cancelled when the user moved on";
    app->tick();
    EXPECT_FALSE(speech.heard("Connected to Navidrome 0.64.2."));
}

// A server that failed to open a track is marked down (queue.md, "Playing it"): Server status says so until it answers.
TEST_F(StreamingTest, ServerStatusSaysNotReachableWhileTheServerIsMarkedDownAndAnAnswerClearsIt) {
    start();
    player.unopenable = {"fake://stream/t1"};
    EXPECT_FALSE(app->playRemote("Weather", weather(), 0));
    ASSERT_TRUE(speech.heard("Cannot play Storm Front. Server not reachable."));
    press(Button::Home);
    open("Server");
    ASSERT_EQ(level().title, "Server");
    for (int i = 0; i < 5; ++i) press(Button::Forward);
    EXPECT_EQ(speech.last(), "Server status, not reachable, 6 of 6");
    press(Button::Ok);
    ASSERT_TRUE(server.replyPing(server.lastRequest().id, {"navidrome", "0.64.2"}));
    app->tick();
    EXPECT_EQ(speech.last(), "Connected to Navidrome 0.64.2.");
    press(Button::Back);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Server status, 6 of 6") << "the answer cleared the mark";
}

TEST_F(StreamingTest, ServerStatusStaysNotReachableWhenThePingDoesNotAnswer) {
    start();
    player.unopenable = {"fake://stream/t1"};
    EXPECT_FALSE(app->playRemote("Weather", weather(), 0));
    press(Button::Home);
    open("Server");
    for (int i = 0; i < 5; ++i) press(Button::Forward);
    press(Button::Ok);
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(speech.last(), "Server not reachable.");
    press(Button::Back);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Server status, not reachable, 6 of 6");
}

TEST_F(StreamingTest, ServerStatusSaysNothingExtraWhileTheServerIsNotMarkedDown) {
    openServer();
    for (int i = 0; i < 5; ++i) press(Button::Forward);
    EXPECT_EQ(speech.last(), "Server status, 6 of 6");
}

TEST_F(StreamingTest, ChoosingATrackPlaysFromItToTheEndOfTheAlbum) {
    openList("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    EXPECT_EQ(speech.last(), "Weather. Loading, 1 of 1");
    EXPECT_EQ(server.lastRequest().kind, Kind::AlbumTracks);
    EXPECT_EQ(server.lastRequest().arg, "al1");
    answer(weather());
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play album", "Storm Front", "Rain", "Sunshine"}));
    EXPECT_EQ(speech.last(), "Play album, 3 tracks, 1 of 4");
    open("Rain");
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t2"}));
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(speech.last(), "Now playing. Weather, Rain.");
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t3") << "the rest of the album follows";
    press(Button::Home);
    EXPECT_EQ(level().title, "Weather") << "Home goes back to the album";
    expectNoUrlSpoken(speech);
}

TEST_F(StreamingTest, PlayAlbumPlaysFromTheFirstTrack) {
    openList("Random albums");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(weather());
    press(Button::Ok); // Play album
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1"}));
    EXPECT_EQ(app->nowPlaying().title, "Weather");
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
}

TEST_F(StreamingTest, AnArtistOpensTheirAlbums) {
    openList("Artists");
    answer(std::vector<RemoteArtist>{{"ar1", "Band", 2}, {"ar2", "Choir", 1}});
    EXPECT_EQ(speech.last(), "Band, 2 albums, 1 of 2");
    open("Choir");
    EXPECT_EQ(server.lastRequest().kind, Kind::ArtistAlbums);
    EXPECT_EQ(server.lastRequest().arg, "ar2");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Choir", 3}});
    EXPECT_EQ(speech.last(), "Weather, 3 tracks, 1 of 1");
    open("Weather");
    EXPECT_EQ(server.lastRequest().kind, Kind::AlbumTracks);
    EXPECT_EQ(server.lastRequest().arg, "al1");
}

TEST_F(StreamingTest, APlaylistOpensItsTracks) {
    indexPlaylist("p1", "Road", 3);
    openPlaylist("Road");
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
    EXPECT_EQ(server.lastRequest().arg, "p1");
    answer(weather());
    EXPECT_EQ(labels().front(), "Play playlist");
    open("Sunshine");
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t3"}));
    EXPECT_EQ(app->nowPlaying().title, "Road");
}

TEST_F(StreamingTest, KeysAreAnsweredWhileATrackOpens) {
    openList("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(weather());
    player.holdStreams = true;
    open("Rain");
    EXPECT_EQ(speech.last(), "Loading.");
    EXPECT_TRUE(player.opening());
    EXPECT_EQ(level().title, "Weather") << "the user stays in the list meanwhile";
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Sunshine, 4 of 4");
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Paused.") << "it will not play once it has opened";
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Loading.") << "it will play again";
    hold(Button::PlayPause); // where am I
    EXPECT_EQ(speech.last(), "Home, Server, Recently added, Weather. Sunshine, 4 of 4. Loading Weather");
    ASSERT_TRUE(player.finishOpen());
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(speech.last(), "Now playing. Weather, Rain.");
    expectNoUrlSpoken(speech);
}

TEST_F(StreamingTest, HomeWhileATrackOpensDropsTheLateResult) {
    openList("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(weather());
    player.holdStreams = true;
    open("Rain");
    press(Button::Home);
    EXPECT_EQ(level().title, "Recently added");
    speech.said.clear();
    EXPECT_FALSE(player.finishOpen()) << "the open was dropped";
    idle(1s);
    EXPECT_TRUE(speech.said.empty());
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(level().title, "Recently added");
    EXPECT_FALSE(app->nowPlaying().loaded) << "nothing was loaded before";
}

TEST_F(StreamingTest, HomeWhileATrackOpensKeepsTheBookThatPlayedBefore) {
    playAlpha();
    player.pos = 20;
    hold(Button::Home);
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(weather());
    player.holdStreams = true;
    open("Rain");
    hold(Button::Home);
    EXPECT_FALSE(player.finishOpen());
    app->tick();
    EXPECT_EQ(app->nowPlaying().title, "Alpha");
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_DOUBLE_EQ(player.pos, 20);
    EXPECT_FALSE(player.playing());
}

TEST_F(StreamingTest, ChoosingAnotherTrackWhileOneOpensPlaysOnlyThatOne) {
    openList("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(weather());
    player.holdStreams = true;
    open("Rain");
    open("Sunshine");
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t2", "fake://stream/t3"}));
    ASSERT_TRUE(player.finishOpen());
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(speech.last(), "Now playing. Weather, Sunshine.");
    EXPECT_FALSE(speech.heard("Now playing. Weather, Rain."));
    EXPECT_FALSE(player.finishOpen()) << "no other open is left";
}

TEST_F(StreamingTest, ATrackThatOpensWhileTheUserIsInAnotherMenuPlaysWithoutLeavingIt) {
    openList("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(weather());
    player.holdStreams = true;
    open("Rain");
    hold(Button::Power); // the Power menu, on top of the album
    std::string menu = level().title;
    ASSERT_TRUE(player.finishOpen());
    app->tick();
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(level().title, menu);
}

// ---------------------------------------------------------------- resuming a stream after a restart

// The place in a remote album or playlist is kept as last_stream.* in
// state.tsv, written only on exit and sleep. After a restart "Now playing" on
// Home loads the list from the server again and opens the track at its place.
class ResumeStreamTest : public StreamingTest {
protected:
    void SetUp() override {
        StreamingTest::SetUp();
        settings.setString("server.url", "https://music.example");
        settings.setString("server.user", "ice");
        settings.setString("server.password", "secret");
    }
    // Home > Server > Recently added > Weather > `track`: it plays.
    void playFromAlbum(const std::string& track, std::vector<RemoteTrack> tracks = weather()) {
        openList("Recently added");
        answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
        open("Weather");
        answer(std::move(tracks));
        open(track);
        ASSERT_TRUE(player.playing());
    }
    // Exit, and a new App on what was written to state.tsv, as after a reboot.
    // `offline` runs between the two, e.g. to edit config.yaml.
    // `older`: the state is what the previous version left, the place of the list in state.tsv
    // (`last_stream.*`, of the playlist `id` when `kind` is "playlist") and no queue.tsv.
    void reboot(const std::function<void()>& offline = {}, bool older = false, const std::string& kind = "album",
                const std::string& id = "al1") {
        app->shutdown();
        app.reset();
        if (older) olderState(kind, id);
        if (offline) offline();
        state = Settings(&stateFile);
        state.load();
        player = FakePlayer{};
        speech.said.clear();
        sounds.played.clear();
        start();
    }
    void expectNoSecretOnDisk() const {
        for (const auto& [key, value] : stateFile.file) {
            EXPECT_EQ(value.find("fake://"), std::string::npos) << key;
            EXPECT_EQ(value.find("secret"), std::string::npos) << key;
        }
        const std::string queue = diskFiles().read(queueFile).value_or("");
        EXPECT_EQ(queue.find("fake://"), std::string::npos);
        EXPECT_EQ(queue.find("secret"), std::string::npos);
    }
    // What queue.tsv holds.
    SavedQueue savedQueue() const {
        auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
        EXPECT_TRUE(saved) << "queue.tsv holds a queue";
        return saved.value_or(SavedQueue{});
    }
};

TEST_F(ResumeStreamTest, ExitAndRestartResumeTheSameAlbumTrackAndPosition) {
    playFromAlbum("Rain");
    player.pos = 42;
    reboot();
    expectNoSecretOnDisk();
    EXPECT_FALSE(stateFile.file.count("last_stream.id")) << "the queue keeps the place now";

    // Like a local book, nothing plays at boot; nothing is asked of the server, not even the list.
    std::size_t requests = server.requests.size();
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(speech.last(), "Home. Now playing, Weather, 1 of 8");
    EXPECT_EQ(server.requests.size(), requests);
    EXPECT_TRUE(player.opened.empty());

    press(Button::Ok); // Now playing
    EXPECT_EQ(server.requests.size(), requests) << "the queue is here already: no list to load";
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(speech.last(), "Now playing. Weather, Rain.");
    app->tick();
    // A resume after a restart is a new play on the server.
    EXPECT_EQ(server.scrobbles.back(), (FakeMusicServer::Scrobble{"t2", false}));
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t3") << "the rest of the album follows";
    expectNoUrlSpoken(speech);
}

TEST_F(ResumeStreamTest, AResumeThatOpensInTheBackgroundEntersThePlayerOnceOpen) {
    playFromAlbum("Rain");
    player.pos = 42;
    reboot({}, true);
    player.holdStreams = true;
    press(Button::Ok);
    answer(weather());
    EXPECT_EQ(app->mode(), App::Mode::Menu) << "still opening";
    ASSERT_TRUE(player.finishOpen());
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_EQ(app->mode(), App::Mode::Player);
}

TEST_F(ResumeStreamTest, PlayAfterARestartResumesTheStreamToo) {
    playFromAlbum("Rain");
    player.pos = 42;
    reboot({}, true);
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Loading.");
    answer(weather());
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
}

TEST_F(ResumeStreamTest, PlayWithNothingSavedStillSaysNothingToPlay) {
    start();
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Nothing to play. Choose a book in the library.");
    EXPECT_EQ(listRequests(), 0u);
}

TEST_F(ResumeStreamTest, ASlashAtTheEndOfTheServerUrlIsTheSameServer) {
    settings.setString("server.url", "https://music.example/");
    playFromAlbum("Rain");
    reboot([this] { settings.setString("server.url", "https://music.example"); });
    EXPECT_EQ(labels().front(), "Now playing");
}

TEST_F(ResumeStreamTest, AnUnreadablePlaceStartsTheTrackFromItsStart) {
    playFromAlbum("Rain");
    reboot({}, true);
    app.reset();
    state.setString("last_stream.pos", "inf");
    start();
    press(Button::Ok);
    answer(weather());
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

TEST_F(ResumeStreamTest, APlaylistIsLoadedAgainToo) {
    indexPlaylist("p1", "Road", 3);
    openPlaylist("Road");
    answer(weather());
    open("Sunshine");
    player.pos = 10;
    reboot({}, true, "playlist", "p1");
    press(Button::Ok);
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
    EXPECT_EQ(server.lastRequest().arg, "p1");
    answer(weather());
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_DOUBLE_EQ(player.pos, 10);
    EXPECT_EQ(app->nowPlaying().title, "Road");
}

TEST_F(ResumeStreamTest, WithTheServerDownResumeSaysServerNotReachableAndStaysResponsive) {
    playFromAlbum("Rain");
    reboot({}, true);
    press(Button::Ok);
    ASSERT_TRUE(server.fail(server.lastRequest().id, FakeMusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(speech.last(), "Server not reachable.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_TRUE(player.opened.empty());
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Library, 2 of 8");
    press(Button::Back);
    press(Button::Ok); // the server is back: try again
    answer(weather());
    EXPECT_EQ(player.file, "fake://stream/t2");
}

TEST_F(ResumeStreamTest, ATrackThatCannotBeOpenedSaysSoAndLeavesNothingLoaded) {
    playFromAlbum("Rain");
    reboot({}, true);
    player.unopenable = {"fake://stream/t2"};
    press(Button::Ok);
    answer(weather());
    EXPECT_EQ(speech.last(), "Cannot play Rain. Server not reachable.");
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_FALSE(app->nowPlaying().loaded);
    EXPECT_EQ(labels().front(), "Now playing") << "it can be tried again";
}

TEST_F(ResumeStreamTest, ATrackThatMovedInTheListIsFoundByItsId) {
    playFromAlbum("Rain");
    player.pos = 42;
    reboot({}, true);
    press(Button::Ok);
    auto tracks = weather();
    std::swap(tracks[0], tracks[1]); // Rain is first now
    answer(tracks);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_EQ(app->nowPlaying().chapterIndex, 0);
}

TEST_F(ResumeStreamTest, ATrackGoneFromTheListPlaysTheOneNowInItsPlaceFromItsStart) {
    playFromAlbum("Rain");
    player.pos = 42;
    reboot({}, true);
    press(Button::Ok);
    auto tracks = weather();
    tracks.erase(tracks.begin() + 1); // Rain is gone
    answer(tracks);
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

TEST_F(ResumeStreamTest, AListThatGotShorterPlaysItsFirstTrackFromItsStart) {
    playFromAlbum("Sunshine");
    player.pos = 42;
    reboot({}, true);
    press(Button::Ok);
    answer(std::vector<RemoteTrack>{weather()[0]});
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

TEST_F(ResumeStreamTest, TheSameTrackTwiceInAPlaylistResumesTheOneThatPlayed) {
    auto tracks = weather();
    tracks.push_back(tracks[1]); // Rain again, at the end
    indexPlaylist("p1", "Road", 4);
    openPlaylist("Road");
    answer(tracks);
    press(Button::Back); // the second Rain, the last item
    press(Button::Ok);
    ASSERT_EQ(app->nowPlaying().chapterIndex, 3);
    reboot({}, true, "playlist", "p1");
    press(Button::Ok);
    answer(tracks);
    EXPECT_EQ(app->nowPlaying().chapterIndex, 3);
}

TEST_F(ResumeStreamTest, LeavingHomeWhileTheListLoadsDropsIt) {
    playFromAlbum("Rain");
    reboot({}, true);
    press(Button::Ok);
    auto id = server.lastRequest().id;
    open("Library");
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), id), server.cancelled.end());
    EXPECT_FALSE(server.reply(id, weather())) << "the late answer is dropped";
    app->tick();
    EXPECT_TRUE(player.opened.empty());
}

TEST_F(ResumeStreamTest, SleepWhileTheListLoadsDropsIt) {
    playFromAlbum("Rain");
    reboot({}, true);
    press(Button::Ok);
    auto id = server.lastRequest().id;
    press(Button::Power);
    EXPECT_FALSE(server.reply(id, weather()));
    press(Button::Ok); // wake
    app->tick();
    EXPECT_TRUE(player.opened.empty());
}

// Play in the Power menu starts a resume; Power off then drops it, since the
// front end goes on ticking while "Goodbye" is spoken.
TEST_F(ResumeStreamTest, PowerOffWhileTheListLoadsDropsIt) {
    playFromAlbum("Rain");
    reboot({}, true);
    hold(Button::Power);
    press(Button::PlayPause);
    ASSERT_EQ(speech.last(), "Loading.");
    auto id = server.lastRequest().id;
    open("Power off");
    ASSERT_TRUE(app->quitRequested());
    EXPECT_FALSE(server.reply(id, weather())) << "the late answer is dropped";
    app->tick();
    EXPECT_TRUE(player.opened.empty());
    EXPECT_EQ(speech.last(), "Powering off. Goodbye.");
}

// Home is not rebuilt while the track opens: OK on "Now playing" again goes to
// the player, and does not load the list and open the track a second time.
TEST_F(ResumeStreamTest, OkOnNowPlayingWhileTheTrackOpensEntersThePlayer) {
    playFromAlbum("Rain");
    player.pos = 42;
    reboot({}, true);
    player.holdStreams = true;
    press(Button::Ok);
    answer(weather());
    ASSERT_TRUE(player.opening());
    std::size_t requests = server.requests.size();
    press(Button::Ok);
    EXPECT_EQ(server.requests.size(), requests) << "the list is not loaded again";
    EXPECT_EQ(player.opened.size(), 1u);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(speech.last(), "Now playing. Weather, Rain, loading.");
    ASSERT_TRUE(player.finishOpen());
    app->tick();
    EXPECT_TRUE(player.playing());
    EXPECT_DOUBLE_EQ(player.pos, 42);
}

// The next track while the resumed one opens is the next track of that list,
// not a track of what was loaded before it (here: nothing).
TEST_F(ResumeStreamTest, NextTrackWhileTheResumedTrackOpensPlaysTheNextOne) {
    playFromAlbum("Rain");
    reboot({}, true);
    player.holdStreams = true;
    press(Button::Ok);
    answer(weather());
    press(Button::Ok); // the player
    press(Button::Forward);
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_EQ(speech.last(), "Sunshine");
    ASSERT_TRUE(player.finishOpen());
    app->tick();
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().title, "Weather");
    EXPECT_EQ(app->nowPlaying().chapter, "Sunshine");
}

// A resumed track that does not open while the user is in the player leaves
// nothing loaded: the player is left, back on Home, as when it fails from Home.
TEST_F(ResumeStreamTest, AResumeThatFailsInThePlayerGoesBackToHome) {
    playFromAlbum("Rain");
    reboot({}, true);
    player.holdStreams = true;
    press(Button::Ok);
    answer(weather());
    press(Button::Ok); // the player
    ASSERT_EQ(app->mode(), App::Mode::Player);
    player.unopenable = {"fake://stream/t2"};
    ASSERT_TRUE(player.finishOpen());
    app->tick();
    EXPECT_EQ(speech.last(), "Cannot play Rain. Server not reachable.");
    EXPECT_FALSE(app->nowPlaying().loaded);
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(level().title, "Home");
    hold(Button::Forward); // in a menu: nothing happens
    press(Button::Home);
    EXPECT_EQ(level().title, "Home");
}

// Sleep drops the open of the resumed track, which leaves nothing loaded: the
// device wakes on Home, not in a player with nothing in it.
TEST_F(ResumeStreamTest, SleepWhileTheResumedTrackOpensInThePlayerWakesOnHome) {
    playFromAlbum("Rain");
    reboot({}, true);
    player.holdStreams = true;
    press(Button::Ok);
    answer(weather());
    press(Button::Ok); // the player
    ASSERT_EQ(app->mode(), App::Mode::Player);
    press(Button::Power);
    press(Button::Ok); // wake
    EXPECT_FALSE(app->nowPlaying().loaded);
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(level().title, "Home");
    EXPECT_FALSE(player.finishOpen()) << "the open was dropped";
}

TEST_F(ResumeStreamTest, ShutdownWhileTheListLoadsDropsIt) {
    playFromAlbum("Rain");
    reboot({}, true);
    press(Button::Ok);
    auto id = server.lastRequest().id;
    app->shutdown();
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), id), server.cancelled.end());
}

TEST_F(ResumeStreamTest, ALocalBookPlayedAfterTheStreamIsResumedInstead) {
    playFromAlbum("Rain");
    hold(Button::Home);
    open("Library");
    open("Alpha");
    player.pos = 20;
    reboot();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_DOUBLE_EQ(player.pos, 20);
    EXPECT_EQ(speech.last(), "Alpha, paused. Press play to continue.");
    press(Button::Home);
    EXPECT_EQ(labels()[1], "Continue") << "the stream is the other player, with its queue";
    EXPECT_EQ(speech.last(), "Now playing, Alpha, 1 of 9");
}

TEST_F(ResumeStreamTest, AStreamPlayedAfterABookIsResumedAndTheBookKeepsItsPlace) {
    start();
    open("Library");
    open("Alpha");
    player.pos = 20;
    hold(Button::Home);
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(weather());
    open("Rain");
    reboot();
    EXPECT_EQ(stateFile.onDisk("last_book"), alpha().string());
    ASSERT_EQ(app->mode(), App::Mode::Menu) << "the book is not resumed: the stream played last";
    EXPECT_EQ(labels().front(), "Now playing");
    EXPECT_EQ(speech.last(), "Home. Now playing, Weather, 1 of 9");
    EXPECT_EQ(labels()[1], "Continue") << "the book played before it";
    open("Library");
    open("Alpha");
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_DOUBLE_EQ(player.pos, 20);
}

TEST_F(ResumeStreamTest, AParkedStreamComesBackAtItsTrackAndPlace) {
    playFromAlbum("Rain");
    player.pos = 42;
    hold(Button::Home);
    open("Library");
    open("Alpha");
    hold(Button::Home);
    ASSERT_EQ(labels()[1], "Continue");
    EXPECT_EQ(valueOf(level().items[1]), "Weather");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->screens().back().kind, Screen::Kind::MusicPlayer);
}

TEST_F(ResumeStreamTest, TheStreamKeepsItsPlaceWhileABookPlaysAndAfterARestart) {
    playFromAlbum("Rain");
    player.pos = 42;
    hold(Button::Home);
    open("Library");
    open("Alpha");
    reboot();
    EXPECT_EQ(stateFile.onDisk("last_player"), "book");
    ASSERT_EQ(app->mode(), App::Mode::Player) << "the book played last";
    hold(Button::Home);
    EXPECT_EQ(valueOf(level().items[1]), "Weather");
    std::size_t requests = server.requests.size();
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(server.requests.size(), requests) << "the parked queue is here: no list to load";
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
}

TEST_F(ResumeStreamTest, ContinueWhileTheSavedStreamLoadsDropsItsLateAnswer) {
    start();
    open("Library");
    open("Alpha");
    player.pos = 20;
    hold(Button::Home);
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(weather());
    open("Rain");
    reboot({}, true);
    ASSERT_EQ(labels()[1], "Continue");
    press(Button::Ok); // Now playing: loads the stream
    auto id = server.lastRequest().id;
    press(Button::Forward);
    press(Button::Ok); // Continue: the book
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), id), server.cancelled.end());
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_EQ(app->screens().back().kind, Screen::Kind::BookPlayer);
    EXPECT_FALSE(server.reply(id, weather())) << "the late answer finds nobody";
    app->tick();
    EXPECT_EQ(player.file, alphaFile(1));
}

TEST_F(ResumeStreamTest, APendingOpenThatIsDroppedBringsBackTheParkedPlayer) {
    playFromAlbum("Rain");
    player.pos = 42;
    hold(Button::Home);
    open("Library");
    open("Alpha"); // the book is loaded, the stream parked
    hold(Button::Home);
    player.holdStreams = true;
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(weather());
    open("Sunshine"); // opening: the book is parked in its place
    hold(Button::Home); // leaves the menu: the open is dropped, the book is back
    EXPECT_EQ(valueOf(level().items[0]), "Alpha");
    ASSERT_EQ(labels()[1], "Continue") << "the stream that was parked is still there";
    EXPECT_EQ(valueOf(level().items[1]), "Weather");
    player.holdStreams = false;
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(player.file, "fake://stream/t2") << "at the track it was parked on";
    EXPECT_DOUBLE_EQ(player.pos, 42);
}

TEST_F(ResumeStreamTest, PlayWithNothingLoadedResumesThePlayerUsedLast) {
    start();
    open("Library");
    open("Alpha");
    hold(Button::Home);
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(weather());
    open("Rain");
    player.pos = 42;
    reboot();
    ASSERT_EQ(app->mode(), App::Mode::Menu);
    press(Button::PlayPause);
    EXPECT_EQ(player.file, "fake://stream/t2") << "the stream, not the book";
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
}

TEST_F(ResumeStreamTest, PlayFallsBackToTheOtherPlayerWhenTheLastOneCannotOpen) {
    playFromAlbum("Rain");
    hold(Button::Home);
    open("Library");
    open("Alpha");
    app->shutdown();
    app.reset();
    state = Settings(&stateFile);
    state.load();
    player = FakePlayer{};
    player.unopenable = {alphaFile(1), alphaFile(2)};
    speech.said.clear();
    start();
    ASSERT_EQ(app->mode(), App::Mode::Menu) << "the book did not open";
    press(Button::PlayPause);
    EXPECT_EQ(player.file, "fake://stream/t2") << "the queue is tried after the book";
    EXPECT_TRUE(player.playing());
}

TEST_F(ResumeStreamTest, AQueueOfAnotherServerIsKeptButItsTracksAreUnavailable) {
    playFromAlbum("Rain");
    reboot([this] { settings.setString("server.url", "https://other.example"); });
    EXPECT_EQ(labels().front(), "Now playing") << "the queue is the user's, whatever server it came from";
    press(Button::Ok);
    EXPECT_TRUE(speech.heard("Cannot play 2 tracks. Server not set up.")) << "skipped, as in any queue";
    EXPECT_TRUE(player.opened.empty());
}

TEST_F(ResumeStreamTest, AQueueOfAnotherUserIsKeptButItsTracksAreUnavailable) {
    playFromAlbum("Rain");
    reboot([this] { settings.setString("server.user", "guest"); });
    EXPECT_EQ(labels().front(), "Now playing");
    press(Button::Ok);
    EXPECT_TRUE(speech.heard("Cannot play 2 tracks. Server not set up."));
    EXPECT_TRUE(player.opened.empty());
}

TEST_F(ResumeStreamTest, WithoutAServerTheQueueIsKeptButItsTracksAreUnavailable) {
    playFromAlbum("Rain");
    reboot([this] { server.isConfigured = false; });
    EXPECT_EQ(labels().front(), "Now playing");
    press(Button::Ok);
    EXPECT_TRUE(speech.heard("Cannot play 2 tracks. Server not set up."));
    EXPECT_TRUE(player.opened.empty());
}

TEST_F(ResumeStreamTest, AListNotFromTheServerMenusIsKeptToo) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 1));
    reboot();
    EXPECT_EQ(labels().front(), "Now playing");
    EXPECT_EQ(valueOf(level().items[0]), "Weather");
    press(Button::Ok);
    EXPECT_EQ(player.file, "fake://stream/t2");
}

TEST_F(ResumeStreamTest, ATitleWithATabAndALineBreakComesBackAsItWas) {
    openList("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Wind\tand\nRain", "Band", 3}});
    open("Wind\tand\nRain");
    answer(weather());
    open("Rain");
    reboot();
    EXPECT_EQ(valueOf(level().items[0]), "Wind\tand\nRain");
    std::ifstream in(queueFile);
    std::string line;
    int lines = 0;
    while (std::getline(in, line)) ++lines;
    EXPECT_EQ(lines, 11) << "the line break did not split a line";
}

TEST_F(ResumeStreamTest, AStreamThatEndedResumesAtTheStartOfTheList) {
    playFromAlbum("Sunshine");
    player.atEnd = true;
    app->tick(); // End of queue
    reboot();
    press(Button::Ok);
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

TEST_F(ResumeStreamTest, AFailedStreamIsKeptAtThePlaceItGotTo) {
    playFromAlbum("Rain");
    player.pos = 77;
    player.broken = true;
    app->tick(); // Server not reachable: closed, opened again at 77 on Play
    reboot();
    press(Button::Ok);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 77);
}

TEST_F(ResumeStreamTest, AnExitWhileTheResumedTrackOpensKeepsItsPlace) {
    playFromAlbum("Rain");
    player.pos = 42;
    reboot();
    player.holdStreams = true;
    press(Button::Ok); // Rain is opening
    ASSERT_TRUE(player.opening());
    reboot();
    press(Button::Ok);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
}

// The SD card is written only when the device sleeps or exits cleanly.
TEST_F(ResumeStreamTest, StreamingWritesNothingToDiskUntilExit) {
    playFromAlbum("Storm Front");
    for (int i = 0; i < 10; ++i) {
        player.pos += 30;
        idle(30s); // minutes of playback
    }
    player.atEnd = true;
    app->tick(); // the next track
    ASSERT_EQ(player.file, "fake://stream/t2");
    press(Button::PlayPause); // pause
    press(Button::PlayPause); // play
    press(Button::Forward);   // a seek
    press(Button::Forward);    // a track change
    ASSERT_EQ(player.file, "fake://stream/t3");
    EXPECT_EQ(stateFile.saves, 0);
    EXPECT_EQ(files.queueWrites, 0);
    EXPECT_EQ(configFile.saves, 0);
}

TEST_F(ResumeStreamTest, AResumedStreamWritesNothingToDiskUntilExit) {
    playFromAlbum("Rain");
    reboot();
    int saves = stateFile.saves;
    int queueWrites = files.queueWrites;
    press(Button::Ok);
    ASSERT_TRUE(player.playing());
    for (int i = 0; i < 10; ++i) {
        player.pos += 30;
        idle(30s);
    }
    press(Button::PlayPause);
    press(Button::PlayPause);
    press(Button::Forward);
    ASSERT_EQ(app->nowPlaying().chapter, "Sunshine");
    EXPECT_EQ(stateFile.saves, saves);
    EXPECT_EQ(files.queueWrites, queueWrites);
    EXPECT_EQ(configFile.saves, 0);
}

TEST_F(ResumeStreamTest, SleepWritesTheQueueOnce) {
    playFromAlbum("Rain");
    player.pos = 33;
    press(Button::Power);
    EXPECT_EQ(stateFile.saves, 1);
    EXPECT_EQ(files.queueWrites, 1);
    EXPECT_DOUBLE_EQ(savedQueue().position, 33);
    EXPECT_EQ(configFile.saves, 0);
}

TEST_F(ResumeStreamTest, PowerOffWritesTheQueueOnce) {
    playFromAlbum("Rain");
    player.pos = 34;
    hold(Button::Power);
    open("Power off");
    EXPECT_EQ(stateFile.saves, 1);
    EXPECT_EQ(files.queueWrites, 1);
    EXPECT_DOUBLE_EQ(savedQueue().position, 34);
    app->shutdown();
    EXPECT_EQ(stateFile.saves, 1) << "nothing changed since";
    EXPECT_EQ(files.queueWrites, 1) << "nothing changed since";
}

TEST_F(ResumeStreamTest, ShutdownWritesTheQueueOnce) {
    playFromAlbum("Rain");
    player.pos = 35;
    app->shutdown();
    EXPECT_EQ(stateFile.saves, 1);
    EXPECT_EQ(files.queueWrites, 1);
    EXPECT_DOUBLE_EQ(savedQueue().position, 35);
    EXPECT_EQ(configFile.saves, 0);
}

// ---------------------------------------------------------------- gesture cancellation

TEST_F(AppTest, ScreenChangeCancelsHeldNavigationAndVolumeUntilReleased) {
    playAlpha();
    app->keyDown(Button::Forward);
    app->keyDown(Button::VolumeUp);
    const float afterPress = volume.volume;
    press(Button::Ok); // a new screen cancels the held keys
    ASSERT_EQ(level().title, "Player menu");
    const auto spoken = speech.last();
    idle(1s);
    app->keyUp(Button::Forward);
    app->keyUp(Button::VolumeUp);
    EXPECT_EQ(speech.last(), spoken);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_FLOAT_EQ(volume.volume, afterPress);
    press(Button::VolumeUp);
    EXPECT_GT(volume.volume, afterPress) << "a fresh press works";
}

TEST_F(AppTest, ScreenChangeDuringPollSuppressesAlreadyQueuedRepeats) {
    playAlpha();
    app->keyDown(Button::VolumeUp);
    const float afterPress = volume.volume;
    app->keyDown(Button::Home); // its Long changes screen in this same tick
    idle(600ms); // poll also returns a volume Repeat, after Home in button order
    EXPECT_FLOAT_EQ(volume.volume, afterPress);
    app->keyUp(Button::Home);
    app->keyUp(Button::VolumeUp);
    EXPECT_EQ(level().title, "Home");
}

TEST_F(AppTest, ReturningToPreviousScreenCancelsOtherHeldKeys) {
    playAlpha();
    press(Button::Ok);
    app->keyDown(Button::Forward);
    press(Button::Home);
    ASSERT_EQ(app->mode(), App::Mode::Player);
    const auto spoken = speech.last();
    idle(1s);
    app->keyUp(Button::Forward);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_EQ(speech.last(), spoken);
}

TEST_F(AppTest, HoldCancellationSuppressesLateDeepAndDuplicateDownUntilBothContactsUp) {
    playAlpha();
    app->keyDown(Button::Forward);
    app->setHold(true);
    app->setHold(false);
    app->keyDown(Button::Forward); // duplicate raw down cannot unlock this press
    app->keyDown(Button::ForwardDeep);
    idle(1s);
    app->keyUp(Button::Forward);
    app->keyUp(Button::ForwardDeep);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_EQ(speech.last(), "Buttons unlocked.");
    press(Button::ForwardDeep);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_EQ(player.pos, 30) << "a new deep gesture seeks";
}

TEST_F(AppTest, SleepingCancelsHeldNavigationWithoutWakingOnItsRelease) {
    playAlpha();
    app->keyDown(Button::Forward);
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    idle(1s);
    app->keyUp(Button::Forward);
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_EQ(speech.last(), "Going to sleep.");
    press(Button::PlayPause);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_FALSE(player.playing());
}

TEST_F(AppTest, ShutdownCancelsInputAndSubsequentTicksCannotReopenOrSpeak) {
    playAlpha();
    app->keyDown(Button::Forward);
    app->shutdown();
    const auto spoken = speech.last();
    const auto saves = stateFile.saves;
    idle(1s);
    app->keyUp(Button::Forward);
    press(Button::PlayPause);
    press(Button::VolumeUp); // immediate-down action must also stay inert
    app->setHold(true);
    EXPECT_EQ(speech.last(), spoken);
    EXPECT_EQ(stateFile.saves, saves);
    EXPECT_FALSE(player.playing());
    EXPECT_FALSE(player.isOpen());
}

// ---------------------------------------------------------------- hold switch and power

TEST_F(AppTest, HoldSwitchBlocksEverythingButPower) {
    start();
    app->setHold(true);
    EXPECT_EQ(speech.last(), "Buttons locked.");
    press(Button::Forward);
    EXPECT_EQ(level().index, 0u);
    EXPECT_EQ(speech.last(), "Locked.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    press(Button::Power);
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
}

TEST_F(AppTest, HoldSwitchLocksAKeyThatIsAlreadyDown) {
    playAlpha();
    player.pos = 100;
    app->keyDown(Button::Forward);
    app->setHold(true);
    app->keyUp(Button::Forward);
    EXPECT_DOUBLE_EQ(player.pos, 100) << "no skip on release";
    app->keyDown(Button::Forward); // locked: "Locked."
    app->keyUp(Button::Forward);
    app->setHold(false);
    app->keyDown(Button::Forward);
    app->setHold(true);
    clock.advance(700ms);
    app->tick();
    EXPECT_EQ(player.file, alphaFile(1)) << "no long press while held";
    app->keyUp(Button::Forward);
    app->setHold(false);
    press(Button::Forward);
    EXPECT_EQ(player.file, alphaFile(2)) << "works again once let go";
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

TEST_F(AppTest, HoldSwitchDoesNotLockPowerAlreadyDown) {
    start();
    app->keyDown(Button::Power);
    app->setHold(true);
    app->keyUp(Button::Power);
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
}

TEST_F(AppTest, PowerSleepsAndAnyKeyWakes) {
    playAlpha();
    press(Button::Power);
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "Going to sleep.");
    press(Button::Ok);
    EXPECT_EQ(app->mode(), App::Mode::Player) << "the waking key must not also act";
    EXPECT_EQ(speech.last(), "Alpha, paused. Storm.");
}

TEST_F(AppTest, SleepTimerSleepsAfterDeadline) {
    playAlpha();
    press(Button::Ok);   // Player menu
    press(Button::Forward); // Sleep timer
    press(Button::Ok);
    press(Button::Forward); // 1 minute
    press(Button::Ok);
    EXPECT_TRUE(speech.heard("Sleep timer set to 1 minute."));
    idle(55s);
    EXPECT_NE(app->mode(), App::Mode::Sleeping);
    idle(6s);
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_FALSE(player.playing());
    EXPECT_TRUE(speech.heard("Sleep timer ended."));
}

TEST_F(AppTest, SleepTimerIsInSettings) {
    playAlpha();
    hold(Button::Home);
    open("Settings");
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "Sleep timer, off, 12 of 12");
    press(Button::Ok);
    open("15 minutes");
    EXPECT_EQ(level().title, "Settings") << "choosing a time goes back";
    EXPECT_EQ(speech.last(), "Sleep timer, 15 minutes left, 12 of 12");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Sleep timer. 15 minutes, current, 3 of 7") << "opens on the timer that is set";
    press(Button::Home);
    idle(14min);
    EXPECT_NE(app->mode(), App::Mode::Sleeping) << "still awake at 14 minutes";
    idle(1min);
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_TRUE(speech.heard("Sleep timer ended."));
}

TEST_F(AppTest, SleepTimerListOpensOnTheTimerThatIsSet) {
    playAlpha();
    press(Button::Ok); // Player menu
    open("Sleep timer");
    EXPECT_EQ(speech.last(), "Sleep timer. Off, current, 1 of 7");
    open("30 minutes");
    press(Button::Ok); // Sleep timer again
    EXPECT_EQ(speech.last(), "Sleep timer. 30 minutes, current, 4 of 7");
    open("End of chapter");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Sleep timer. End of chapter, current, 7 of 7");
}

TEST_F(AppTest, EndOfChapterIsOfferedOnlyWhenSomethingIsLoaded) {
    start();
    open("Settings");
    hold(Button::Forward);
    press(Button::Ok); // Sleep timer
    EXPECT_EQ(labels(), (std::vector<std::string>{"Off", "1 minute", "15 minutes", "30 minutes", "45 minutes", "1 hour"}));
    hold(Button::Home);
    open("Library");
    open("Alpha");
    hold(Button::Home);
    open("Settings");
    hold(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(labels().back(), "End of chapter");
}

TEST_F(AppTest, AutoSleepWhenIdle) {
    start();
    idle(4min);
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    idle(61s);
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
}

TEST_F(AppTest, NoAutoSleepWhilePlayingOrSpeaking) {
    playAlpha();
    idle(10min);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    press(Button::PlayPause);
    speech.isSpeaking = true;
    idle(10min);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    speech.isSpeaking = false;
    idle(10ms);
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
}

TEST_F(AppTest, HoldingPowerAgainReturnsToThePowerMenu) {
    start();
    hold(Button::Power);
    press(Button::Forward); // Power off
    hold(Button::Power);
    EXPECT_EQ(app->screens().size(), 2u) << "not a second Power menu on top";
    EXPECT_EQ(level().title, "Power");
    EXPECT_EQ(speech.last(), "Power. Sleep, 1 of 4");
    press(Button::Home);
    EXPECT_EQ(level().title, "Home");
}

TEST_F(AppTest, SleepFromThePowerMenuWakesWhereYouWere) {
    playAlpha();
    hold(Button::Power);
    press(Button::Ok); // Sleep
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    press(Button::Ok); // wake
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(speech.last(), "Alpha, paused. Storm.");
}

TEST_F(AppTest, PowerOffFromPowerMenu) {
    playAlpha();
    player.pos = 33;
    hold(Button::Power);
    EXPECT_EQ(level().title, "Power");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_TRUE(app->quitRequested());
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(stateFile.saves, 1);
    EXPECT_EQ(stateFile.onDisk("pos:" + alpha().string()), "0:33.0");
    EXPECT_EQ(configFile.saves, 0) << "config.yaml is not touched";
    EXPECT_EQ(speech.last(), "Powering off. Goodbye.");
}

TEST_F(AppTest, ShutdownAfterPowerOffDoesNotWriteTheSameStateAgain) {
    playAlpha();
    player.pos = 33;
    hold(Button::Power);
    open("Power off");
    app->shutdown(); // what the front end does once the goodbye is heard
    EXPECT_EQ(stateFile.saves, 1);
    EXPECT_EQ(stateFile.onDisk("pos:" + alpha().string()), "0:33.0");
}

TEST_F(AppTest, AStateWriteThatFailedIsTriedAgainOnTheNextExit) {
    playAlpha();
    player.pos = 33;
    stateFile.failSaves = true; // the card is read-only for a moment
    press(Button::Power);       // sleep: the write fails
    stateFile.failSaves = false;
    press(Button::Ok); // wake
    hold(Button::Power);
    open("Power off"); // nothing changed since the sleep, but it is not on disk yet
    EXPECT_EQ(stateFile.onDisk("pos:" + alpha().string()), "0:33.0");
}

TEST_F(AppTest, RestartWritesTheState) {
    playAlpha();
    player.pos = 12;
    hold(Button::Power);
    open("Restart");
    EXPECT_EQ(stateFile.onDisk("pos:" + alpha().string()), "0:12.0");
    EXPECT_EQ(configFile.saves, 0);
}

TEST_F(AppTest, FactoryResetKeepsTheServerAndForgetsThePlace) {
    settings.setString("server.url", "https://music.example");
    settings.setString("server.password", "1234");
    settings.setString("servers.0.url", "https://books.example");
    settings.setString("servers.0.api_key", "k3y");
    settings.setInt("volume", 33);
    settings.setInt("speech.rate", 310);
    playAlpha();
    player.pos = 99;
    press(Button::Power); // sleep writes the place
    press(Button::Ok);    // wake
    ASSERT_EQ(stateFile.onDisk("pos:" + alpha().string()), "0:99.0");
    hold(Button::Home);
    open("System");
    open("Factory reset");
    open("Erase all settings");
    EXPECT_TRUE(speech.heard("Settings erased."));
    EXPECT_EQ(configFile.onDisk("server.url"), "https://music.example");
    EXPECT_EQ(configFile.onDisk("server.password"), "1234");
    EXPECT_EQ(configFile.onDisk("servers.0.url"), "https://books.example");
    EXPECT_EQ(configFile.onDisk("servers.0.api_key"), "k3y");
    EXPECT_EQ(configFile.onDisk("volume"), "") << "the volume goes";
    EXPECT_EQ(configFile.onDisk("speech.rate"), "") << "user settings go";
    EXPECT_EQ(speech.rate, 220) << "the default applies at once";
    for (const auto& [key, value] : stateFile.file) EXPECT_NE(key.rfind("pos:", 0), 0u) << key;
    EXPECT_EQ(stateFile.onDisk("last_book"), "");
    EXPECT_EQ(app->mode(), App::Mode::Menu) << "nothing to resume";
    app->shutdown();
    for (const auto& [key, value] : stateFile.file) EXPECT_NE(key.rfind("pos:", 0), 0u) << key;
}

// ---------------------------------------------------------------- streaming quality

TEST_F(AppTest, StreamingQualityIsASettingThatDefaultsToTheOriginalFile) {
    start();
    open("Settings");
    open("Streaming quality");
    EXPECT_EQ(speech.last(), "Streaming quality. original file, current, 1 of 4");
    EXPECT_EQ(labels(), (std::vector<std::string>{"original file", "320 kilobits per second", "192 kilobits per second",
                                                  "128 kilobits per second"}));
}

TEST_F(AppTest, TheOriginalFileStreamsWithoutATranscode) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 1));
    EXPECT_EQ(player.file, "fake://stream/t2") << "no maxBitRate, no format";
    EXPECT_FALSE(player.transcoded) << "it seeks with Range";
}

// A transcode seeks by opening its offset URL (timeOffset), with the length the server lists.
TEST_F(AppTest, ATranscodeIsOpenedWithItsOffsetUrlAndTheServersLength) {
    settings.setInt("streaming.bitrate", 128);
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 1));
    EXPECT_EQ(player.file, "fake://stream/t2?maxBitRate=128");
    ASSERT_TRUE(player.transcoded);
    EXPECT_EQ(player.transcoded->offsetUrl, "fake://stream/t2?maxBitRate=128&timeOffset=");
    EXPECT_DOUBLE_EQ(player.transcoded->duration, 180) << "Rain, as the server lists it";
    player.pos = 170;
    deep(Button::Forward); // 30 s on would be past the end of Rain's 180 s
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_DOUBLE_EQ(player.pos, 180);
}

TEST_F(AppTest, ChangingTheQualityTakesEffectWithTheNextTrack) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    hold(Button::Home);
    open("Settings");
    open("Streaming quality");
    open("192 kilobits per second");
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1"})) << "Storm Front plays on as it is";
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t2?maxBitRate=192");
    ASSERT_TRUE(player.transcoded);
    EXPECT_DOUBLE_EQ(player.transcoded->duration, 180);
}

TEST_F(AppTest, TheStreamingQualityIsSavedAndKeptAcrossARestart) {
    start();
    open("Settings");
    open("Streaming quality");
    open("320 kilobits per second");
    EXPECT_EQ(configFile.onDisk("streaming.bitrate"), "320");
    app->shutdown();
    app.reset();
    Settings reloaded{&configFile}; // config.yaml as the next start reads it
    reloaded.load();
    app = std::make_unique<App>(Services{speech, player, sounds, volume, linkTo(reloaded, server, downloader), indexStore, scanner, reloaded, state, clock, nullptr}, root, music);
    app->start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    EXPECT_EQ(player.file, "fake://stream/t1?maxBitRate=320");
    app.reset(); // before `reloaded` goes
}

// After a dropout Play opens the track again where it stopped: a transcode
// does that through its offset URL, so the player is told it is one.
TEST_F(AppTest, PlayAfterATranscodeBrokeOffOpensItAsATranscodeAtItsPlace) {
    settings.setInt("streaming.bitrate", 128);
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.pos = 42;
    player.broken = true;
    app->tick();
    ASSERT_TRUE(speech.heard("Server not reachable. Press play to try again."));
    player.transcoded.reset();
    press(Button::PlayPause);
    answerPing();
    EXPECT_EQ(player.file, "fake://stream/t1?maxBitRate=128");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    ASSERT_TRUE(player.transcoded);
    EXPECT_EQ(player.transcoded->offsetUrl, "fake://stream/t1?maxBitRate=128&timeOffset=");
}

// The player takes a pre-open over only for the same URL. A real server's URL
// has a fresh salt each time, so the pre-opened stream is the one opened.
TEST_F(ReadAheadTest, ATranscodeIsPreOpenedAtTheQualitySetAndOpenedWithTheSameUrl) {
    settings.setInt("streaming.bitrate", 128);
    server.saltUrls = true;
    playWeather();
    ASSERT_DOUBLE_EQ(player.duration(), 200) << "Storm Front's length from the server";
    player.pos = 195;
    app->tick();
    ASSERT_TRUE(player.preloaded);
    EXPECT_EQ(player.preloaded->rfind("fake://stream/t2?maxBitRate=128#", 0), 0u) << *player.preloaded;
    std::string preloaded = *player.preloaded;
    app->tick();
    EXPECT_EQ(player.preloads.size(), 1u) << "not asked again at every tick";
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, preloaded);
    EXPECT_EQ(player.adopted, 1);
    ASSERT_TRUE(player.transcoded);
    EXPECT_DOUBLE_EQ(player.transcoded->duration, 180);
}

TEST_F(ReadAheadTest, ChangingTheQualityDuringAPreOpenPreOpensAtTheNewOne) {
    nearTheEnd();
    settings.setInt("streaming.bitrate", 192); // as the Settings menu sets it
    app->tick();
    EXPECT_EQ(player.preloaded, "fake://stream/t2?maxBitRate=192");
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t2?maxBitRate=192");
    EXPECT_EQ(player.adopted, 1);
}

// ------------------------------------------------------- playing from the cache

// Downloaded tracks (TrackCache, step 3.1) play from their files: a track
// resolves to its complete cached file, else to the stream.
class CachedPlayTest : public AsyncOpenTest {
protected:
    void SetUp() override {
        AsyncOpenTest::SetUp();
        cache = base / "cache";
        settings.setString("server.url", "https://music.example");
        settings.setString("server.user", "bob");
    }
    // Track `id` of Weather is downloaded into the cache; returns its file.
    std::string download(const std::string& id) {
        TrackCache store(diskFiles(), cache, "https://music.example", "bob");
        std::vector<CachedTrack> tracks = store.load(albumSource("weather")).tracks;
        tracks.push_back({id, id + ".mp3", id, 100, "", true, 0});
        fs::create_directories(store.listFolder(albumSource("weather")));
        touch(store.listFolder(albumSource("weather")) / (id + ".mp3"));
        EXPECT_TRUE(store.save(cachedAlbum("weather", tracks)));
        return (store.listFolder(albumSource("weather")) / (id + ".mp3")).string();
    }
    std::vector<std::string> streamsOpened() const {
        std::vector<std::string> out;
        for (const std::string& p : player.opened)
            if (p.find("://") != std::string::npos) out.push_back(p);
        return out;
    }
};

TEST_F(CachedPlayTest, AFullyCachedAlbumPlaysItsFilesAndOpensNoStream) {
    std::string f1 = download("t1"), f2 = download("t2"), f3 = download("t3");
    start();
    player.holdStreams = true;
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    EXPECT_FALSE(player.opening()) << "a file opens at once";
    EXPECT_FALSE(speech.heard("Loading."));
    EXPECT_EQ(player.file, f1);
    EXPECT_TRUE(player.playing());
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, f2);
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, f3);
    EXPECT_TRUE(streamsOpened().empty());
    EXPECT_TRUE(player.preloads.empty());
    expectNoUrlSpoken(speech);
}

TEST_F(CachedPlayTest, InAMixedAlbumEachTrackUsesItsOwnSource) {
    std::string f1 = download("t1"), f3 = download("t3");
    start();
    player.holdStreams = true;
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    EXPECT_EQ(player.file, f1);
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_TRUE(player.opening()) << "the stream waits for the server";
    ASSERT_TRUE(player.finishOpen());
    app->tick();
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, f3);
    EXPECT_EQ(streamsOpened(), (std::vector<std::string>{"fake://stream/t2"}));
}

TEST_F(CachedPlayTest, ACachedFileDeletedOnDiskFallsBackToTheStream) {
    std::string f1 = download("t1");
    fs::remove(f1);
    start();
    player.holdStreams = true;
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    EXPECT_EQ(player.file, "fake://stream/t1");
    ASSERT_TRUE(player.finishOpen());
    app->tick();
    EXPECT_TRUE(player.playing());
}

TEST_F(CachedPlayTest, ACachedFileThatCannotBeOpenedFallsBackToTheStream) {
    std::string f1 = download("t1");
    player.unopenable = {f1};
    start();
    player.holdStreams = true;
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    EXPECT_EQ(player.file, "fake://stream/t1");
    ASSERT_TRUE(player.finishOpen());
    app->tick();
    EXPECT_TRUE(player.playing());
    EXPECT_FALSE(speech.heard("Cannot play Storm Front. Server not reachable."));
}

TEST_F(CachedPlayTest, AFileThatCannotBeOpenedIsNotTriedAgainOnTheNextPlay) {
    std::string f1 = download("t1");
    player.unopenable = {f1};
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), f1), 1) << "dropped from the index after the first failure";
}

TEST_F(CachedPlayTest, ACachedTrackSeeksInItsFileEvenWhenTheQualityIsLowered) {
    std::string f1 = download("t1");
    settings.setInt("streaming.bitrate", 128);
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    EXPECT_EQ(player.file, f1);
    EXPECT_FALSE(player.transcoded) << "no transcode offset for a file";
    EXPECT_DOUBLE_EQ(player.duration(), player.length);
}

TEST_F(CachedPlayTest, ReadAheadPreOpensAStreamedNextTrackButNotACachedOne) {
    download("t3");
    playWeather();
    player.pos = player.length - 5;
    app->tick();
    EXPECT_EQ(player.preloaded, "fake://stream/t2") << "t2 is streamed";
    player.atEnd = true;
    app->tick();
    ASSERT_EQ(player.adopted, 1);
    player.pos = player.length - 5;
    app->tick();
    EXPECT_TRUE(player.preloaded == std::nullopt) << "t3 is a file";
    EXPECT_EQ(player.preloads, (std::vector<std::string>{"fake://stream/t2"}));
}

TEST_F(CachedPlayTest, ACachedTrackIsScrobbledLikeAStreamedOne) {
    download("t1");
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    idle(1s);
    EXPECT_EQ(server.scrobbles, (std::vector<FakeMusicServer::Scrobble>{{"t1", false}}));
    for (int i = 0; i < 241; ++i) idle(1s);
    EXPECT_EQ(server.scrobbles, (std::vector<FakeMusicServer::Scrobble>{{"t1", false}, {"t1", true}}));
}

TEST_F(CachedPlayTest, AnotherServersCacheIsNotUsed) {
    download("t1");
    settings.setString("server.url", "https://other.example");
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    EXPECT_EQ(player.file, "fake://stream/t1") << "another server's cache";
}

// ------------------------------------------------------- the scrobble outbox

// Every submission is kept in scrobbles.tsv, with the time it was heard, until its server answers it
// (docs/fixes.md, "Scrobbles"). Weather's tracks are t1..t3 of https://music.example, user bob.
class ScrobbleOutboxTest : public CachedPlayTest {
protected:
    using Scrobble = FakeMusicServer::Scrobble;
    static constexpr std::int64_t kHeardAt = 1'700'000'123;
    std::string ourKey() const { return TrackCache::serverKey("https://music.example", "bob"); }
    std::string otherKey() const { return TrackCache::serverKey("https://other.example", "bob"); }
    std::vector<ScrobbleEntry> outbox() const { return parseScrobbles(diskFiles().read(scrobbleFile).value_or("")); }
    void writeOutbox(const std::vector<ScrobbleEntry>& entries) {
        ASSERT_TRUE(diskFiles().writeAtomically(scrobbleFile, formatScrobbles(entries)));
    }
    std::vector<Scrobble> submissions() const {
        std::vector<Scrobble> out;
        for (const Scrobble& s : server.scrobbles)
            if (s.submission) out.push_back(s);
        return out;
    }
    void listen(std::chrono::milliseconds ms) {
        for (; ms >= 1s; ms -= 1s) idle(1s);
        if (ms > 0ms) idle(ms);
    }
    // Weather's first track streams and is heard for 100 s of its 200: the submission goes out at the end of this.
    void hearFirstTrack() {
        player.length = 200;
        start();
        ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
        listen(99s);
        ASSERT_TRUE(submissions().empty());
        clock.setWallTime(kHeardAt);
        listen(2s); // the first tick counts nothing: 100 s are heard now
    }
    // The start-up sync runs to its end (the server has nothing); `beforeEnd` runs before its last answer.
    void syncToEnd(const std::function<void()>& beforeEnd) {
        auto lastOf = [&](FakeMusicServer::Kind kind) {
            for (auto it = server.requests.rbegin(); it != server.requests.rend(); ++it)
                if (it->kind == kind) return it->id;
            return FakeMusicServer::RequestId{0};
        };
        ASSERT_TRUE(server.replyVersion(lastOf(FakeMusicServer::Kind::CatalogueVersion), "v1"));
        app->tick();
        ASSERT_TRUE(server.reply(lastOf(FakeMusicServer::Kind::Albums), std::vector<RemoteAlbum>{}));
        app->tick();
        if (beforeEnd) beforeEnd();
        ASSERT_TRUE(server.reply(lastOf(FakeMusicServer::Kind::Playlists), std::vector<RemotePlaylist>{}));
        app->tick();
    }
    // The stream breaks: the server is marked down, and its ping (10 s later) is answered.
    void markDownThenPingAnswers() {
        player.broken = true;
        app->tick();
        ASSERT_EQ(speech.last(), "Server not reachable. Press play to try again.");
        idle(11s);
        answerPing();
    }
};

TEST_F(ScrobbleOutboxTest, ASubmissionIsInTheFileWhenItIsSentAndBeforeTheServerAnswersIt) {
    server.holdScrobbles = true;
    std::vector<ScrobbleEntry> onDiskWhenSent;
    server.onScrobble = [&](const Scrobble& s) {
        if (s.submission) onDiskWhenSent = outbox();
    };
    hearFirstTrack();
    const std::vector<ScrobbleEntry> expected = {{ourKey(), "t1", kHeardAt * 1000}};
    EXPECT_EQ(onDiskWhenSent, expected) << "written before the request";
    EXPECT_EQ(outbox(), expected) << "and still there: the server has not answered";
    ASSERT_EQ(submissions().size(), 1u);
    EXPECT_EQ(submissions()[0].timeMs, kHeardAt * 1000);
}

TEST_F(ScrobbleOutboxTest, TheFileHoldsNeitherTheTokenNorAStreamUrl) {
    server.holdScrobbles = true;
    hearFirstTrack();
    const std::string text = diskFiles().read(scrobbleFile).value_or("");
    EXPECT_NE(text.find("t1"), std::string::npos);
    for (const char* secret : {"://", "stream", "https", "bob", "music.example/"}) EXPECT_EQ(text.find(secret), std::string::npos) << secret;
}

TEST_F(ScrobbleOutboxTest, AnAnsweredSubmissionLeavesTheFile) {
    hearFirstTrack();
    ASSERT_EQ(outbox().size(), 1u);
    idle(1s); // the answer arrives
    EXPECT_TRUE(outbox().empty());
    EXPECT_EQ(diskFiles().read(scrobbleFile), "");
    listen(10s);
    EXPECT_EQ(submissions().size(), 1u) << "not sent again";
}

TEST_F(ScrobbleOutboxTest, ANewAppOnTheFilesSendsASubmissionTheServerNeverAnswered) {
    server.holdScrobbles = true;
    hearFirstTrack();
    ASSERT_EQ(outbox().size(), 1u);
    app.reset(); // the power is cut: no shutdown, no answer
    server.dropHeldScrobbles();
    server.scrobbles.clear();
    start();
    ASSERT_EQ(submissions().size(), 1u) << "sent at start";
    EXPECT_EQ(submissions()[0], Scrobble("t1", true));
    EXPECT_EQ(submissions()[0].timeMs, kHeardAt * 1000) << "with the time it was heard";
    ASSERT_TRUE(server.answerScrobble());
    app->tick();
    EXPECT_TRUE(outbox().empty());
}

TEST_F(ScrobbleOutboxTest, ASubmissionThatCouldNotReachTheServerIsSentAgainWithItsTimeAfterAPingClearsTheMark) {
    server.scrobbleError = MusicServer::Error::Unreachable;
    hearFirstTrack();
    idle(1s);
    EXPECT_EQ(outbox().size(), 1u) << "kept";
    EXPECT_EQ(submissions().size(), 1u) << "the round stopped";
    server.scrobbleError = MusicServer::Error::None;
    clock.setWallTime(kHeardAt + 500);
    const size_t said = speech.said.size();
    markDownThenPingAnswers();
    ASSERT_EQ(submissions().size(), 2u);
    EXPECT_EQ(submissions()[1], Scrobble("t1", true));
    EXPECT_EQ(submissions()[1].timeMs, kHeardAt * 1000) << "the time it was heard, not the time it is sent";
    idle(1s);
    EXPECT_TRUE(outbox().empty());
    EXPECT_EQ(speech.said.size(), said + 1) << "only the stream's failure was spoken";
}

TEST_F(ScrobbleOutboxTest, ASubmissionTheServerRefusesIsNotSentAgain) {
    server.scrobbleError = MusicServer::Error::Server;
    hearFirstTrack();
    idle(1s);
    EXPECT_TRUE(outbox().empty()) << "dropped";
    server.scrobbleError = MusicServer::Error::None;
    markDownThenPingAnswers();
    EXPECT_EQ(submissions().size(), 1u);
}

TEST_F(ScrobbleOutboxTest, AFailedLoginKeepsTheSubmissionForLater) {
    server.scrobbleError = MusicServer::Error::BadLogin;
    hearFirstTrack();
    idle(1s);
    EXPECT_EQ(outbox().size(), 1u);
    EXPECT_EQ(submissions().size(), 1u);
}

TEST_F(ScrobbleOutboxTest, WithTheServerMarkedDownASubmissionGoesToTheOutboxWithoutARequest) {
    download("t1");
    start();
    player.unopenable = {"fake://stream/t2"};
    EXPECT_FALSE(app->playRemote("Weather", weather(), 1)); // marks the server down
    ASSERT_TRUE(speech.heard("Cannot play Rain. Server not reachable."));
    player.unopenable.clear();
    player.length = 200;
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0)); // t1 plays from its file
    ASSERT_EQ(player.file.find("://"), std::string::npos);
    clock.setWallTime(kHeardAt);
    listen(101s);
    EXPECT_TRUE(submissions().empty()) << "no request";
    EXPECT_EQ(outbox(), (std::vector<ScrobbleEntry>{{ourKey(), "t1", kHeardAt * 1000}}));
    const size_t said = speech.said.size();
    idle(11s);
    answerPing();
    ASSERT_EQ(submissions().size(), 1u) << "sent once the server answers";
    EXPECT_EQ(submissions()[0].timeMs, kHeardAt * 1000);
    EXPECT_EQ(speech.said.size(), said) << "nothing is spoken";
    idle(1s);
    EXPECT_TRUE(outbox().empty());
}

TEST_F(ScrobbleOutboxTest, NowPlayingIsNeverKept) {
    server.scrobbleError = MusicServer::Error::Unreachable;
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    idle(1s);
    ASSERT_EQ(server.scrobbles, (std::vector<Scrobble>{{"t1", false}}));
    idle(1s);
    EXPECT_TRUE(outbox().empty());
    EXPECT_FALSE(fs::exists(scrobbleFile));
    app.reset();
    server.scrobbles.clear();
    start();
    EXPECT_TRUE(server.scrobbles.empty()) << "a new App has nothing to send";
}

TEST_F(ScrobbleOutboxTest, OnlyTheNewest1000EntriesAreKept) {
    server.holdScrobbles = true;
    std::vector<ScrobbleEntry> old;
    for (std::size_t i = 0; i < kMaxScrobbles; ++i) old.push_back({otherKey(), "x" + std::to_string(i), std::int64_t(i + 1)});
    writeOutbox(old);
    hearFirstTrack(); // the 1001st
    const std::string text = diskFiles().read(scrobbleFile).value_or("");
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), static_cast<long>(kMaxScrobbles) + 1) << "the header and 1000 lines";
    const std::vector<ScrobbleEntry> kept = outbox();
    ASSERT_EQ(kept.size(), kMaxScrobbles);
    EXPECT_EQ(kept.front().trackId, "x1") << "the oldest went";
    EXPECT_EQ(kept.back(), (ScrobbleEntry{ourKey(), "t1", kHeardAt * 1000})) << "the newest stays";
}

TEST_F(ScrobbleOutboxTest, EntriesOfAnotherServerStayAndAreNotSentUntilItIsSetUpAgain) {
    writeOutbox({{otherKey(), "x1", 1000}, {otherKey(), "x2", 2000}});
    hearFirstTrack();
    idle(1s);
    EXPECT_EQ(submissions().size(), 1u);
    EXPECT_EQ(submissions()[0], Scrobble("t1", true)) << "ours; the other server's are not sent here";
    EXPECT_EQ(outbox(), (std::vector<ScrobbleEntry>{{otherKey(), "x1", 1000}, {otherKey(), "x2", 2000}}));
    app.reset();
    server.scrobbles.clear();
    server.holdScrobbles = true;
    settings.setString("server.url", "https://other.example");
    start();
    ASSERT_EQ(submissions().size(), 1u) << "one at a time";
    EXPECT_EQ(submissions()[0], Scrobble("x1", true)) << "oldest first";
    EXPECT_EQ(submissions()[0].timeMs, 1000);
    ASSERT_TRUE(server.answerScrobble());
    app->tick();
    ASSERT_EQ(submissions().size(), 2u);
    EXPECT_EQ(submissions()[1], Scrobble("x2", true));
    EXPECT_EQ(submissions()[1].timeMs, 2000);
    ASSERT_TRUE(server.answerScrobble());
    app->tick();
    EXPECT_TRUE(outbox().empty());
}

TEST_F(ScrobbleOutboxTest, AnAnswerAfterShutdownClearsItsEntryAndSendsNoMore) {
    writeOutbox({{ourKey(), "t0", 500}, {ourKey(), "t5", 600}});
    server.holdScrobbles = true;
    start();
    ASSERT_EQ(submissions().size(), 1u);
    app->shutdown();
    ASSERT_TRUE(server.answerScrobble()); // the front end lets the exit answer through (Subsonic::finish)
    app->tick();
    EXPECT_EQ(outbox(), (std::vector<ScrobbleEntry>{{ourKey(), "t5", 600}})) << "t0 is taken: not sent again at the next start";
    EXPECT_EQ(submissions().size(), 1u) << "nothing is sent while leaving";
}

TEST_F(ScrobbleOutboxTest, AFinishedSyncSendsWhatIsWaiting) {
    writeOutbox({{ourKey(), "t7", 7000}});
    server.scrobbleError = MusicServer::Error::Unreachable;
    start();
    idle(1s);
    ASSERT_EQ(submissions().size(), 1u) << "tried at start";
    server.scrobbleError = MusicServer::Error::None;
    EXPECT_EQ(submissions().size(), 1u);
    syncToEnd(/*before the last answer:*/ [&] { EXPECT_EQ(submissions().size(), 1u) << "the sync is not over"; });
    ASSERT_EQ(submissions().size(), 2u);
    EXPECT_EQ(submissions()[1].timeMs, 7000);
}

TEST_F(ScrobbleOutboxTest, WithNoServerSetUpNothingIsKept) {
    settings.setString("server.url", "");
    hearFirstTrack();
    idle(1s);
    EXPECT_FALSE(fs::exists(scrobbleFile));
    EXPECT_TRUE(submissions().empty());
}

TEST_F(ScrobbleOutboxTest, OnlyOneSubmissionIsOutAtATimeEvenWhenTheServerAnswersMeanwhile) {
    writeOutbox({{ourKey(), "t7", 7000}, {ourKey(), "t8", 8000}});
    server.holdScrobbles = true;
    start();
    ASSERT_EQ(submissions().size(), 1u);
    syncToEnd({}); // the server answers a request: still waiting for the first scrobble
    EXPECT_EQ(submissions().size(), 1u);
    ASSERT_TRUE(server.answerScrobble());
    app->tick();
    ASSERT_EQ(submissions().size(), 2u);
    EXPECT_EQ(submissions()[1], Scrobble("t8", true));
}

// ------------------------------------------------------- downloading albums

// "Download album" in a server album (step 3.4): the tracks go one after the
// other into the cache, each in the manifest and pinned once it is whole.
class DownloadTest : public StreamingTest {
protected:
    using Result = Downloader::Result;
    void SetUp() override {
        StreamingTest::SetUp();
        cache = base / "cache";
        settings.setString("server.url", "https://music.example");
        settings.setString("server.user", "bob");
    }
    // Three tracks of 5 bytes, which is what FakeDownloader::finish() writes.
    static std::vector<RemoteTrack> sized() {
        return {{"t1", "Storm Front", "Band", "Weather", 200, 1, "flac", 5},
                {"t2", "Rain", "Band", "Weather", 180, 2, "mp3", 5},
                {"t3", "Sunshine", "Band", "Weather", 240, 3, "flac", 5}};
    }
    TrackCache store() const { return TrackCache(diskFiles(), cache, "https://music.example", "bob"); }
    // Streaming > Recently added > Weather, on its first item.
    void openAlbum(std::vector<RemoteTrack> tracks = sized()) {
        openList("Recently added");
        answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", int(tracks.size())}});
        open("Weather");
        answer(std::move(tracks));
    }
    // Pressing OK on "Download album" with the album open.
    void startDownload() {
        open("Download album");
        ASSERT_FALSE(downloader.requests.empty());
    }
    // The newest request ends with `result`, and App sees it.
    void finishLast(Result result = Result::Ok, bool writeFile = true) {
        ASSERT_TRUE(downloader.finish(downloader.lastRequest().id, result, writeFile));
        app->tick();
    }
    std::vector<std::string> trackIdsRequested() const {
        std::vector<std::string> out;
        for (const auto& r : downloader.requests) out.push_back(r.trackId);
        return out;
    }
};

TEST_F(DownloadTest, DownloadQualityDefaultsToOriginalAndPersistsIndependently) {
    start();
    open("Settings");
    open("Download quality");
    EXPECT_EQ(speech.last(), "Download quality. original file, current, 1 of 4");
    EXPECT_EQ(labels(), (std::vector<std::string>{"original file", "320 kilobits per second", "192 kilobits per second", "128 kilobits per second"}));
    open("128 kilobits per second");
    EXPECT_EQ(configFile.onDisk("downloads.bitrate"), "128");
    Settings reloaded{&configFile};
    reloaded.load();
    EXPECT_EQ(reloaded.getInt("downloads.bitrate", -1), 128);
    open("Streaming quality");
    EXPECT_EQ(speech.last(), "Streaming quality. original file, current, 1 of 4");
}

TEST_F(DownloadTest, DownloadedTranscodesRecordTheCapturedQualityAndMp3File) {
    settings.setInt("downloads.bitrate", 128);
    openAlbum();
    startDownload();
    EXPECT_EQ(downloader.lastRequest().bitrate, 128);
    EXPECT_TRUE(downloader.lastRequest().dest.ends_with("t1~128.mp3"));
    settings.setInt("downloads.bitrate", 320); // an ongoing album retains its chosen quality
    finishLast();
    EXPECT_EQ(downloader.lastRequest().bitrate, 128);
    finishLast();
    finishLast();
    auto saved = store().load(albumSource("al1"));
    ASSERT_EQ(saved.tracks.size(), 3u);
    for (const auto& track : saved.tracks) {
        EXPECT_EQ(track.bitrate, 128);
        EXPECT_TRUE(track.file.ends_with("~128.mp3"));
        EXPECT_TRUE(store().completeFile(albumSource("al1"), track));
    }
}

TEST_F(DownloadTest, ATranscodedFilenameCannotOverwriteAnotherTracksOriginal) {
    settings.setInt("downloads.bitrate", 128);
    const auto source = albumSource("al1");
    const auto old = store().listFolder(source) / "t1.128.mp3";
    touch(old);
    ASSERT_TRUE(store().save(cachedAlbum("al1", {{"t1.128", "t1.128.mp3", "Other song", 10, "", true, 0, 0}})));
    auto tracks = sized();
    tracks.resize(2);
    tracks[1].id = "t1.128";
    tracks[1].suffix = "mp3";
    openAlbum(tracks);
    startDownload();
    EXPECT_NE(fs::path(downloader.lastRequest().dest), old);
    EXPECT_TRUE(fs::exists(old));
}

TEST_F(DownloadTest, TranscodeSpaceUsesDurationAndBitrateWithByteConversion) {
    settings.setInt("downloads.bitrate", 128);
    auto tracks = sized();
    tracks.resize(1);
    tracks[0].duration = 2.5;
    tracks[0].size = 1000000;
    openAlbum(tracks);
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 39999; });
    open("Download album");
    EXPECT_EQ(speech.last(), "Not enough space.");
    EXPECT_TRUE(downloader.requests.empty());
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 40000; });
    press(Button::Ok);
    ASSERT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(downloader.lastRequest().bitrate, 128);
}

TEST_F(DownloadTest, TranscodeWithoutDurationUsesOriginalSizeForSpace) {
    settings.setInt("downloads.bitrate", 128);
    auto tracks = sized();
    tracks.resize(1);
    tracks[0].duration = 0;
    tracks[0].size = 100;
    openAlbum(tracks);
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 99; });
    open("Download album");
    EXPECT_EQ(speech.last(), "Not enough space.");
    EXPECT_TRUE(downloader.requests.empty());
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 100; });
    press(Button::Ok);
    EXPECT_EQ(downloader.requests.size(), 1u);
}

TEST_F(DownloadTest, AnOriginalDownloadKeepsTheLowerCopyPlayableUntilReplacementCompletes) {
    const auto source = albumSource("al1");
    const auto old = store().listFolder(source) / "t1~128.mp3";
    touch(old);
    ASSERT_TRUE(store().save(cachedAlbum("al1", {{"t1", old.filename().string(), "Storm Front", 200, "", false, 123, 128}})));
    auto tracks = sized();
    tracks.resize(1);
    openAlbum(tracks);
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 4; });
    open("Download album");
    EXPECT_EQ(speech.last(), "Not enough space.");
    EXPECT_TRUE(downloader.requests.empty()) << "replacement needs room for the new file alongside the old";
    EXPECT_TRUE(fs::exists(old));
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 5; });
    startDownload();
    ASSERT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(downloader.lastRequest().bitrate, 0);
    EXPECT_EQ(speech.last(), "Downloading, 0 of 1 track.");
    EXPECT_EQ(store().load(source).tracks.front().bitrate, 128);
    open("Storm Front");
    EXPECT_EQ(player.file, old.string()) << "the in-memory cache index also keeps the old copy";
    EXPECT_TRUE(player.playing());
    finishLast();
    const auto saved = store().load(source);
    ASSERT_EQ(saved.tracks.size(), 1u);
    EXPECT_EQ(saved.tracks.front().bitrate, 0);
    EXPECT_TRUE(saved.tracks.front().pinned);
    EXPECT_EQ(saved.tracks.front().file, "t1.flac");
    EXPECT_FALSE(fs::exists(old)) << "the old file must not become an unlisted orphan";
    EXPECT_TRUE(fs::exists(store().listFolder(source) / "t1.flac"));
    ASSERT_TRUE(app->playRemote("Weather", tracks, 0));
    EXPECT_EQ(player.file, (store().listFolder(source) / "t1.flac").string());
}

TEST_F(DownloadTest, ReplacingACopyRetargetsLocalQueueEntriesAndOpenAlbumActions) {
    CachedList list = cachedAlbum("al1", {});
    for (const auto& track : sized()) {
        const std::string name = track.id + "~128.mp3";
        touch(store().listFolder(list.source) / name);
        list.tracks.push_back({track.id, name, track.title, track.duration, "", true, 123, 128});
    }
    ASSERT_TRUE(store().save(list));
    openAlbum();
    startDownload();
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    open("Weather");
    open("Storm Front");
    EXPECT_TRUE(player.file.ends_with("t1~128.mp3"));
    finishLast();
    finishLast();
    finishLast();
    press(Button::Forward);
    EXPECT_EQ(player.file, (store().listFolder(list.source) / "t2.mp3").string());
    EXPECT_TRUE(player.playing());
    press(Button::Home); // the already-open album still holds its earlier file names
    open("Sunshine");
    EXPECT_EQ(player.file, (store().listFolder(list.source) / "t3.flac").string());
    press(Button::Home);
    press(Button::Options); // the old Sunshine action can also be queued through Options
    open("Play next");
    hold(Button::Home);
    open("Now playing");
    press(Button::Forward);
    EXPECT_EQ(player.file, (store().listFolder(list.source) / "t3.flac").string());
    app->shutdown();
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    ASSERT_EQ(saved->queue.size(), 4u);
    for (const auto& entry : saved->queue.originalEntries()) {
        EXPECT_EQ(entry.path.find("~128"), std::string::npos);
        EXPECT_TRUE(fs::exists(entry.path));
    }
}

TEST_F(DownloadTest, ReplacingACopyRetargetsTheParkedAndSavedMusicQueue) {
    CachedList list = cachedAlbum("al1", {});
    for (const auto& track : sized()) {
        const std::string name = track.id + "~128.mp3";
        touch(store().listFolder(list.source) / name);
        list.tracks.push_back({track.id, name, track.title, track.duration, "", true, 123, 128});
    }
    ASSERT_TRUE(store().save(list));
    openAlbum();
    startDownload();
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    open("Weather");
    open("Storm Front");
    hold(Button::Home);
    open("Library");
    open("Alpha");
    const std::string bookFile = player.file;
    finishLast();
    finishLast();
    finishLast();
    EXPECT_EQ(player.file, bookFile) << "replacement does not disturb the book player";
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    for (const auto& entry : saved->queue.originalEntries()) {
        EXPECT_EQ(entry.path.find("~128"), std::string::npos);
        EXPECT_TRUE(fs::exists(entry.path));
    }
    hold(Button::Home);
    open("Continue");
    EXPECT_EQ(player.file, (store().listFolder(list.source) / "t1.flac").string());
    press(Button::Forward);
    EXPECT_EQ(player.file, (store().listFolder(list.source) / "t2.mp3").string());
    EXPECT_TRUE(player.playing());
}

TEST_F(DownloadTest, DownloadPinsOriginalHigherAndEqualCopiesWithoutFetchingOrRequiringSpace) {
    settings.setInt("downloads.bitrate", 128);
    auto tracks = sized();
    const int qualities[] = {0, 320, 128};
    CachedList list = cachedAlbum("al1", {});
    for (size_t i = 0; i < tracks.size(); ++i) {
        const std::string name = tracks[i].id + ".mp3";
        touch(store().listFolder(list.source) / name);
        list.tracks.push_back({tracks[i].id, name, tracks[i].title, tracks[i].duration, "", false, 123, qualities[i]});
    }
    ASSERT_TRUE(store().save(list));
    openAlbum(tracks);
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 0; });
    open("Download album");
    EXPECT_EQ(speech.last(), "Already downloaded.");
    EXPECT_TRUE(downloader.requests.empty());
    const auto saved = store().load(list.source);
    ASSERT_EQ(saved.tracks.size(), 3u);
    for (size_t i = 0; i < saved.tracks.size(); ++i) {
        EXPECT_TRUE(saved.tracks[i].pinned);
        EXPECT_EQ(saved.tracks[i].bitrate, qualities[i]);
        EXPECT_EQ(saved.tracks[i].lastPlayed, 123);
        EXPECT_TRUE(store().completeFile(list.source, saved.tracks[i]));
    }
}

TEST_F(DownloadTest, AnUpgradePinsTheOldCopyAgainstEvictionWhileDownloading) {
    const auto source = albumSource("al1");
    const auto old = store().listFolder(source) / "t1~128.mp3";
    touch(old);
    std::ofstream(old) << "old";
    ASSERT_TRUE(store().save(cachedAlbum("al1", {{"t1", old.filename().string(), "Storm Front", 200, "", false, 123, 128}})));
    auto tracks = sized();
    tracks.resize(1);
    openAlbum(tracks);
    app->setCacheGigabyte(1); // this three-byte automatic copy would otherwise be evicted on sleep
    startDownload();
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_TRUE(fs::exists(old));
    const auto saved = store().load(source);
    ASSERT_EQ(saved.tracks.size(), 1u);
    EXPECT_TRUE(saved.tracks.front().pinned);
    EXPECT_EQ(saved.tracks.front().bitrate, 128);
    press(Button::Ok); // wake; the explicit download is still in flight
    open("Storm Front");
    EXPECT_EQ(player.file, old.string());
}

TEST_F(DownloadTest, AFailedTranscodeUpgradeKeepsTheOldCopyAndRetryReplacesItOnce) {
    settings.setInt("downloads.bitrate", 320);
    const auto source = albumSource("al1");
    const auto old = store().listFolder(source) / "t1~128.mp3";
    touch(old);
    ASSERT_TRUE(store().save(cachedAlbum("al1", {{"t1", old.filename().string(), "Storm Front", 200, "", true, 123, 128}})));
    auto tracks = sized();
    tracks.resize(1);
    tracks.push_back(tracks.front()); // a duplicate must not schedule a second replacement
    openAlbum(tracks);
    startDownload();
    ASSERT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(downloader.lastRequest().bitrate, 320);
    finishLast(Result::Unreachable, false);
    EXPECT_EQ(speech.last(), "Cannot download Storm Front. Server not reachable.");
    EXPECT_TRUE(fs::exists(old));
    EXPECT_EQ(store().load(source).tracks.front().bitrate, 128);
    open("Storm Front");
    EXPECT_EQ(player.file, old.string());
    press(Button::Home);
    startDownload();
    ASSERT_EQ(downloader.requests.size(), 2u);
    finishLast();
    EXPECT_EQ(downloader.requests.size(), 2u);
    const auto saved = store().load(source);
    ASSERT_EQ(saved.tracks.size(), 1u);
    EXPECT_EQ(saved.tracks.front().bitrate, 320);
    EXPECT_EQ(saved.tracks.front().file, "t1~320.mp3");
    EXPECT_TRUE(saved.tracks.front().pinned);
    EXPECT_FALSE(fs::exists(old));
    ASSERT_TRUE(app->playRemote("Weather", tracks, 0));
    EXPECT_EQ(player.file, (store().listFolder(source) / "t1~320.mp3").string());
}

TEST_F(DownloadTest, AReplacementWhoseManifestSaveFailsLeavesTheOldCopyPlayable) {
    class SwitchableFiles : public TextFiles {
    public:
        bool fail = false;
        std::optional<std::string> read(const fs::path& file) override { return diskFiles().read(file); }
        bool writeAtomically(const fs::path& file, const std::string& contents) override {
            return !fail && diskFiles().writeAtomically(file, contents);
        }
    } switchable;
    textFiles = &switchable;
    const auto source = albumSource("al1");
    const auto old = store().listFolder(source) / "t1~128.mp3";
    touch(old);
    ASSERT_TRUE(store().save(cachedAlbum("al1", {{"t1", old.filename().string(), "Storm Front", 200, "", true, 123, 128}})));
    auto tracks = sized();
    tracks.resize(1);
    openAlbum(tracks);
    startDownload();
    ASSERT_FALSE(downloader.requests.empty());
    const auto replacement = downloader.lastRequest().dest;
    switchable.fail = true;
    finishLast();
    EXPECT_EQ(speech.last(), "Cannot download Storm Front. Cannot write the file.");
    EXPECT_TRUE(fs::exists(old));
    EXPECT_FALSE(fs::exists(replacement));
    EXPECT_EQ(store().load(source).tracks.front().bitrate, 128);
    open("Storm Front");
    EXPECT_EQ(player.file, old.string());
    app.reset(); // before switchable goes
}

TEST_F(DownloadTest, ADownloadOfTheFirstServerLeavesTheSecondsDownloaderAlone) {
    FakeMusicServer second;
    FakeDownloader secondDownloader;
    moreLinks.push_back(ServerLink{ServerConfig{}, "other-1", "Server 2", second, secondDownloader});
    openAlbum();
    startDownload();
    EXPECT_EQ(downloader.requests.size(), 1u);
    EXPECT_TRUE(secondDownloader.requests.empty());
}

TEST_F(DownloadTest, AlbumAndPlaylistOfferDownloadRightAfterPlay) {
    indexPlaylist("p1", "Road", 3);
    openAlbum();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play album", "Download album", "Storm Front", "Rain", "Sunshine"}));
    hold(Button::Home);
    open("Music");
    open("Playlists");
    open("Road");
    answer(sized());
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play playlist", "Download playlist", "Storm Front", "Rain", "Sunshine", "Keep offline"}));
}

TEST_F(DownloadTest, NoDownloadItemWithoutACacheFolderAServerOrADownloader) {
    cache.clear();
    openAlbum();
    EXPECT_EQ(labels().at(1), "Storm Front") << "no cache folder";
    app.reset();
    cache = base / "cache";
    downloader.isConfigured = false;
    start();
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(sized());
    EXPECT_EQ(labels().at(1), "Storm Front") << "a downloader that is not set up";
    app.reset();
    downloader.isConfigured = true;
    settings.setString("server.url", "");
    start();
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(sized());
    EXPECT_EQ(labels().at(1), "Storm Front") << "no server address to key the cache by";
}

TEST_F(DownloadTest, AnAppWithoutTextFilesHasNoCacheEvenWithAFolder) {
    app = std::make_unique<App>(Services{speech, player, sounds, volume, linkTo(settings, server, downloader), indexStore, scanner, settings, state, clock, nullptr}, root, music, cache);
    app->start();
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(sized());
    EXPECT_EQ(labels().at(1), "Storm Front") << "no Download item: manifests cannot be read or written";
    press(Button::Home);
    auto items = labels();
    EXPECT_EQ(std::count(items.begin(), items.end(), "Downloaded"), 0) << "and no Downloaded list";
}

TEST_F(DownloadTest, TracksAreFetchedInOrderIntoTheListFolderAndPinnedOneByOne) {
    openAlbum();
    startDownload();
    EXPECT_EQ(speech.last(), "Downloading, 0 of 3 tracks.");
    ASSERT_EQ(downloader.requests.size(), 1u) << "one at a time";
    EXPECT_EQ(downloader.lastRequest().trackId, "t1");
    EXPECT_EQ(downloader.lastRequest().dest, (store().listFolder(albumSource("al1")) / "t1.flac").string());
    finishLast();
    ASSERT_EQ(downloader.requests.size(), 2u);
    EXPECT_EQ(downloader.lastRequest().trackId, "t2");
    EXPECT_EQ(downloader.lastRequest().dest, (store().listFolder(albumSource("al1")) / "t2.mp3").string());
    auto manifest = store().load(albumSource("al1")).tracks;
    ASSERT_EQ(manifest.size(), 1u) << "written after each track";
    EXPECT_EQ(manifest[0].id, "t1");
    EXPECT_EQ(manifest[0].file, "t1.flac");
    EXPECT_EQ(manifest[0].title, "Storm Front");
    EXPECT_DOUBLE_EQ(manifest[0].duration, 200);
    EXPECT_TRUE(manifest[0].pinned);
    finishLast();
    finishLast();
    EXPECT_EQ(trackIdsRequested(), (std::vector<std::string>{"t1", "t2", "t3"}));
    manifest = store().load(albumSource("al1")).tracks;
    ASSERT_EQ(manifest.size(), 3u);
    EXPECT_EQ(manifest[2].id, "t3");
    for (const CachedTrack& t : manifest) EXPECT_TRUE(t.pinned) << t.id;
    EXPECT_EQ(store().find("t3"), store().listFolder(albumSource("al1")) / "t3.flac");
    EXPECT_EQ(speech.last(), "Download finished, Weather.");
    expectNoUrlSpoken(speech);
}

TEST_F(DownloadTest, TracksAlreadyCompleteAreSkippedAndKeptAndPinned) {
    TrackCache c = store();
    fs::create_directories(c.listFolder(albumSource("al1")));
    touch(c.listFolder(albumSource("al1")) / "t2.mp3");
    touch(c.listFolder(albumSource("al1")) / "old.flac");
    CachedTrack cached{"t2", "t2.mp3", "Rain", 180, "", false, 77}; // kept by the automatic cache, not pinned
    CachedTrack other{"x9", "old.flac", "Other", 10, "", true, 0};   // not in this album
    CachedTrack lost{"t1", "t1.flac", "Storm Front", 200, "", false, 5}; // its file is gone: fetched again
    ASSERT_TRUE(c.save(cachedAlbum("al1", {cached, other, lost})));
    openAlbum();
    startDownload();
    EXPECT_EQ(speech.last(), "Downloading, 1 of 3 tracks.");
    EXPECT_EQ(downloader.lastRequest().trackId, "t1");
    finishLast();
    EXPECT_EQ(downloader.lastRequest().trackId, "t3") << "t2 is whole already";
    finishLast();
    EXPECT_EQ(trackIdsRequested(), (std::vector<std::string>{"t1", "t3"}));
    auto manifest = c.load(albumSource("al1")).tracks;
    ASSERT_EQ(manifest.size(), 3u) << "x9 is not in the album any more: dropped";
    EXPECT_EQ(manifest[0].id, "t1") << "in the order of the server's list";
    EXPECT_TRUE(manifest[0].pinned);
    EXPECT_EQ(manifest[0].lastPlayed, 5) << "a track fetched again keeps its play time";
    EXPECT_EQ(manifest[1].id, "t2");
    EXPECT_TRUE(manifest[1].pinned);
    EXPECT_EQ(manifest[1].lastPlayed, 77) << "what was there is kept";
    EXPECT_EQ(manifest[2].id, "t3");
    EXPECT_FALSE(fs::exists(c.listFolder(albumSource("al1")) / "old.flac")) << "and so is its file";
}

// Seeds the Weather album folder with these tracks (id, pinned), each with a whole file, in this order.
static void seedWeather(const TrackCache& c, std::vector<std::pair<std::string, bool>> tracks) {
    std::vector<CachedTrack> manifest;
    for (const auto& [id, pinned] : tracks) {
        touch(c.listFolder(albumSource("al1")) / (id + ".mp3"));
        manifest.push_back({id, id + ".mp3", id, 100, "", pinned, 0});
    }
    ASSERT_TRUE(c.save(cachedAlbum("al1", manifest)));
}

static std::vector<std::string> idsOf(const std::vector<CachedTrack>& tracks) {
    std::vector<std::string> ids;
    for (const CachedTrack& t : tracks) ids.push_back(t.id);
    return ids;
}

// ---- Download in Options (fixes.md 2.6)

class DownloadOptionsTest : public DownloadTest {
protected:
    // Streaming > Recently added with Weather and Rain Songs, on Weather; Options of it is open.
    void openAlbumOptions(bool running = false) {
        if (running) { // the App already runs: from Home
            open("Server");
            open("Recently added");
        } else {
            openList("Recently added");
        }
        answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}, {"al2", "Rain Songs", "Band", 3}});
        press(Button::Options);
        ASSERT_EQ(level().title, "Options, Weather");
    }
    // The same on the second album.
    void openSecondAlbumOptions(bool running = false) {
        if (running) {
            open("Server");
            open("Recently added");
        } else {
            openList("Recently added");
        }
        answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}, {"al2", "Rain Songs", "Band", 3}});
        press(Button::Forward);
        press(Button::Options);
        ASSERT_EQ(level().title, "Options, Rain Songs");
    }
    // Home > Music > Playlists > Road, Options open.
    void openPlaylistOptions() {
        indexPlaylist("p1", "Road", 3);
        start();
        open("Music");
        open("Playlists");
        press(Button::Options);
        ASSERT_EQ(level().title, "Options, Road");
    }
};

TEST_F(DownloadOptionsTest, TheOptionsOfAServerAlbumAndPlaylistOfferDownloadAfterAddToQueue) {
    openAlbumOptions();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play now", "Play next", "Add to queue", "Download album"}));
    app.reset();
    openPlaylistOptions();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play now", "Play next", "Add to queue", "Download playlist"}));
}

TEST_F(DownloadOptionsTest, NoDownloadInOptionsWithoutACacheFolder) {
    cache.clear();
    openAlbumOptions();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play now", "Play next", "Add to queue"}));
    app.reset();
    openPlaylistOptions();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play now", "Play next", "Add to queue"}));
}

TEST_F(DownloadOptionsTest, AnAlbumWholeOnTheDeviceHasNoDownloadInOptions) {
    openAlbum();
    startDownload();
    finishLast();
    finishLast();
    finishLast();
    ASSERT_EQ(speech.last(), "Download finished, Weather.");
    hold(Button::Home);
    openAlbumOptions(true);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play now", "Play next", "Add to queue"}));
    hold(Button::Home);
    openSecondAlbumOptions(true);
    EXPECT_EQ(labels().back(), "Download album") << "another album still has it";
}

TEST_F(DownloadOptionsTest, AWholeDownloadOffersAnUpgradeOnlyWhenTheRequestedQualityIsBetter) {
    CachedList list = cachedAlbum("al1", {});
    for (const auto& track : sized()) {
        const std::string name = track.id + "~128.mp3";
        touch(store().listFolder(list.source) / name);
        list.tracks.push_back({track.id, name, track.title, track.duration, "", true, 123, 128});
    }
    ASSERT_TRUE(store().save(list));
    openAlbumOptions(); // original requested by default
    EXPECT_EQ(labels().back(), "Download album");
    for (const auto& [quality, offered] : std::vector<std::pair<int, bool>>{{128, false}, {320, true}, {0, true}, {64, false}}) {
        settings.setInt("downloads.bitrate", quality);
        press(Button::Home);
        press(Button::Options);
        const auto items = labels();
        EXPECT_EQ(std::find(items.begin(), items.end(), "Download album") != items.end(), offered) << quality;
    }
}

TEST_F(DownloadOptionsTest, AnAlbumWhoseDownloadFailedOnTheLastTrackIsNotWhole) {
    openAlbum();
    startDownload();
    finishLast();
    finishLast();
    finishLast(Result::Unreachable, false);
    ASSERT_EQ(speech.last(), "Cannot download Sunshine. Server not reachable.");
    hold(Button::Home);
    openAlbumOptions(true);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play now", "Play next", "Add to queue", "Download album"}))
        << "two of three tracks are on the disk: Download is still offered";
    app.reset(); // and after a restart too
    openAlbumOptions();
    EXPECT_EQ(labels().back(), "Download album");
}

TEST_F(DownloadOptionsTest, APlaylistWhoseDownloadFailedLoadsFromTheServerInMusicPlaylists) {
    openPlaylistOptions();
    open("Download playlist");
    answer(sized());
    finishLast();
    finishLast(Result::Unreachable, false);
    hold(Button::Home);
    open("Music");
    open("Playlists");
    open("Road");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Loading"})) << "not played from the one track on the disk";
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
}

TEST_F(DownloadOptionsTest, ADownloadThatFailsBeforeAnyTrackArrivesLeavesTheListNotWhole) {
    seedWeather(store(), {{"t1", true}}); // what an earlier download left: one track, pinned
    openAlbum();
    startDownload();
    finishLast(Result::Unreachable, false);
    hold(Button::Home);
    openAlbumOptions(true);
    EXPECT_EQ(labels().back(), "Download album") << "the one track on the disk is not the album";
}

TEST_F(DownloadOptionsTest, AFailedDownloadGoneOnWithEndsWhole) {
    openAlbum();
    startDownload();
    finishLast();
    finishLast(Result::Unreachable, false);
    open("Download album"); // goes on with what is missing
    finishLast();
    finishLast();
    ASSERT_EQ(speech.last(), "Download finished, Weather.");
    hold(Button::Home);
    openAlbumOptions(true);
    EXPECT_EQ(labels().size(), 3u) << "whole now: no Download";
}

TEST_F(DownloadOptionsTest, ADownloadWithNothingLeftToFetchEndsAnUnfinishedMarkWithoutFetching) {
    openAlbum();
    startDownload();
    finishLast();
    finishLast();
    finishLast();
    TrackCache c = store();
    CachedList list = c.load(albumSource("al1"));
    ASSERT_EQ(list.tracks.size(), 3u);
    for (CachedTrack& t : list.tracks) t.changed = kPartialMark; // the app was stopped before the mark was cleared
    ASSERT_TRUE(c.save(list));
    app.reset();
    openAlbum();
    open("Download album");
    EXPECT_EQ(speech.last(), "Already downloaded.");
    for (const CachedTrack& t : c.load(albumSource("al1")).tracks) EXPECT_EQ(t.changed, "") << t.id;
}

TEST_F(DownloadOptionsTest, OkLoadsTheTracksThenStartsTheDownloadAndGoesBack) {
    openAlbumOptions();
    open("Download album");
    EXPECT_EQ(speech.last(), "Loading.");
    EXPECT_EQ(level().title, "Options, Weather") << "waits on the Options screen";
    EXPECT_EQ(server.lastRequest().kind, Kind::AlbumTracks);
    EXPECT_EQ(server.lastRequest().arg, "al1");
    EXPECT_TRUE(downloader.requests.empty());
    answer(sized());
    EXPECT_EQ(speech.last(), "Downloading, 0 of 3 tracks.");
    EXPECT_EQ(level().title, "Recently added");
    EXPECT_EQ(trackIdsRequested(), (std::vector<std::string>{"t1"}));
    EXPECT_EQ(downloader.lastRequest().dest, (store().listFolder(albumSource("al1")) / "t1.flac").string());
    finishLast();
    finishLast();
    finishLast();
    EXPECT_EQ(trackIdsRequested(), (std::vector<std::string>{"t1", "t2", "t3"}));
    EXPECT_EQ(speech.last(), "Download finished, Weather.");
}

TEST_F(DownloadOptionsTest, OkOnAPlaylistDownloadsIt) {
    openPlaylistOptions();
    open("Download playlist");
    EXPECT_EQ(speech.last(), "Loading.");
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
    EXPECT_EQ(server.lastRequest().arg, "p1");
    answer(sized());
    EXPECT_EQ(speech.last(), "Downloading, 0 of 3 tracks.");
    EXPECT_EQ(level().title, "Playlists");
    EXPECT_EQ(trackIdsRequested(), (std::vector<std::string>{"t1"}));
    EXPECT_EQ(downloader.lastRequest().dest, (store().listFolder({RemoteSource::Kind::Playlist, "p1"}) / "t1.flac").string());
}

TEST_F(DownloadOptionsTest, ALoadThatFailsSpeaksTheErrorStaysInOptionsAndDownloadsNothing) {
    openAlbumOptions();
    open("Download album");
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(speech.last(), "Server not reachable.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(level().title, "Options, Weather");
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(DownloadOptionsTest, AnAlbumWithNoTracksSaysSoStaysInOptionsAndDownloadsNothing) {
    openAlbumOptions();
    open("Download album");
    answer(std::vector<RemoteTrack>{});
    EXPECT_EQ(speech.last(), "Weather has no tracks.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(level().title, "Options, Weather");
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(DownloadOptionsTest, ARefusedDownloadIsSpokenOnTheOptionsAndTheyStay) {
    seedWeather(store(), {{"t1", false}, {"t2", false}, {"t3", false}}); // all there, kept by the cache, not downloaded
    openAlbumOptions();
    EXPECT_EQ(labels().back(), "Download album") << "not a download of the user's: still offered";
    open("Download album");
    answer(sized());
    EXPECT_EQ(speech.last(), "Already downloaded.");
    EXPECT_EQ(level().title, "Options, Weather");
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(DownloadOptionsTest, LeavingOptionsWhileLoadingCancelsTheDownload) {
    openAlbumOptions();
    open("Download album");
    const auto id = server.lastRequest().id;
    press(Button::Home);
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), id), server.cancelled.end());
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(DownloadOptionsTest, WhileADownloadRunsOkOnTheSameAlbumGivesTheProgressWithoutLoading) {
    openAlbum();
    startDownload();
    finishLast();
    hold(Button::Home);
    openAlbumOptions(true);
    const size_t asked = server.requests.size();
    open("Download album");
    EXPECT_EQ(speech.last(), "Downloading, 1 of 3 tracks.");
    EXPECT_EQ(server.requests.size(), asked) << "no request";
    EXPECT_EQ(downloader.requests.size(), 2u) << "no second job";
    EXPECT_EQ(level().title, "Options, Weather");
}

TEST_F(DownloadOptionsTest, WhileADownloadRunsAnotherAlbumSaysSoWithoutLoading) {
    openAlbum();
    startDownload();
    hold(Button::Home);
    openSecondAlbumOptions(true);
    const size_t asked = server.requests.size();
    open("Download album");
    EXPECT_EQ(speech.last(), "Another download is running.");
    EXPECT_EQ(server.requests.size(), asked) << "no request";
    EXPECT_EQ(downloader.requests.size(), 1u);
}

TEST_F(DownloadTest, TheManifestFollowsTheServersOrderWhenTheListIsDownloadedAgain) {
    seedWeather(store(), {{"t3", true}, {"t1", true}}); // an earlier download, when the list was ordered otherwise
    openAlbum();
    startDownload();
    EXPECT_EQ(trackIdsRequested(), (std::vector<std::string>{"t2"}));
    finishLast();
    EXPECT_EQ(idsOf(store().load(albumSource("al1")).tracks), (std::vector<std::string>{"t1", "t2", "t3"}));
}

TEST_F(DownloadTest, AnAlbumThatIsAllDownloadedIsReorderedAndLosesTheTracksTheServerDropped) {
    seedWeather(store(), {{"t3", true}, {"x9", true}, {"u1", false}, {"t2", true}, {"t1", true}});
    openAlbum();
    open("Download album");
    EXPECT_EQ(speech.last(), "Already downloaded.");
    EXPECT_TRUE(downloader.requests.empty());
    EXPECT_EQ(idsOf(store().load(albumSource("al1")).tracks), (std::vector<std::string>{"t1", "t2", "t3", "u1"}))
        << "x9 left the server's list; u1 is a copy of the automatic cache and stays, after the others";
    EXPECT_FALSE(fs::exists(store().listFolder(albumSource("al1")) / "x9.mp3")) << "the dropped track's file goes";
    EXPECT_TRUE(fs::exists(store().listFolder(albumSource("al1")) / "u1.mp3"));
}

TEST_F(DownloadTest, ATrackListedTwiceIsFetchedAndCountedOnce) {
    auto tracks = sized();
    tracks.push_back(tracks[0]); // t1 again: four entries, three tracks
    openAlbum(tracks);
    startDownload();
    EXPECT_EQ(speech.last(), "Downloading, 0 of 3 tracks.");
    finishLast();
    finishLast();
    finishLast();
    EXPECT_EQ(trackIdsRequested(), (std::vector<std::string>{"t1", "t2", "t3"})) << "t1 is not fetched a second time";
    EXPECT_EQ(speech.last(), "Download finished, Weather.");
    EXPECT_EQ(idsOf(store().load(albumSource("al1")).tracks), (std::vector<std::string>{"t1", "t2", "t3"}));
}

TEST_F(DownloadTest, ADamagedManifestLeavesNoOrphanedFilesWhenTheListIsDownloadedAgain) {
    TrackCache c = store();
    seedWeather(c, {{"t1", true}});
    touch(c.listFolder(albumSource("al1")) / "old.flac");
    touch(c.listFolder(albumSource("al1")) / "t2.mp3.part"); // a transfer that may be running
    touch(c.listFolder(albumSource("al2")) / "other.flac");  // another list
    std::ofstream(c.manifestFile(albumSource("al1")), std::ios::trunc) << "garbage\n";
    openAlbum();
    startDownload();
    finishLast();
    EXPECT_EQ(idsOf(c.load(albumSource("al1")).tracks), (std::vector<std::string>{"t1"})) << "readable again";
    EXPECT_FALSE(fs::exists(c.listFolder(albumSource("al1")) / "old.flac")) << "not listed by anything: gone";
    EXPECT_FALSE(fs::exists(c.listFolder(albumSource("al1")) / "t2.mp3")) << "(t2 is fetched again anyway)";
    EXPECT_TRUE(fs::exists(c.listFolder(albumSource("al1")) / "t2.mp3.part")) << "never a .part";
    EXPECT_TRUE(fs::exists(c.listFolder(albumSource("al2")) / "other.flac")) << "never another folder";
}

TEST_F(DownloadTest, ANextTrackThatGotCachedWhileItsStreamWasPreOpenedDropsTheStream) {
    openAlbum();
    startDownload();
    finishLast(); // t1 whole, t2 on its way
    ASSERT_TRUE(app->playRemote("Other", {{"z1", "Zed", "Band", "Other", 100, 1}, {"t2", "Rain", "Band", "Weather", 180, 2}}, 0));
    player.pos = player.length - 5;
    app->tick();
    ASSERT_EQ(player.preloaded, "fake://stream/t2");
    finishLast(); // t2 is a file now
    app->tick();
    EXPECT_FALSE(player.preloaded) << "no stream is kept open for a track that plays from its file";
}

TEST_F(DownloadTest, AnAlbumThatIsAllDownloadedStartsNothing) {
    TrackCache c = store();
    std::vector<CachedTrack> all;
    for (const RemoteTrack& t : sized()) {
        touch(c.listFolder(albumSource("al1")) / (t.id + ".mp3"));
        all.push_back({t.id, t.id + ".mp3", t.title, t.duration, "", true, 0});
    }
    ASSERT_TRUE(c.save(cachedAlbum("al1", all)));
    openAlbum();
    open("Download album");
    EXPECT_EQ(speech.last(), "Already downloaded.");
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(DownloadTest, NotEnoughSpaceStartsNothingAndSaysSo) {
    openAlbum();
    std::optional<fs::path> asked;
    app->setFreeSpace([&](const fs::path& folder) -> std::optional<std::uintmax_t> {
        asked = folder;
        return 14; // the three tracks are 15 bytes
    });
    open("Download album");
    EXPECT_EQ(speech.last(), "Not enough space.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_TRUE(downloader.requests.empty());
    EXPECT_EQ(asked, cache) << "the disk of the cache folder";
    press(Button::Ok);
    EXPECT_TRUE(downloader.requests.empty());
    EXPECT_FALSE(speech.heard("Downloading, 0 of 3 tracks."));
}

TEST_F(DownloadTest, ExactlyEnoughSpaceStartsAndOnlyWhatIsMissingCounts) {
    TrackCache c = store();
    touch(c.listFolder(albumSource("al1")) / "t1.flac");
    ASSERT_TRUE(c.save(cachedAlbum("al1", {{"t1", "t1.flac", "Storm Front", 200, "", true, 0}})));
    openAlbum();
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 10; }); // t2 and t3
    open("Download album");
    EXPECT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(speech.last(), "Downloading, 1 of 3 tracks.");
}

TEST_F(DownloadTest, ASpaceThatCannotBeToldDoesNotStopTheDownload) {
    openAlbum();
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return std::nullopt; });
    open("Download album");
    EXPECT_EQ(downloader.requests.size(), 1u);
}

TEST_F(DownloadTest, TheRealDiskIsAskedByDefault) {
    openAlbum(); // sizes of 5 bytes fit on any disk the tests run on
    open("Download album");
    EXPECT_EQ(downloader.requests.size(), 1u);
    EXPECT_TRUE(fs::is_directory(cache)) << "the folder exists to be asked";
}

TEST_F(DownloadTest, OkOnTheItemAndWhereAmISayTheProgress) {
    openAlbum();
    startDownload();
    finishLast();
    press(Button::Ok); // on Download album again
    EXPECT_EQ(speech.last(), "Downloading, 1 of 3 tracks.");
    EXPECT_EQ(downloader.requests.size(), 2u) << "no second download of the same list";
    finishLast();
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Home, Server, Recently added, Weather. Download album, 2 of 5. Downloading, 2 of 3 tracks.");
}

TEST_F(DownloadTest, WhereAmIInThePlayerSaysTheProgressToo) {
    openAlbum();
    startDownload();
    finishLast();
    press(Button::Forward);
    press(Button::Ok); // Storm Front plays
    ASSERT_EQ(app->mode(), App::Mode::Player);
    hold(Button::PlayPause);
    EXPECT_NE(speech.last().find("Downloading, 1 of 3 tracks."), std::string::npos) << speech.last();
}

TEST_F(DownloadTest, WhereAmISaysNothingOfDownloadsWhenNoneRuns) {
    openAlbum();
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Home, Server, Recently added, Weather. Play album, 3 tracks, 1 of 5");
}

TEST_F(DownloadTest, AnotherListCannotStartWhileOneDownloads) {
    indexPlaylist("p1", "Road", 3);
    openAlbum();
    startDownload();
    hold(Button::Home);
    open("Music");
    open("Playlists");
    open("Road");
    answer(sized());
    open("Download playlist");
    EXPECT_EQ(speech.last(), "Another download is running.");
    EXPECT_EQ(downloader.requests.size(), 1u);
}

TEST_F(DownloadTest, TheManifestKeepsTheKindNameAndArtistOfTheList) {
    openAlbum();
    startDownload();
    finishLast();
    CachedList list = store().load(albumSource("al1"));
    EXPECT_EQ(list.source.kind, RemoteSource::Kind::Album);
    EXPECT_EQ(list.source.id, "al1");
    EXPECT_EQ(list.name, "Weather");
    EXPECT_EQ(list.artist, "Band");
}

TEST_F(DownloadTest, APlaylistIsKeptApartFromAnAlbumWithTheSameId) {
    indexPlaylist("al1", "Road", 3);
    openPlaylist("Road");
    answer(sized());
    open("Download playlist");
    finishLast();
    CachedList list = store().load(playlistSource("al1"));
    EXPECT_EQ(list.source.kind, RemoteSource::Kind::Playlist);
    EXPECT_EQ(list.name, "Road");
    EXPECT_EQ(list.artist, "");
    EXPECT_EQ(list.tracks.size(), 1u);
    EXPECT_TRUE(store().load(albumSource("al1")).tracks.empty());
    EXPECT_EQ(downloader.lastRequest().dest.find((store().listFolder(playlistSource("al1")) / "").string()), 0u);
}

TEST_F(DownloadTest, AFailedTrackIsSpokenOnceAndTheDownloadStops) {
    openAlbum();
    startDownload();
    finishLast();
    speech.said.clear();
    finishLast(Result::Unreachable, false);
    EXPECT_EQ(speech.said, (std::vector<std::string>{"Cannot download Rain. Server not reachable."}));
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(downloader.requests.size(), 2u) << "t3 is not started";
    app->tick();
    app->tick();
    EXPECT_EQ(speech.said.size(), 1u);
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last().find("Downloading"), std::string::npos) << "it no longer runs";
    EXPECT_EQ(store().load(albumSource("al1")).tracks.size(), 1u);
}

TEST_F(DownloadTest, OkOnDownloadAgainResumesWhereItStopped) {
    openAlbum();
    startDownload();
    finishLast();
    ASSERT_TRUE(downloader.finish(downloader.lastRequest().id, Result::Unreachable, false, "", "ra"));
    app->tick();
    std::string failedDest = downloader.lastRequest().dest;
    press(Button::Ok); // on Download album
    EXPECT_EQ(speech.last(), "Downloading, 1 of 3 tracks.");
    ASSERT_EQ(downloader.requests.size(), 3u);
    EXPECT_EQ(downloader.lastRequest().trackId, "t2") << "t1 is not fetched again";
    EXPECT_EQ(downloader.lastRequest().dest, failedDest) << "the same file: its .part continues";
    EXPECT_TRUE(fs::exists(failedDest + ".part")) << "kept for the downloader to continue";
    finishLast();
    finishLast();
    EXPECT_EQ(store().load(albumSource("al1")).tracks.size(), 3u);
    EXPECT_EQ(speech.last(), "Download finished, Weather.");
}

namespace {
// Reads from disk; writes nothing (the card is full).
class ReadOnlyFiles : public TextFiles {
public:
    std::optional<std::string> read(const fs::path& path) override { return diskFiles().read(path); }
    bool writeAtomically(const fs::path&, const std::string&) override { return false; }
};
} // namespace

TEST_F(DownloadTest, AFinishedFileWhoseManifestCannotBeWrittenIsNotLeftUnlisted) {
    ReadOnlyFiles readOnly;
    textFiles = &readOnly;
    openAlbum();
    startDownload();
    std::string dest = downloader.lastRequest().dest;
    finishLast();
    EXPECT_EQ(speech.last(), "Cannot download Storm Front. Cannot write the file.");
    EXPECT_FALSE(fs::exists(dest)) << "an orphan would count against no limit and never be evicted";
    app.reset(); // before readOnly goes
}

TEST_F(DownloadTest, EachFailureIsNamedByItsReason) {
    openAlbum();
    startDownload();
    finishLast(Result::Disk, false);
    EXPECT_EQ(speech.last(), "Cannot download Storm Front. Cannot write the file.");
    press(Button::Ok);
    finishLast(Result::Server, false);
    EXPECT_EQ(speech.last(), "Cannot download Storm Front. Server error.");
    press(Button::Ok);
    finishLast(Result::NotConfigured, false);
    EXPECT_EQ(speech.last(), "Cannot download Storm Front. No server set up.");
    expectNoUrlSpoken(speech);
}

TEST_F(DownloadTest, ATrackWithoutATitleIsNamedByItsNumber) {
    auto tracks = sized();
    tracks[0].title.clear();
    openAlbum(tracks);
    startDownload();
    finishLast(Result::Unreachable, false);
    EXPECT_EQ(speech.last(), "Cannot download track 1. Server not reachable.");
}

TEST_F(DownloadTest, TheListedSizeGoesToTheDownloaderWhichJudgesCompleteness) {
    openAlbum();
    startDownload();
    EXPECT_EQ(downloader.lastRequest().expectedSize, 5);
}

TEST_F(DownloadTest, AFinishedFileIsKeptWhateverItsSizeBecauseTheDownloaderSaidOk) {
    openAlbum(); // the server lists 5 bytes; a stale size or a transcode gives another length
    startDownload();
    ASSERT_TRUE(downloader.finish(downloader.lastRequest().id, Result::Ok, true, "audi"));
    app->tick();
    EXPECT_EQ(downloader.requests.size(), 2u) << "on to the next track, no failure";
    EXPECT_EQ(store().load(albumSource("al1")).tracks.size(), 1u);
    EXPECT_TRUE(fs::exists(store().listFolder(albumSource("al1")) / "t1.flac"));
}

TEST_F(DownloadTest, ATrackOfUnknownSizeIsAcceptedWhateverItsLength) {
    auto tracks = sized();
    for (RemoteTrack& t : tracks) t.size = 0;
    openAlbum(tracks);
    startDownload();
    finishLast();
    EXPECT_EQ(store().load(albumSource("al1")).tracks.size(), 1u);
}

TEST_F(DownloadTest, LeavingTheMenuGoingHomeOrPlayingDoesNotStopTheDownload) {
    openAlbum();
    startDownload();
    press(Button::Home); // back to the Server menu
    press(Button::Home);
    finishLast();
    EXPECT_EQ(downloader.requests.size(), 2u);
    EXPECT_TRUE(downloader.cancelled.empty());
    ASSERT_TRUE(app->playRemote("Other", {{"z1", "Zed", "Band", "Other", 100, 1}}, 0));
    finishLast();
    EXPECT_EQ(downloader.requests.size(), 3u);
    hold(Button::Home);
    finishLast();
    EXPECT_EQ(store().load(albumSource("al1")).tracks.size(), 3u);
    EXPECT_TRUE(downloader.cancelled.empty());
}

TEST_F(DownloadTest, FinishingWhileSomethingPlaysIsAnEarconNotSpeech) {
    openAlbum();
    startDownload();
    ASSERT_TRUE(app->playRemote("Other", {{"z1", "Zed", "Band", "Other", 100, 1}}, 0));
    ASSERT_TRUE(player.playing());
    finishLast();
    finishLast();
    speech.said.clear();
    finishLast();
    EXPECT_TRUE(speech.said.empty()) << speech.said.front();
    EXPECT_EQ(sounds.last(), Earcon::Open);
    EXPECT_EQ(store().load(albumSource("al1")).tracks.size(), 3u);
}

TEST_F(DownloadTest, ADownloadThatEndsWhileTheDeviceSleepsIsSilent) {
    openAlbum();
    startDownload();
    finishLast();
    finishLast();
    press(Button::Power);
    ASSERT_TRUE(speech.heard("Going to sleep."));
    speech.said.clear();
    finishLast();
    EXPECT_TRUE(speech.said.empty());
    EXPECT_EQ(store().load(albumSource("al1")).tracks.size(), 3u);
}

TEST_F(DownloadTest, ShutdownCancelsTheTransferAndKeepsWhatIsDone) {
    openAlbum();
    startDownload();
    finishLast();
    auto id = downloader.lastRequest().id;
    app->shutdown();
    EXPECT_EQ(downloader.cancelled, (std::vector<Downloader::RequestId>{id}));
    EXPECT_EQ(store().load(albumSource("al1")).tracks.size(), 1u);
}

// The cache index in memory (step 3.4): a track downloaded after one was looked up plays from its file.
TEST_F(DownloadTest, ATrackDownloadedAfterTheIndexWasBuiltPlaysFromItsFile) {
    openAlbum();
    ASSERT_TRUE(app->playRemote("Other", {{"z1", "Zed", "Band", "Other", 100, 1}}, 0)); // builds the index
    press(Button::Home);
    EXPECT_TRUE(player.opened.back().find("fake://") == 0);
    startDownload();
    finishLast();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0));
    EXPECT_EQ(player.file, (store().listFolder(albumSource("al1")) / "t1.flac").string());
}

TEST_F(DownloadTest, TheIndexIsNotReadFromDiskAtEveryTick) {
    auto tracks = sized();
    TrackCache c = store();
    touch(c.listFolder(albumSource("al1")) / "t3.flac");
    ASSERT_TRUE(c.save(cachedAlbum("al1", {{"t3", "t3.flac", "Sunshine", 240, "", true, 0}})));
    start();
    ASSERT_TRUE(app->playRemote("Weather", tracks, 1));
    player.pos = player.length - 5;
    app->tick(); // the next track is looked up: the index is made
    fs::remove(c.manifestFile(albumSource("al1")));
    player.preloads.clear();
    app->tick();
    app->tick();
    EXPECT_TRUE(player.preloads.empty()) << "t3 is still known to be a file: no pre-open";
}

TEST_F(DownloadTest, ACachedFileThatWentAwayAfterTheIndexWasMadeIsStreamed) {
    TrackCache c = store();
    touch(c.listFolder(albumSource("al1")) / "t1.flac");
    ASSERT_TRUE(c.save(cachedAlbum("al1", {{"t1", "t1.flac", "Storm Front", 200, "", true, 0}})));
    start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0)); // from the file
    EXPECT_EQ(player.file, (c.listFolder(albumSource("al1")) / "t1.flac").string());
    fs::remove(c.listFolder(albumSource("al1")) / "t1.flac");
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0));
    EXPECT_EQ(player.file, "fake://stream/t1");
}

// ------------------------------------------------------- Streaming > Downloaded

// Streaming > Downloaded (step 3.5): the albums and playlists in the manifests.
// The cache is filled directly here, as a finished download leaves it.
class DownloadedTest : public DownloadTest {
protected:
    struct T {
        std::string id, title;
        bool onDisk = true;
    };
    // A list in the cache with these tracks; a track `onDisk` has a file, the others only a manifest entry.
    RemoteSource seed(RemoteSource source, const std::string& name, const std::string& artist, std::vector<T> tracks,
                      TrackCache cache) {
        CachedList list{source, name, artist, {}};
        for (const T& t : tracks) {
            list.tracks.push_back({t.id, t.id + ".mp3", t.title, 100, "", true, 0});
            if (t.onDisk) touch(cache.listFolder(source) / (t.id + ".mp3"));
        }
        EXPECT_TRUE(cache.save(list));
        return source;
    }
    RemoteSource seed(RemoteSource source, const std::string& name, const std::string& artist, std::vector<T> tracks) {
        return seed(std::move(source), name, artist, std::move(tracks), store());
    }
    RemoteSource seedWeather() {
        return seed(albumSource("al1"), "Weather", "Band", {{"t1", "Storm Front"}, {"t2", "Rain"}, {"t3", "Sunshine"}});
    }
    // Home > Server > Downloaded
    void openDownloaded() {
        openServer();
        open("Downloaded");
    }
    // ... > Downloaded > `name`
    void openDownload(const std::string& name) {
        openDownloaded();
        open(name);
    }
    std::string fileOf(const RemoteSource& source, const std::string& id) const {
        return (store().listFolder(source) / (id + ".mp3")).string();
    }
    bool anyStreamOpened() const {
        for (const std::string& p : player.opened)
            if (p.find("://") != std::string::npos) return true;
        return false;
    }
};

// Each server has its own cache folder: a second server shows none of the first one's downloads and does not
// play from them, even for an album with the same id.
TEST_F(DownloadedTest, ASecondServerShowsNoneOfTheFirstServersDownloadsAndAsksItsServerForAnAlbumWithTheSameId) {
    FakeMusicServer second;
    FakeDownloader secondDownloader;
    ServerConfig config;
    config.name = "Audiobooks";
    config.url = "https://books.example";
    moreLinks.push_back(ServerLink{config, "books-1", "Audiobooks", second, secondDownloader});
    seedWeather(); // album "al1" is whole on the disk
    start();
    open("Audiobooks");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Recently added", "Albums A to Z", "Artists", "Random albums",
                                                  "Starred", "Downloaded", "Server status"}));
    open("Downloaded");
    EXPECT_EQ(labels(), (std::vector<std::string>{"No downloads"}));
    hold(Button::Home);
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"al1", "Weather", "Other", 3}}, false));
    app->tick();
    press(Button::Options);
    open("Play now");
    ASSERT_EQ(second.lastRequest().kind, Kind::AlbumTracks) << "not answered from the first server's files";
    EXPECT_EQ(second.lastRequest().arg, "al1");
}

TEST_F(DownloadedTest, AListThatFailedOnASecondServerOffersNoDownloaded) {
    FakeMusicServer second;
    FakeDownloader secondDownloader;
    ServerConfig config;
    config.url = "https://books.example";
    moreLinks.push_back(ServerLink{config, "books-1", "Audiobooks", second, secondDownloader});
    seedWeather();
    start();
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.fail(second.lastRequest().id, MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not reachable", "Try again"}));
}

TEST_F(DownloadedTest, TheServerMenuOffersDownloadedOnlyWithACacheFolder) {
    openServer();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Recently added", "Albums A to Z", "Artists",
                                                  "Random albums", "Starred", "Downloaded", "Server status"}));
    EXPECT_EQ(listRequests(), 0u) << "it is not a list from the server";
}

TEST_F(DownloadedTest, NoCacheFolderMeansNoDownloadedItem) {
    cache.clear();
    openServer();
    for (const std::string& label : labels()) EXPECT_NE(label, "Downloaded");
}

TEST_F(DownloadedTest, TheListMatchesTheManifestsByName) {
    seed(albumSource("al9"), "weather", "Zed", {{"w1", "One"}});
    seed(playlistSource("pl1"), "Road songs", "", {{"r1", "Highway"}});
    seed(albumSource("al2"), "Autumn", "", {{"a1", "Leaves"}});
    seed(albumSource("al3"), "Half", "Band", {{"h1", "Only a part", false}}); // nothing whole: not listed
    seed(albumSource("al4"), "Mixed", "Band", {{"m1", "Here"}, {"m2", "Gone", false}});
    TrackCache elsewhere(diskFiles(), cache, "https://other.example", "bob");
    seed(albumSource("al5"), "Other server", "", {{"o1", "X"}}, elsewhere);
    openDownloaded();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Autumn", "Mixed", "Road songs", "weather"}));
    EXPECT_EQ(speech.last(), "Downloaded. Autumn, 1 of 4") << "no value to speak";
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Mixed, Band, 2 of 4");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Road songs, playlist, 3 of 4");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "weather, Zed, 4 of 4");
}

TEST_F(DownloadedTest, AnAlbumAndAPlaylistWithTheSameIdAreBothListed) {
    seed(albumSource("7"), "Album seven", "Band", {{"a1", "One"}});
    seed(playlistSource("7"), "Playlist seven", "", {{"p1", "Two"}});
    openDownloaded();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Album seven", "Playlist seven"}));
}

TEST_F(DownloadedTest, AnEmptyListSaysWhyAndWhatToDo) {
    openDownloaded();
    EXPECT_EQ(labels(), (std::vector<std::string>{"No downloads"}));
    EXPECT_EQ(speech.last(), "Downloaded. No downloads, 1 of 1");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Nothing is downloaded. Choose Download album in an album.");
}

TEST_F(DownloadedTest, AListWithoutANameIsSpokenAsWhatItIs) {
    seed(albumSource("al1"), "", "", {{"t1", "One"}});
    seed(playlistSource("pl1"), "", "", {{"t2", "Two"}});
    openDownloaded();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Album", "Playlist"}));
}

TEST_F(DownloadedTest, AFinishedDownloadShowsUpInTheList) {
    openAlbum();
    startDownload();
    finishLast();
    finishLast();
    finishLast();
    press(Button::Home);
    press(Button::Home);
    open("Downloaded");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Weather"}));
    open("Weather");
    EXPECT_EQ(labels().front(), "Play album");
    EXPECT_EQ(speech.last(), "Weather. Play album, 3 tracks, 1 of 5");
}

TEST_F(DownloadedTest, AListOpensWithPlayFirstThenDeleteThenItsCompleteTracks) {
    seed(albumSource("al4"), "Mixed", "Band", {{"m1", "Here"}, {"m2", "Gone", false}, {"m3", "Also here"}});
    openDownload("Mixed");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play album", "Delete download", "Here", "Also here"}));
    EXPECT_EQ(speech.last(), "Mixed. Play album, 2 tracks, 1 of 4");
    EXPECT_EQ(level().title, "Mixed");
}

TEST_F(DownloadedTest, APlaylistOffersPlayPlaylist) {
    seed(playlistSource("pl1"), "Road songs", "", {{"r1", "Highway"}});
    openDownload("Road songs");
    EXPECT_EQ(labels().front(), "Play playlist");
}

TEST_F(DownloadedTest, PlayAlbumPlaysTheFilesAndAsksTheServerForNothing) {
    RemoteSource al = seedWeather();
    openDownload("Weather");
    press(Button::Ok); // Play album
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, fileOf(al, "t1"));
    EXPECT_FALSE(anyStreamOpened());
    EXPECT_EQ(listRequests(), 0u);
    EXPECT_FALSE(speech.heard("Loading.")) << "a file opens at once";
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, fileOf(al, "t2")) << "the next track is a file too";
    expectNoUrlSpoken(speech);
}

TEST_F(DownloadedTest, ATrackPlaysFromThereAndKeepsItsNameFromTheManifest) {
    RemoteSource al = seedWeather();
    openDownload("Weather");
    open("Rain");
    EXPECT_EQ(player.file, fileOf(al, "t2"));
    EXPECT_EQ(app->nowPlaying().title, "Weather");
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
    press(Button::Home);
    EXPECT_EQ(level().title, "Weather") << "back in the list it was chosen from";
}

TEST_F(DownloadedTest, DeleteDownloadOpensOnCancel) {
    RemoteSource al = seedWeather();
    openDownload("Weather");
    open("Delete download");
    EXPECT_EQ(level().title, "Delete download");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Cancel", "Yes, delete"}));
    EXPECT_EQ(level().index, 0u);
    EXPECT_EQ(speech.last(), "Delete download. Cancel, 1 of 2");
    press(Button::Ok); // Cancel twice by accident does no harm
    EXPECT_EQ(level().title, "Weather");
    EXPECT_TRUE(fs::exists(store().listFolder(al) / "t1.mp3"));
    EXPECT_EQ(store().lists().size(), 1u);
}

TEST_F(DownloadedTest, HomeLeavesTheDeleteListWithoutDeleting) {
    RemoteSource al = seedWeather();
    openDownload("Weather");
    open("Delete download");
    press(Button::Forward);
    press(Button::Home);
    EXPECT_TRUE(fs::exists(store().listFolder(al) / "t1.mp3"));
}

TEST_F(DownloadedTest, ConfirmingDeletesTheFolderAndTheListUpdates) {
    RemoteSource al = seedWeather();
    seed(albumSource("al2"), "Zodiac", "Band", {{"z1", "Fish"}});
    openDownload("Weather");
    open("Delete download");
    press(Button::Forward);
    press(Button::Ok); // Yes, delete
    EXPECT_FALSE(fs::exists(store().listFolder(al)));
    EXPECT_TRUE(fs::exists(store().listFolder(albumSource("al2")) / "z1.mp3")) << "another list is not touched";
    EXPECT_EQ(level().title, "Downloaded");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Zodiac"}));
    EXPECT_EQ(speech.last(), "Deleted Weather. Zodiac, Band, 1 of 1");
    EXPECT_EQ(app->screens().size(), 3u) << "Home, Server, Downloaded";
}

TEST_F(DownloadedTest, DeletingTheLastListLeavesTheEmptyMessage) {
    seedWeather();
    openDownload("Weather");
    open("Delete download");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(labels(), (std::vector<std::string>{"No downloads"}));
    EXPECT_EQ(speech.last(), "Deleted Weather. No downloads, 1 of 1");
}

TEST_F(DownloadedTest, DeletingAPlaylistKeepsTheAlbumWithTheSameId) {
    seed(albumSource("7"), "Album seven", "Band", {{"a1", "One"}});
    RemoteSource pl = seed(playlistSource("7"), "Playlist seven", "", {{"p1", "Two"}});
    openDownload("Playlist seven");
    open("Delete download");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_FALSE(fs::exists(store().listFolder(pl)));
    EXPECT_EQ(labels(), (std::vector<std::string>{"Album seven"}));
}

TEST_F(DownloadedTest, DeletedTracksStopPlayingFromTheirFiles) {
    // The index of cached files is built while something plays; deleting must drop it.
    RemoteSource al = seedWeather();
    start();
    ASSERT_TRUE(app->playRemote("Other", {{"t1", "Storm Front", "Band", "Weather", 200, 1}}, 0)); // builds the index
    ASSERT_TRUE(app->playRemote("Free", {{"z9", "Zed", "Band", "Free", 200, 1}}, 0, std::nullopt));
    press(Button::Home); // out of the player
    open("Server");
    open("Downloaded");
    open("Weather");
    open("Delete download");
    press(Button::Forward);
    press(Button::Ok);
    ASSERT_FALSE(fs::exists(store().listFolder(al)));
    ASSERT_TRUE(app->playRemote("Next", {{"x1", "Ex", "Band", "Next", 100, 1}, {"t2", "Rain", "Band", "Weather", 180, 2}}, 0));
    player.pos = player.length - 5;
    player.preloads.clear();
    app->tick();
    ASSERT_EQ(player.preloads.size(), 1u) << "t2 is a stream again, so it is read ahead";
    EXPECT_NE(player.preloads.back().find("fake://"), std::string::npos);
}

TEST_F(DownloadedTest, TheListThatIsLoadedInThePlayerCannotBeDeleted) {
    RemoteSource al = seedWeather();
    openDownload("Weather");
    press(Button::Ok); // plays
    ASSERT_EQ(app->mode(), App::Mode::Player);
    press(Button::Home); // back to the list
    open("Delete download");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_TRUE(fs::exists(store().listFolder(al) / "t1.mp3"));
    EXPECT_EQ(speech.last(), "Cannot delete Weather while it is loaded in the player. Play something else first.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(level().title, "Delete download") << "stays where it was";
    EXPECT_TRUE(player.playing());
}

TEST_F(DownloadedTest, AnotherListCanBeDeletedWhileOneIsLoaded) {
    seedWeather();
    RemoteSource other = seed(albumSource("al2"), "Zodiac", "Band", {{"z1", "Fish"}});
    openDownload("Zodiac");
    press(Button::Ok); // plays Zodiac
    press(Button::Home);
    press(Button::Home);
    open("Weather");
    open("Delete download");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Zodiac"}));
    EXPECT_TRUE(fs::exists(store().listFolder(other) / "z1.mp3"));
    EXPECT_TRUE(player.playing());
}

TEST_F(DownloadedTest, DeletingTheListThatDownloadsCancelsTheDownload) {
    openAlbum();
    startDownload();
    finishLast(); // t1 is whole and listed, t2 is being fetched
    auto running = downloader.lastRequest().id;
    ASSERT_EQ(downloader.pendingCount(), 1u);
    press(Button::Home);
    press(Button::Home);
    open("Downloaded");
    open("Weather");
    open("Delete download");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_NE(std::find(downloader.cancelled.begin(), downloader.cancelled.end(), running), downloader.cancelled.end());
    EXPECT_FALSE(fs::exists(store().listFolder(albumSource("al1"))));
    EXPECT_EQ(labels(), (std::vector<std::string>{"No downloads"}));
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last().find("Downloading"), std::string::npos) << "no download runs any more";
    EXPECT_FALSE(downloader.finish(running, Result::Ok)) << "a late answer finds nothing";
    app->tick();
    EXPECT_FALSE(fs::exists(store().listFolder(albumSource("al1")))) << "nothing comes back";
}

TEST_F(DownloadedTest, DeletingAnotherListLeavesTheRunningDownloadAlone) {
    seed(albumSource("al2"), "Zodiac", "Band", {{"z1", "Fish"}});
    openAlbum();
    startDownload();
    press(Button::Home);
    press(Button::Home);
    open("Downloaded");
    open("Zodiac");
    open("Delete download");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_TRUE(downloader.cancelled.empty());
    EXPECT_EQ(downloader.pendingCount(), 1u);
    hold(Button::PlayPause);
    EXPECT_NE(speech.last().find("Downloading, 0 of 3 tracks"), std::string::npos);
}

TEST_F(DownloadedTest, AListWhoseFilesVanishedAfterTheMenuWasBuiltSaysSo) {
    RemoteSource al = seed(albumSource("al1"), "Weather", "Band", {{"t1", "One"}});
    openDownloaded();
    fs::remove(store().listFolder(al) / "t1.mp3");
    open("Weather");
    EXPECT_EQ(labels(), (std::vector<std::string>{"No tracks"}));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "The tracks of this download are gone from the disk.");
}

// ------------------------------------------------------------------ offline

// Step 3.6: with the server down, the downloads still play: the error list offers
// Downloaded, and a restart resumes the album from its files.
class OfflineTest : public DownloadedTest {
protected:
    void SetUp() override {
        DownloadedTest::SetUp();
        settings.setString("server.password", "secret");
    }
    // Exit, and a new App on what was written to state.tsv, as after a reboot.
    void reboot() {
        app->shutdown();
        app.reset();
        olderState(); // the stream resumes from the place the previous version kept
        state = Settings(&stateFile);
        state.load();
        player = FakePlayer{};
        speech.said.clear();
        sounds.played.clear();
        start();
    }
    // Plays "Rain" of the downloaded Weather, stopped at 42 s, then a restart.
    void playRainThenRestart() {
        openDownload("Weather");
        open("Rain");
        ASSERT_TRUE(player.playing());
        player.pos = 42;
        reboot();
    }
    // Marks every track of the list as not pinned, as the automatic cache leaves them.
    void unpin(const RemoteSource& source) {
        CachedList list = store().load(source);
        for (CachedTrack& t : list.tracks) t.pinned = false;
        ASSERT_TRUE(store().save(list));
    }
    bool spokeUnreachable() const {
        for (const std::string& s : speech.said)
            if (s.find("not reachable") != std::string::npos) return true;
        return false;
    }
};

TEST_F(OfflineTest, ALoadErrorOffersDownloadedAndItPlaysFromTheFiles) {
    RemoteSource al = seedWeather();
    openList("Recently added");
    ASSERT_TRUE(server.fail(server.lastRequest().id, FakeMusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not reachable", "Try again", "Downloaded"}));
    EXPECT_EQ(speech.last(), "Server not reachable, 1 of 3");
    press(Button::Forward);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Downloaded, 3 of 3");
    press(Button::Ok);
    open("Weather");
    speech.said.clear();
    open("Play album");
    EXPECT_EQ(player.file, fileOf(al, "t1"));
    EXPECT_TRUE(player.playing());
    EXPECT_FALSE(anyStreamOpened());
    EXPECT_FALSE(speech.heard("Loading."));
    EXPECT_FALSE(spokeUnreachable());
}

TEST_F(OfflineTest, ALoadErrorWithNothingDownloadedOffersOnlyTryAgain) {
    openList("Recently added");
    ASSERT_TRUE(server.fail(server.lastRequest().id, FakeMusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not reachable", "Try again"}));
}

TEST_F(OfflineTest, TryAgainDropsDownloadedAndTheListFillsWithoutIt) {
    seedWeather();
    openList("Recently added");
    ASSERT_TRUE(server.fail(server.lastRequest().id, FakeMusicServer::Error::Unreachable));
    app->tick();
    press(Button::Forward);
    press(Button::Ok); // Try again
    EXPECT_EQ(labels(), (std::vector<std::string>{"Loading"}));
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    EXPECT_EQ(labels(), (std::vector<std::string>{"Weather"}));
}

TEST_F(OfflineTest, AWholeDownloadResumesFromItsFilesAtOnceWithoutAskingTheServer) {
    RemoteSource al = seedWeather();
    playRainThenRestart();
    std::size_t requests = server.requests.size();
    press(Button::Ok); // Now playing
    EXPECT_EQ(server.requests.size(), requests) << "nothing is asked of the server";
    EXPECT_FALSE(speech.heard("Loading."));
    EXPECT_EQ(player.file, fileOf(al, "t2"));
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->mode(), App::Mode::Player);
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, fileOf(al, "t3")) << "the rest of the album follows from the disk";
    EXPECT_FALSE(anyStreamOpened());
    EXPECT_FALSE(spokeUnreachable());
    EXPECT_EQ(requestsOf(Kind::AlbumTracks), 0u);
}

TEST_F(OfflineTest, ADownloadWithAMissingFileAsksTheServerAndFallsBackToTheFilesWhenItIsDown) {
    RemoteSource al = seedWeather();
    playRainThenRestart();
    fs::remove(fileOf(al, "t3"));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Loading.");
    ASSERT_EQ(server.lastRequest().kind, Kind::AlbumTracks);
    ASSERT_TRUE(server.fail(server.lastRequest().id, FakeMusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(player.file, fileOf(al, "t2"));
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_FALSE(spokeUnreachable());
    EXPECT_EQ(app->mode(), App::Mode::Player);
}

TEST_F(OfflineTest, APartlyKeptAlbumIsLoadedFromTheServerFirstSoNoTrackIsLost) {
    RemoteSource al = seedWeather();
    playRainThenRestart();
    unpin(al);
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Loading.");
    ASSERT_EQ(server.lastRequest().kind, Kind::AlbumTracks);
    std::vector<RemoteTrack> longer = weather();
    longer.push_back({"t4", "Hail", "Band", "Weather", 100, 4, "mp3", 5}); // only the server has it
    answer(longer);
    EXPECT_EQ(player.file, fileOf(al, "t2")) << "a track on disk still plays from it";
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, fileOf(al, "t3"));
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t4") << "the server's whole list plays";
}

TEST_F(OfflineTest, APartlyKeptAlbumWithTheServerDownPlaysWhatIsOnDisk) {
    RemoteSource al = seedWeather();
    playRainThenRestart();
    unpin(al);
    press(Button::Ok);
    ASSERT_TRUE(server.fail(server.lastRequest().id, FakeMusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(player.file, fileOf(al, "t2"));
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_FALSE(spokeUnreachable());
    EXPECT_NE(sounds.last(), Earcon::Error);
}

TEST_F(OfflineTest, WhenTheSavedTrackIsNotOnDiskTheFirstOneOnDiskPlaysFromItsStart) {
    RemoteSource al = seedWeather();
    playRainThenRestart();
    unpin(al);
    CachedList list = store().load(al);
    list.tracks.erase(list.tracks.begin() + 1); // Rain is no longer in the cache
    ASSERT_TRUE(store().save(list));
    press(Button::Ok);
    ASSERT_TRUE(server.fail(server.lastRequest().id, FakeMusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(player.file, fileOf(al, "t1"));
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

TEST_F(OfflineTest, ResumeWithTheServerDownAndNothingOfThatListOnDiskStillSaysServerNotReachable) {
    RemoteSource al = seedWeather();
    playRainThenRestart();
    ASSERT_TRUE(store().remove(al));
    seed(albumSource("other"), "Zodiac", "Band", {{"z1", "Fish"}}); // downloads of another list do not count
    press(Button::Ok);
    ASSERT_TRUE(server.fail(server.lastRequest().id, FakeMusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(speech.last(), "Server not reachable.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_TRUE(player.opened.empty());
    EXPECT_EQ(app->mode(), App::Mode::Menu);
}

TEST_F(OfflineTest, LeavingHomeWhileTheFallbackWaitsForTheServerDropsIt) {
    RemoteSource al = seedWeather();
    playRainThenRestart();
    unpin(al);
    press(Button::Ok);
    auto id = server.lastRequest().id;
    press(Button::Forward); // another screen or item: the user has moved on
    press(Button::Ok);
    EXPECT_FALSE(server.fail(id, FakeMusicServer::Error::Unreachable)) << "the request was cancelled";
    app->tick();
    EXPECT_TRUE(player.opened.empty());
}

// ---------------------------------------------------------------- Music > Playlists (docs/fixes.md 2.5)

// The index's playlists and the downloaded ones in one local list, opened like a Streaming playlist.
class MusicPlaylistsTest : public OfflineTest {
protected:
    // Home > Music > Playlists
    void openPlaylists() {
        start();
        open("Music");
        open("Playlists");
    }
    // The newest request of `kind` the App made.
    MusicServer::RequestId lastOf(FakeMusicServer::Kind kind) const {
        for (auto it = server.requests.rbegin(); it != server.requests.rend(); ++it)
            if (it->kind == kind) return it->id;
        ADD_FAILURE() << "no such request";
        return 0;
    }
    // The start-up sync runs to its end: the server has no albums and these playlists.
    void syncWith(std::vector<RemotePlaylist> playlists) {
        ASSERT_TRUE(server.replyVersion(lastOf(Kind::CatalogueVersion), "v1"));
        app->tick();
        ASSERT_TRUE(server.reply(lastOf(Kind::Albums), std::vector<RemoteAlbum>{}));
        app->tick();
        ASSERT_TRUE(server.reply(lastOf(Kind::Playlists), std::move(playlists)));
        app->tick();
    }
    std::vector<std::string> values() const {
        std::vector<std::string> out;
        for (const MenuItem& item : level().items) out.push_back(item.value ? item.value() : "");
        return out;
    }
    RemoteSource seedRoad() { return seed(playlistSource("p1"), "Road Trip", "", {{"t1", "Sunrise"}, {"t2", "Highway"}}); }
};

TEST_F(MusicPlaylistsTest, ListsIndexedAndDownloadedPlaylistsSortedWithTheirTrackCount) {
    indexPlaylist("p1", "Road Trip", 12);
    indexPlaylist("p2", "chill", 3);
    seed(playlistSource("p3"), "Alpha", "", {{"a1", "One"}, {"a2", "Two"}});
    openPlaylists();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "chill", "Road Trip"}));
    EXPECT_EQ(values(), (std::vector<std::string>{"2 tracks", "3 tracks", "12 tracks"}));
    EXPECT_EQ(speech.last(), "Playlists. Alpha, 2 tracks, 1 of 3");
    press(Button::Forward);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Road Trip, 12 tracks, 3 of 3");
    EXPECT_EQ(listRequests(), 0u) << "a local menu: the server is not asked";
}

TEST_F(MusicPlaylistsTest, APlaylistInTheIndexAndDownloadedIsOneEntryNamedByTheIndex) {
    indexPlaylist("p1", "Road Trip", 12);
    seed(playlistSource("p1"), "Road Trip, as it was", "", {{"t1", "Sunrise"}});
    seed(albumSource("p5"), "An album downloaded", "Band", {{"a1", "One"}}); // not a playlist
    openPlaylists();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Road Trip"}));
    EXPECT_EQ(values(), (std::vector<std::string>{"12 tracks"}));
}

TEST_F(MusicPlaylistsTest, ThePlaylistsOfAnotherServerAreNotListed) {
    indexPlaylist("p1", "Mine", 2);
    indexStore.playlists.push_back({TrackCache::serverKey("https://other.example", "bob"), "p9", "Not mine", 4, 0});
    openPlaylists();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Mine"}));
}

TEST_F(MusicPlaylistsTest, ADownloadedPlaylistIsListedWithNoMusicAndNoSync) {
    fs::remove_all(music);
    seedRoad();
    start();
    open("Music");
    EXPECT_EQ(labels(), (std::vector<std::string>{"No music", "Playlists", "Refresh library"}));
    open("Playlists");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Road Trip"}));
    EXPECT_EQ(speech.last(), "Playlists. Road Trip, 2 tracks, 1 of 1");
}

TEST_F(MusicPlaylistsTest, ADownloadedPlaylistIsListedWhileTheFirstScanRuns) {
    seedRoad();
    start(/*scan=*/false);
    open("Music");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Scanning", "Playlists", "Refresh library"}));
    open("Playlists");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Road Trip"}));
}

TEST_F(MusicPlaylistsTest, ADownloadedPlaylistDeletedOnTheServerIsStillListedAfterASync) {
    indexPlaylist("p1", "Road Trip", 2);
    indexPlaylist("p2", "Gone and not downloaded", 5);
    seedRoad();
    start();
    syncWith({}); // the server has neither
    open("Music");
    open("Playlists");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Road Trip"}));
    EXPECT_EQ(values(), (std::vector<std::string>{"2 tracks"}));
}

TEST_F(MusicPlaylistsTest, ASyncAddsTheServersPlaylistsToTheList) {
    start();
    syncWith({{"p1", "New one", 7}});
    open("Music");
    open("Playlists");
    EXPECT_EQ(labels(), (std::vector<std::string>{"New one"}));
    EXPECT_EQ(values(), (std::vector<std::string>{"7 tracks"}));
}

TEST_F(MusicPlaylistsTest, WithTheServerDownADownloadedPlaylistPlaysFromItsFilesWithoutLoading) {
    RemoteSource pl = seedRoad();
    indexPlaylist("p1", "Road Trip", 2);
    openPlaylists();
    speech.said.clear();
    const size_t asked = server.requests.size();
    open("Road Trip");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play playlist", "Delete download", "Sunrise", "Highway", "Keep offline"}));
    EXPECT_FALSE(speech.heard("Loading."));
    open("Highway");
    EXPECT_EQ(player.file, fileOf(pl, "t2"));
    EXPECT_FALSE(anyStreamOpened());
    EXPECT_EQ(server.requests.size(), asked) << "the server is never asked";
    EXPECT_FALSE(spokeUnreachable());
}

TEST_F(MusicPlaylistsTest, WithTheServerDownThePlayNowOfADownloadedPlaylistPlaysFromItsFilesWithoutLoading) {
    RemoteSource pl = seedRoad();
    indexPlaylist("p1", "Road Trip", 2);
    openPlaylists();
    speech.said.clear();
    const size_t asked = server.requests.size();
    press(Button::Options);
    open("Play now");
    EXPECT_FALSE(speech.heard("Loading."));
    EXPECT_EQ(server.requests.size(), asked) << "the server is never asked";
    EXPECT_EQ(player.file, fileOf(pl, "t1"));
    EXPECT_FALSE(anyStreamOpened());
    EXPECT_TRUE(speech.heard("Playing Road Trip."));
}

TEST_F(MusicPlaylistsTest, WithTheServerDownAPlaylistThatIsNotDownloadedOffersTryAgainAndDownloaded) {
    seedRoad();
    indexPlaylist("p2", "Chill", 3);
    openPlaylists();
    open("Chill");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Loading"}));
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
    EXPECT_EQ(server.lastRequest().arg, "p2");
    ASSERT_TRUE(server.fail(server.lastRequest().id, FakeMusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not reachable", "Try again", "Downloaded"}));
    EXPECT_EQ(speech.last(), "Server not reachable, 1 of 3");
    press(Button::Forward);
    press(Button::Ok); // Try again
    EXPECT_EQ(labels(), (std::vector<std::string>{"Loading"}));
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
}

TEST_F(MusicPlaylistsTest, APlaylistOnlyPartlyOnTheDiskLoadsFromTheServer) {
    seed(playlistSource("p1"), "Road Trip", "", {{"t1", "Sunrise"}, {"t2", "Highway", false}});
    openPlaylists();
    open("Road Trip");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Loading"}));
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
}

TEST_F(MusicPlaylistsTest, AnEmptyListSaysNoPlaylistsAndOkSaysWhatToDo) {
    openPlaylists();
    EXPECT_EQ(labels(), (std::vector<std::string>{"No playlists"}));
    EXPECT_EQ(speech.last(), "Playlists. No playlists, 1 of 1");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Make a playlist on the server, or save the queue as a playlist.");
}

TEST_F(MusicPlaylistsTest, BackAndForwardVisitEveryPlaylist) {
    indexPlaylist("p1", "Alpha", 1);
    indexPlaylist("p2", "Beta", 1);
    indexPlaylist("p3", "Bravo", 1);
    indexPlaylist("p4", "Delta", 1);
    openPlaylists();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Beta, 1 track, 2 of 4");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Bravo, 1 track, 3 of 4");
    press(Button::Back);
    EXPECT_EQ(speech.last(), "Beta, 1 track, 2 of 4");
}

TEST_F(MusicPlaylistsTest, StreamingHasNoPlaylistsButDownloadedStillListsTheDownloadedOnes) {
    seedRoad();
    openServer();
    EXPECT_FALSE(hasLabel("Playlists"));
    open("Downloaded");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Road Trip"}));
    EXPECT_EQ(values(), (std::vector<std::string>{"playlist"}));
}

// ------------------------------------------------------- automatic cache (step 3.7)

// A streamed track that has been heard for a while is fetched into the cache,
// unpinned, silently. Above the limit the least recently played unpinned tracks go.
class AutoCacheTest : public OfflineTest {
protected:
    void SetUp() override {
        OfflineTest::SetUp();
        clock.setWallTime(kNow);
    }
    static constexpr std::time_t kNow = 1'800'000'000;
    // Weather plays from the server, from track `index`, with its sizes and file types.
    void playSized(size_t index = 0) {
        start();
        ASSERT_TRUE(app->playRemote("Weather", sized(), index, albumSource("al1"), 0, "Band"));
        ASSERT_TRUE(player.playing());
    }
    // Ticks through `ms` of playback, a second at a time, as the device's loop would.
    void listen(std::chrono::milliseconds ms) {
        for (; ms >= 1s; ms -= 1s) idle(1s);
        if (ms > 0ms) idle(ms);
    }
    // A track of `bytes` bytes of list `source` in the cache, as an earlier session left it.
    void cacheTrack(const std::string& id, bool pinned, std::int64_t played, size_t bytes = 5,
                    RemoteSource source = albumSource("al1"), const std::string& name = "Weather") {
        CachedList list = store().load(source);
        if (list.source.id.empty()) list = {source, name, "Band", {}};
        list.tracks.push_back({id, id + ".mp3", id, 100, "", pinned, played});
        fs::create_directories(store().listFolder(source));
        std::ofstream(store().listFolder(source) / (id + ".mp3"), std::ios::binary) << std::string(bytes, 'x');
        ASSERT_TRUE(store().save(list));
    }
    std::string weatherFolderFile(const std::string& name) const {
        return (store().listFolder(albumSource("al1")) / name).string();
    }
    bool cached(const std::string& id) const { return store().find(id).has_value(); }
};

class CacheUpgradeTest : public AutoCacheTest {
protected:
    void seedLower(bool pinned = false, int bitrate = 128) {
        CachedList list{albumSource("al1"), "Weather", "Band", {}};
        for (const auto& track : sized()) {
            auto name = track.id + "~" + std::to_string(bitrate) + ".mp3";
            touch(store().listFolder(list.source) / name);
            list.tracks.push_back({track.id, name, track.title, track.duration, "", pinned, 123, bitrate});
        }
        ASSERT_TRUE(store().save(list));
    }
    std::string lower(const std::string& id = "t1") const { return weatherFolderFile(id + "~128.mp3"); }
    void begin(double at = 0) {
        start();
        ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), at, "Band"));
    }
    void enable() { settings.setInt("cache.upgrade", 1); }
};

TEST_F(CacheUpgradeTest, ReplaceCachedCopiesDefaultsOffAndPersistsFromItsList) {
    start();
    open("Settings");
    open("Replace cached copies");
    EXPECT_EQ(speech.last(), "Replace cached copies. off, current, 1 of 2");
    EXPECT_EQ(labels(), (std::vector<std::string>{"off", "on"}));
    open("on");
    EXPECT_EQ(level().title, "Settings");
    EXPECT_EQ(configFile.onDisk("cache.upgrade"), "1");
    Settings reloaded{&configFile};
    reloaded.load();
    EXPECT_EQ(reloaded.getInt("cache.upgrade", 0), 1);
}

TEST_F(CacheUpgradeTest, DisabledOrSufficientCopiesPlayWithoutAStreamOrTransfer) {
    seedLower();
    begin();
    EXPECT_EQ(player.file, lower());
    EXPECT_FALSE(anyStreamOpened());
    listen(11s);
    EXPECT_TRUE(downloader.requests.empty());
    enable();
    settings.setInt("streaming.bitrate", 128);
    press(Button::Forward);
    EXPECT_EQ(player.file, lower("t2"));
    listen(11s);
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(CacheUpgradeTest, ABetterStreamReplacesThePinnedCopyAndUpdatesItsQualityIndex) {
    seedLower(true);
    enable();
    begin();
    EXPECT_EQ(player.file, "fake://stream/t1");
    listen(9s);
    EXPECT_TRUE(downloader.requests.empty());
    listen(2s);
    ASSERT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(downloader.lastRequest().bitrate, 0);
    EXPECT_TRUE(fs::exists(lower()));
    finishLast();
    auto list = store().load(albumSource("al1"));
    EXPECT_EQ(list.tracks[0].bitrate, 0);
    EXPECT_TRUE(list.tracks[0].pinned);
    EXPECT_FALSE(fs::exists(lower()));
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    EXPECT_EQ(player.file, weatherFolderFile("t1.flac"));
}

TEST_F(CacheUpgradeTest, ATranscodedUpgradeKeepsItsOpenedQualityAndFailureKeepsTheCopy) {
    seedLower();
    enable();
    settings.setInt("streaming.bitrate", 320);
    begin();
    ASSERT_TRUE(player.transcoded.has_value());
    settings.setInt("streaming.bitrate", 128);
    listen(11s);
    ASSERT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(downloader.lastRequest().bitrate, 320);
    finishLast(Result::Unreachable);
    EXPECT_TRUE(fs::exists(lower()));
    EXPECT_EQ(store().load(albumSource("al1")).tracks[0].bitrate, 128);
    settings.setInt("streaming.bitrate", 320);
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    ASSERT_EQ(downloader.requests.size(), 2u);
    finishLast();
    const auto list = store().load(albumSource("al1"));
    EXPECT_EQ(list.tracks[0].bitrate, 320);
    EXPECT_FALSE(list.tracks[0].pinned);
    EXPECT_FALSE(fs::exists(lower()));
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    EXPECT_EQ(player.file, weatherFolderFile("t1~320.mp3"));
}

TEST_F(CacheUpgradeTest, ImmediateFailureFallsBackAtTheSamePositionAndMarksTheServerDown) {
    seedLower();
    enable();
    player.unopenable.push_back("fake://stream/t1");
    begin(42);
    EXPECT_EQ(player.file, lower());
    EXPECT_DOUBLE_EQ(player.position(), 42);
    EXPECT_TRUE(player.playing());
    EXPECT_FALSE(spokeUnreachable());
    listen(11s);
    EXPECT_TRUE(downloader.requests.empty());
    press(Button::Forward);
    EXPECT_EQ(player.file, lower("t2"));
    EXPECT_EQ(player.opened.size(), 3u) << "marked-down server uses the next copy directly";
}

TEST_F(CacheUpgradeTest, AsyncFailureFallsBackAtTheSamePositionAndHonorsPauseIntent) {
    seedLower();
    enable();
    player.holdStreams = true;
    begin(42);
    ASSERT_TRUE(player.opening());
    press(Button::PlayPause);
    player.unopenable.push_back("fake://stream/t1");
    ASSERT_TRUE(player.finishOpen());
    app->tick();
    EXPECT_EQ(player.file, lower());
    EXPECT_DOUBLE_EQ(player.position(), 42);
    EXPECT_FALSE(player.playing());
    EXPECT_FALSE(spokeUnreachable());
    idle(10s); // still paused: recover before any fallback audio has been heard
    answerPing();
    press(Button::PlayPause);
    EXPECT_TRUE(player.playing());
    listen(11s); // server recovery must not turn local fallback playback into an upgrade
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(CacheUpgradeTest, TimeoutFallsBackAndCancelledUpgradeNeverPlaysALateCopy) {
    seedLower();
    enable();
    player.holdStreams = true;
    begin(37);
    idle(16s);
    EXPECT_EQ(player.file, lower());
    EXPECT_DOUBLE_EQ(player.position(), 37);
    EXPECT_TRUE(player.playing());
    EXPECT_FALSE(player.finishOpen());
    EXPECT_FALSE(spokeUnreachable());
    // A successful fresh stream clears the down mark; cancel a later pending upgrade by sleeping.
    player.holdStreams = false;
    ASSERT_TRUE(app->playRemote("Fresh", {{"fresh", "Fresh", "Band", "Fresh", 200}}, 0));
    player.holdStreams = true;
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 31, "Band"));
    ASSERT_TRUE(player.opening());
    press(Button::Power);
    press(Button::Ok);
    EXPECT_FALSE(player.finishOpen());
    app->tick();
    EXPECT_FALSE(player.playing());
    EXPECT_NE(player.file, lower()) << "cancelled open must not trigger fallback";
}

TEST_F(CacheUpgradeTest, ReadAheadPreloadsALowerQualityCopyOnlyWhenUpgradesAreEnabled) {
    seedLower();
    enable();
    begin();
    player.pos = player.duration() - 5;
    app->tick();
    ASSERT_FALSE(player.preloads.empty());
    EXPECT_EQ(player.preloads.back(), "fake://stream/t2");
    settings.setInt("cache.upgrade", 0);
    app->tick();
    EXPECT_FALSE(player.preloaded.has_value());
    press(Button::Forward);
    EXPECT_EQ(player.file, lower("t2"));
}

TEST_F(AutoCacheTest, AStreamedTrackIsFetchedUnpinnedIntoItsListFolderOnceHeardLongEnough) {
    playSized();
    listen(9s);
    EXPECT_TRUE(downloader.requests.empty()) << "a track that is skipped costs no bandwidth";
    listen(2s);
    ASSERT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(downloader.lastRequest().trackId, "t1");
    EXPECT_EQ(downloader.lastRequest().dest, weatherFolderFile("t1.flac"));
    listen(30s);
    EXPECT_EQ(downloader.requests.size(), 1u) << "once per play";
    size_t said = speech.said.size(), played = sounds.played.size();
    finishLast();
    ASSERT_EQ(store().find("t1"), store().listFolder(albumSource("al1")) / "t1.flac");
    CachedList list = store().load(albumSource("al1"));
    EXPECT_EQ(list.name, "Weather");
    EXPECT_EQ(list.artist, "Band");
    ASSERT_EQ(list.tracks.size(), 1u);
    EXPECT_EQ(list.tracks[0].title, "Storm Front");
    EXPECT_DOUBLE_EQ(list.tracks[0].duration, 200);
    EXPECT_FALSE(list.tracks[0].pinned);
    EXPECT_EQ(list.tracks[0].lastPlayed, kNow);
    EXPECT_EQ(speech.said.size(), said) << "silent";
    EXPECT_EQ(sounds.played.size(), played) << "no earcon";
    expectNoUrlSpoken(speech);
}

TEST_F(AutoCacheTest, TracksAreFetchedOneAtATimeInTheOrderTheyWereHeard) {
    playSized();
    listen(11s);
    press(Button::Forward); // Rain
    listen(11s);
    ASSERT_EQ(downloader.requests.size(), 1u) << "Rain waits for Storm Front";
    finishLast();
    ASSERT_EQ(downloader.requests.size(), 2u);
    EXPECT_EQ(downloader.lastRequest().trackId, "t2");
    EXPECT_EQ(downloader.lastRequest().dest, weatherFolderFile("t2.mp3"));
    finishLast();
    EXPECT_TRUE(cached("t1"));
    EXPECT_TRUE(cached("t2"));
}

TEST_F(AutoCacheTest, TranscodedJobsKeepTheOpenedQualityWhileSettingsAndTracksChange) {
    settings.setInt("streaming.bitrate", 128);
    settings.setInt("downloads.bitrate", 320);
    playSized();
    EXPECT_TRUE(player.transcoded.has_value());
    settings.setInt("streaming.bitrate", 192); // applies to the next track
    listen(11s);
    ASSERT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(downloader.lastRequest().bitrate, 128);
    EXPECT_EQ(downloader.lastRequest().dest, weatherFolderFile("t1~128.mp3"));
    press(Button::Forward);
    listen(11s); // the 192 job waits behind the 128 job
    settings.setInt("streaming.bitrate", 320);
    finishLast();
    auto first = store().load(albumSource("al1"));
    ASSERT_EQ(first.tracks.size(), 1u);
    EXPECT_EQ(first.tracks[0].bitrate, 128);
    EXPECT_FALSE(first.tracks[0].pinned);
    EXPECT_EQ(store().find("t1"), fs::path(weatherFolderFile("t1~128.mp3")));
    ASSERT_EQ(downloader.requests.size(), 2u);
    EXPECT_EQ(downloader.lastRequest().bitrate, 192);
    EXPECT_EQ(downloader.lastRequest().dest, weatherFolderFile("t2~192.mp3"));
    finishLast();
    auto list = store().load(albumSource("al1"));
    ASSERT_EQ(list.tracks.size(), 2u);
    EXPECT_EQ(list.tracks[1].bitrate, 192);
    EXPECT_EQ(store().find("t2"), fs::path(weatherFolderFile("t2~192.mp3")));
}

TEST_F(AutoCacheTest, AnOriginalStreamStaysOriginalWhenTheSettingChanges) {
    settings.setInt("streaming.bitrate", 0);
    settings.setInt("downloads.bitrate", 128);
    playSized();
    EXPECT_FALSE(player.transcoded.has_value());
    settings.setInt("streaming.bitrate", 128);
    listen(11s);
    ASSERT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(downloader.lastRequest().bitrate, 0);
    EXPECT_EQ(downloader.lastRequest().dest, weatherFolderFile("t1.flac"));
    finishLast();
    auto list = store().load(albumSource("al1"));
    ASSERT_EQ(list.tracks.size(), 1u);
    EXPECT_EQ(list.tracks[0].bitrate, 0);
    EXPECT_EQ(store().find("t1"), fs::path(weatherFolderFile("t1.flac")));
}

TEST_F(AutoCacheTest, ASkippedTrackIsNotFetched) {
    playSized();
    listen(5s);
    press(Button::Forward);
    listen(5s);
    press(Button::Forward);
    listen(5s);
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(AutoCacheTest, PausedTimeDoesNotCountAsHeard) {
    playSized();
    listen(5s);
    press(Button::PlayPause);
    listen(60s);
    EXPECT_TRUE(downloader.requests.empty());
    press(Button::PlayPause);
    listen(8s);
    EXPECT_EQ(downloader.requests.size(), 1u);
}

TEST_F(AutoCacheTest, WithTheLimitAtZeroNothingIsFetched) {
    settings.setInt("cache.limit_gb", 0);
    playSized();
    listen(60s);
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(AutoCacheTest, WithTheLimitAtZeroAnExplicitDownloadStillWorks) {
    settings.setInt("cache.limit_gb", 0);
    start();
    openAlbum();
    startDownload();
    EXPECT_EQ(downloader.lastRequest().trackId, "t1");
    finishLast();
    ASSERT_TRUE(cached("t1"));
    EXPECT_TRUE(store().load(albumSource("al1")).tracks[0].pinned);
}

TEST_F(AutoCacheTest, SwitchingTheCacheOffDropsTracksStillWaiting) {
    playSized();
    listen(11s);
    press(Button::Forward);
    ASSERT_EQ(player.file, "fake://stream/t2");
    listen(11s); // Rain waits behind Storm Front
    settings.setInt("cache.limit_gb", 0);
    finishLast();
    listen(1s);
    EXPECT_EQ(downloader.requests.size(), 1u);
}

TEST_F(AutoCacheTest, ATrackAlreadyOnDiskIsNotFetchedAgain) {
    cacheTrack("t1", false, 100);
    playSized();
    ASSERT_EQ(player.file, weatherFolderFile("t1.mp3"));
    listen(60s);
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(AutoCacheTest, WithoutAWorkingDownloaderNothingIsAsked) {
    downloader.isConfigured = false;
    playSized();
    listen(60s);
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(AutoCacheTest, AFailedFetchIsSilentAndNotTriedAgainDuringThatPlay) {
    playSized();
    listen(11s);
    size_t said = speech.said.size(), played = sounds.played.size();
    finishLast(Result::Unreachable, false);
    listen(60s);
    EXPECT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(speech.said.size(), said);
    EXPECT_EQ(sounds.played.size(), played);
    EXPECT_FALSE(cached("t1"));
    EXPECT_TRUE(store().load(albumSource("al1")).tracks.empty());
    EXPECT_TRUE(player.playing()) << "the music goes on";
}

TEST_F(AutoCacheTest, TheListedSizeGoesToTheDownloaderAndItsOkIsTrusted) {
    playSized();
    listen(11s);
    EXPECT_EQ(downloader.lastRequest().expectedSize, 5);
    ASSERT_TRUE(downloader.finish(downloader.lastRequest().id, Result::Ok, true, "short!!"));
    app->tick();
    EXPECT_TRUE(cached("t1"));
}

TEST_F(AutoCacheTest, TheSizeAskedForIsThatOfTheTrackThatPlayedNotTheFirstOfTheList) {
    start();
    std::vector<RemoteTrack> tracks = sized();
    tracks[1].size = 7;
    ASSERT_TRUE(app->playRemote("Weather", tracks, 1, albumSource("al1"), 0, "Band"));
    listen(11s);
    ASSERT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(downloader.lastRequest().trackId, "t2");
    EXPECT_EQ(downloader.lastRequest().expectedSize, 7);
}

TEST_F(AutoCacheTest, TheTrackPlaysFromItsFileTheNextTimeAndIsNotStreamed) {
    playSized();
    listen(11s);
    finishLast();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    EXPECT_EQ(player.file, weatherFolderFile("t1.flac"));
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t1"), 1) << "only the first play";
}

TEST_F(AutoCacheTest, APinnedTrackWhoseFileWentAwayStaysPinnedWhenItIsFetchedAgain) {
    cacheTrack("t1", true, 0);
    fs::remove(weatherFolderFile("t1.mp3"));
    playSized();
    listen(11s);
    finishLast();
    ASSERT_EQ(store().load(albumSource("al1")).tracks.size(), 1u);
    EXPECT_TRUE(store().load(albumSource("al1")).tracks[0].pinned);
    EXPECT_TRUE(cached("t1"));
}

TEST_F(AutoCacheTest, AListTheUserDownloadedKeepsItsNameAndPinnedTracks) {
    cacheTrack("t2", true, 0);
    CachedList list = store().load(albumSource("al1"));
    list.name = "My name";
    ASSERT_TRUE(store().save(list));
    playSized();
    listen(11s);
    finishLast();
    list = store().load(albumSource("al1"));
    EXPECT_EQ(list.name, "My name");
    ASSERT_EQ(list.tracks.size(), 2u);
    EXPECT_TRUE(list.tracks[0].pinned) << "t2 stays pinned";
    EXPECT_FALSE(list.tracks[1].pinned);
}

// ---- priority of an explicit download

class AutoCachePriorityTest : public AutoCacheTest {
protected:
    // Weather plays from the server and Storm Front is being fetched; the other album
    // "Other" is open in Streaming with its Download item ready.
    void fetchingWhileOtherAlbumIsOpen() {
        start();
        ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
        listen(11s);
        ASSERT_EQ(downloader.requests.size(), 1u);
        auto_ = downloader.lastRequest().id;
        press(Button::Home);
        open("Server");
        open("Recently added");
        answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}, {"al2", "Other", "Duo", 2}});
        open("Other");
        answer(std::vector<RemoteTrack>{{"o1", "One", "Duo", "Other", 100, 1, "mp3", 5}, {"o2", "Two", "Duo", "Other", 100, 2, "mp3", 5}});
    }
    Downloader::RequestId auto_ = 0;
};

TEST_F(AutoCachePriorityTest, AnExplicitDownloadCancelsTheTransferAndGoesFirst) {
    fetchingWhileOtherAlbumIsOpen();
    startDownload();
    EXPECT_NE(std::find(downloader.cancelled.begin(), downloader.cancelled.end(), auto_), downloader.cancelled.end());
    ASSERT_EQ(downloader.requests.size(), 2u);
    EXPECT_EQ(downloader.lastRequest().trackId, "o1");
    EXPECT_EQ(speech.last(), "Downloading, 0 of 2 tracks.") << "the automatic fetch is not counted";
    EXPECT_FALSE(downloader.finish(auto_, Result::Ok)) << "its answer is dropped";
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Downloading, 0 of 2 tracks.");
}

TEST_F(AutoCachePriorityTest, NothingAutomaticStartsWhileTheDownloadRuns) {
    fetchingWhileOtherAlbumIsOpen();
    startDownload();
    app->playRemote("Weather", sized(), 1, albumSource("al1"), 0, "Band");
    listen(30s); // Rain is heard, and waits
    EXPECT_EQ(downloader.requests.size(), 2u);
    finishLast();
    EXPECT_EQ(downloader.requests.size(), 3u);
    EXPECT_EQ(downloader.lastRequest().trackId, "o2");
}

TEST_F(AutoCachePriorityTest, TheTransferContinuesAfterTheDownloadAndTheDownloadCountsOnlyItsOwnTracks) {
    fetchingWhileOtherAlbumIsOpen();
    startDownload();
    finishLast();
    finishLast();
    EXPECT_EQ(sounds.last(), Earcon::Open) << "the download ended over music";
    ASSERT_EQ(downloader.requests.size(), 4u);
    EXPECT_EQ(downloader.lastRequest().trackId, "t1") << "Storm Front is fetched again from where it was";
    EXPECT_EQ(downloader.lastRequest().dest, weatherFolderFile("t1.flac"));
    size_t said = speech.said.size();
    finishLast();
    EXPECT_TRUE(cached("t1"));
    EXPECT_FALSE(store().load(albumSource("al1")).tracks[0].pinned);
    EXPECT_TRUE(store().load(albumSource("al2")).tracks[0].pinned);
    EXPECT_EQ(speech.said.size(), said);
}

TEST_F(AutoCachePriorityTest, ATrackTheDownloadFetchedMeanwhileIsNotFetchedAgain) {
    start();
    openAlbum();
    open("Play album");
    listen(11s);
    ASSERT_EQ(downloader.requests.size(), 1u);
    press(Button::Home);
    startDownload(); // the same album: Storm Front comes with it
    for (int i = 0; i < 3; ++i) finishLast();
    EXPECT_EQ(trackIdsRequested(), (std::vector<std::string>{"t1", "t1", "t2", "t3"}));
    listen(5s);
    EXPECT_EQ(downloader.requests.size(), 4u);
}

TEST_F(AutoCachePriorityTest, DeletingAListDropsItsTransfer) {
    cacheTrack("t3", true, 0);
    start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    auto id = downloader.lastRequest().id;
    press(Button::Forward);
    ASSERT_EQ(player.file, "fake://stream/t2");
    listen(11s); // Rain waits behind Storm Front
    ASSERT_TRUE(app->playRemote("Other", {{"o1", "One", "Duo", "Other", 100, 1, "mp3", 5}}, 0, albumSource("al2")));
    press(Button::Home);
    open("Server");
    open("Downloaded");
    open("Weather");
    open("Delete download");
    open("Yes, delete");
    EXPECT_NE(std::find(downloader.cancelled.begin(), downloader.cancelled.end(), id), downloader.cancelled.end());
    EXPECT_FALSE(fs::exists(store().listFolder(albumSource("al1"))));
    listen(5s);
    EXPECT_EQ(downloader.requests.size(), 1u) << "Rain is not fetched either";
}

// ---- the limit

class AutoCacheLimitTest : public AutoCacheTest {
protected:
    void SetUp() override {
        AutoCacheTest::SetUp();
    }
    // Starts the app with one "gigabyte" of `bytes` bytes.
    void startSmall(std::uintmax_t bytes = 10) {
        start();
        app->setCacheGigabyte(bytes);
    }
};

TEST_F(AutoCacheLimitTest, OverTheLimitTheLeastRecentlyPlayedUnpinnedTrackGoesFirst) {
    cacheTrack("a", false, 100);
    cacheTrack("b", false, 50);
    cacheTrack("c", false, 200);
    startSmall(15); // with the new one 20 bytes
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    press(Button::Forward);
    ASSERT_EQ(player.file, "fake://stream/t2"); // Storm Front is not the open one any more
    finishLast();
    EXPECT_FALSE(cached("b")) << "the oldest play goes";
    EXPECT_TRUE(cached("a"));
    EXPECT_TRUE(cached("c"));
    EXPECT_TRUE(cached("t1")) << "the track just cached played last";
    EXPECT_FALSE(fs::exists(weatherFolderFile("b.mp3")));
}

TEST_F(AutoCacheLimitTest, PinnedTracksNeverGoAndDoNotCount) {
    cacheTrack("pin", true, 1, 500);
    cacheTrack("a", false, 100);
    cacheTrack("b", false, 200);
    startSmall();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    finishLast();
    EXPECT_TRUE(cached("pin"));
    EXPECT_FALSE(cached("a")) << "15 bytes of unpinned tracks, limit 10";
    EXPECT_TRUE(cached("b"));
    EXPECT_TRUE(cached("t1"));
}

TEST_F(AutoCacheLimitTest, AnEvictedTrackIsStreamedAgainAndTheNewOneIsPlayedFromItsFile) {
    cacheTrack("a", false, 100);
    cacheTrack("b", false, 200);
    startSmall();
    std::vector<RemoteTrack> tracks = sized(); // Storm Front, Rain, Sunshine
    tracks.insert(tracks.begin() + 2, {"a", "A", "Band", "Weather", 100, 4, "mp3", 5});
    ASSERT_TRUE(app->playRemote("Weather", tracks, 1, albumSource("al1"), 0, "Band")); // Rain streams
    listen(11s);
    finishLast(); // 15 bytes: "a" goes
    ASSERT_FALSE(cached("a"));
    player.pos = player.length - 5;
    app->tick();
    EXPECT_EQ(player.preloaded, "fake://stream/a") << "the index forgot the file: the next track is pre-opened";
    ASSERT_TRUE(app->playRemote("Weather", tracks, 2, albumSource("al1"), 0, "Band"));
    EXPECT_EQ(player.file, "fake://stream/a");
    ASSERT_TRUE(app->playRemote("Weather", tracks, 1, albumSource("al1"), 0, "Band"));
    EXPECT_EQ(player.file, weatherFolderFile("t2.mp3"));
}

TEST_F(AutoCacheLimitTest, TheFileThePlayerHasOpenIsKeptWhateverTheLimit) {
    cacheTrack("a", false, 100);
    cacheTrack("b", false, 200);
    startSmall(1);
    ASSERT_TRUE(app->playRemote("Weather", {{"a", "A", "Band", "Weather", 100, 1}, {"b", "B", "Band", "Weather", 100, 2}}, 0, albumSource("al1")));
    ASSERT_EQ(player.file, weatherFolderFile("a.mp3"));
    app->shutdown();
    EXPECT_TRUE(cached("a")) << "it is open";
    EXPECT_FALSE(cached("b"));
}

TEST_F(AutoCacheLimitTest, TheLimitIsAppliedOnExitWhenItWasLowered) {
    cacheTrack("a", false, 100);
    cacheTrack("pin", true, 100);
    startSmall(1);
    EXPECT_TRUE(cached("a")) << "nothing is deleted while the user steps through the values";
    app->shutdown();
    EXPECT_FALSE(cached("a"));
    EXPECT_TRUE(cached("pin"));
    EXPECT_FALSE(fs::exists(weatherFolderFile("a.mp3")));
}

TEST_F(AutoCacheLimitTest, TheLimitIsAppliedOnSleepToo) {
    cacheTrack("a", false, 100);
    startSmall(1);
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_FALSE(cached("a"));
}

// ---- lastPlayed

TEST_F(AutoCacheLimitTest, APlayOfACachedTrackIsWrittenToItsManifestOnExitNotBefore) {
    cacheTrack("t1", false, 100);
    cacheTrack("t2", false, 100);
    start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 1, albumSource("al1"), 0, "Band"));
    listen(30s);
    clock.setWallTime(kNow + 500);
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    EXPECT_EQ(store().load(albumSource("al1")).tracks[0].lastPlayed, 100) << "no flash write at every play";
    app->shutdown();
    CachedList list = store().load(albumSource("al1"));
    EXPECT_EQ(list.tracks[0].lastPlayed, kNow + 500) << "t1";
    EXPECT_EQ(list.tracks[1].lastPlayed, kNow) << "t2";
}

TEST_F(AutoCacheLimitTest, APlayedCachedTrackOutlivesOneThatWasNotWhenTheLimitBites) {
    cacheTrack("a", false, 100);
    cacheTrack("b", false, 200);
    startSmall();
    ASSERT_TRUE(app->playRemote("Weather", {{"a", "A", "Band", "Weather", 100, 1}}, 0, albumSource("al1")));
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"))); // a is not open any more
    app->setCacheGigabyte(5);                                                 // room for one of them
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_FALSE(cached("b")) << "a was played after b";
    EXPECT_TRUE(cached("a"));
}

// ---- review fixes for 3.7

namespace {
// Counts the manifests read: a scan of the whole cache shows as one read per list.
class CountingFiles : public TextFiles {
public:
    std::optional<std::string> read(const fs::path& path) override {
        reads++;
        return diskFiles().read(path);
    }
    bool writeAtomically(const fs::path& path, const std::string& text) override { return diskFiles().writeAtomically(path, text); }
    int reads = 0;
};
constexpr std::uintmax_t kReserve = std::uintmax_t(256) << 20; // App's kAutoCacheReserve
} // namespace

TEST_F(AutoCacheLimitTest, ATrackThatWouldLeaveLessThanTheReserveFreeIsNotFetched) {
    std::uintmax_t room = kReserve + 5 - 1;
    start();
    app->setFreeSpace([&](const fs::path&) -> std::optional<std::uintmax_t> { return room; });
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    size_t said = speech.said.size(), played = sounds.played.size();
    listen(11s);
    EXPECT_TRUE(downloader.requests.empty()) << "5 bytes would leave 1 byte under the reserve";
    EXPECT_EQ(speech.said.size(), said) << "skipped silently";
    EXPECT_EQ(sounds.played.size(), played);
    room = kReserve + 5; // exactly enough
    press(Button::Forward);
    listen(11s);
    ASSERT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(downloader.lastRequest().trackId, "t2") << "the skipped track is not tried again";
}

TEST_F(AutoCacheLimitTest, AnAbandonedPartFileCountsTowardTheLimitAndGoesBeforeAnyTrack) {
    cacheTrack("a", false, kNow);
    std::string part = weatherFolderFile("gone.flac.part");
    std::ofstream(part, std::ios::binary) << std::string(10, 'p');
    startSmall(12); // a (5) + the part (10) are over
    app->shutdown();
    EXPECT_FALSE(fs::exists(part));
    EXPECT_TRUE(cached("a"));
}

TEST_F(AutoCacheLimitTest, ThePartFileOfARunningOrWaitingTransferIsNeverTouched) {
    startSmall(5);
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    press(Button::Forward);
    listen(11s); // t1 runs, t2 waits
    ASSERT_EQ(downloader.requests.size(), 1u);
    fs::create_directories(store().listFolder(albumSource("al1")));
    std::string running = weatherFolderFile("t1.flac.part"), waiting = weatherFolderFile("t2.mp3.part");
    std::ofstream(running, std::ios::binary) << std::string(10, 'p');
    std::ofstream(waiting, std::ios::binary) << std::string(10, 'p');
    app->shutdown();
    EXPECT_TRUE(fs::exists(running));
    EXPECT_TRUE(fs::exists(waiting));
}

TEST_F(AutoCacheLimitTest, TranscodedPartsOfRunningAndWaitingJobsSurviveTrimming) {
    settings.setInt("streaming.bitrate", 128);
    startSmall(4'000'000);
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    press(Button::Forward);
    listen(11s);
    ASSERT_EQ(downloader.requests.size(), 1u);
    fs::create_directories(store().listFolder(albumSource("al1")));
    const auto running = weatherFolderFile("t1~128.mp3.part");
    const auto waiting = weatherFolderFile("t2~128.mp3.part");
    std::ofstream(running, std::ios::binary) << std::string(10, 'p');
    std::ofstream(waiting, std::ios::binary) << std::string(10, 'p');
    app->setCacheGigabyte(5); // force a trim of the protected partial files
    app->shutdown();
    EXPECT_TRUE(fs::exists(running));
    EXPECT_TRUE(fs::exists(waiting));
}

TEST_F(AutoCacheLimitTest, TranscodeSpaceAndLimitUseTheStreamedSize) {
    settings.setInt("streaming.bitrate", 128);
    startSmall(16'000);
    auto tracks = sized();
    for (auto& track : tracks) {
        track.duration = 1;
        track.size = 100'000'000; // original is larger than both the limit and free space
    }
    std::uintmax_t room = kReserve + 15'999;
    app->setFreeSpace([&](const fs::path&) -> std::optional<std::uintmax_t> { return room; });
    ASSERT_TRUE(app->playRemote("Weather", tracks, 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    EXPECT_TRUE(downloader.requests.empty()) << "one byte short of the reserve";
    room++;
    press(Button::Forward);
    listen(11s);
    ASSERT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(downloader.lastRequest().trackId, "t2");
    EXPECT_EQ(downloader.lastRequest().bitrate, 128);
}

TEST_F(AutoCacheLimitTest, TheFolderOfAnAbandonedTransferGoesWithItsPartWhenOverTheLimit) {
    startSmall(5);
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    ASSERT_TRUE(downloader.finish(downloader.lastRequest().id, Result::Unreachable, true, "", "halfhalf"));
    app->tick(); // failed: the .part is left, nobody resumes it
    ASSERT_TRUE(fs::exists(weatherFolderFile("t1.flac.part")));
    press(Button::Forward);
    ASSERT_EQ(player.file, "fake://stream/t2"); // the play moves on
    app->shutdown();       // (t2 is queued and its folder is the same one)
    EXPECT_FALSE(fs::exists(weatherFolderFile("t1.flac.part")));
}

TEST_F(AutoCacheLimitTest, ANewPlayReplacesALastPlayedThatAWrongClockWroteInTheFuture) {
    cacheTrack("t1", false, kNow + 9'000'000);
    start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    app->shutdown();
    EXPECT_EQ(store().load(albumSource("al1")).tracks[0].lastPlayed, kNow);
}

TEST_F(AutoCacheLimitTest, AClockThatIsNotSetDoesNotRecordAPlay) {
    cacheTrack("t1", false, kNow - 1000);
    clock.setWallTime(5);
    start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    app->shutdown();
    EXPECT_EQ(store().load(albumSource("al1")).tracks[0].lastPlayed, kNow - 1000);
}

TEST_F(AutoCacheLimitTest, ATrackBiggerThanTheLimitIsNotFetched) {
    startSmall(4); // the tracks have 5 bytes
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(30s);
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(AutoCacheLimitTest, ATrackExactlyTheLimitIsFetched) {
    startSmall(5);
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    EXPECT_EQ(downloader.requests.size(), 1u);
}

TEST_F(AutoCacheLimitTest, WithTheCacheOffWhatIsCachedStaysOnExitAndSleep) {
    cacheTrack("a", false, 100);
    cacheTrack("b", false, 200);
    cacheTrack("t1", false, 300);
    settings.setInt("cache.limit_gb", 0);
    start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band")); // a play to write on sleep and exit
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_TRUE(cached("a"));
    app->shutdown();
    EXPECT_TRUE(cached("a"));
    EXPECT_TRUE(cached("b"));
    EXPECT_TRUE(fs::exists(weatherFolderFile("a.mp3")));
}

TEST_F(AutoCacheLimitTest, WithTheCacheOffPlaysAreStillRecordedForWhenItIsSwitchedOn) {
    cacheTrack("t1", false, 100);
    cacheTrack("other", false, 100);
    settings.setInt("cache.limit_gb", 0);
    start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    app->shutdown();
    EXPECT_EQ(store().load(albumSource("al1")).tracks[0].lastPlayed, kNow);
    EXPECT_TRUE(cached("other")) << "writing the plays evicts nothing";
}

TEST_F(AutoCacheLimitTest, TheTransferStopsWhenTheDeviceSleepsAndContinuesAfterTheWake) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    ASSERT_EQ(downloader.requests.size(), 1u);
    auto first = downloader.lastRequest().id;
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_NE(std::find(downloader.cancelled.begin(), downloader.cancelled.end(), first), downloader.cancelled.end());
    listen(30s);
    EXPECT_EQ(downloader.requests.size(), 1u) << "nothing starts while asleep";
    press(Button::Ok); // wake
    ASSERT_NE(app->mode(), App::Mode::Sleeping);
    listen(1s);
    ASSERT_EQ(downloader.requests.size(), 2u);
    EXPECT_EQ(downloader.lastRequest().trackId, "t1") << "the same track, from its .part";
}

TEST_F(AutoCacheLimitTest, ATrackCachedWhileTheClockIsNotSetHasNoPlayTime) {
    clock.setWallTime(5);
    start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    finishLast();
    ASSERT_TRUE(cached("t1"));
    EXPECT_EQ(store().load(albumSource("al1")).tracks[0].lastPlayed, 0);
}

TEST_F(AutoCacheLimitTest, CachingATrackDoesNotScanTheManifestsAgainWhileTheCacheIsUnderTheLimit) {
    cacheTrack("x", false, 100, 5, albumSource("other1"), "One");
    cacheTrack("y", false, 100, 5, albumSource("other2"), "Two");
    CountingFiles counting;
    app = std::make_unique<App>(Services{speech, player, sounds, volume, linkTo(settings, server, downloader), indexStore, scanner, settings, state, clock, &counting}, root, music, cache);
    app->start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    int before = counting.reads;
    finishLast();
    int first = counting.reads - before;
    EXPECT_GE(first, 4) << "its own list (read to load and to save) and a scan of the two others";
    press(Button::Forward);
    listen(11s);
    before = counting.reads;
    finishLast();
    EXPECT_EQ(counting.reads - before, 2) << "only its own list, to load and to save: no scan";
    app.reset();
}

TEST_F(AutoCacheLimitTest, TracksCachedAfterTheFirstScanAreCountedToTheLimitWithoutAScan) {
    cacheTrack("a", false, 100);
    startSmall(12);
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    finishLast(); // 10 bytes: the scan
    ASSERT_TRUE(cached("a"));
    press(Button::Forward);
    listen(11s);
    finishLast(); // 15 bytes
    EXPECT_FALSE(cached("a"));
    EXPECT_TRUE(cached("t1"));
    EXPECT_TRUE(cached("t2"));
}

TEST_F(AutoCacheLimitTest, ATransferThatFailsAfterTheFirstScanLeavesAPartThatIsStillCleanedOnExit) {
    cacheTrack("a", false, 100);
    startSmall(12);
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    finishLast(); // 10 bytes: the scan; within the limit
    press(Button::Forward);
    listen(11s);
    ASSERT_TRUE(downloader.finish(downloader.lastRequest().id, Result::Unreachable, true, "", "halfhalf"));
    app->tick();
    press(Button::Forward); // nothing waits for t2's part any more
    ASSERT_TRUE(fs::exists(weatherFolderFile("t2.mp3.part")));
    app->shutdown();
    EXPECT_FALSE(fs::exists(weatherFolderFile("t2.mp3.part"))) << "10 + 8 bytes are over 12";
}

TEST_F(AutoCacheLimitTest, EvictingOneCopyOfATrackKeepsTheOtherOneFindable) {
    cacheTrack("dup", false, 100, 5, albumSource("album"), "Album");
    cacheTrack("dup", true, 200, 5, albumSource("list"), "List"); // a pinned copy in another list
    startSmall(1);
    ASSERT_TRUE(cached("dup"));
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band")); // builds the index
    std::vector<RemoteTrack> tracks = {{"dup", "Dup", "Band", "Weather", 100, 1}};
    app->shutdown(); // the unpinned copy goes
    ASSERT_FALSE(fs::exists(store().listFolder(albumSource("album")) / "dup.mp3"));
    ASSERT_TRUE(app->playRemote("Dup", tracks, 0, albumSource("list")));
    EXPECT_EQ(player.file, (store().listFolder(albumSource("list")) / "dup.mp3").string()) << "plays from the pinned copy, not the stream";
}

TEST_F(AutoCacheLimitTest, ThePartOfAFailedExplicitDownloadIsKeptForTheNextTry) {
    cacheTrack("a", false, 100);
    openAlbum(); // (starts the app)
    app->setCacheGigabyte(5);
    startDownload();
    ASSERT_TRUE(downloader.finish(downloader.lastRequest().id, Result::Unreachable, true, "", "0123456789"));
    app->tick();
    ASSERT_TRUE(fs::exists(weatherFolderFile("t1.flac.part")));
    app->shutdown(); // 15 bytes with the part: over the limit of 5
    EXPECT_TRUE(fs::exists(weatherFolderFile("t1.flac.part"))) << "OK on Download continues from it";
}

TEST_F(AutoCacheLimitTest, EvictionDoesNotMakeTheNextLookupScanTheManifestsAgain) {
    cacheTrack("a", false, 100, 5, albumSource("other1"), "One");
    cacheTrack("b", false, 100, 5, albumSource("other2"), "Two");
    CountingFiles counting;
    app = std::make_unique<App>(Services{speech, player, sounds, volume, linkTo(settings, server, downloader), indexStore, scanner, settings, state, clock, &counting}, root, music, cache);
    app->setCacheGigabyte(10);
    app->start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    finishLast(); // 15 bytes: "a" goes
    ASSERT_FALSE(cached("a"));
    int before = counting.reads;
    ASSERT_TRUE(app->playRemote("Weather", sized(), 1, albumSource("al1"), 0, "Band"));
    EXPECT_EQ(counting.reads - before, 0) << "the index was updated, not made again";
    app.reset();
}

TEST_F(AutoCacheLimitTest, PoweringOffTrimsOnce) {
    cacheTrack("t1", false, 100); // its play is written to the manifest on exit
    cacheTrack("a", false, 100, 5, albumSource("other1"), "One");
    cacheTrack("b", false, 100, 5, albumSource("other2"), "Two");
    CountingFiles counting;
    app = std::make_unique<App>(Services{speech, player, sounds, volume, linkTo(settings, server, downloader), indexStore, scanner, settings, state, clock, &counting}, root, music, cache);
    app->start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    app->shutdown(); // the first exit scans, and the plays go to the manifests
    int before = counting.reads;
    app->shutdown();
    EXPECT_EQ(counting.reads - before, 0) << "the second saveOnExit finds nothing to do";
    app.reset();
}

// ---- the setting

TEST_F(AutoCacheLimitTest, CacheSizeIsInSettingsAndOffersTheFourSizes) {
    start();
    open("Settings");
    open("Cache size");
    EXPECT_EQ(labels(), (std::vector<std::string>{"off", "1 gigabyte", "4 gigabytes", "16 gigabytes"}));
    EXPECT_EQ(level().index, 1u) << "opens on the default";
}

TEST_F(AutoCacheLimitTest, TheSizeIsSavedAndKeptAcrossARestart) {
    start();
    open("Settings");
    open("Cache size");
    open("4 gigabytes");
    EXPECT_EQ(configFile.onDisk("cache.limit_gb"), "4");
    app->shutdown();
    app.reset();
    Settings reloaded{&configFile};
    reloaded.load();
    EXPECT_EQ(reloaded.getInt("cache.limit_gb", 1), 4);
    app = std::make_unique<App>(Services{speech, player, sounds, volume, linkTo(reloaded, server, downloader), indexStore, scanner, reloaded, state, clock, &diskFiles()}, root, music, cache);
    app->start();
    open("Settings");
    open("Cache size");
    EXPECT_EQ(level().index, 2u);
    open("off");
    app->shutdown();
    app.reset();
    Settings off{&configFile};
    off.load();
    app = std::make_unique<App>(Services{speech, player, sounds, volume, linkTo(off, server, downloader), indexStore, scanner, off, state, clock, &diskFiles()}, root, music, cache);
    app->start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1")));
    listen(60s);
    EXPECT_TRUE(downloader.requests.empty()) << "off, as it was saved";
    app.reset(); // before `reloaded` goes
}

// ---- Downloaded means what was downloaded

TEST_F(AutoCacheLimitTest, DownloadedListsOnlyWhatWasDownloaded) {
    cacheTrack("a1", false, 100, 5, albumSource("auto"), "Automatic");
    cacheTrack("m1", false, 100, 5, albumSource("mixed"), "Mixed");
    cacheTrack("m2", true, 100, 5, albumSource("mixed"), "Mixed");
    cacheTrack("p", true, 100, 5, albumSource("mine"), "Mine");
    openDownloaded();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Mine", "Mixed"}));
}

TEST_F(AutoCacheLimitTest, WhenOnlyAutomaticTracksAreCachedDownloadedIsEmpty) {
    cacheTrack("a1", false, 100);
    openDownloaded();
    EXPECT_EQ(labels(), (std::vector<std::string>{"No downloads"}));
}

TEST_F(AutoCacheLimitTest, TheErrorListDoesNotOfferDownloadedForOnlyAutomaticTracks) {
    cacheTrack("a1", false, 100);
    openList("Recently added");
    ASSERT_TRUE(server.fail(server.lastRequest().id, FakeMusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not reachable", "Try again"}));
}

// ---------------------------------------------------------------- server catalogue in the index

// The server catalogue (docs/streaming.md 4.7): at start-up, silently, and from Music -> Refresh
// library, the App compares the server's catalogue version with the stored one and fetches the
// album list only when it changed. Server items are in the index (the store) but not listed yet.
class CatalogueTest : public AppTest {
protected:
    void SetUp() override {
        AppTest::SetUp();
        settings.setString("server.url", "https://music.example");
        settings.setString("server.user", "ice");
        settings.setString("server.password", "sesame");
    }

    std::string key(const std::string& user = "ice") const { return TrackCache::serverKey("https://music.example", user); }
    std::string ref(const std::string& id, const std::string& serverKey = "") const {
        return LibraryIndex::seenRef(serverKey.empty() ? key() : serverKey, SeenKind::Album, id);
    }

    // A server album as an earlier sync saved it.
    void seedServerAlbum(const std::string& id, const std::string& title, const std::string& serverKey = "") {
        IndexItem item;
        item.id = ref(id, serverKey);
        item.source = IndexItem::Source::Server;
        item.title = title;
        item.firstSeen = 100;
        item.seenOn = {item.id};
        indexStore.items[item.id] = item;
    }
    void seedVersion(const std::string& version) {
        state.setString(versionKey(), version);
    }
    std::string versionKey(const std::string& serverKey = "") const { return "catalogue." + (serverKey.empty() ? key() : serverKey) + ".version"; }
    std::string folderKey(const std::string& serverKey = "") const { return "catalogue." + (serverKey.empty() ? key() : serverKey) + ".folder"; }

    // The newest request of `kind`.
    MusicServer::RequestId lastOf(FakeMusicServer::Kind kind) const {
        for (auto it = server.requests.rbegin(); it != server.requests.rend(); ++it)
            if (it->kind == kind) return it->id;
        ADD_FAILURE() << "no such request";
        return 0;
    }
    size_t countOf(FakeMusicServer::Kind kind) const {
        return size_t(std::count_if(server.requests.begin(), server.requests.end(),
                                    [kind](const auto& r) { return r.kind == kind; }));
    }
    // The sync ends with getPlaylists: unless a test holds that answer back (holdPlaylists), the
    // server answers it with serverPlaylists as soon as it is asked.
    std::vector<RemotePlaylist> serverPlaylists;
    bool holdPlaylists = false;
    void settlePlaylists() {
        if (holdPlaylists || server.requests.empty() || server.requests.back().kind != Kind::Playlists) return;
        if (server.reply(server.requests.back().id, serverPlaylists)) app->tick();
    }
    void answerVersion(const std::string& version) {
        ASSERT_TRUE(server.replyVersion(lastOf(Kind::CatalogueVersion), version));
        app->tick();
        settlePlaylists();
    }
    void answerAlbums(std::vector<RemoteAlbum> albums, bool more = false) {
        ASSERT_TRUE(server.reply(lastOf(Kind::Albums), std::move(albums), more));
        app->tick();
        settlePlaylists();
    }
    void fail(FakeMusicServer::Kind kind, MusicServer::Error error) {
        ASSERT_TRUE(server.fail(lastOf(kind), error));
        app->tick();
    }
    static RemoteAlbum album(const std::string& id, const std::string& name) {
        RemoteAlbum a;
        a.id = id;
        a.name = name;
        a.artist = "Band";
        a.songCount = 10;
        return a;
    }
    void refresh() {
        open("Music");
        open("Refresh library");
    }
    // True when no saved item holds `text` anywhere.
    bool storeHolds(const std::string& text) const {
        for (const auto& [id, item] : indexStore.items) {
            std::string all = id + item.title + item.artist + item.album + item.genre + item.musicBrainzId;
            for (const auto& r : item.seenOn) all += r;
            for (const auto& t : item.tracks) {
                all += t.location + t.title;
                for (const auto& r : t.seenOn) all += r;
            }
            if (all.find(text) != std::string::npos) return true;
        }
        return false;
    }
    bool saidAnythingWith(const std::string& text) const {
        for (const auto& s : speech.said)
            if (s.find(text) != std::string::npos) return true;
        return false;
    }
    using Kind = FakeMusicServer::Kind;
};

TEST_F(CatalogueTest, StartUpAsksTheVersionOnceAndNothingElseWhenItIsUnchanged) {
    seedServerAlbum("al1", "Kept");
    seedVersion("v1");
    start();
    ASSERT_EQ(countOf(Kind::CatalogueVersion), 1u);
    const int saves = indexStore.saves;
    answerVersion("v1");
    EXPECT_EQ(countOf(Kind::Albums), 0u) << "unchanged: no album list";
    EXPECT_EQ(indexStore.saves, saves) << "and nothing is written";
    EXPECT_TRUE(indexStore.items.count(ref("al1")));
}

TEST_F(CatalogueTest, ChangedVersionFetchesAllAlbumsAndReplacesTheServerItems) {
    seedServerAlbum("al1", "Old name");
    seedServerAlbum("gone", "Dropped by the server");
    seedVersion("v1");
    start();
    answerVersion("v2");
    ASSERT_EQ(countOf(Kind::Albums), 1u);
    EXPECT_EQ(server.lastRequest().arg, "alphabeticalByName");
    EXPECT_EQ(server.lastRequest().offset, 0);
    EXPECT_EQ(server.lastRequest().size, 500);
    RemoteAlbum a = album("al1", "New name");
    a.genre = "Rock";
    a.year = 1999;
    a.musicBrainzId = "mb-1";
    answerAlbums({a, album("al3", "Fresh")});

    ASSERT_TRUE(indexStore.items.count(ref("al1")));
    const IndexItem& item = indexStore.items.at(ref("al1"));
    EXPECT_EQ(item.title, "New name");
    EXPECT_EQ(item.artist, "Band");
    EXPECT_EQ(item.genre, "Rock");
    EXPECT_EQ(item.year, 1999);
    EXPECT_EQ(item.musicBrainzId, "mb-1");
    EXPECT_EQ(item.source, IndexItem::Source::Server);
    EXPECT_EQ(item.kind, IndexItem::Kind::Music);
    EXPECT_EQ(item.seenOn, std::vector<std::string>{key() + ":album:al1"});
    EXPECT_EQ(item.firstSeen, 100) << "an album the index knew keeps its first-seen time";
    EXPECT_TRUE(indexStore.items.count(ref("al3")));
    EXPECT_FALSE(indexStore.items.count(ref("gone"))) << "an album the server dropped is gone";
    EXPECT_EQ(state.getString(versionKey()), "v2");
}

TEST_F(CatalogueTest, AllPagesAreFetchedWhileTheServerHasMore) {
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One"), album("a2", "Two")}, true);
    ASSERT_EQ(countOf(Kind::Albums), 2u);
    EXPECT_EQ(server.lastRequest().offset, 2);
    EXPECT_FALSE(indexStore.items.count(ref("a1"))) << "nothing is replaced until the last page";
    answerAlbums({album("a3", "Three")});
    EXPECT_EQ(countOf(Kind::Albums), 2u);
    EXPECT_TRUE(indexStore.items.count(ref("a1")));
    EXPECT_TRUE(indexStore.items.count(ref("a2")));
    EXPECT_TRUE(indexStore.items.count(ref("a3")));
}

TEST_F(CatalogueTest, AnEmptyPageEndsPagingEvenIfTheServerSaysThereIsMore) {
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")}, true);
    answerAlbums({}, true);
    EXPECT_EQ(countOf(Kind::Albums), 2u) << "no third request";
    EXPECT_TRUE(indexStore.items.count(ref("a1")));
    EXPECT_EQ(server.pendingCount(), 0u);
}

TEST_F(CatalogueTest, AServerThatNeverStopsPagingEndsInAnErrorAndKeepsTheSavedItems) {
    seedServerAlbum("old", "Old");
    start();
    answerVersion("v1");
    std::vector<RemoteAlbum> page;
    for (int i = 0; i < 500; ++i) page.push_back(album("x" + std::to_string(i), "X"));
    for (int i = 0; i < 1000 && server.pendingCount() > 0; ++i) answerAlbums(page, true);
    EXPECT_EQ(server.pendingCount(), 0u) << "the sync gave up";
    EXPECT_TRUE(indexStore.items.count(ref("old")));
    EXPECT_FALSE(indexStore.items.count(ref("x0")));
    EXPECT_EQ(state.getString(versionKey()), "");
}

TEST_F(CatalogueTest, ASyncErrorAtStartUpSaysNothingAndKeepsTheSavedItems) {
    seedServerAlbum("al1", "Kept");
    seedVersion("v1");
    start();
    const size_t spoken = speech.said.size();
    const int saves = indexStore.saves;
    fail(Kind::CatalogueVersion, MusicServer::Error::Unreachable);
    EXPECT_EQ(speech.said.size(), spoken);
    EXPECT_TRUE(indexStore.items.count(ref("al1")));
    EXPECT_EQ(indexStore.saves, saves);
}

TEST_F(CatalogueTest, AnAlbumListErrorAtStartUpSaysNothingKeepsTheItemsAndTheOldVersion) {
    seedServerAlbum("al1", "Kept");
    seedVersion("v1");
    start();
    const size_t spoken = speech.said.size();
    answerVersion("v2");
    fail(Kind::Albums, MusicServer::Error::Server);
    EXPECT_EQ(speech.said.size(), spoken);
    EXPECT_TRUE(indexStore.items.count(ref("al1")));
    EXPECT_EQ(state.getString(versionKey()), "v1") << "the next start tries again";
}

TEST_F(CatalogueTest, ASyncThatChangedNothingWritesNothing) {
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")});
    app->shutdown();
    app.reset();
    const int saves = indexStore.saves;
    start(); // the version is stored in state, which the test keeps in memory
    state.setString(versionKey(), "other"); // a changed version, the same albums
    answerVersion("v2");
    answerAlbums({album("a1", "One")});
    EXPECT_EQ(indexStore.saves, saves);
}

TEST_F(CatalogueTest, AServerWithoutAVersionIsSyncedEveryTime) {
    seedServerAlbum("al1", "One");
    seedVersion("");
    start();
    answerVersion("");
    EXPECT_EQ(countOf(Kind::Albums), 1u);
}

TEST_F(CatalogueTest, AnIndexWithoutServerItemsIsSyncedEvenIfTheVersionMatches) {
    seedVersion("v1"); // the library database was lost, state.tsv was not
    start();
    answerVersion("v1");
    EXPECT_EQ(countOf(Kind::Albums), 1u);
}

TEST_F(CatalogueTest, AFailedSaveDoesNotStoreTheVersion) {
    start();
    answerVersion("v1");
    indexStore.failSaves = true;
    answerAlbums({album("a1", "One")});
    EXPECT_EQ(state.getString(versionKey()), "");
}

// ---- Playlists in the index (docs/fixes.md 2.4)

static RemotePlaylist remotePlaylist(const std::string& id, const std::string& name, int songs, const std::string& changed = "") {
    RemotePlaylist p;
    p.id = id;
    p.name = name;
    p.songCount = songs;
    p.changed = changed;
    return p;
}

TEST_F(CatalogueTest, AfterASyncTheIndexHasTheServersPlaylists) {
    serverPlaylists = {remotePlaylist("p1", "Road Trip", 12, "2026-03-01T10:00:00Z"), remotePlaylist("p2", "Chill", 3)};
    start();
    answerVersion("v1");
    ASSERT_EQ(countOf(Kind::Playlists), 0u) << "after the catalogue, not before";
    answerAlbums({album("a1", "One")});
    ASSERT_EQ(countOf(Kind::Playlists), 1u);
    EXPECT_EQ(indexStore.playlists, (std::vector<IndexPlaylist>{{key(), "p1", "Road Trip", 12, 1772359200}, {key(), "p2", "Chill", 3, 0}}));
}

TEST_F(CatalogueTest, ASyncOfAnUnchangedCatalogueStillFetchesThePlaylists) {
    seedServerAlbum("al1", "Kept");
    seedVersion("v1");
    serverPlaylists = {remotePlaylist("p1", "New on the server", 4)};
    start();
    answerVersion("v1");
    EXPECT_EQ(countOf(Kind::Albums), 0u);
    ASSERT_EQ(countOf(Kind::Playlists), 1u);
    ASSERT_EQ(indexStore.playlists.size(), 1u);
    EXPECT_EQ(indexStore.playlists[0].name, "New on the server");
}

TEST_F(CatalogueTest, APlaylistGoneFromTheServersListIsGoneAfterTheNextSync) {
    indexStore.playlists = {{key(), "p1", "Kept", 5, 10}, {key(), "p2", "Deleted on the server", 7, 20}};
    seedServerAlbum("al1", "Kept");
    seedVersion("v1");
    serverPlaylists = {remotePlaylist("p1", "Kept, renamed", 6)};
    start();
    answerVersion("v1");
    EXPECT_EQ(indexStore.playlists, (std::vector<IndexPlaylist>{{key(), "p1", "Kept, renamed", 6, 0}}));
}

TEST_F(CatalogueTest, AServerWithNoPlaylistsLeavesNoneInTheIndex) {
    indexStore.playlists = {{key(), "p1", "Only one", 5, 10}};
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")});
    EXPECT_TRUE(indexStore.playlists.empty());
}

TEST_F(CatalogueTest, ASyncThatFailsKeepsTheOldPlaylists) {
    const std::vector<IndexPlaylist> old = {{key(), "p1", "Old", 5, 10}};
    serverPlaylists = {remotePlaylist("p9", "Never stored", 1)};
    {
        SCOPED_TRACE("the version fails");
        indexStore.playlists = old;
        start();
        fail(Kind::CatalogueVersion, MusicServer::Error::Unreachable);
        EXPECT_EQ(indexStore.playlists, old);
        EXPECT_EQ(countOf(Kind::Playlists), 0u);
        app->shutdown();
        app.reset();
    }
    {
        SCOPED_TRACE("the albums fail");
        start();
        answerVersion("v2");
        fail(Kind::Albums, MusicServer::Error::Server);
        EXPECT_EQ(indexStore.playlists, old);
        EXPECT_EQ(countOf(Kind::Playlists), 0u);
        app->shutdown();
        app.reset();
    }
    {
        SCOPED_TRACE("getPlaylists fails");
        holdPlaylists = true;
        start();
        answerVersion("v3");
        answerAlbums({album("a1", "One")});
        fail(Kind::Playlists, MusicServer::Error::Unreachable);
        EXPECT_EQ(indexStore.playlists, old);
    }
}

TEST_F(CatalogueTest, PlaylistsThatCannotBeWrittenDoNotStopTheCatalogueVersionBeingStored) {
    serverPlaylists = {remotePlaylist("p1", "Road Trip", 12)};
    indexStore.failPlaylistSaves = true;
    indexStore.playlists = {{key("former"), "x", "Of the old user", 1, 0}}; // start-up drops it, and cannot write that
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")});
    EXPECT_EQ(state.getString(versionKey()), "v1") << "or every start would fetch all the albums again";
    EXPECT_EQ(indexStore.items.count(ref("a1")), 1u);
    ASSERT_EQ(indexStore.playlists.size(), 1u) << "what was stored stays";
    EXPECT_EQ(indexStore.playlists[0].id, "x");
    indexStore.failPlaylistSaves = false;
    app->shutdown();
    app.reset();
    start();
    answerVersion("v1");
    EXPECT_EQ(countOf(Kind::Albums), 1u) << "unchanged: no second album list";
    ASSERT_EQ(indexStore.playlists.size(), 1u) << "the playlists are written at the next sync";
    EXPECT_EQ(indexStore.playlists[0].id, "p1");
}

TEST_F(CatalogueTest, RefreshSpeaksAGetPlaylistsError) {
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")});
    holdPlaylists = true;
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v2");
    answerAlbums({album("a1", "One")});
    EXPECT_EQ(speech.last(), "Updating the library.") << "the sync waits for the playlists";
    fail(Kind::Playlists, MusicServer::Error::Unreachable);
    EXPECT_EQ(speech.last(), "Server not reachable.");
}

TEST_F(CatalogueTest, ARefreshWaitsForThePlaylistsBeforeItSaysTheLibraryIsUpdated) {
    serverPlaylists = {remotePlaylist("p1", "Road Trip", 12)};
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")});
    holdPlaylists = true;
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v1");
    EXPECT_EQ(speech.last(), "Updating the library.");
    holdPlaylists = false;
    settlePlaylists();
    EXPECT_EQ(speech.last(), "Library updated, 3 albums.");
}

TEST_F(CatalogueTest, PlaylistsOfAnotherServerOrUserAreDroppedAtStartUp) {
    indexStore.playlists = {{key("former"), "p1", "Of the old user", 1, 0}, {key(), "p2", "Of ice", 2, 0}};
    seedServerAlbum("al2", "Of ice");
    seedVersion("v1");
    start(false);
    EXPECT_EQ(indexStore.playlists, (std::vector<IndexPlaylist>{{key(), "p2", "Of ice", 2, 0}}));
}

TEST_F(CatalogueTest, WithNoServerThePlaylistsOfEveryServerAreDropped) {
    indexStore.playlists = {{key(), "p1", "Gone", 1, 0}};
    settings.setString("server.url", "");
    start(false);
    EXPECT_TRUE(indexStore.playlists.empty());
}

TEST_F(CatalogueTest, ItemsOfAnotherServerOrUserAreDroppedAtStartUp) {
    seedServerAlbum("al1", "Of the old user", key("former"));
    seedServerAlbum("al2", "Of ice");
    seedVersion("v1");
    // A local album that was matched to a server album of the other key.
    IndexItem local = savedItem("local:album:Band\x1fMine", IndexItem::Kind::Music, "Mine", {"/m/1.mp3"});
    local.seenOn = {ref("al1", key("former"))};
    indexStore.items[local.id] = local;
    start(false); // before the scan, which would not find /m/1.mp3
    EXPECT_FALSE(indexStore.items.count(ref("al1", key("former")))) << "its server item went";
    ASSERT_TRUE(indexStore.items.count(local.id));
    EXPECT_TRUE(indexStore.items.at(local.id).seenOn.empty()) << "and so did the reference on the local item";
    EXPECT_TRUE(indexStore.items.count(ref("al2")));
    EXPECT_FALSE(storeHolds(key("former")));
}

TEST_F(CatalogueTest, AStoredVersionOfAnotherServerIsNotThisServers) {
    seedServerAlbum("al1", "One");
    state.setString(versionKey(key("former")), "v1");
    start();
    answerVersion("v1");
    EXPECT_EQ(countOf(Kind::Albums), 1u);
}

TEST_F(CatalogueTest, ANewServerOnTheSameIndexIsSyncedFromScratch) {
    seedServerAlbum("al1", "Of the old server", TrackCache::serverKey("https://old.example", "ice"));
    state.setString(versionKey(TrackCache::serverKey("https://old.example", "ice")), "v1"); // the old server's version, not this one's
    start();
    answerVersion("v1");
    EXPECT_EQ(countOf(Kind::Albums), 1u) << "the same version of another server means nothing";
    answerAlbums({album("n1", "New")});
    EXPECT_FALSE(storeHolds("old.example"));
    EXPECT_TRUE(indexStore.items.count(ref("n1")));
}

TEST_F(CatalogueTest, WithNoServerNothingIsSyncedAndWhatWasSeenOnAServerIsDropped) {
    settings.setString("server.url", "");
    seedServerAlbum("al1", "Of the server that was set up");
    start(false);
    EXPECT_TRUE(server.requests.empty());
    EXPECT_FALSE(indexStore.items.count(ref("al1")));
}

TEST_F(CatalogueTest, AServerThisBuildCannotUseKeepsItsSavedItems) {
    seedServerAlbum("al1", "Kept");
    server.isConfigured = false; // set up in config.yaml, but not as a server the player can reach
    start();
    EXPECT_TRUE(server.requests.empty());
    EXPECT_TRUE(indexStore.items.count(ref("al1")));
}

TEST_F(CatalogueTest, ShutdownCancelsTheSyncInFlight) {
    start();
    const auto id = lastOf(Kind::CatalogueVersion);
    app->shutdown();
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), id), server.cancelled.end());
}

TEST_F(CatalogueTest, NothingSpokenOrSavedHoldsTheServerAddressOrPassword) {
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")});
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v2");
    answerAlbums({album("a1", "One")});
    for (const char* secret : {"music.example", "sesame", "https://", "fake://"}) {
        EXPECT_FALSE(saidAnythingWith(secret)) << secret;
        EXPECT_FALSE(storeHolds(secret)) << secret;
        for (const auto& [name, value] : state.values()) {
            EXPECT_EQ(name.find(secret), std::string::npos) << name;
            EXPECT_EQ(value.find(secret), std::string::npos) << name;
        }
    }
}

// ---- Music -> Refresh library

TEST_F(CatalogueTest, RefreshIsTheLastItemOfMusic) {
    start();
    open("Music");
    EXPECT_EQ(labels().back(), "Refresh library");
}

TEST_F(CatalogueTest, RefreshSaysUpdatingThenTheResultOnceTheScanAndTheSyncAreDone) {
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One"), album("a2", "Two")});
    refresh();
    EXPECT_EQ(speech.last(), "Updating the library.");
    EXPECT_EQ(scanner.pendingCount(), 1u) << "Refresh rescans the files";
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    EXPECT_EQ(speech.last(), "Updating the library.") << "the server has not answered yet";
    answerVersion("v2");
    EXPECT_EQ(speech.last(), "Updating the library.");
    answerAlbums({album("a1", "One"), album("a2", "Two"), album("a3", "Three")});
    EXPECT_EQ(speech.last(), "Library updated, 5 albums.") << "2 on the device and 3 on the server";
    EXPECT_EQ(level().title, "Music") << "it reports and stays";
}

TEST_F(CatalogueTest, RefreshWaitsForTheScanEvenWhenTheServerAnsweredFirst) {
    seedServerAlbum("a1", "One");
    seedVersion("v1");
    start();
    answerVersion("v1");
    refresh();
    answerVersion("v1");
    EXPECT_EQ(speech.last(), "Updating the library.") << "the files are still being scanned";
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    EXPECT_EQ(speech.last(), "Library updated, 3 albums.");
}

TEST_F(CatalogueTest, RefreshOfAnUnchangedCatalogueFetchesNoAlbumsAndWritesNothing) {
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")});
    const int saves = indexStore.saves;
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v1");
    EXPECT_EQ(countOf(Kind::Albums), 1u);
    EXPECT_EQ(speech.last(), "Library updated, 3 albums.");
    EXPECT_EQ(indexStore.saves, saves);
}

TEST_F(CatalogueTest, RefreshPicksUpAnAlbumAddedOnTheServer) {
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")});
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v2");
    answerAlbums({album("a1", "One"), album("a2", "Two")});
    EXPECT_TRUE(indexStore.items.count(ref("a2")));
}

TEST_F(CatalogueTest, RefreshSpeaksEachError) {
    const std::vector<std::pair<MusicServer::Error, std::string>> cases = {
        {MusicServer::Error::Unreachable, "Server not reachable."},
        {MusicServer::Error::BadLogin, "Wrong user name or password."},
        {MusicServer::Error::Server, "Server error."}};
    for (const auto& [error, text] : cases) {
        SCOPED_TRACE(text);
        speech.said.clear();
        seedServerAlbum("a1", "One");
        seedVersion("v1");
        start();
        answerVersion("v1");
        refresh();
        ASSERT_NO_FATAL_FAILURE(deliverScan());
        fail(Kind::CatalogueVersion, error);
        EXPECT_EQ(speech.last(), text);
        EXPECT_TRUE(indexStore.items.count(ref("a1"))) << "the saved items stay";
        app->shutdown();
        app.reset();
    }
}

TEST_F(CatalogueTest, RefreshSpeaksAnAlbumListError) {
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")});
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v2");
    fail(Kind::Albums, MusicServer::Error::Unreachable);
    EXPECT_EQ(speech.last(), "Server not reachable.");
}

TEST_F(CatalogueTest, ASecondPressWhileItRunsSaysStillUpdating) {
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")});
    refresh();
    EXPECT_EQ(speech.last(), "Updating the library.");
    const size_t scans = scanner.requests.size();
    const size_t versions = countOf(Kind::CatalogueVersion);
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Still updating.");
    EXPECT_EQ(scanner.requests.size(), scans) << "no second scan";
    EXPECT_EQ(countOf(Kind::CatalogueVersion), versions) << "and no second sync";
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v1");
    EXPECT_EQ(speech.last(), "Library updated, 3 albums.");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Updating the library.") << "once it is done, it runs again";
}

TEST_F(CatalogueTest, RefreshWhileTheStartUpSyncRunsJoinsItAndSpeaksItsResult) {
    start(false);
    refresh();
    EXPECT_EQ(speech.last(), "Updating the library.");
    EXPECT_EQ(scanner.requests.size(), 1u) << "the start-up scan is joined";
    EXPECT_EQ(countOf(Kind::CatalogueVersion), 1u);
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    fail(Kind::CatalogueVersion, MusicServer::Error::Unreachable);
    EXPECT_EQ(speech.last(), "Server not reachable.");
}

TEST_F(CatalogueTest, ALateResultIsNotSpokenOnAnotherScreen) {
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")});
    refresh();
    press(Button::Home);
    const size_t spoken = speech.said.size();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v2");
    answerAlbums({album("a1", "One"), album("a2", "Two")});
    EXPECT_EQ(speech.said.size(), spoken);
    EXPECT_TRUE(indexStore.items.count(ref("a2"))) << "but the index is updated";
}

TEST_F(CatalogueTest, WithNoServerRefreshOnlyRescansTheFiles) {
    settings.setString("server.url", "");
    start();
    refresh();
    EXPECT_EQ(speech.last(), "Updating the library.");
    EXPECT_TRUE(server.requests.empty());
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    EXPECT_EQ(speech.last(), "Library updated, 2 albums.");
}

TEST_F(CatalogueTest, RefreshFindsAFolderAddedOnTheDevice) {
    settings.setString("server.url", "");
    start();
    touch(music / "New Album" / "1.mp3");
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    EXPECT_EQ(speech.last(), "Library updated, 3 albums.");
}

// ---------------------------------------------------------------- duplicates and downloads

// Docs/streaming.md 4.8: an album that is on the device and on the server, or downloaded, is one
// entry in Music, played from the preferred source, and keeps the server's reference.
class MergeTest : public CatalogueTest {
protected:
    void SetUp() override {
        CatalogueTest::SetUp();
        cache = base / "cache";
    }
    // Road Trip has two tracks by Band, tagged, like the server's album of the same name.
    void tagRoadTrip(const std::string& musicBrainzId = "") {
        for (const char* f : {"01 - Sunrise.mp3", "02 - Highway.mp3"}) {
            TrackTags t;
            t.album = "Road Trip";
            t.albumArtist = "Band";
            t.title = f;
            t.musicBrainzAlbumId = musicBrainzId;
            tags.tags[roadTrip() / f] = t;
        }
    }
    static RemoteAlbum roadTripOnServer(const std::string& id = "rt", int tracks = 2) {
        RemoteAlbum a = album(id, "Road Trip");
        a.songCount = tracks;
        return a;
    }
    // The start-up sync answers: the server has `albums`.
    void sync(std::vector<RemoteAlbum> albums, const std::string& version = "v1") {
        answerVersion(version);
        answerAlbums(std::move(albums));
    }
    TrackCache store() const { return TrackCache(diskFiles(), cache, "https://music.example", "ice"); }
    // A pinned download of album `id` called `name` with tracks t1..tN, as a download leaves it.
    void download(const std::string& id, const std::string& name, int tracks = 3) {
        RemoteSource source = albumSource(id);
        CachedList list{source, name, "Band", {}};
        fs::create_directories(store().listFolder(source));
        for (int i = 1; i <= tracks; ++i) {
            std::string t = id + "-t" + std::to_string(i);
            list.tracks.push_back({t, t + ".mp3", "Song " + std::to_string(i), 100, "", true, 0});
            std::ofstream(store().listFolder(source) / (t + ".mp3"), std::ios::binary) << "xxxxx";
        }
        ASSERT_TRUE(store().save(list));
    }
    std::string downloadedFile(const std::string& id, int n) const {
        return (store().listFolder(albumSource(id)) / (id + "-t" + std::to_string(n) + ".mp3")).string();
    }
    // Music > Albums A to Z > Options on `album` > Album info, and what it said.
    std::string albumInfo(const std::string& name) {
        open("Music");
        open("Albums A to Z");
        while (level().items[level().index].label != name) press(Button::Forward);
        press(Button::Options);
        open("Album info");
        std::string said = speech.last();
        EXPECT_EQ(level().title, "Options, " + name) << "it only reports, so the Options screen stays";
        hold(Button::Home);
        return said;
    }
    const IndexItem& stored(const std::string& id) { return indexStore.items.at(id); }
    std::string roadTripId() const { return "local:album:Band\x1f" "Road Trip"; }
};

TEST_F(MergeTest, TheSameAlbumLocalAndOnTheServerIsOneEntryAndPlaysLocalFiles) {
    tagRoadTrip();
    start();
    sync({roadTripOnServer(), album("other", "Server Only")});
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip", "Server Only"})) << "Road Trip once";
    open("Road Trip");
    open("Highway");
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    EXPECT_EQ(player.file.find("://"), std::string::npos) << "no stream URL";
    EXPECT_EQ(countOf(Kind::AlbumTracks), 0u) << "the server is not asked for the tracks";
    EXPECT_EQ(stored(roadTripId()).seenOn, std::vector<std::string>{ref("rt")}) << "the entry keeps the server's reference";
}

TEST_F(MergeTest, ASyncAfterTheScanMergesJustAsAScanAfterTheSync) {
    tagRoadTrip();
    start(false);
    sync({roadTripOnServer()});
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip"}));
    EXPECT_EQ(stored(roadTripId()).seenOn, std::vector<std::string>{ref("rt")});
}

TEST_F(MergeTest, TheSameNameWithAnotherTrackCountIsNotMerged) {
    tagRoadTrip();
    start();
    sync({roadTripOnServer("rt", 3)});
    EXPECT_TRUE(stored(roadTripId()).seenOn.empty()) << "two entries: the local one has no reference";
    EXPECT_TRUE(indexStore.items.count(ref("rt")));
    EXPECT_EQ(albumInfo("Road Trip"), "On the device. Band. 2 tracks.");
}

TEST_F(MergeTest, AMatchingMusicBrainzIdMergesDifferentSpellings) {
    tagRoadTrip("9b3a");
    start();
    RemoteAlbum a = album("rt", "The Road Trip (Deluxe Edition)");
    a.artist = "Band feat. Friends";
    a.songCount = 17;
    a.musicBrainzId = "9b3a";
    sync({a});
    EXPECT_EQ(stored(roadTripId()).seenOn, std::vector<std::string>{ref("rt")});
    EXPECT_EQ(albumInfo("Road Trip"), "On the device. Also on the server. Band. 2 tracks.");
}

TEST_F(MergeTest, ADownloadedServerAlbumIsCachedPlaysItsFilesAndCarriesTheServersReference) {
    ASSERT_NO_FATAL_FAILURE(download("al1", "Weather"));
    start();
    sync({album("al1", "Weather")});
    const IndexItem& item = stored("cached:" + ref("al1"));
    EXPECT_EQ(item.source, IndexItem::Source::Cached);
    EXPECT_EQ(item.seenOn, std::vector<std::string>{ref("al1")});
    ASSERT_EQ(item.tracks.size(), 3u);
    EXPECT_EQ(item.tracks[0].location, downloadedFile("al1", 1));
    EXPECT_EQ(item.tracks[0].seenOn, std::vector<std::string>{LibraryIndex::seenRef(key(), SeenKind::Track, "al1-t1")});

    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip", "Weather"}));
    open("Weather");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Song 1", "Song 2", "Song 3"}));
    open("Song 2");
    EXPECT_EQ(player.file, downloadedFile("al1", 2));
    EXPECT_EQ(countOf(Kind::AlbumTracks), 0u);
}

TEST_F(MergeTest, ADownloadIsListedOnceBeforeTheFirstSyncAndKeepsItsReference) {
    ASSERT_NO_FATAL_FAILURE(download("al1", "Weather"));
    start();
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip", "Weather"}));
    EXPECT_EQ(stored("cached:" + ref("al1")).seenOn, std::vector<std::string>{ref("al1")});
}

TEST_F(MergeTest, ADownloadAndTheLocalCopyAreOneEntryPlayedFromTheLocalFiles) {
    tagRoadTrip();
    ASSERT_NO_FATAL_FAILURE(download("rt", "Road Trip", 2));
    start();
    sync({roadTripOnServer()});
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip"}));
    open("Road Trip");
    open("Sunrise");
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
}

TEST_F(MergeTest, PlayAllPlaysTheDownloadsToo) {
    ASSERT_NO_FATAL_FAILURE(download("al1", "Weather"));
    start();
    open("Music");
    EXPECT_EQ(level().items[0].label, "Play all");
    EXPECT_EQ(level().items[0].value(), "6 tracks") << "Lullaby, Road Trip and Weather";
}

TEST_F(MergeTest, AfterAMergeASyncWhereTheServerRenamesTheAlbumKeepsOneEntryMatchedByReference) {
    tagRoadTrip();
    start();
    sync({roadTripOnServer()});
    ASSERT_EQ(stored(roadTripId()).seenOn, std::vector<std::string>{ref("rt")});

    ASSERT_NO_FATAL_FAILURE(refresh());
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    RemoteAlbum renamed = roadTripOnServer();
    renamed.name = "Road Trip (2026 remaster)";
    renamed.songCount = 14; // and by name it would no longer match
    sync({renamed}, "v2");

    EXPECT_EQ(stored(roadTripId()).seenOn, std::vector<std::string>{ref("rt")});
    EXPECT_EQ(stored(ref("rt")).title, "Road Trip (2026 remaster)");
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip"})) << "still one entry";
    hold(Button::Home);
    EXPECT_EQ(albumInfo("Road Trip"), "On the device. Also on the server. Band. 2 tracks.");
}

TEST_F(MergeTest, AnAlbumRemovedFromTheServerLeavesTheLocalEntryWithNoReference) {
    tagRoadTrip();
    start();
    sync({roadTripOnServer()});
    ASSERT_FALSE(stored(roadTripId()).seenOn.empty());

    ASSERT_NO_FATAL_FAILURE(refresh());
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    sync({}, "v2");

    EXPECT_FALSE(indexStore.items.count(ref("rt")));
    ASSERT_TRUE(indexStore.items.count(roadTripId()));
    EXPECT_TRUE(stored(roadTripId()).seenOn.empty());
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip"}));
    hold(Button::Home);
    EXPECT_EQ(albumInfo("Road Trip"), "On the device. Band. 2 tracks.");
}

TEST_F(MergeTest, ADownloadOfAnAlbumTheServerDroppedStaysWithoutAReference) {
    ASSERT_NO_FATAL_FAILURE(download("al1", "Weather"));
    start();
    sync({album("al1", "Weather")});
    ASSERT_FALSE(stored("cached:" + ref("al1")).seenOn.empty());

    ASSERT_NO_FATAL_FAILURE(refresh());
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    sync({album("other", "Something else")}, "v2");

    EXPECT_TRUE(stored("cached:" + ref("al1")).seenOn.empty());
    hold(Button::Home);
    EXPECT_EQ(albumInfo("Weather"), "Downloaded. Band. 3 tracks.");
}

TEST_F(MergeTest, ADownloadedAlbumPlaysFromMusicAndIsNamedByItsTrackTitlesNotItsFiles) {
    ASSERT_NO_FATAL_FAILURE(download("al1", "Weather"));
    start();
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    open("Weather");
    open("Song 2");
    EXPECT_EQ(app->nowPlaying().title, "Weather");
    EXPECT_EQ(app->nowPlaying().chapter, "Song 2");
    EXPECT_EQ(app->nowPlaying().chapterIndex, 1);
    EXPECT_EQ(player.file, downloadedFile("al1", 2));
    EXPECT_TRUE(player.playing());
    EXPECT_TRUE(server.scrobbles.empty()) << "a file on the device is not scrobbled";
}

TEST_F(MergeTest, AlbumInfoNamesEachSource) {
    tagRoadTrip();
    ASSERT_NO_FATAL_FAILURE(download("al1", "Weather"));
    ASSERT_NO_FATAL_FAILURE(download("al2", "Climate", 1));
    start();
    sync({roadTripOnServer(), album("al1", "Weather")});
    EXPECT_EQ(albumInfo("Road Trip"), "On the device. Also on the server. Band. 2 tracks.");
    EXPECT_EQ(albumInfo("Weather"), "Downloaded. Also on the server. Band. 3 tracks.");
    EXPECT_EQ(albumInfo("Climate"), "Downloaded. Band. 1 track.");
}

TEST_F(MergeTest, ADeletedDownloadLeavesMusic) {
    ASSERT_NO_FATAL_FAILURE(download("al1", "Weather"));
    start();
    open("Music");
    open("Albums A to Z");
    ASSERT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip", "Weather"}));
    hold(Button::Home);
    ASSERT_TRUE(store().remove(albumSource("al1")));
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip"}));
    EXPECT_FALSE(indexStore.items.count("cached:" + ref("al1")));
}

TEST_F(MergeTest, TheSyncStoresTheTrackCountAndTheTimeTheServerAddedTheAlbum) {
    start();
    RemoteAlbum a = album("x", "Fresh");
    a.songCount = 12;
    a.created = "2026-10-20T18:01:44.120Z";
    sync({a, album("y", "Undated")});
    EXPECT_EQ(stored(ref("x")).trackCount, 12);
    EXPECT_EQ(stored(ref("x")).created, 1792519304);
    EXPECT_EQ(stored(ref("y")).created, 0);
}

TEST_F(MergeTest, NothingSpokenOrSavedHoldsAStreamUrlOrToken) {
    tagRoadTrip();
    ASSERT_NO_FATAL_FAILURE(download("al1", "Weather"));
    start();
    sync({roadTripOnServer(), album("al1", "Weather")});
    albumInfo("Road Trip");
    EXPECT_FALSE(saidAnythingWith("://"));
    EXPECT_FALSE(storeHolds("sesame"));
    EXPECT_FALSE(storeHolds("://"));
}

TEST_F(MergeTest, RefreshCountsAMergedAlbumOnceAndCountsDownloads) {
    tagRoadTrip();
    start();
    sync({roadTripOnServer(), album("other", "Server Only")});
    open("Music");
    ASSERT_NO_FATAL_FAILURE(download("al1", "Weather")); // made since the menu was built
    open("Refresh library");
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v1"); // unchanged: no album list
    EXPECT_EQ(speech.last(), "Library updated, 4 albums.") << "Lullaby, Road Trip (once), Weather and the server's own";
}

TEST_F(MergeTest, ADownloadedPlaylistIsNotAnAlbumInMusic) {
    CachedList list{playlistSource("pl1"), "Mix", "", {}};
    fs::create_directories(store().listFolder(playlistSource("pl1")));
    list.tracks.push_back({"p1", "p1.mp3", "Song", 100, "", true, 0});
    std::ofstream(store().listFolder(playlistSource("pl1")) / "p1.mp3", std::ios::binary) << "xxxxx";
    ASSERT_TRUE(store().save(list));
    start();
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip"}));
}

TEST_F(MergeTest, PlayAllPlaysTheAlbumsInTheOrderTheyAreListed) {
    ASSERT_NO_FATAL_FAILURE(download("al1", "Weather")); // listed last, but its id sorts before the local ones
    start();
    open("Music");
    press(Button::Ok);
    EXPECT_EQ(player.file, (music / "Lullaby.mp3").string());
}

// A download's files are named by the server's track ids: Play all speaks its tracks by their titles,
// as the album itself does, in every place a track is named.
TEST_F(MergeTest, PlayAllSpeaksTheTitlesOfADownloadNotItsFileNames) {
    ASSERT_NO_FATAL_FAILURE(download("al1", "Accordion")); // listed first, so Play all starts on it
    ASSERT_NO_FATAL_FAILURE(download("al2", "Weather", 2)); // listed last, after the local albums
    start();
    open("Music");
    open("Play all");
    ASSERT_EQ(player.file, downloadedFile("al1", 1));
    EXPECT_EQ(speech.last(), "Now playing. All music, Song 1.");
    press(Button::Forward); // the next track
    ASSERT_EQ(player.file, downloadedFile("al1", 2));
    EXPECT_NE(speech.last().find("Song 2"), std::string::npos) << speech.last();
    EXPECT_EQ(speech.last().find("al1-t"), std::string::npos) << speech.last();
    EXPECT_EQ(app->nowPlaying().chapter, "Song 2");
    player.atEnd = true; // the track ends: the next one
    app->tick();
    ASSERT_EQ(player.file, downloadedFile("al1", 3));
    EXPECT_EQ(app->nowPlaying().chapter, "Song 3");
    player.atEnd = true; // into the local albums, still named by their files
    app->tick();
    ASSERT_EQ(player.file, (music / "Lullaby.mp3").string());
    EXPECT_EQ(app->nowPlaying().chapter, "Lullaby");
    for (int i = 0; i < 3; ++i) { // Sunrise, Highway, then the second download
        player.atEnd = true;
        app->tick();
    }
    ASSERT_EQ(player.file, downloadedFile("al2", 1));
    EXPECT_EQ(app->nowPlaying().chapter, "Song 1");
    player.atEnd = true;
    app->tick();
    ASSERT_EQ(player.file, downloadedFile("al2", 2));
    EXPECT_EQ(app->nowPlaying().chapter, "Song 2");
}

// A queue saved by an older Play all has a download's file name as its title: it is spoken by its title.
TEST_F(MergeTest, ARestoredPlayAllQueueSpeaksTheTitleOfADownloadSavedByItsFileName) {
    ASSERT_NO_FATAL_FAILURE(download("al1", "Accordion"));
    start();
    open("Music"); // the download is in the index
    app->shutdown();
    app.reset();
    // Shuffled: Sunrise, al1-t1, al1-t2, and al1-t2 is current. Only the titles may change.
    const std::string text = "oap-queue\t1\ntitle\tAll music\nshuffle\ttracks\nposition\t2\norder\t2\t0\t1\n"
                             "L\t" + downloadedFile("al1", 1) + "\tal1-t1\tAccordion\t\n"
                             "L\t" + downloadedFile("al1", 2) + "\tal1-t2\tAccordion\t\n"
                             "L\t" + (roadTrip() / "01 - Sunrise.mp3").string() + "\tSunrise\tRoad Trip\t\n";
    ASSERT_TRUE(diskFiles().writeAtomically(queueFile, text));
    player = FakePlayer{};
    speech.said.clear();
    start();
    ASSERT_EQ(labels().front(), "Now playing");
    press(Button::Ok);
    ASSERT_EQ(player.file, downloadedFile("al1", 2)) << "its place is kept";
    EXPECT_EQ(speech.last(), "Now playing. All music, Song 2.");
    press(Button::Back); // the track before, in the shuffled order
    ASSERT_EQ(player.file, downloadedFile("al1", 1));
    EXPECT_EQ(app->nowPlaying().chapter, "Song 1");
    press(Button::Back);
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string()) << "the shuffled order is kept";
    EXPECT_EQ(app->nowPlaying().chapter, "Sunrise");
}

// ---------------------------------------------------------------- Music tree (docs/streaming.md 4.9)

class MusicTreeTest : public MergeTest {
protected:
    // A tagged local album of `artist` in its own folder, one track.
    void localAlbum(const std::string& folder, const std::string& artist, const std::string& albumName) {
        touch(music / folder / "1.mp3");
        TrackTags t;
        t.album = albumName;
        t.albumArtist = artist;
        t.title = "Song";
        tags.tags[music / folder / "1.mp3"] = t;
    }
    static RemoteAlbum byArtist(const std::string& id, const std::string& name, const std::string& artist) {
        RemoteAlbum a = album(id, name);
        a.artist = artist;
        return a;
    }
    void answerTracks(std::vector<RemoteTrack> tracks) {
        ASSERT_TRUE(server.reply(lastOf(Kind::AlbumTracks), std::move(tracks)));
        app->tick();
    }
    // Home > Music > Albums A to Z > `name`.
    void openAlbum(const std::string& name) {
        open("Music");
        open("Albums A to Z");
        open(name);
    }
};

TEST_F(MusicTreeTest, ArtistsListLocalAndServerArtistsOnceEachSortedWithoutArticles) {
    localAlbum("Abbey", "The Beatles", "Abbey");
    localAlbum("Ночь", "Агата Кристи", "Ночь");
    tagRoadTrip(); // Band
    start();
    sync({byArtist("s1", "Other", "band"), byArtist("s2", "Voulez", "ABBA"), byArtist("s3", "Тест", "Би-2"),
          byArtist("s4", "Selected", "Aphex Twin"), byArtist("s5", "Hero", "The Alpha")});
    open("Music");
    open("Artists");
    EXPECT_EQ(labels(), (std::vector<std::string>{"ABBA", "The Alpha", "Aphex Twin", "Band", "The Beatles", "Агата Кристи", "Би-2",
                                                  "Unknown artist"}))
        << "Band and band are one, The Alpha sorts under A, Cyrillic after Latin, the album with no artist last";
    for (int i = 0; i < 3; ++i) press(Button::Forward);
    EXPECT_EQ(speech.last(), "Band, 2 albums, 4 of 8");
}

TEST_F(MusicTreeTest, AnArtistsAlbumsIncludeBothSources) {
    tagRoadTrip();
    start();
    sync({roadTripOnServer(), byArtist("s1", "Another", "Band"), byArtist("s2", "Not theirs", "Choir")});
    open("Music");
    open("Artists");
    open("Band");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Another", "Road Trip"})) << "the merged Road Trip once, and the server's";
    EXPECT_EQ(level().title, "Band");
    open("Road Trip");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Sunrise", "Highway"})) << "the device files, at once";
    EXPECT_EQ(countOf(Kind::AlbumTracks), 0u);
}

TEST_F(MusicTreeTest, AServerAlbumOfAnArtistLoadsLikeInStreaming) {
    start();
    sync({byArtist("s1", "Weather", "Band")});
    open("Music");
    open("Artists");
    open("Band");
    open("Weather");
    EXPECT_EQ(speech.last(), "Weather. Loading, 1 of 1");
    EXPECT_EQ(server.lastRequest().arg, "s1");
}

TEST_F(MusicTreeTest, ALocalAlbumPlaysAtOnceWithNoRequest) {
    start();
    sync({album("s1", "Weather")});
    const size_t asked = server.requests.size();
    openAlbum("Road Trip");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Sunrise", "Highway"}));
    EXPECT_EQ(speech.last(), "Road Trip. Sunrise, 1 of 2");
    press(Button::Ok);
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    EXPECT_EQ(server.requests.size(), asked) << "nothing is asked of the server";
}

TEST_F(MusicTreeTest, AServerAlbumShowsLoadingThenItsTracks) {
    start();
    sync({album("s1", "Weather")});
    openAlbum("Weather");
    EXPECT_EQ(speech.last(), "Weather. Loading, 1 of 1");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Loading"}));
    EXPECT_EQ(server.lastRequest().kind, Kind::AlbumTracks);
    EXPECT_EQ(server.lastRequest().arg, "s1") << "the server's id, not the reference";
    ASSERT_NO_FATAL_FAILURE(answerTracks(weather()));
    EXPECT_EQ(labels().front(), "Play album");
    EXPECT_EQ(labels().back(), "Sunshine") << "no Album info at the end";
    EXPECT_NE(std::find(labels().begin(), labels().end(), "Rain"), labels().end());
    EXPECT_EQ(speech.last(), "Play album, 3 tracks, 1 of " + std::to_string(labels().size()));
    press(Button::Back); // the last track
    press(Button::Ok);
    EXPECT_EQ(player.file, "fake://stream/t3");
    expectNoUrlSpoken(speech);
}

TEST_F(MusicTreeTest, WithTheServerUnreachableAServerAlbumGivesTheErrorAndALocalAlbumStillPlays) {
    start();
    sync({album("s1", "Weather")});
    openAlbum("Weather");
    ASSERT_NO_FATAL_FAILURE(fail(Kind::AlbumTracks, MusicServer::Error::Unreachable));
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not reachable", "Try again"})) << "no tracks";
    EXPECT_EQ(speech.last(), "Server not reachable, 1 of 2");
    press(Button::Forward);
    press(Button::Ok); // Try again
    EXPECT_EQ(labels(), (std::vector<std::string>{"Loading"}));
    ASSERT_NO_FATAL_FAILURE(answerTracks(weather()));
    EXPECT_EQ(labels().front(), "Play album");
    press(Button::Home);
    open("Road Trip");
    open("Highway");
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    EXPECT_TRUE(player.playing());
}

TEST_F(MusicTreeTest, AMergedAlbumPlaysItsLocalFilesWithoutAskingTheServer) {
    tagRoadTrip();
    start();
    sync({roadTripOnServer()});
    openAlbum("Road Trip");
    open("Sunrise");
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    EXPECT_EQ(countOf(Kind::AlbumTracks), 0u) << "the server is never asked, so it cannot be unreachable";
}

TEST_F(MusicTreeTest, PlayAllPlaysOnlyDeviceFilesNeverTheServer) {
    ASSERT_NO_FATAL_FAILURE(download("al1", "Weather"));
    start();
    sync({album("s1", "Zeta On The Server")});
    const size_t asked = server.requests.size();
    open("Music");
    EXPECT_EQ(speech.last(), "Music. Play all, 6 tracks, 1 of 8") << "Lullaby, Road Trip and the download: no server album";
    press(Button::Ok);
    std::vector<std::string> played{player.file};
    for (int i = 0; i < 5; ++i) {
        player.atEnd = true;
        app->tick();
        played.push_back(player.file);
    }
    for (const std::string& f : played) EXPECT_EQ(f.find("fake://"), std::string::npos) << f;
    EXPECT_EQ(played.back(), downloadedFile("al1", 3));
    player.atEnd = true;
    app->tick();
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(server.requests.size(), asked);
}

TEST_F(MusicTreeTest, PlayAllWithOnlyServerAlbumsSaysNoMusic) {
    fs::remove_all(music);
    start();
    sync({album("s1", "Weather")});
    open("Music");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play all", "Artists", "Albums A to Z", "Playlists", "Genres", "Recently added", "Recently played", "Refresh library"}));
    EXPECT_EQ(speech.last(), "Music. Play all, 1 of 8");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "No music");
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_FALSE(player.isOpen());
}

TEST_F(MusicTreeTest, HomeWhileAServerAlbumLoadsDropsTheLateAnswerAndSaysNothing) {
    start();
    sync({album("s1", "Weather")});
    openAlbum("Weather");
    auto id = lastOf(Kind::AlbumTracks);
    press(Button::Home);
    EXPECT_EQ(level().title, "Albums A to Z");
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), id), server.cancelled.end());
    EXPECT_FALSE(server.reply(id, weather())) << "the answer is dropped";
    speech.said.clear();
    app->tick();
    EXPECT_TRUE(speech.said.empty());
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip", "Weather"}));
}

TEST_F(MusicTreeTest, AListYouComeBackToShowsWhatChangedMeanwhile) {
    start();
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip"}));
    press(Button::Home);
    sync({album("s1", "Weather")});
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play all", "Artists", "Albums A to Z", "Playlists", "Genres", "Recently added", "Recently played", "Refresh library"}));
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip", "Weather"}));
    EXPECT_EQ(level().items[2].value(), "10 tracks");
}

TEST_F(MusicTreeTest, AnAlbumWithNoArtistTagTakesTheArtistOfItsServerCopy) {
    for (const char* f : {"01 - Sunrise.mp3", "02 - Highway.mp3"}) {
        TrackTags t;
        t.album = "Road Trip";
        t.title = f;
        t.musicBrainzAlbumId = "mb-1";
        tags.tags[roadTrip() / f] = t; // no artist
    }
    start();
    RemoteAlbum a = roadTripOnServer();
    a.musicBrainzId = "mb-1";
    sync({a});
    open("Music");
    open("Artists");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Band", "Unknown artist"})) << "Road Trip is Band's, Lullaby has none";
    open("Band");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Road Trip"}));
}

TEST_F(MusicTreeTest, AnArtistWhoseAlbumsWentWhileTheListWasOpenIsNeverAnEmptyMenu) {
    start();
    sync({byArtist("s1", "Weather", "Solo")});
    open("Music");
    open("Artists");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Solo", "Unknown artist"}));
    press(Button::Home);
    open("Refresh library");
    open("Artists"); // keys work while it runs
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v2");
    answerAlbums({}); // Solo's only album left the server
    press(Button::Ok); // on Solo, which is still listed
    EXPECT_EQ(level().title, "Solo");
    EXPECT_EQ(labels(), (std::vector<std::string>{"No albums"}));
}

TEST_F(MusicTreeTest, PlayAllBeforeTheScanFindsFilesSaysTheScanIsRunning) {
    seedServerAlbum("s1", "Weather");
    seedVersion("v1");
    start(false);
    open("Music");
    EXPECT_EQ(labels().front(), "Play all");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Looking for music. Try again in a moment.");
    EXPECT_FALSE(player.isOpen());
}

// ---------------------------------------------------------------- genres, recently added and played

class RecentMusicTest : public MusicTreeTest {
protected:
    static constexpr int64_t kStart = 1'700'000'000; // FakeClock's wall time: when a scan first sees an album
    static RemoteAlbum made(const std::string& id, const std::string& name, const std::string& created, const std::string& genre = "") {
        RemoteAlbum a = album(id, name);
        a.created = created;
        a.genre = genre;
        return a;
    }
    static RemoteAlbum roadTripMade() {
        RemoteAlbum a = roadTripOnServer();
        a.created = "2020-06-01T00:00:00Z";
        return a;
    }
    void tagGenre(const std::string& genre) {
        for (const char* f : {"01 - Sunrise.mp3", "02 - Highway.mp3"}) {
            TrackTags t = tags.tags.at(roadTrip() / f);
            t.genre = genre;
            tags.tags[roadTrip() / f] = t;
        }
    }
    // Music > `list`.
    void openMusicList(const std::string& list) {
        open("Music");
        open(list);
    }
    // Plays Music > Albums A to Z > `name` (a loose track, or its first track), then goes Home.
    void playFromMusic(const std::string& name) {
        openAlbum(name); // a loose track plays at once
        if (app->mode() != App::Mode::Player) press(Button::Ok); // an album opened: its first track
        ASSERT_EQ(app->mode(), App::Mode::Player);
        goHome();
    }
    void goHome() {
        for (int i = 0; i < 6 && !(app->mode() == App::Mode::Menu && app->screens().size() == 1); ++i) hold(Button::Home);
        ASSERT_EQ(app->screens().size(), 1u);
    }
};

TEST_F(RecentMusicTest, AGenreListsTheAlbumsOfBothSourcesAndNamesThatDifferInCaseAreOne) {
    tagRoadTrip();
    tagGenre("rock");
    start();
    sync({made("s1", "Weather", "2022-01-01T00:00:00Z", "Rock"), made("s2", "Blues Night", "2022-01-01T00:00:00Z", " ROCK "),
          made("s3", "Smooth", "2022-01-01T00:00:00Z", "Jazz"), made("s4", "No genre", "2022-01-01T00:00:00Z")});
    openMusicList("Genres");
    ASSERT_EQ(labels().size(), 2u) << "Rock, rock and ROCK are one genre; albums with no genre have none";
    EXPECT_EQ(labels().front(), "Jazz");
    EXPECT_EQ(speech.last(), "Genres. Jazz, 1 album, 1 of 2");
    press(Button::Forward);
    EXPECT_NE(speech.last().find(", 3 albums, 2 of 2"), std::string::npos) << speech.last();
    press(Button::Ok);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Blues Night", "Road Trip", "Weather"})) << "the local album and two server albums";
    press(Button::Forward); // Road Trip: on the device, it opens at once
    press(Button::Ok);
    EXPECT_EQ(labels().front(), "Sunrise");
    EXPECT_EQ(countOf(Kind::AlbumTracks), 0u);
}

TEST_F(RecentMusicTest, AnAlbumThatTheServerFilesUnderAnotherGenreIsInBoth) {
    tagRoadTrip();
    tagGenre("Rock");
    start();
    RemoteAlbum a = roadTripOnServer();
    a.genre = "Alternative";
    sync({a});
    openMusicList("Genres");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alternative", "Rock"}));
    open("Alternative");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Road Trip"}));
}

TEST_F(RecentMusicTest, ANoGenreLibraryHasAGenresItemThatSaysSo) {
    start();
    openMusicList("Genres");
    EXPECT_EQ(labels(), (std::vector<std::string>{"No genres"}));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "No album has a genre yet.");
}

TEST_F(RecentMusicTest, RecentlyAddedIsNewestFirstAndAMergedAlbumIsAsOldAsItsEarliestItem) {
    tagRoadTrip();
    start(); // the scan sees Lullaby and Road Trip at kStart
    sync({made("new", "Fresh", "2026-10-20T18:01:44Z"), made("mid", "Zeta", "2022-01-01T00:00:00Z"),
          roadTripMade(), made("none", "Undated", "")});
    openMusicList("Recently added");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Fresh", "Lullaby", "Undated", "Zeta", "Road Trip"}))
        << "Undated has no `created`: its first-seen time (the sync, with Lullaby) is used";
    EXPECT_EQ(level().title, "Recently added");
}

TEST_F(RecentMusicTest, RecentlyAddedPutsANewLocalAlbumBeforeAnOlderServerAlbumAndAfterANewerOne) {
    start();
    clock.setWallTime(kStart + 86400 * 400); // a year later the user copies an album
    touch(music / "Newer" / "1.mp3");
    press(Button::Home);
    open("Music");
    open("Refresh library");
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v1");
    answerAlbums({made("s1", "Server old", "2020-01-02T00:00:00Z"), made("s2", "Server newest", "2030-01-01T00:00:00Z")});
    press(Button::Home);
    openMusicList("Recently added");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server newest", "Newer", "Lullaby", "Road Trip", "Server old"}))
        << "Newer: " << (kStart + 86400 * 400);
}

TEST_F(RecentMusicTest, RecentlyAddedListsAtMostFiftyAlbums) {
    start();
    std::vector<RemoteAlbum> many;
    for (int i = 0; i < 60; ++i) many.push_back(made("m" + std::to_string(i), "Album " + std::to_string(i), "2024-01-01T00:00:00Z"));
    sync(many);
    openMusicList("Recently added");
    EXPECT_EQ(labels().size(), 50u);
}

TEST_F(RecentMusicTest, AnAlbumPlayedFromMusicIsFirstInRecentlyPlayedAfterARestart) {
    tagRoadTrip();
    start();
    clock.setWallTime(kStart + 100);
    ASSERT_NO_FATAL_FAILURE(playFromMusic("Lullaby"));
    clock.setWallTime(kStart + 200);
    ASSERT_NO_FATAL_FAILURE(playFromMusic("Road Trip"));
    app->shutdown();
    start(); // a new App on the same store
    openMusicList("Recently played");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Road Trip", "Lullaby"})) << "not the order of titles or of first sight";
    EXPECT_EQ(level().title, "Recently played");
    press(Button::Ok); // the same entries as Albums A to Z
    EXPECT_EQ(labels().front(), "Sunrise");
}

TEST_F(RecentMusicTest, PlayingWritesNothingToDiskUntilExitOrSleep) {
    start();
    const int saves = indexStore.saves;
    clock.setWallTime(kStart + 100);
    ASSERT_NO_FATAL_FAILURE(playFromMusic("Lullaby"));
    openMusicList("Recently played"); // building Music saves what changed, which is nothing
    press(Button::Home);
    idle(60s);
    EXPECT_EQ(indexStore.saves, saves) << "nothing written by playing";
    for (const auto& [key, item] : indexStore.items) EXPECT_EQ(item.lastPlayed, 0) << key;
    press(Button::Power); // sleep
    EXPECT_GT(indexStore.saves, saves);
    std::vector<std::string> played;
    for (const auto& [key, item] : indexStore.items)
        if (item.lastPlayed != 0) played.push_back(key);
    ASSERT_EQ(played.size(), 1u);
    EXPECT_EQ(indexStore.items.at(played.front()).lastPlayed, kStart + 100);
    EXPECT_EQ(indexStore.items.at(played.front()).title, "Lullaby");
    EXPECT_EQ(indexStore.savedIds.back(), played.front()) << "only the played album is written";
    EXPECT_EQ(std::count(indexStore.savedIds.begin(), indexStore.savedIds.end(), played.front()), 2) << "the scan, then the play time";
}

TEST_F(RecentMusicTest, ARescanAfterPlayingWritesNothingAndTheTimeSurvives) {
    start();
    clock.setWallTime(kStart + 100);
    ASSERT_NO_FATAL_FAILURE(playFromMusic("Lullaby"));
    const int saves = indexStore.saves;
    open("Music");
    open("Refresh library");
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    EXPECT_EQ(indexStore.saves, saves) << "a scan that finds the same files does not flush play times";
    ASSERT_NO_FATAL_FAILURE(goHome());
    openMusicList("Recently played");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby"}));
}

TEST_F(RecentMusicTest, ExitWritesThePlayTimeOnceAndASecondExitWritesNothing) {
    start();
    ASSERT_NO_FATAL_FAILURE(playFromMusic("Road Trip"));
    app->shutdown();
    const int saves = indexStore.saves;
    EXPECT_GT(saves, 0);
    app->shutdown();
    EXPECT_EQ(indexStore.saves, saves);
}

TEST_F(RecentMusicTest, AServerAlbumPlayedFromMusicIsRecentlyPlayedAndAMergedEntryUsesTheLatestOfItsItems) {
    tagRoadTrip();
    seedServerAlbum("rt", "Road Trip");
    indexStore.items.at(ref("rt")).lastPlayed = kStart + 500; // played from the server before
    start();
    sync({roadTripOnServer(), album("s1", "Weather")});
    clock.setWallTime(kStart + 300);
    ASSERT_NO_FATAL_FAILURE(playFromMusic("Lullaby"));
    clock.setWallTime(kStart + 100);
    ASSERT_NO_FATAL_FAILURE(playFromMusic("Road Trip")); // the device item: 100, the server item: 500
    openAlbum("Weather");
    ASSERT_NO_FATAL_FAILURE(answerTracks(weather()));
    clock.setWallTime(kStart + 400);
    press(Button::Ok); // Play album
    press(Button::Home);
    while (app->screens().size() > 1) hold(Button::Home);
    openMusicList("Recently played");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Road Trip", "Weather", "Lullaby"}));
}

TEST_F(RecentMusicTest, AWallTimeBeforeTheFloorIsNotAPlayTimeNorAnAddedTime) {
    clock.setWallTime(1000); // a clock that was never set
    start();
    ASSERT_NO_FATAL_FAILURE(playFromMusic("Lullaby"));
    app->shutdown();
    for (const auto& [key, item] : indexStore.items) EXPECT_EQ(item.lastPlayed, 0) << key;
    start();
    openMusicList("Recently played");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Nothing played yet"}));
    ASSERT_NO_FATAL_FAILURE(goHome());
    openMusicList("Recently added");
    EXPECT_EQ(labels(), (std::vector<std::string>{"No new albums"})) << "first seen at 1000: no time at all";
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "No new albums.");
}

TEST_F(RecentMusicTest, ASavedPlayTimeBeforeTheFloorIsIgnoredWhenListing) {
    start();
    for (auto& [key, item] : indexStore.items) item.lastPlayed = 5;
    start();
    openMusicList("Recently played");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Nothing played yet"}));
}

TEST_F(RecentMusicTest, EmptyListsSayWhy) {
    start();
    openMusicList("Recently played");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Nothing played yet"}));
    EXPECT_EQ(speech.last(), "Recently played. Nothing played yet, 1 of 1");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Nothing played yet.");
}

// ---------------------------------------------------------------- audiobooks from the server (docs/streaming.md 4.11)

class ServerBookTest : public MusicTreeTest {
protected:
    void SetUp() override {
        MusicTreeTest::SetUp();
        settings.setString("server.audiobook_folder", "Books");
    }
    // The start-up sync with the audiobook folder set: the server has the folders Music and Books,
    // `books` are in Books, and `all` is its whole catalogue.
    void syncWithFolder(std::vector<RemoteAlbum> books, std::vector<RemoteAlbum> all, const std::string& version = "v1") {
        answerVersion(version);
        ASSERT_EQ(server.lastRequest().kind, Kind::MusicFolders);
        ASSERT_TRUE(server.reply(lastOf(Kind::MusicFolders), std::vector<RemoteFolder>{{"1", "Music"}, {"2", "Books"}}));
        app->tick();
        ASSERT_EQ(server.lastRequest().kind, Kind::Albums);
        EXPECT_EQ(server.lastRequest().folder, "2") << "the albums of the Books folder";
        answerAlbums(std::move(books));
        ASSERT_EQ(server.lastRequest().kind, Kind::Albums);
        EXPECT_EQ(server.lastRequest().folder, "") << "then the whole catalogue";
        answerAlbums(std::move(all));
    }
    static RemoteAlbum dune() { return album("b1", "Dune"); }
    // Home > Library > `name`, then the server answers with `tracks`.
    void openServerBook(const std::string& name, std::vector<RemoteTrack> tracks = weather()) {
        if (level().title != "Home") hold(Button::Home);
        open("Library");
        open(name);
        ASSERT_EQ(server.lastRequest().kind, Kind::AlbumTracks);
        answerTracks(std::move(tracks));
    }
    std::string bookKey(const std::string& id) const { return "pos:" + ref(id); }
};

TEST_F(ServerBookTest, SyncingSubsonicBookOffersServerChapterAndSavesPause) {
    server.progressSync = true;
    state.setString("pos:" + ref("b1"), "0:10.0");
    state.setString("posat:" + ref("b1"), "1699999990");
    state.setString("pushed:" + ref("b1"), "1699999990");
    start(); syncWithFolder({dune()}, {dune()});
    openServerBook("Dune");
    ASSERT_EQ(server.lastRequest().kind, Kind::Progress);
    ASSERT_TRUE(server.replyProgress(lastOf(Kind::Progress), RemoteProgress{250, 620, false, 1700000010000}));
    app->tick();
    EXPECT_EQ(speech.last(), "The server has a later place, chapter 2. It is in Options.");
    press(Button::PlayPause);
    press(Button::Options);
    ASSERT_FALSE(labels().empty());
    EXPECT_NE(labels().front().find("Go to the server's place, chapter 2"), std::string::npos);
    press(Button::Ok);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 50);
    ASSERT_FALSE(server.progressSaves.empty());
    EXPECT_DOUBLE_EQ(server.progressSaves.front().progress.seconds, 250);
}

TEST_F(ServerBookTest, TheFolderAlbumsAreInLibraryAfterTheLocalBooksAndNotInMusic) {
    start();
    syncWithFolder({dune()}, {dune(), album("s1", "Weather")});
    EXPECT_EQ(indexStore.items.at(ref("b1")).kind, IndexItem::Kind::Book);
    EXPECT_EQ(indexStore.items.at(ref("s1")).kind, IndexItem::Kind::Music);
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta", "Dune"}));
    EXPECT_EQ(level().items[0].value ? level().items[0].value() : "", "2 parts");
    ASSERT_TRUE(level().items[2].value);
    EXPECT_EQ(level().items[2].value(), "on the server");
    EXPECT_EQ(speech.last(), "Library. Alpha, 2 parts, 1 of 3");
    press(Button::Forward);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Dune, on the server, 3 of 3");
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip", "Weather"})) << "no Dune";
    hold(Button::Home);
    open("Music");
    open("Artists");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Band", "Unknown artist"}));
    open("Band");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Weather"})) << "Dune is by Band too, and is not here";
    hold(Button::Home);
    open("Music");
    open("Recently added");
    for (const auto& label : labels()) EXPECT_NE(label, "Dune");
    hold(Button::Home);
    open("Music");
    open("Refresh library");
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v1");
    EXPECT_EQ(speech.last(), "Library updated, 3 albums, 1 book.") << "Lullaby, Road Trip and Weather, and the book apart";
}

TEST_F(ServerBookTest, WithoutTheFolderNothingChangesAndTheServerIsNotAskedForFolders) {
    settings.clearPrefix("server.audiobook_folder");
    start();
    sync({dune(), album("s1", "Weather")});
    EXPECT_EQ(countOf(Kind::MusicFolders), 0u);
    for (const auto& r : server.requests) EXPECT_EQ(r.folder, "");
    EXPECT_EQ(indexStore.items.at(ref("b1")).kind, IndexItem::Kind::Music);
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta"}));
    EXPECT_EQ(state.values().count(folderKey()), 0u) << "nothing new in the state";
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Dune", "Lullaby", "Road Trip", "Weather"}));
}

TEST_F(ServerBookTest, AFolderTheServerDoesNotHaveMakesNoBooks) {
    start();
    answerVersion("v1");
    ASSERT_TRUE(server.reply(lastOf(Kind::MusicFolders), std::vector<RemoteFolder>{{"1", "Music"}}));
    app->tick();
    ASSERT_EQ(server.lastRequest().kind, Kind::Albums);
    EXPECT_EQ(server.lastRequest().folder, "") << "straight to the whole catalogue";
    answerAlbums({dune()});
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta"}));
    EXPECT_EQ(state.getString(versionKey()), "") << "not remembered: the next start asks again, in case the name was a typo";
    EXPECT_EQ(state.values().count(folderKey()), 0u);
}

TEST_F(ServerBookTest, AFailedFolderListKeepsTheSavedItemsAndSaysNothing) {
    seedServerAlbum("old", "Old");
    start();
    answerVersion("v1");
    const size_t spoken = speech.said.size();
    fail(Kind::MusicFolders, MusicServer::Error::Unreachable);
    EXPECT_EQ(speech.said.size(), spoken);
    EXPECT_EQ(countOf(Kind::Albums), 0u) << "no album list is taken without knowing which are books";
    EXPECT_TRUE(indexStore.items.count(ref("old")));
    EXPECT_EQ(state.getString(versionKey()), "");
}

TEST_F(ServerBookTest, TheFolderIsTakenInPagesLikeTheCatalogue) {
    start();
    answerVersion("v1");
    ASSERT_TRUE(server.reply(lastOf(Kind::MusicFolders), std::vector<RemoteFolder>{{"2", "Books"}}));
    app->tick();
    answerAlbums({dune()}, true);
    ASSERT_EQ(server.lastRequest().kind, Kind::Albums);
    EXPECT_EQ(server.lastRequest().folder, "2");
    EXPECT_EQ(server.lastRequest().offset, 1);
    answerAlbums({album("b2", "Emma")});
    EXPECT_EQ(server.lastRequest().folder, "");
    answerAlbums({dune(), album("b2", "Emma")});
    EXPECT_EQ(indexStore.items.at(ref("b2")).kind, IndexItem::Kind::Book);
}

TEST_F(ServerBookTest, ChangingTheFolderSyncsAgainEvenWhenTheCatalogueVersionIsTheSame) {
    seedServerAlbum("b1", "Dune");
    seedVersion("v1"); // synced before the setting was made
    start();
    answerVersion("v1");
    EXPECT_EQ(server.lastRequest().kind, Kind::MusicFolders);
}

TEST_F(ServerBookTest, TheSameFolderAndVersionFetchNothing) {
    seedServerAlbum("b1", "Dune");
    seedVersion("v1");
    state.setString(folderKey(), "Books");
    start();
    answerVersion("v1");
    EXPECT_EQ(countOf(Kind::MusicFolders), 0u);
    EXPECT_EQ(countOf(Kind::Albums), 0u);
}

TEST_F(ServerBookTest, TheFolderIsRememberedInTheStateWithTheVersion) {
    start();
    syncWithFolder({dune()}, {dune()});
    EXPECT_EQ(state.getString(folderKey()), "Books");
    EXPECT_EQ(state.getString(versionKey()), "v1");
}

TEST_F(ServerBookTest, OpeningABookShowsLoadingThenPlaysItAsChaptersAndSaysChapter) {
    start();
    syncWithFolder({dune()}, {dune()});
    open("Library");
    open("Dune");
    EXPECT_EQ(speech.last(), "Loading.");
    EXPECT_EQ(server.lastRequest().kind, Kind::AlbumTracks);
    EXPECT_EQ(server.lastRequest().arg, "b1") << "the server's id, not the reference";
    EXPECT_EQ(level().title, "Library") << "still there while it loads";
    answerTracks(weather());
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_TRUE(player.playing());
    player.pos = 5;
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Book player. Dune, by Band. Chapter 1 of 3, Storm Front. 9 minutes 55 seconds left in chapter. Playing.");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Rain");
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_EQ(app->nowPlaying().title, "Dune");
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
    press(Button::Ok); // Player menu
    EXPECT_EQ(level().title, "Player menu");
    open("Chapters");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Storm Front", "Rain", "Sunshine"}));
}

// Only the music queue skips what cannot play and pings the server (queue.md): a book stops at the chapter.
TEST_F(ServerBookTest, ANextChapterThatCannotBeOpenedStopsThereAndPlayOpensItAgainWithoutAsking) {
    start();
    syncWithFolder({dune()}, {dune()});
    openServerBook("Dune");
    ASSERT_EQ(player.file, "fake://stream/t1");
    const size_t requests = server.requests.size();
    player.unopenable = {"fake://stream/t2"};
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(speech.last(), "Cannot play Rain. Server not reachable.");
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t3"), 0) << "no other chapter tried";
    EXPECT_FALSE(player.playing());
    player.unopenable.clear();
    press(Button::PlayPause);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(server.requests.size(), requests) << "no ping: the book player does not mark the server down";
    idle(5min);
    EXPECT_EQ(server.requests.size(), requests);
}

TEST_F(ServerBookTest, PauseAnotherBookThenThisBookAgainResumesThePlace) {
    start();
    syncWithFolder({dune()}, {dune()});
    ASSERT_NO_FATAL_FAILURE(openServerBook("Dune"));
    press(Button::Forward);
    ASSERT_EQ(player.file, "fake://stream/t2");
    player.pos = 100;
    press(Button::PlayPause); // pause
    EXPECT_FALSE(player.playing());
    hold(Button::Home);
    open("Library");
    open("Alpha"); // another book
    EXPECT_EQ(player.file, alphaFile(1));
    player.opened.clear();
    ASSERT_NO_FATAL_FAILURE(openServerBook("Dune"));
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t2"}));
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 100);
    EXPECT_TRUE(player.playing());
    hold(Button::PlayPause);
    EXPECT_NE(speech.last().find("Chapter 2 of 3, Rain"), std::string::npos) << speech.last();
}

TEST_F(ServerBookTest, ThePlaceSurvivesARestart) {
    start();
    syncWithFolder({dune()}, {dune()});
    ASSERT_NO_FATAL_FAILURE(openServerBook("Dune"));
    press(Button::Forward);
    player.pos = 77.5;
    app->shutdown();
    app.reset();
    start(false);
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("v1"); // unchanged
    open("Library");
    open("Dune");
    answerTracks(weather());
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 77.5);
}

TEST_F(ServerBookTest, ASavedPlaceBeyondTheChaptersStartsAtTheFirstChapter) {
    state.setString(bookKey("b1"), "9:12.0");
    start();
    syncWithFolder({dune()}, {dune()});
    ASSERT_NO_FATAL_FAILURE(openServerBook("Dune"));
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

TEST_F(ServerBookTest, TheBooksOfTwoServerIdsKeepSeparatePlaces) {
    start();
    syncWithFolder({dune(), album("b2", "Emma")}, {dune(), album("b2", "Emma")});
    ASSERT_NO_FATAL_FAILURE(openServerBook("Dune"));
    press(Button::Forward);
    player.pos = 40;
    press(Button::PlayPause);
    ASSERT_NO_FATAL_FAILURE(openServerBook("Emma"));
    EXPECT_EQ(player.file, "fake://stream/t1") << "Emma has no place of its own yet";
    EXPECT_DOUBLE_EQ(player.pos, 0);
    app->shutdown();
    EXPECT_EQ(state.getString(bookKey("b1")), "1:40.0");
    EXPECT_FALSE(state.getString(bookKey("b2")).empty());
}

TEST_F(ServerBookTest, NoPosKeyIsMadeFromAUrlAndNothingSpokenOrSavedHoldsOne) {
    start();
    syncWithFolder({dune()}, {dune()});
    ASSERT_NO_FATAL_FAILURE(openServerBook("Dune"));
    press(Button::Forward);
    ASSERT_EQ(player.file, "fake://stream/t2");
    player.pos = 12;
    hold(Button::PlayPause);
    press(Button::PlayPause);
    app->shutdown();
    bool anyPos = false;
    for (const auto& [k, v] : state.values()) {
        if (k.rfind("pos:", 0) == 0 && k.find("album:b1") != std::string::npos) anyPos = true;
        for (const std::string& text : {k, v})
            for (const char* bad : {"http", "://", "t=", "s=", "u=", "sesame", "music.example"})
                EXPECT_EQ(text.find(bad), std::string::npos) << k << " = " << v << " has " << bad;
    }
    EXPECT_TRUE(anyPos);
    EXPECT_EQ(state.getString("last_book"), ref("b1")) << "the item id, no URL";
    for (const std::string& said : speech.said)
        for (const char* bad : {"http", "://", "t=", "s=", "sesame"}) EXPECT_EQ(said.find(bad), std::string::npos) << said;
    EXPECT_FALSE(storeHolds("://"));
    EXPECT_FALSE(storeHolds("sesame"));
    EXPECT_FALSE(storeHolds("t="));
}

TEST_F(ServerBookTest, WithTheServerDownTheBookSaysServerNotReachableAndStaysInLibrary) {
    start();
    syncWithFolder({dune()}, {dune()});
    open("Library");
    open("Dune");
    ASSERT_NO_FATAL_FAILURE(fail(Kind::AlbumTracks, MusicServer::Error::Unreachable));
    EXPECT_EQ(speech.last(), "Server not reachable.");
    EXPECT_EQ(level().title, "Library");
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(player.file, "");
}

TEST_F(ServerBookTest, ABookWithNoChaptersOnTheServerSaysSo) {
    start();
    syncWithFolder({dune()}, {dune()});
    open("Library");
    open("Dune");
    answerTracks({});
    EXPECT_EQ(speech.last(), "This book has no chapters on the server.");
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(player.file, "");
}

TEST_F(ServerBookTest, AServerBookIsNotCachedAutomaticallyAndSoNeverAppearsInMusic) {
    start();
    syncWithFolder({dune()}, {dune()});
    std::vector<RemoteTrack> sized = {{"t1", "One", "Band", "Dune", 200, 1, "mp3", 5}, {"t2", "Two", "Band", "Dune", 180, 2, "mp3", 5}};
    ASSERT_NO_FATAL_FAILURE(openServerBook("Dune", sized));
    for (int i = 0; i < 20; ++i) idle(1s);
    ASSERT_TRUE(player.playing());
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(ServerBookTest, LeavingLibraryWhileABookLoadsDropsTheLateAnswer) {
    start();
    syncWithFolder({dune()}, {dune()});
    open("Library");
    open("Dune");
    auto id = lastOf(Kind::AlbumTracks);
    press(Button::Home);
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), id), server.cancelled.end());
    EXPECT_FALSE(server.reply(id, weather()));
    app->tick();
    EXPECT_EQ(player.file, "");
    EXPECT_EQ(app->mode(), App::Mode::Menu);
}

TEST_F(ServerBookTest, AServerBookOfAnotherServerIsNotListed) {
    seedServerAlbum("old", "Elsewhere", key("other"));
    indexStore.items.at(ref("old", key("other"))).kind = IndexItem::Kind::Book;
    start();
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta"}));
}

// ---------------------------------------------------------------- books of a Books server in Library (audiobookshelf.md 2.5)

class BooksServerTest : public MusicTreeTest {
protected:
    void SetUp() override {
        MusicTreeTest::SetUp();
        server.catalogueKind = MusicServer::Catalogue::Books;
    }
    // The start-up sync: a Books server's version is not looked at, the book list is always fetched.
    void syncBooks(std::vector<RemoteAlbum> books, bool more = false) {
        answerVersion("");
        answerAlbums(std::move(books), more);
    }
    static RemoteAlbum book(const std::string& id, const std::string& title) {
        RemoteAlbum a = album(id, title);
        a.artist = "Author";
        return a;
    }
    static RemoteBook bookOf(std::vector<RemoteTrack> tracks) {
        RemoteBook b;
        b.tracks = std::move(tracks);
        return b;
    }
    static RemoteTrack file(const std::string& id, double seconds) { return {id, id, "Author", "Dune", seconds, 0}; }
    // Two files (200 s and 180 s) and three chapters: Intro and Middle in file 1 (Middle at 100 s), End in file 2 at 30 s,
    // so Middle goes from file 1 into file 2.
    static RemoteBook twoFilesThreeChapters() {
        RemoteBook b = bookOf({file("t1", 200), file("t2", 180)});
        b.chapters = {{"Intro", 0}, {"Middle", 100}, {"End", 230}};
        return b;
    }
    // Home > Library > Dune, and the server gives `book`; it plays at its first chapter.
    void playBook(RemoteBook book) {
        start();
        syncBooks({BooksServerTest::book("b1", "Dune")});
        open("Library");
        open("Dune");
        ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), std::move(book)));
        app->tick();
        ASSERT_EQ(app->mode(), App::Mode::Player);
    }
    std::vector<std::string> chapterLabels() {
        press(Button::Ok); // Player menu
        open("Chapters");
        return labels();
    }
};

TEST_F(BooksServerTest, OpeningTheBookThatPlaysAgainFromLibraryKeepsTheProgressMadeSinceItOpened) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    press(Button::Forward);
    ASSERT_DOUBLE_EQ(player.pos, 100); // Middle
    player.pos = 150;
    hold(Button::Home);
    open("Library");
    open("Dune");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters()));
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 150) << "not the place saved when it was opened";
}

TEST_F(BooksServerTest, LibraryListsTheBooksAfterTheLocalOnesWithOnTheServer) {
    start();
    syncBooks({book("b2", "Zeta"), book("b1", "Dune")});
    EXPECT_EQ(countOf(Kind::MusicFolders), 0u);
    ASSERT_TRUE(indexStore.items.count(ref("b1")));
    EXPECT_EQ(indexStore.items.at(ref("b1")).kind, IndexItem::Kind::Book);
    EXPECT_EQ(indexStore.items.at(ref("b1")).source, IndexItem::Source::Server);
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta", "Dune", "Zeta"}));
    ASSERT_TRUE(level().items[2].value);
    EXPECT_EQ(level().items[2].value(), "on the server");
    press(Button::Forward);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Dune, on the server, 3 of 4");
}

TEST_F(BooksServerTest, TheBookListIsFetchedWheneverTheServerIsSyncedAndNoVersionIsKept) {
    start();
    syncBooks({book("b1", "Dune")});
    EXPECT_EQ(state.getString(versionKey()), "");
    app->shutdown();
    app.reset();
    start(false);
    ASSERT_EQ(countOf(Kind::Albums), 1u);
    state.setString(versionKey(), "v9");
    answerVersion("v9"); // even a version that matches the stored one
    EXPECT_EQ(countOf(Kind::Albums), 2u) << "the second start-up asks for the list again";
}

TEST_F(BooksServerTest, ASecondPageIsAskedWhileTheServerHasMore) {
    start();
    syncBooks({book("b1", "Dune")}, true);
    ASSERT_EQ(server.lastRequest().kind, Kind::Albums);
    EXPECT_EQ(server.lastRequest().offset, 1);
    EXPECT_EQ(server.lastRequest().size, 500);
    EXPECT_EQ(server.lastRequest().folder, "");
    EXPECT_FALSE(indexStore.items.count(ref("b1"))) << "nothing is replaced until the last page";
    answerAlbums({book("b2", "Emma")});
    EXPECT_TRUE(indexStore.items.count(ref("b1")));
    EXPECT_TRUE(indexStore.items.count(ref("b2")));
}

TEST_F(BooksServerTest, ABookTheServerDroppedIsGoneAfterTheNextSync) {
    start();
    syncBooks({book("b1", "Dune"), book("b2", "Emma")});
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("");
    answerAlbums({book("b1", "Dune")});
    EXPECT_FALSE(indexStore.items.count(ref("b2")));
    hold(Button::Home);
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta", "Dune"}));
}

TEST_F(BooksServerTest, ASyncThatChangedNothingWritesNoIndex) {
    start();
    syncBooks({book("b1", "Dune")});
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    const int saves = indexStore.saves;
    answerVersion("");
    answerAlbums({book("b1", "Dune")});
    EXPECT_EQ(indexStore.saves, saves);
    EXPECT_EQ(speech.last(), "Library updated, 2 albums, 1 book.");
}

TEST_F(BooksServerTest, MusicNeverListsTheBooks) {
    start();
    syncBooks({book("b1", "Dune")});
    open("Music");
    open("Albums A to Z");
    for (const auto& label : labels()) EXPECT_NE(label, "Dune");
    hold(Button::Home);
    open("Music");
    open("Artists");
    for (const auto& label : labels()) EXPECT_NE(label, "Author");
    hold(Button::Home);
    open("Music");
    open("Recently added");
    for (const auto& label : labels()) EXPECT_NE(label, "Dune");
}

TEST_F(BooksServerTest, RefreshCountsTheBooks) {
    start();
    syncBooks({book("b1", "Dune"), book("b2", "Emma")});
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("");
    answerAlbums({book("b1", "Dune"), book("b2", "Emma")});
    EXPECT_EQ(speech.last(), "Library updated, 2 albums, 2 books.");
}

TEST_F(BooksServerTest, RefreshWithOnlyBooksAndNoLocalAlbumsSaysTheBooks) {
    fs::remove_all(music);
    start();
    syncBooks({book("b1", "Dune"), book("b2", "Emma")});
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("");
    answerAlbums({book("b1", "Dune"), book("b2", "Emma")});
    EXPECT_EQ(speech.last(), "Library updated, 2 books.");
}

TEST_F(BooksServerTest, OpeningABookAsksTheServerForTheBookAndPlaysItsTracks) {
    start();
    syncBooks({book("b1", "Dune")});
    open("Library");
    open("Dune");
    EXPECT_EQ(speech.last(), "Loading.");
    EXPECT_EQ(countOf(Kind::AlbumTracks), 0u) << "through book(), not the tracks of an album";
    ASSERT_EQ(server.lastRequest().kind, Kind::Book);
    EXPECT_EQ(server.lastRequest().arg, "b1");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), bookOf(weather())));
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
}

TEST_F(BooksServerTest, ABookTheServerCannotGiveSaysSo) {
    start();
    syncBooks({book("b1", "Dune")});
    open("Library");
    open("Dune");
    ASSERT_NO_FATAL_FAILURE(fail(Kind::Book, MusicServer::Error::Unreachable));
    EXPECT_EQ(speech.last(), "Server not reachable.");
    EXPECT_EQ(app->mode(), App::Mode::Menu);
}

TEST_F(BooksServerTest, ABookWithNoTracksSaysItHasNoChapters) {
    start();
    syncBooks({book("b1", "Dune")});
    open("Library");
    open("Dune");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), bookOf({})));
    app->tick();
    EXPECT_EQ(speech.last(), "This book has no chapters on the server.");
    EXPECT_EQ(app->mode(), App::Mode::Menu);
}

// ---------------------------------------------------------------- server chapters in the book player (audiobookshelf.md 2.6)

TEST_F(BooksServerTest, TheChaptersListHasTheServerChaptersNotTheFiles) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    EXPECT_EQ(chapterLabels(), (std::vector<std::string>{"Intro", "Middle", "End"}));
    EXPECT_EQ(level().index, 0u) << "the chapter that plays is selected";
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
}

TEST_F(BooksServerTest, ChoosingAChapterInTheMiddleOfAFileSeeksThereAndOpensTheFileOfALaterOne) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    press(Button::Ok);
    open("Chapters");
    open("Middle");
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 100);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    press(Button::Ok);
    open("Chapters");
    EXPECT_EQ(level().index, 1u);
    open("End");
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 30);
    EXPECT_EQ(app->nowPlaying().chapter, "End");
}

TEST_F(BooksServerTest, NextChapterFromTheFirstSeeksInsideTheFirstFile) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 10;
    press(Button::Forward);
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1"})) << "no second open: it is a seek";
    EXPECT_DOUBLE_EQ(player.pos, 100);
    EXPECT_EQ(speech.last(), "Middle");
    EXPECT_EQ(app->nowPlaying().chapterIndex, 1);
    EXPECT_TRUE(player.playing());
}

TEST_F(BooksServerTest, NextChapterFromTheSecondOpensTheSecondFileAtTheRightSecond) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 150; // Middle, in file 1
    press(Button::Forward);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 30);
    EXPECT_EQ(speech.last(), "End");
    EXPECT_EQ(app->nowPlaying().chapterIndex, 2);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Last chapter.");
    EXPECT_EQ(player.file, "fake://stream/t2");
}

TEST_F(BooksServerTest, ASeekToAChapterInTheFileThatPlaysIsNotANewPlayOfTheFile) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    idle(1s);
    ASSERT_EQ(server.scrobbles.size(), 1u) << "now playing, once the file plays";
    press(Button::Forward);
    ASSERT_DOUBLE_EQ(player.pos, 100); // Middle: a seek in file 1
    idle(1s);
    EXPECT_EQ(server.scrobbles.size(), 1u);
}

TEST_F(BooksServerTest, AStreamThatFailsKeepsTheChapterAndTheEndOfChapterTimer) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 150;
    press(Button::Forward); // End, in file 2
    ASSERT_EQ(player.file, "fake://stream/t2");
    player.pos = 50;
    press(Button::Ok); // Player menu
    open("Sleep timer");
    open("End of chapter");
    player.broken = true;
    app->tick(); // the stream gave up: the player is closed, the place is kept for Play
    EXPECT_NE(app->mode(), App::Mode::Sleeping) << "the chapter did not end";
    EXPECT_EQ(app->nowPlaying().chapter, "End");
    EXPECT_EQ(app->nowPlaying().chapterIndex, 2);
}

TEST_F(BooksServerTest, AChapterGoesOnInTheNextFileUntilTheNextChapterStarts) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 150;
    press(Button::Forward);
    press(Button::Back); // End, 0 s in: the chapter before
    EXPECT_EQ(speech.last(), "Middle");
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 100);
    player.pos = 190;
    player.atEnd = true;
    app->tick(); // file 1 ends, and Middle goes on in file 2
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_EQ(app->nowPlaying().chapter, "Middle");
    EXPECT_EQ(app->nowPlaying().chapterIndex, 1);
    player.pos = 31;
    app->tick();
    EXPECT_EQ(app->nowPlaying().chapter, "End");
}

TEST_F(BooksServerTest, WhereAmINamesTheChapterAndNotTheFile) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.length = 200;
    player.pos = 150; // Middle ends at 230 s of the book: 80 s on; the book has 230 s left
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Book player. Dune, by Author. Chapter 2 of 3, Middle. 1 minute 20 seconds left in chapter. "
                             "3 minutes 50 seconds left in book. Playing.");
}

TEST_F(BooksServerTest, WhereAmIWithAStreamOfUnknownLengthUsesTheLengthsTheServerGave) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.length = 0;
    player.pos = 150;
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Book player. Dune, by Author. Chapter 2 of 3, Middle. 1 minute 20 seconds left in chapter. "
                             "3 minutes 50 seconds left in book. Playing.");
}

TEST_F(BooksServerTest, WhereAmIInAChapterThatBeganInTheFileBeforeCountsTheTimeLeftToTheNextChapter) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 150;
    press(Button::Forward); // End, file 2 at 30 s
    press(Button::Back);    // Middle again, file 1 at 100 s
    press(Button::Forward);
    player.length = 180;
    player.pos = 50; // in the last chapter: 130 s of file 2 left
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Book player. Dune, by Author. Chapter 3 of 3, End. 2 minutes 10 seconds left in chapter. "
                             "2 minutes 10 seconds left in book. Playing.");
}

TEST_F(BooksServerTest, WhereAmIInMiddleSpanningTwoFilesCountsTheTimeLeftAcrossThem) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.length = 180;
    player.pos = 190;
    player.atEnd = true;
    app->tick(); // file 1 ended; file 2 is open at 0
    player.pos = 10; // book time 210 s, in Middle, which ends at 230 s
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Book player. Dune, by Author. Chapter 2 of 3, Middle. 20 seconds left in chapter. "
                             "2 minutes 50 seconds left in book. Playing.");
}

TEST_F(BooksServerTest, PreviousChapterLongIntoAChapterThatBeganInAnEarlierFileGoesBackToItsStart) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 190;
    player.atEnd = true;
    app->tick(); // file 2 at 0: Middle, 100 s in
    player.pos = 2;
    press(Button::Back);
    EXPECT_EQ(speech.last(), "Middle") << "not the Intro: it is 102 s into Middle, not 2";
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 100);
}

TEST_F(BooksServerTest, PreviousChapterJustAfterTheStartGoesToTheChapterBefore) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 150;
    press(Button::Forward); // End at file 2, 30 s
    player.pos = 31;
    press(Button::Back);
    EXPECT_EQ(speech.last(), "Middle");
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 100);
    press(Button::Back); // at 100 s exactly: the chapter before
    EXPECT_EQ(speech.last(), "Intro");
    EXPECT_DOUBLE_EQ(player.pos, 0);
    press(Button::Back);
    EXPECT_EQ(speech.last(), "First chapter.");
}

TEST_F(BooksServerTest, PreviousChapterWellIntoAChapterRestartsIt) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 150;
    press(Button::Forward); // End: file 2, 30 s
    player.pos = 60;
    press(Button::Back);
    EXPECT_EQ(speech.last(), "End");
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 30);
    EXPECT_EQ(player.opened.size(), 2u) << "a seek in the open file";
}

TEST_F(BooksServerTest, AOneFileBookUsesTheServerChaptersAndNotTheMarksInTheFile) {
    RemoteBook b = bookOf({file("t1", 600)});
    b.chapters = {{"One", 0}, {"Two", 200}, {"Three", 400}};
    ASSERT_NO_FATAL_FAILURE(playBook(std::move(b)));
    player.embedded = {{"Mark", 0}};
    player.pos = 10;
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Two");
    EXPECT_DOUBLE_EQ(player.pos, 200);
    player.pos = 450;
    EXPECT_EQ(app->nowPlaying().chapter, "Three");
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Book player. Dune, by Author. Chapter 3 of 3, Three. 2 minutes 30 seconds left in chapter. "
                             "2 minutes 30 seconds left in book. Playing.") << "player.length is 600";
    EXPECT_EQ(chapterLabels(), (std::vector<std::string>{"One", "Two", "Three"}));
}

TEST_F(BooksServerTest, ABookWithNoServerChaptersIsOneChapterPerFile) {
    ASSERT_NO_FATAL_FAILURE(playBook(bookOf(weather())));
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Rain");
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_EQ(chapterLabels(), (std::vector<std::string>{"Storm Front", "Rain", "Sunshine"}));
}

TEST_F(BooksServerTest, ChaptersAreSortedAndAnEmptyTitleGetsANumber) {
    RemoteBook b = bookOf({file("t1", 600)});
    b.chapters = {{"Two", 200}, {"", 0}};
    ASSERT_NO_FATAL_FAILURE(playBook(std::move(b)));
    EXPECT_EQ(chapterLabels(), (std::vector<std::string>{"Chapter 1", "Two"}));
}

TEST_F(BooksServerTest, AChapterBeforeTheStartIsAtTheStartAndOneAtTheEndOfTheBookIsDropped) {
    RemoteBook b = bookOf({file("t1", 200), file("t2", 180)});
    b.chapters = {{"Early", -5}, {"Late", 100}, {"Gone", 380}, {"Beyond", 500}};
    ASSERT_NO_FATAL_FAILURE(playBook(std::move(b)));
    player.pos = 50;
    press(Button::Back);
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_EQ(chapterLabels(), (std::vector<std::string>{"Early", "Late"}));
}

TEST_F(BooksServerTest, AChapterThatStartsAHairBeforeTheEndOfAFileStartsTheNextFile) {
    RemoteBook b = bookOf({file("t1", 199.99), file("t2", 180)});
    b.chapters = {{"One", 0}, {"Two", 199.98}};
    ASSERT_NO_FATAL_FAILURE(playBook(std::move(b)));
    player.pos = 10;
    press(Button::Forward);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

TEST_F(BooksServerTest, ABookWithAFileOfUnknownLengthCannotPlaceChaptersAndUsesTheFiles) {
    RemoteBook b = bookOf({file("t1", 200), file("t2", 0)});
    b.chapters = {{"One", 0}, {"Two", 100}, {"Three", 250}};
    ASSERT_NO_FATAL_FAILURE(playBook(std::move(b)));
    EXPECT_EQ(chapterLabels(), (std::vector<std::string>{"t1", "t2"}));
}

TEST_F(BooksServerTest, AFileOfUnknownLengthIsFineInAOneFileBook) {
    RemoteBook b = bookOf({file("t1", 0)});
    b.chapters = {{"One", 0}, {"Two", 100}};
    ASSERT_NO_FATAL_FAILURE(playBook(std::move(b)));
    EXPECT_EQ(chapterLabels(), (std::vector<std::string>{"One", "Two"}));
}

TEST_F(BooksServerTest, PauseAnotherBookThenThisOneResumesTheFileAndSecond) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 150;
    press(Button::Forward); // End: file 2, 30 s
    player.pos = 50;
    press(Button::PlayPause);
    ASSERT_FALSE(player.playing());
    hold(Button::Home);
    open("Library");
    open("Alpha");
    ASSERT_EQ(app->nowPlaying().title, "Alpha");
    hold(Button::Home);
    open("Library");
    open("Dune");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters()));
    app->tick();
    EXPECT_EQ(app->nowPlaying().title, "Dune");
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 50);
    EXPECT_EQ(app->nowPlaying().chapter, "End");
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
}

TEST_F(BooksServerTest, ARestartKeepsTheServerChaptersOfTheBookToResume) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 150;
    press(Button::Forward);
    player.pos = 50;
    app->shutdown();
    app.reset();
    player = FakePlayer{};
    speech.said.clear();
    start(false);
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    syncBooks({book("b1", "Dune")});
    ASSERT_EQ(level().items[0].label, "Now playing");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Loading.");
    EXPECT_EQ(countOf(Kind::AlbumTracks), 0u);
    ASSERT_EQ(server.lastRequest().kind, Kind::Book) << "the chapters come with the book, not with the tracks of an album";
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters()));
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 50);
    EXPECT_EQ(app->nowPlaying().chapter, "End");
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
    EXPECT_EQ(speech.last(), "Now playing. Dune.");
}

TEST_F(BooksServerTest, PlayWithNothingLoadedAfterARestartAlsoGetsTheChapters) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    app->shutdown();
    app.reset();
    player = FakePlayer{};
    start(false);
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    syncBooks({book("b1", "Dune")});
    press(Button::PlayPause);
    ASSERT_EQ(server.lastRequest().kind, Kind::Book);
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters()));
    app->tick();
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
}

TEST_F(BooksServerTest, SleepAtTheEndOfAChapterInTheMiddleOfAFileWaitsForTheNextChapter) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 10;
    press(Button::Ok);
    open("Sleep timer");
    open("End of chapter");
    press(Button::Home); // back to the player
    ASSERT_EQ(app->mode(), App::Mode::Player);
    ASSERT_TRUE(player.playing());
    player.pos = 99;
    app->tick();
    EXPECT_NE(app->mode(), App::Mode::Sleeping);
    player.pos = 100.5;
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_TRUE(speech.heard("End of chapter. Sleep timer ended."));
}

TEST_F(BooksServerTest, SleepAtTheEndOfAChapterThatGoesOnInTheNextFileIgnoresTheEndOfTheFile) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 150; // Middle
    app->tick();
    press(Button::Ok);
    open("Sleep timer");
    open("End of chapter");
    press(Button::Home); // back to the player
    ASSERT_EQ(app->mode(), App::Mode::Player);
    player.pos = 150;
    ASSERT_TRUE(player.playing() || (press(Button::PlayPause), player.playing()));
    player.atEnd = true;
    app->tick();
    EXPECT_NE(app->mode(), App::Mode::Sleeping) << "Middle goes on in file 2";
    EXPECT_EQ(player.file, "fake://stream/t2");
    player.pos = 29;
    app->tick();
    EXPECT_NE(app->mode(), App::Mode::Sleeping);
    player.pos = 31;
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
}

TEST_F(BooksServerTest, SleepAtTheEndOfAChapterThatEndsWithItsFileSleepsAtOnce) {
    RemoteBook b = bookOf({file("t1", 200), file("t2", 180)});
    b.chapters = {{"One", 0}, {"Two", 200}};
    ASSERT_NO_FATAL_FAILURE(playBook(std::move(b)));
    press(Button::Ok);
    open("Sleep timer");
    open("End of chapter");
    press(Button::Home); // back to the player
    ASSERT_EQ(app->mode(), App::Mode::Player);
    player.pos = 190;
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_TRUE(speech.heard("End of chapter. Sleep timer ended."));
    EXPECT_FALSE(player.playing()) << "the next chapter did not start";
}

TEST_F(BooksServerTest, SleepAtTheEndOfTheLastChapterListedGoesOnPastTheEndOfAFileWhileFilesFollow) {
    RemoteBook b = bookOf({file("t1", 200), file("t2", 180)});
    b.chapters = {{"One", 0}, {"Two", 100}}; // Two goes to the end of the book
    ASSERT_NO_FATAL_FAILURE(playBook(std::move(b)));
    player.pos = 150;
    press(Button::Ok);
    open("Sleep timer");
    open("End of chapter");
    press(Button::Home);
    player.pos = 190;
    player.atEnd = true;
    app->tick();
    EXPECT_NE(app->mode(), App::Mode::Sleeping);
    EXPECT_EQ(player.file, "fake://stream/t2");
    player.pos = 170;
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
}

TEST_F(BooksServerTest, SleepAtTheEndOfAChapterWhenTheFileEndsBeforeTheNextChapterSleepsThere) {
    RemoteBook b = bookOf({file("t1", 200), file("t2", 180)});
    b.chapters = {{"One", 0}, {"Two", 195}}; // the file is shorter than the server said
    ASSERT_NO_FATAL_FAILURE(playBook(std::move(b)));
    press(Button::Ok);
    open("Sleep timer");
    open("End of chapter");
    press(Button::Home);
    player.holdStreams = true; // the next file waits to open: a sleep must not wait for it
    player.pos = 190;
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_FALSE(player.playing());
}

TEST_F(BooksServerTest, SleepAtTheEndOfTheLastChapterSleepsWhenTheBookEnds) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 150;
    press(Button::Forward); // End
    press(Button::Ok);
    open("Sleep timer");
    open("End of chapter");
    press(Button::Home); // back to the player
    ASSERT_EQ(app->mode(), App::Mode::Player);
    player.pos = 170;
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
}

TEST_F(BooksServerTest, NextChapterMovesTheSleepTimerToTheChapterThatPlays) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    press(Button::Ok);
    open("Sleep timer");
    open("End of chapter");
    press(Button::Home); // back to the player
    ASSERT_EQ(app->mode(), App::Mode::Player);
    press(Button::PlayPause);
    press(Button::Forward); // Middle at 100 s: the timer is for Middle now
    EXPECT_NE(app->mode(), App::Mode::Sleeping);
    player.pos = 150;
    app->tick();
    EXPECT_NE(app->mode(), App::Mode::Sleeping);
}

// ---------------------------------------------------------------- resume a server book (docs/streaming.md 4.12)

class ServerBookResumeTest : public ServerBookTest {
protected:
    // The tracks of Dune as a download of it names them.
    static std::vector<RemoteTrack> duneTracks() {
        return {{"b1-t1", "One", "Band", "Dune", 200, 1, "mp3", 5},
                {"b1-t2", "Two", "Band", "Dune", 180, 2, "mp3", 5},
                {"b1-t3", "Three", "Band", "Dune", 160, 3, "mp3", 5}};
    }
    // Dune is opened, left at chapter 2 and 77.5 s, and the App is closed and started again. The start-up
    // sync of the new App waits for an answer (the test gives it, or fails it).
    void playDuneThenRestart(std::vector<RemoteTrack> tracks = duneTracks()) {
        start();
        ASSERT_NO_FATAL_FAILURE(syncWithFolder({dune()}, {dune()}));
        ASSERT_NO_FATAL_FAILURE(openServerBook("Dune", std::move(tracks)));
        press(Button::Forward);
        ASSERT_EQ(app->nowPlaying().chapterIndex, 1) << "resume setup moved to chapter two";
        player.pos = 77.5;
        restart();
    }
    void restart() {
        app->shutdown();
        app.reset();
        player = FakePlayer{};
        speech.said.clear();
        sounds.played.clear();
        start(false);
        ASSERT_NO_FATAL_FAILURE(deliverScan());
    }
    static void expectNoUrl(const std::string& text) {
        for (const char* bad : {"http", "://", "t=", "s=", "u=", "sesame", "music.example"})
            EXPECT_EQ(text.find(bad), std::string::npos) << text << " has " << bad;
    }
};

TEST_F(ServerBookResumeTest, ExitAndRestartResumeTheServerBookAtItsChapterAndPlace) {
    ASSERT_NO_FATAL_FAILURE(playDuneThenRestart());
    EXPECT_EQ(state.getString("last_book"), ref("b1"));
    EXPECT_EQ(app->mode(), App::Mode::Menu) << "nothing plays and the server is not asked until the user chooses it";
    EXPECT_EQ(level().title, "Home");
    ASSERT_EQ(level().items[0].label, "Now playing");
    EXPECT_EQ(level().items[0].value(), "Dune");
    EXPECT_EQ(player.file, "");
    const size_t asked = countOf(Kind::AlbumTracks);
    ASSERT_NO_FATAL_FAILURE(answerVersion("v1"));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Loading.");
    ASSERT_EQ(countOf(Kind::AlbumTracks), asked + 1);
    EXPECT_EQ(server.lastRequest().arg, "b1");
    ASSERT_NO_FATAL_FAILURE(answerTracks(duneTracks()));
    EXPECT_EQ(player.file, "fake://stream/b1-t2");
    EXPECT_DOUBLE_EQ(player.pos, 77.5);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(speech.last(), "Now playing. Dune.");
}

TEST_F(ServerBookResumeTest, ContinueOnAParkedServerBookMakesItTheBookToResumeAfterARestart) {
    start();
    ASSERT_NO_FATAL_FAILURE(syncWithFolder({dune()}, {dune()}));
    ASSERT_NO_FATAL_FAILURE(openServerBook("Dune"));
    hold(Button::Home);
    open("Music");
    open("Play all"); // the music player takes over, Dune is parked
    hold(Button::Home);
    ASSERT_EQ(level().items[1].label, "Continue");
    state.setString("last_book", "/somewhere/else"); // as if another book had played meanwhile
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(state.getString("last_book"), ref("b1"));
}

TEST_F(ServerBookResumeTest, PlayWithNothingLoadedAfterARestartResumesTheServerBook) {
    ASSERT_NO_FATAL_FAILURE(playDuneThenRestart());
    ASSERT_NO_FATAL_FAILURE(answerVersion("v1"));
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Loading.");
    ASSERT_NO_FATAL_FAILURE(answerTracks(duneTracks()));
    EXPECT_EQ(player.file, "fake://stream/b1-t2");
    EXPECT_DOUBLE_EQ(player.pos, 77.5);
}

TEST_F(ServerBookResumeTest, ATrackListThatGotShorterStartsAtTheFirstChapter) {
    ASSERT_NO_FATAL_FAILURE(playDuneThenRestart());
    ASSERT_NO_FATAL_FAILURE(answerVersion("v1"));
    press(Button::Ok);
    ASSERT_NO_FATAL_FAILURE(answerTracks({duneTracks()[0]}));
    EXPECT_EQ(player.file, "fake://stream/b1-t1");
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

TEST_F(ServerBookResumeTest, WithTheServerDownADownloadedBookPlaysFromItsFilesAtOnce) {
    ASSERT_NO_FATAL_FAILURE(playDuneThenRestart());
    ASSERT_NO_FATAL_FAILURE(download("b1", "Dune"));
    ASSERT_NO_FATAL_FAILURE(fail(Kind::CatalogueVersion, MusicServer::Error::Unreachable));
    ASSERT_EQ(level().items[0].label, "Now playing");
    const size_t asked = server.requests.size();
    speech.said.clear();
    press(Button::Ok);
    EXPECT_EQ(server.requests.size(), asked) << "nothing is asked of the server";
    EXPECT_FALSE(speech.heard("Loading."));
    EXPECT_EQ(player.file, downloadedFile("b1", 2));
    EXPECT_DOUBLE_EQ(player.pos, 77.5);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->mode(), App::Mode::Player);
    for (const std::string& said : speech.said) EXPECT_EQ(said.find("not reachable"), std::string::npos) << said;
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, downloadedFile("b1", 3)) << "the next chapter comes from the disk too";
}

TEST_F(ServerBookResumeTest, WithTheServerDownABookThatIsNotDownloadedSaysSoAndHomeStays) {
    ASSERT_NO_FATAL_FAILURE(playDuneThenRestart());
    ASSERT_NO_FATAL_FAILURE(fail(Kind::CatalogueVersion, MusicServer::Error::Unreachable));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Loading.");
    ASSERT_NO_FATAL_FAILURE(fail(Kind::AlbumTracks, MusicServer::Error::Unreachable));
    EXPECT_EQ(speech.last(), "Server not reachable.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(level().title, "Home");
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(player.file, "");
    EXPECT_EQ(level().items[0].label, "Now playing") << "it can be tried again";
}

TEST_F(ServerBookResumeTest, ABookOnlyPartlyDownloadedIsAskedOfTheServerFirst) {
    ASSERT_NO_FATAL_FAILURE(playDuneThenRestart());
    ASSERT_NO_FATAL_FAILURE(download("b1", "Dune"));
    CachedList list = store().load(albumSource("b1"));
    list.tracks[2].pinned = false; // the download did not finish
    ASSERT_TRUE(store().save(list));
    ASSERT_NO_FATAL_FAILURE(answerVersion("v1"));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Loading.") << "its chapters would shift if only some played";
    ASSERT_NO_FATAL_FAILURE(fail(Kind::AlbumTracks, MusicServer::Error::Unreachable));
    EXPECT_EQ(speech.last(), "Server not reachable.");
    EXPECT_EQ(level().title, "Home");
}

TEST_F(ServerBookResumeTest, ADownloadedBookOpenedFromLibraryPlaysItsFilesWithoutAskingTheServer) {
    start();
    ASSERT_NO_FATAL_FAILURE(syncWithFolder({dune()}, {dune()}));
    ASSERT_NO_FATAL_FAILURE(download("b1", "Dune"));
    state.setString(bookKey("b1"), "0:12.0");
    const size_t asked = server.requests.size();
    open("Library");
    open("Dune");
    EXPECT_EQ(server.requests.size(), asked);
    EXPECT_EQ(player.file, downloadedFile("b1", 1));
    EXPECT_DOUBLE_EQ(player.pos, 12);
    EXPECT_EQ(state.getString("last_book"), ref("b1"));
}

TEST_F(ServerBookResumeTest, OpeningAServerBookKeepsTheMusicPlayersPlace) {
    state.setString("last_stream.server", "https://music.example");
    state.setString("last_stream.user", "ice");
    state.setString("last_stream.kind", "album");
    state.setString("last_stream.id", "al1");
    state.setString("last_stream.title", "Weather");
    state.setString("last_stream.track", "1");
    start();
    ASSERT_NO_FATAL_FAILURE(syncWithFolder({dune()}, {dune()}));
    ASSERT_NO_FATAL_FAILURE(openServerBook("Dune"));
    EXPECT_EQ(state.getString("last_stream.id"), "al1") << "the music player keeps its own place";
    EXPECT_EQ(state.getString("last_stream.title"), "Weather");
    EXPECT_EQ(state.getString("last_book"), ref("b1"));
    EXPECT_EQ(state.getString("last_player"), "book");
}

TEST_F(ServerBookResumeTest, AServerBookThatFailsToOpenChangesNeitherLastBookNorLastStream) {
    state.setString("last_stream.server", "https://music.example");
    state.setString("last_stream.user", "ice");
    state.setString("last_stream.kind", "album");
    state.setString("last_stream.id", "al1");
    start();
    ASSERT_NO_FATAL_FAILURE(syncWithFolder({dune()}, {dune()}));
    open("Library");
    open("Dune");
    ASSERT_NO_FATAL_FAILURE(fail(Kind::AlbumTracks, MusicServer::Error::Unreachable));
    EXPECT_EQ(state.getString("last_book"), "");
    EXPECT_EQ(state.getString("last_stream.id"), "al1");
    player.openSucceeds = false; // the answer comes, but no track opens
    open("Dune");
    ASSERT_NO_FATAL_FAILURE(answerTracks(weather()));
    EXPECT_EQ(state.getString("last_book"), "");
    EXPECT_EQ(state.getString("last_stream.id"), "al1");
}

TEST_F(ServerBookResumeTest, ALocalLastBookStillResumesPausedAtItsPlace) {
    state.setString("last_book", alpha().string());
    state.setString("pos:" + alpha().string(), "1:5.0");
    start();
    ASSERT_NO_FATAL_FAILURE(syncWithFolder({dune()}, {dune()}));
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(countOf(Kind::AlbumTracks), 0u);
}

TEST_F(ServerBookResumeTest, ALastStreamStillResumesAndWinsOverAnOlderServerBook) {
    state.setString("last_book", ref("b1"));
    seedServerAlbum("b1", "Dune");
    indexStore.items.at(ref("b1")).kind = IndexItem::Kind::Book;
    state.setString("last_stream.server", "https://music.example");
    state.setString("last_stream.user", "ice");
    state.setString("last_stream.kind", "album");
    state.setString("last_stream.id", "al1");
    state.setString("last_stream.title", "Weather");
    state.setString("last_stream.track", "1");
    state.setString("last_stream.track_id", "t2");
    state.setString("last_stream.pos", "42.0");
    start();
    EXPECT_EQ(level().title, "Home");
    ASSERT_EQ(level().items[0].label, "Now playing");
    EXPECT_EQ(level().items[0].value(), "Weather");
    press(Button::Ok);
    EXPECT_EQ(server.lastRequest().kind, Kind::AlbumTracks);
    EXPECT_EQ(server.lastRequest().arg, "al1");
    ASSERT_NO_FATAL_FAILURE(answerTracks(weather()));
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
}

TEST_F(ServerBookResumeTest, ALastBookOfAnotherServerOrOneThatLeftTheIndexIsNotResumed) {
    seedServerAlbum("b1", "Dune", key("other"));
    indexStore.items.at(ref("b1", key("other"))).kind = IndexItem::Kind::Book;
    state.setString("last_book", ref("b1", key("other")));
    start();
    EXPECT_NE(level().items[0].label, "Now playing");
    app->shutdown();
    app.reset();
    state.setString("last_book", ref("gone"));
    start();
    EXPECT_NE(level().items[0].label, "Now playing");
    app->shutdown();
    app.reset();
    seedServerAlbum("m1", "Weather"); // an album of this server, not a book
    state.setString("last_book", ref("m1"));
    start();
    EXPECT_NE(level().items[0].label, "Now playing");
}

TEST_F(ServerBookResumeTest, ALastBookOfAServerThisBuildCannotUseIsNotResumed) {
    seedServerAlbum("b1", "Dune");
    indexStore.items.at(ref("b1")).kind = IndexItem::Kind::Book;
    state.setString("last_book", ref("b1"));
    server.isConfigured = false;
    start();
    EXPECT_NE(level().items[0].label, "Now playing");
}

TEST_F(ServerBookResumeTest, ADownloadedServerBookIsNotInMusicAndStaysInLibrary) {
    ASSERT_NO_FATAL_FAILURE(download("b1", "Dune"));
    ASSERT_NO_FATAL_FAILURE(download("m1", "Weather"));
    start();
    ASSERT_NO_FATAL_FAILURE(syncWithFolder({dune()}, {dune(), album("m1", "Weather")}));
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Road Trip", "Weather"})) << "an album download is in Music, a book is not";
    EXPECT_EQ(indexStore.items.count("cached:" + ref("b1")), 0u);
    EXPECT_EQ(indexStore.items.count("cached:" + ref("m1")), 1u);
    hold(Button::Home);
    open("Library");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta", "Dune"}));
}

TEST_F(ServerBookResumeTest, NothingSpokenOrSavedHoldsAUrlOnTheResumePaths) {
    ASSERT_NO_FATAL_FAILURE(playDuneThenRestart());
    ASSERT_NO_FATAL_FAILURE(answerVersion("v1"));
    press(Button::Ok);
    ASSERT_NO_FATAL_FAILURE(answerTracks(duneTracks()));
    ASSERT_NO_FATAL_FAILURE(download("b1", "Dune"));
    app->shutdown();
    for (const auto& [k, v] : state.values()) {
        expectNoUrl(k);
        expectNoUrl(v);
    }
    EXPECT_EQ(state.getString("last_book"), ref("b1"));
    for (const std::string& said : speech.said) expectNoUrl(said);
    EXPECT_FALSE(storeHolds("://"));
    EXPECT_FALSE(storeHolds("sesame"));
}

// ---------------------------------------------------------------- Left / Right in menus

// Music with these albums, besides Lullaby and Road Trip: 7 Seconds, Abba Gold, Aria, Beta, Dune,
// Dusk and Zeta. Albums A to Z is then: 7 Seconds, Abba Gold, Aria, Beta, Dune, Dusk, Lullaby,
// Road Trip, Zeta.
class MenuJumpTest : public MusicTreeTest {
protected:
    void SetUp() override {
        MusicTreeTest::SetUp();
        for (const char* name : {"7 Seconds", "Abba Gold", "Aria", "Beta", "Dune", "Dusk", "Zeta"})
            localAlbum(name, "Band", name);
    }
    void openAlbums() {
        start();
        open("Music");
        open("Albums A to Z");
    }
    void goTo(const std::string& label) { // from the first item
        hold(Button::Back);
        for (size_t i = 0; i < level().items.size() && level().items[level().index].label != label; ++i) press(Button::Forward);
        ASSERT_EQ(level().items[level().index].label, label);
    }
};

TEST_F(MenuJumpTest, SortedListsMoveOneOrTenWithoutLetterAnnouncements) {
    for (int i = 0; i < 20; ++i) localAlbum("Extra " + std::to_string(i), "Band", "Extra " + std::to_string(i));
    openAlbums();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Abba Gold, 1 track, 2 of 29");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Aria, 1 track, 3 of 29");
    deep(Button::Forward);
    EXPECT_EQ(level().index, 12u);
    EXPECT_EQ(speech.last(), level().items[12].label + ", 1 track, 13 of 29");
    deep(Button::Back);
    EXPECT_EQ(speech.last(), "Aria, 1 track, 3 of 29");
    hold(Button::Back);
    EXPECT_EQ(speech.last(), "7 Seconds, 1 track, 1 of 29");
}

TEST_F(MenuJumpTest, DeepThenHoldUsesInitialDeadlineAndDoesNotRepeat) {
    for (int i = 0; i < 20; ++i) localAlbum("Extra " + std::to_string(i), "Band", "Extra " + std::to_string(i));
    openAlbums();
    app->keyDown(Button::Forward);
    clock.advance(100ms);
    app->keyDown(Button::ForwardDeep);
    EXPECT_EQ(level().index, 10u);
    clock.advance(499ms);
    app->tick();
    EXPECT_EQ(level().index, 10u);
    clock.advance(1ms);
    app->tick();
    EXPECT_EQ(level().index, 28u);
    const auto said = speech.said.size();
    clock.advance(5s);
    app->tick();
    app->keyUp(Button::ForwardDeep);
    app->keyUp(Button::Forward);
    EXPECT_EQ(speech.said.size(), said) << "no repeating end jump or trailing short action";
    app->keyDown(Button::Back);
    app->keyDown(Button::BackDeep);
    EXPECT_EQ(level().index, 18u);
    clock.advance(600ms);
    app->tick();
    EXPECT_EQ(level().index, 0u);
    app->keyUp(Button::BackDeep);
    app->keyUp(Button::Back);
}

TEST_F(MenuJumpTest, EmptyAndSingleItemListsHaveStableBoundaries) {
    std::filesystem::remove_all(root / "Alpha");
    std::filesystem::remove(beta());
    start();
    open("Library");
    ASSERT_EQ(labels(), (std::vector<std::string>{"Library is empty"}));
    for (Button key : {Button::Back, Button::Forward}) {
        press(key);
        EXPECT_EQ(speech.last(), "Library is empty, 1 of 1");
        deep(key);
        EXPECT_EQ(speech.last(), "Library is empty, 1 of 1");
        hold(key);
        EXPECT_EQ(speech.last(), "Library is empty, 1 of 1");
        EXPECT_EQ(sounds.last(), Earcon::Edge);
    }
    press(Button::Home);
    open("Podcasts");
    for (Button key : {Button::Back, Button::Forward}) {
        deep(key);
        EXPECT_EQ(speech.last(), "No podcasts yet, 1 of 1");
        EXPECT_EQ(sounds.last(), Earcon::Edge);
        hold(key);
        EXPECT_EQ(speech.last(), "No podcasts yet, 1 of 1");
        EXPECT_EQ(sounds.last(), Earcon::Edge);
    }
}

TEST_F(MenuJumpTest, ExpertSortedListSpeaksTheDestinationWithoutPositionOrLetter) {
    settings.setInt("verbosity", 2);
    openAlbums();
    deep(Button::Forward);
    EXPECT_EQ(speech.last(), "Zeta, 1 track");
}

// ---------------------------------------------------------------- the music queue

// The music player plays a queue that mixes local files and server tracks. Nothing in the menus builds
// a mixed one before Options (queue.md, step 2.4), so these tests give App one through its public
// playQueue(), as playRemote() is given a list.
class MusicQueueTest : public ScrobbleTest {
protected:
    void SetUp() override {
        ScrobbleTest::SetUp();
        settings.setString("server.url", "https://music.example"); // the server the remote entries belong to
        settings.setString("server.user", "ann");
    }
    QueueEntry localEntry(const std::string& path, const std::string& title, const std::string& album = "Mixed") const {
        QueueEntry e;
        e.path = path;
        e.title = title;
        e.album = album;
        return e;
    }
    QueueEntry remoteEntry(const std::string& id, const std::string& title, const std::string& album = "Mixed",
                           double duration = 200) const {
        QueueEntry e;
        e.kind = QueueEntry::Kind::Remote;
        e.serverUrl = "https://music.example";
        e.user = "ann";
        e.trackId = id;
        e.title = title;
        e.album = album;
        e.duration = duration;
        return e;
    }
    // Storm (local), Rain (server), Calm (local).
    std::vector<QueueEntry> mixed() const {
        return {localEntry(alphaFile(1), "Storm"), remoteEntry("t2", "Rain"), localEntry(alphaFile(2), "Calm")};
    }
    void playMixed(size_t start = 0) {
        start_();
        ASSERT_TRUE(app->playQueue("Mixed", mixed(), start));
    }
    void start_() {
        start();
        player.holdStreams = true;
    }
    // The track that plays ends; a server track then opens in the background.
    void trackEnds() {
        player.atEnd = true;
        app->tick();
    }
};

class QueueSyncTest : public MusicQueueTest {
protected:
    void begin() { server.queueSync = true; start(); }
    FakeMusicServer::RequestId queueRequest() const {
        for (auto it = server.requests.rbegin(); it != server.requests.rend(); ++it)
            if (it->kind == FakeMusicServer::Kind::PlayQueue) return it->id;
        return 0;
    }
    RemoteQueue phone(std::int64_t changed = 1700000001000LL) {
        RemoteQueue q;
        RemoteTrack a; a.id = "one"; a.title = "First"; a.duration = 100;
        RemoteTrack b; b.id = "two"; b.title = "Second"; b.duration = 100;
        q.tracks = {a, b, a}; q.current = 2; q.position = 12000; q.changed = changed;
        return q;
    }
};

TEST_F(QueueSyncTest, PhoneQueueLoadsPausedAndRestoresItsDuplicatePosition) {
    begin();
    ASSERT_NE(queueRequest(), 0u);
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone())); app->tick();
    EXPECT_FALSE(player.playing());
    ASSERT_TRUE(hasLabel("Now playing"));
    open("Now playing");
    EXPECT_EQ(player.file, "fake://stream/one");
    EXPECT_NEAR(player.pos, 12, .01);
    app->shutdown();
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->queue.position(), 2u);
    EXPECT_EQ(saved->queue.size(), 3u);
}

TEST_F(QueueSyncTest, NewLocalQueueIsSentAndMixedCurrentIsDeferred) {
    begin();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Local"), remoteEntry("one", "First"), remoteEntry("one", "Again")}, 0));
    app->tick();
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone(1699999999000LL))); app->tick();
    clock.advance(31s); app->tick();
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone(1699999999000LL))); app->tick();
    EXPECT_TRUE(server.queueSaves.empty());
    press(Button::Forward);
    clock.advance(31s); app->tick();
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone(1699999999000LL))); app->tick();
    ASSERT_EQ(server.queueSaves.size(), 1u);
    EXPECT_EQ(server.queueSaves[0].tracks.size(), 2u);
    EXPECT_EQ(server.queueSaves[0].current, 0u);
    EXPECT_EQ(server.queueSaves[0].tracks[0].id, "one");
    EXPECT_EQ(server.queueSaves[0].tracks[1].id, "one");
}

TEST_F(QueueSyncTest, OfflineQueueRetainsPendingWorkAcrossRestart) {
    begin();
    ASSERT_TRUE(app->playQueue("Remote", {remoteEntry("one", "First")}));
    app->tick();
    ASSERT_TRUE(server.fail(queueRequest(), MusicServer::Error::Unreachable)); app->tick();
    app->shutdown();
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->queue.size(), 1u);
    EXPECT_FALSE(saved->pendingServers.empty());
    app.reset();
    start();
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone(1699999999000LL))); app->tick();
    ASSERT_EQ(server.queueSaves.size(), 1u);
    EXPECT_EQ(server.queueSaves[0].tracks[0].id, "one");
}

TEST_F(QueueSyncTest, LatePhoneReplyCannotReplaceANewerLocalSelection) {
    begin();
    const auto request = queueRequest();
    ASSERT_TRUE(app->playQueue("Local", {localEntry(alphaFile(1), "Local")}));
    press(Button::PlayPause);
    hold(Button::Home);
    ASSERT_TRUE(server.replyQueue(request, phone())); app->tick();
    app->shutdown();
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    ASSERT_TRUE(saved->queue.current());
    EXPECT_EQ(saved->queue.current()->path, alphaFile(1));
}

TEST_F(QueueSyncTest, APhoneQueueDoesNotInterruptABook) {
    begin();
    open("Library"); open("Alpha");
    ASSERT_TRUE(player.playing());
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone())); app->tick();
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(player.file, alphaFile(1));
    app->shutdown();
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->queue.position(), 2u);
    EXPECT_EQ(saved->queue.current()->trackId, "one");
}

TEST_F(QueueSyncTest, AnAcknowledgedServerSubsetDoesNotReplaceTheMixedQueue) {
    begin();
    auto initial = queueRequest();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Local"), remoteEntry("one", "First")}, 1));
    app->tick();
    ASSERT_TRUE(server.replyQueue(initial, phone(1699999999000LL))); app->tick();
    clock.advance(31s); app->tick();
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone(1699999999000LL))); app->tick();
    ASSERT_EQ(server.queueSaves.size(), 1u);
    auto echo = server.queueSaves.back(); echo.changed = 1700000002000LL;
    ASSERT_TRUE(server.replyCreated(server.lastRequest().id)); app->tick();
    ASSERT_TRUE(server.replyQueue(queueRequest(), echo)); app->tick();
    app->shutdown();
    auto beforeRestart = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(beforeRestart);
    ASSERT_EQ(beforeRestart->acknowledgedServers.size(), 1u);
    EXPECT_EQ(beforeRestart->acknowledgedServers[0].second, echo.changed);
    app.reset();
    start();
    ASSERT_TRUE(server.replyQueue(queueRequest(), echo)); app->tick();
    app->shutdown();
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->queue.size(), 2u);
    EXPECT_EQ(saved->queue.at(0).path, alphaFile(1));
    EXPECT_EQ(saved->queue.at(1).trackId, "one");
}

TEST_F(QueueSyncTest, NewerMatchingPhoneSubsetStillReplacesTheMixedQueue) {
    Queue q; q.replace({localEntry(alphaFile(1), "Local"), remoteEntry("one", "First")}, 1);
    SavedQueue seed{"Mixed", q, 0};
    seed.changed = 1700000000000LL;
    seed.acknowledgedServers = {{TrackCache::serverKey("https://music.example", "ann"), 1699999999000LL}};
    ASSERT_TRUE(diskFiles().writeAtomically(queueFile, formatQueue(seed)));
    begin();
    RemoteQueue newer; RemoteTrack track; track.id = "one"; track.title = "First";
    newer.tracks = {track}; newer.changed = 1700000001000LL;
    ASSERT_TRUE(server.replyQueue(queueRequest(), newer)); app->tick();
    app->shutdown();
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    ASSERT_EQ(saved->queue.size(), 1u);
    EXPECT_EQ(saved->queue.current()->trackId, "one");
}

TEST_F(QueueSyncTest, ANewerPhoneQueueWaitsForTheMusicPlayerToBeLeft) {
    begin();
    auto initial = queueRequest();
    ASSERT_TRUE(app->playQueue("Remote", {remoteEntry("local", "Local")}));
    app->tick();
    ASSERT_TRUE(server.replyQueue(initial, phone(1699999999000LL))); app->tick();
    clock.advance(31s); app->tick();
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone(1700000010000LL))); app->tick();
    EXPECT_EQ(player.file, "fake://stream/local");
    EXPECT_TRUE(player.playing());
    press(Button::PlayPause);
    app->tick();
    EXPECT_FALSE(player.isOpen()) << "the newer queue is ready, paused";
    hold(Button::Home);
    app->tick();
    ASSERT_TRUE(hasLabel("Now playing"));
    open("Now playing");
    EXPECT_EQ(player.file, "fake://stream/one");
}

TEST_F(QueueSyncTest, UnsupportedServerDoesNotRequestQueueSync) {
    start();
    ASSERT_TRUE(app->playQueue("Remote", {remoteEntry("local", "Local")}));
    app->tick();
    EXPECT_EQ(queueRequest(), 0u);
    EXPECT_TRUE(server.queueSaves.empty());
}

TEST_F(QueueSyncTest, LocalSelectionDiscardsADeferredPhoneQueue) {
    begin();
    auto initial = queueRequest();
    ASSERT_TRUE(app->playQueue("Remote", {remoteEntry("one", "First")}));
    app->tick();
    ASSERT_TRUE(server.replyQueue(initial, phone(1699999999000LL))); app->tick();
    clock.advance(31s); app->tick();
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone(1700000010000LL))); app->tick();
    ASSERT_TRUE(app->playQueue("New", {localEntry(alphaFile(2), "New")}));
    press(Button::PlayPause);
    hold(Button::Home); app->tick();
    app->shutdown();
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    ASSERT_TRUE(saved->queue.current());
    EXPECT_EQ(saved->queue.current()->path, alphaFile(2));
}

TEST_F(QueueSyncTest, UnsetClockDefersLocalUpload) {
    clock.setWallTime(0);
    begin();
    auto initial = queueRequest();
    ASSERT_TRUE(app->playQueue("Remote", {remoteEntry("one", "First")}));
    app->tick();
    ASSERT_TRUE(server.replyQueue(initial, phone(1699999999000LL))); app->tick();
    clock.advance(31s); app->tick();
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone(1699999999000LL))); app->tick();
    EXPECT_TRUE(server.queueSaves.empty());
}

TEST_F(QueueSyncTest, UntimestampedEmptyServerQueueDefersUploadAcrossRestart) {
    begin();
    auto initial = queueRequest();
    ASSERT_TRUE(app->playQueue("Remote", {remoteEntry("one", "First")}));
    app->tick();
    RemoteQueue unknown;
    ASSERT_TRUE(server.replyQueue(initial, unknown)); app->tick();
    clock.advance(31s); app->tick();
    ASSERT_TRUE(server.replyQueue(queueRequest(), unknown)); app->tick();
    EXPECT_TRUE(server.queueSaves.empty());
    app->shutdown();
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    EXPECT_FALSE(saved->pendingServers.empty());
    app.reset();
    start();
    ASSERT_TRUE(server.replyQueue(queueRequest(), unknown)); app->tick();
    EXPECT_TRUE(server.queueSaves.empty());
    app->shutdown();
    saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    EXPECT_FALSE(saved->pendingServers.empty());
    EXPECT_EQ(saved->queue.current()->trackId, "one");
}

TEST_F(QueueSyncTest, FactoryResetCancelsTheOldSyncAndErasesPendingQueue) {
    begin();
    auto request = queueRequest();
    ASSERT_TRUE(app->playQueue("Remote", {remoteEntry("one", "First")}));
    app->tick();
    hold(Button::Home);
    open("System"); open("Factory reset"); open("Erase all settings");
    EXPECT_FALSE(server.replyQueue(request, phone()));
    auto text = diskFiles().read(queueFile).value_or("");
    EXPECT_FALSE(parseQueue(text));
}

TEST_F(QueueSyncTest, TimestampedRemoteClearRemovesParkedMusicWithoutStoppingBook) {
    begin();
    open("Library"); open("Alpha");
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone())); app->tick();
    clock.advance(31s); app->tick();
    RemoteQueue cleared; cleared.changed = 1700000002000LL;
    ASSERT_TRUE(server.replyQueue(queueRequest(), cleared)); app->tick();
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(player.file, alphaFile(1));
    app->shutdown();
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    EXPECT_TRUE(saved->queue.empty());
}

TEST_F(QueueSyncTest, RecentMusicPositionIsPendingAtShutdown) {
    Queue q; q.replace({remoteEntry("one", "First")});
    SavedQueue seed{"Remote", q, 0};
    seed.changed = 1700000000000LL;
    const auto key = TrackCache::serverKey("https://music.example", "ann");
    seed.acknowledgedServers = {{key, 1700000000000LL}};
    ASSERT_TRUE(diskFiles().writeAtomically(queueFile, formatQueue(seed)));
    begin();
    RemoteQueue echo; RemoteTrack track; track.id = "one"; track.title = "First";
    echo.tracks = {track}; echo.changed = 1700000000000LL;
    ASSERT_TRUE(server.replyQueue(queueRequest(), echo)); app->tick();
    open("Now playing");
    app->tick(); // record the queue change before advancing its place
    player.pos = 12;
    clock.setWallTime(1700000008);
    app->shutdown();
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    EXPECT_DOUBLE_EQ(saved->position, 12);
    EXPECT_EQ(saved->changed, 1700000008000LL);
    EXPECT_EQ(saved->pendingServers, std::vector<std::string>{key});
}

TEST_F(QueueSyncTest, SleepPersistsThePendingPosition) {
    begin();
    ASSERT_TRUE(app->playQueue("Remote", {remoteEntry("one", "First")}));
    player.pos = 9;
    press(Button::Power);
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved);
    EXPECT_DOUBLE_EQ(saved->position, 9);
    EXPECT_FALSE(saved->pendingServers.empty());
}

TEST_F(QueueSyncTest, ClearingALocalOnlyQueueDoesNotClearTheServerQueue) {
    begin();
    auto initial = queueRequest();
    ASSERT_TRUE(app->playQueue("Local", {localEntry(alphaFile(1), "Local")}));
    app->tick();
    ASSERT_TRUE(server.replyQueue(initial, phone(1699999999000LL))); app->tick();
    clock.advance(31s); app->tick();
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone(1699999999000LL))); app->tick();
    EXPECT_TRUE(server.queueSaves.empty());
    press(Button::Ok); open("Queue"); open("Clear queue"); open("Clear queue");
    app->tick();
    clock.advance(31s); app->tick();
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone(1699999999000LL))); app->tick();
    EXPECT_TRUE(server.queueSaves.empty());
}

TEST_F(QueueSyncTest, ASeekInProgressDoesNotSendAnOldPosition) {
    begin();
    auto initial = queueRequest();
    ASSERT_TRUE(app->playQueue("Remote", {remoteEntry("one", "First")}));
    app->tick();
    ASSERT_TRUE(server.replyQueue(initial, phone(1699999999000LL))); app->tick();
    player.pos = 5;
    player.deferSeeks = true;
    app->keyDown(Button::Back); clock.advance(700ms); app->tick();
    clock.advance(31s); app->tick();
    ASSERT_TRUE(server.replyQueue(queueRequest(), phone(1699999999000LL))); app->tick();
    EXPECT_TRUE(server.queueSaves.empty());
    app->keyUp(Button::Back);
}

TEST_F(MusicQueueTest, AMixedQueuePlaysLocalRemoteLocalInOrder) {
    playMixed();
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_EQ(app->nowPlaying().chapter, "Storm");
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
    trackEnds();
    ASSERT_EQ(player.file, "fake://stream/t2");
    finishOpen();
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
    EXPECT_FALSE(speech.heard("Loading.")) << "quiet between two tracks";
    trackEnds();
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Calm");
    expectNoUrlSpoken(speech);
}

TEST_F(MusicQueueTest, StartingInTheMiddleOfAMixedQueuePlaysFromThere) {
    playMixed(1);
    EXPECT_EQ(player.file, "fake://stream/t2");
    finishOpen();
    trackEnds();
    EXPECT_EQ(player.file, alphaFile(2));
}

TEST_F(MusicQueueTest, ARemoteTrackThatCannotBeReachedInTheMiddleOfAQueueIsSkippedAndTheQueueGoesOn) {
    playMixed();
    player.unopenable = {"fake://stream/t2"};
    trackEnds();
    finishOpen();
    EXPECT_EQ(speech.last(), "Rain, source unavailable.");
    EXPECT_EQ(player.file, alphaFile(2)) << "the local track after it plays";
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Calm");
    EXPECT_FALSE(speech.heard("Cannot play Rain. Server not reachable.")) << "the skip says it";
    expectNoUrlSpoken(speech);
}

TEST_F(MusicQueueTest, TheEndOfTheQueuePausesAtItsFirstEntry) {
    playMixed(2);
    ASSERT_EQ(player.file, alphaFile(2));
    trackEnds();
    EXPECT_EQ(speech.last(), "End of queue. Press play to start again.");
    EXPECT_EQ(sounds.last(), Earcon::Close);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(player.file, alphaFile(1)) << "the first entry is loaded";
    EXPECT_EQ(app->nowPlaying().chapter, "Storm");
    press(Button::PlayPause);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(player.file, alphaFile(1));
}

TEST_F(MusicQueueTest, TheEndOfAQueueThatStartsWithAServerTrackOpensItOnlyOnPlay) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {remoteEntry("t1", "Storm Front"), localEntry(alphaFile(2), "Calm")}));
    finishOpen();
    trackEnds();
    ASSERT_EQ(player.file, alphaFile(2));
    trackEnds();
    EXPECT_EQ(speech.last(), "End of queue. Press play to start again.");
    EXPECT_FALSE(player.isOpen()) << "no stream is opened for a paused queue";
    EXPECT_FALSE(player.opening());
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t1"), 1) << "only the first play opened it";
    EXPECT_EQ(app->nowPlaying().chapter, "Storm Front");
    press(Button::PlayPause);
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_TRUE(player.playing());
}

TEST_F(MusicQueueTest, ChoosingATrackReplacesTheQueue) {
    playMixed();
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    open("Road Trip");
    open("Sunrise");
    ASSERT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    EXPECT_EQ(app->nowPlaying().title, "Road Trip");
    EXPECT_EQ(app->nowPlaying().chapterCount, 2) << "the old entries are gone";
    trackEnds();
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    trackEnds();
    EXPECT_EQ(speech.last(), "End of queue. Press play to start again.");
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
}

TEST_F(MusicQueueTest, PlayAllReplacesTheQueueToo) {
    playMixed();
    hold(Button::Home);
    open("Music");
    open("Play all");
    EXPECT_EQ(player.file, (music / "Lullaby.mp3").string());
    EXPECT_EQ(app->nowPlaying().title, "All music");
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
}

TEST_F(MusicQueueTest, TheQueueListHasTheEntriesOfTheQueue) {
    playMixed();
    press(Button::Ok);
    open("Queue");
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"Storm", "Rain", "Calm"}));
    EXPECT_EQ(level().index, 0u);
    press(Button::Forward);
    press(Button::Ok); // Rain
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
}

TEST_F(MusicQueueTest, TheNextServerTrackIsPreOpenedNearTheEndOfALocalOne) {
    playMixed();
    player.pos = player.length - 12;
    app->tick();
    EXPECT_TRUE(player.preloads.empty()) << "not yet";
    player.pos = player.length - 5;
    app->tick();
    EXPECT_EQ(player.preloads, (std::vector<std::string>{"fake://stream/t2"}));
    trackEnds();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_EQ(player.adopted, 1);
    EXPECT_FALSE(player.opening()) << "starts at once";
    EXPECT_TRUE(player.playing());
}

TEST_F(MusicQueueTest, NothingIsPreOpenedWhenTheNextEntryIsLocal) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {remoteEntry("t1", "Storm Front"), localEntry(alphaFile(2), "Calm"),
                                         remoteEntry("t3", "Sunshine")}));
    finishOpen();
    player.pos = player.length - 5;
    app->tick();
    EXPECT_TRUE(player.preloads.empty()) << "Calm is local; Sunshine is not next";
    trackEnds();
    ASSERT_EQ(player.file, alphaFile(2));
    player.pos = player.length - 5;
    app->tick();
    EXPECT_EQ(player.preloads, (std::vector<std::string>{"fake://stream/t3"}));
}

TEST_F(MusicQueueTest, TheLastEntryPreOpensNothingNotEvenTheFirstOne) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {remoteEntry("t1", "Storm Front"), localEntry(alphaFile(2), "Calm"),
                                         remoteEntry("t3", "Sunshine")}, 2));
    finishOpen();
    player.pos = player.length - 5;
    app->tick();
    EXPECT_TRUE(player.preloads.empty()) << "the first entry is remote, but the queue ends here";
}

TEST_F(MusicQueueTest, ALocalTrackIsNeverScrobbledButAServerTrackInTheSameQueueIs) {
    playMixed();
    listen(10min);
    EXPECT_TRUE(server.scrobbles.empty()) << "Storm is a local file";
    trackEnds();
    finishOpen();
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t2")}));
    listen(310s); // more than half of the 600 s the player reports
    EXPECT_EQ(server.scrobbles, (std::vector<Scrobble>{nowPlaying("t2"), played("t2")}));
    trackEnds();
    ASSERT_EQ(player.file, alphaFile(2));
    listen(10min);
    EXPECT_EQ(server.scrobbles.size(), 2u) << "Calm is local again: nothing new";
    expectNoUrlSpoken(speech);
}




TEST_F(MusicQueueTest, LocalFilesThatCannotBeOpenedLeaveTheServerTrackThatWasPlayingForPlay) {
    start_();
    player.unopenable = {alphaFile(2), (roadTrip() / "01 - Sunrise.mp3").string()};
    ASSERT_TRUE(app->playQueue("Mixed", {remoteEntry("t2", "Rain"), localEntry(alphaFile(2), "Calm"),
                                         localEntry((roadTrip() / "01 - Sunrise.mp3").string(), "Sunrise")}));
    finishOpen();
    ASSERT_EQ(app->nowPlaying().chapter, "Rain");
    player.pos = 42;
    press(Button::Forward); // Calm: cannot be opened; neither can Sunrise
    EXPECT_TRUE(speech.heard("Cannot open 02 - Calm.mp3"));
    EXPECT_EQ(app->nowPlaying().chapter, "Rain") << "the server track is still the current one";
    player.unopenable.clear();
    press(Button::PlayPause);
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
}

TEST_F(MusicQueueTest, TheMusicQueueIsKeptWhenABookTakesTheTurnAndContinueBringsItBack) {
    playMixed();
    trackEnds();
    finishOpen();
    player.pos = 42;
    hold(Button::Home);
    open("Library");
    open("Alpha");
    ASSERT_EQ(app->screens().back().kind, Screen::Kind::BookPlayer);
    hold(Button::Home);
    ASSERT_EQ(labels()[1], "Continue");
    press(Button::Forward);
    press(Button::Ok);
    finishOpen();
    ASSERT_EQ(app->screens().back().kind, Screen::Kind::MusicPlayer);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
    trackEnds();
    EXPECT_EQ(player.file, alphaFile(2)) << "the rest of the queue follows";
}

// ---------------------------------------------------------------- unavailable sources and server checks

// Entries that cannot play are skipped, a server that fails is marked down and pinged until it answers
// (queue.md, "Playing it").
class UnavailableTest : public MusicQueueTest {
protected:
    std::string lullaby() const { return (music / "Lullaby.mp3").string(); }
    QueueEntry ofServer(const std::string& url, const std::string& user, const std::string& id, const std::string& title) const {
        QueueEntry e = remoteEntry(id, title);
        e.serverUrl = url;
        e.user = user;
        return e;
    }
    int pings() const {
        return int(std::count_if(server.requests.begin(), server.requests.end(),
                                 [](const FakeMusicServer::Request& r) { return r.kind == FakeMusicServer::Kind::Ping; }));
    }
    // The ping App sent last gets no answer.
    void failPing() {
        ASSERT_EQ(server.lastRequest().kind, FakeMusicServer::Kind::Ping);
        ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable));
        app->tick();
    }
    // Storm (local), Rain and Fog (server), Calm (local), Dusk (server): Rain cannot be opened when Storm ends,
    // so the server is marked down and Calm plays, with Dusk ahead.
    void rainFails() {
        start_();
        ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), remoteEntry("t2", "Rain"),
                                             remoteEntry("t3", "Fog"), localEntry(alphaFile(2), "Calm"),
                                             remoteEntry("t5", "Dusk")}));
        player.unopenable = {"fake://stream/t2"};
        trackEnds();
        finishOpen();
        ASSERT_EQ(player.file, alphaFile(2));
        ASSERT_TRUE(player.playing());
        player.unopenable.clear();
    }
    // Calm (local), Rain (server) plays and breaks off at 120 s: the queue waits at Rain.
    void rainBreaksOff() {
        start_();
        ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(2), "Calm"), remoteEntry("t2", "Rain")}, 1));
        finishOpen();
        player.pos = 120;
        player.broken = true;
        app->tick();
        ASSERT_EQ(speech.last(), "Server not reachable. Press play to try again.");
    }
};

TEST_F(UnavailableTest, AMissingLocalFileIsSkippedWithSourceUnavailable) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), localEntry(alphaFile(2), "Calm"),
                                         localEntry(lullaby(), "Lullaby")}));
    fs::remove(alphaFile(2));
    trackEnds();
    EXPECT_EQ(speech.last(), "Calm, source unavailable.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(player.file, lullaby());
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Lullaby");
    EXPECT_FALSE(speech.heard("Cannot open 02 - Calm.mp3"));
}

TEST_F(UnavailableTest, AQueueWhoseFirstFileIsMissingStartsAtTheNextOne) {
    start_();
    fs::remove(alphaFile(1));
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), localEntry(alphaFile(2), "Calm")}));
    EXPECT_TRUE(speech.heard("Storm, source unavailable."));
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_EQ(app->nowPlaying().chapter, "Calm");
}

TEST_F(UnavailableTest, ATrackOfAnotherServerIsSkipped) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"),
                                         ofServer("https://other.example", "ann", "t2", "Rain"), localEntry(alphaFile(2), "Calm")}));
    trackEnds();
    EXPECT_EQ(speech.last(), "Cannot play Rain. Server not set up.");
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t2"), 0) << "not even tried";
    EXPECT_TRUE(server.requests.empty() || pings() == 0) << "another server is not a failure of this one";
}

TEST_F(UnavailableTest, ATrackOfAnotherUserOfTheServerIsSkipped) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"),
                                         ofServer("https://music.example", "bob", "t2", "Rain"), localEntry(alphaFile(2), "Calm")}));
    trackEnds();
    EXPECT_EQ(speech.last(), "Cannot play Rain. Server not set up.");
    EXPECT_EQ(player.file, alphaFile(2));
}

TEST_F(UnavailableTest, ServerTracksAreSkippedWhenNoServerIsSetUp) {
    start_();
    server.isConfigured = false;
    ASSERT_TRUE(app->playQueue("Mixed", mixed()));
    trackEnds();
    EXPECT_EQ(speech.last(), "Cannot play Rain. Server not set up.");
    EXPECT_EQ(player.file, alphaFile(2));
}

TEST_F(UnavailableTest, ARunOfSkippedEntriesIsSaidOnceWithTheCount) {
    start_();
    fs::remove(alphaFile(2));
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"),
                                         ofServer("https://other.example", "ann", "t2", "Rain"), localEntry(alphaFile(2), "Calm"),
                                         ofServer("https://music.example", "bob", "t4", "Fog"), localEntry(lullaby(), "Lullaby")}));
    speech.said.clear();
    trackEnds();
    EXPECT_EQ(speech.said, (std::vector<std::string>{"3 tracks, source unavailable."}));
    EXPECT_EQ(player.file, lullaby());
}

TEST_F(UnavailableTest, WhenNothingAfterTheLastTrackCanPlayTheQueueEndsAfterTheSkip) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), ofServer("https://other.example", "ann", "t2", "Rain")}));
    trackEnds();
    const auto& said = speech.said;
    ASSERT_GE(said.size(), 2u);
    EXPECT_EQ(said[said.size() - 2], "Cannot play Rain. Server not set up.");
    EXPECT_EQ(said.back(), "End of queue. Press play to start again.");
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Storm");
}

TEST_F(UnavailableTest, ASkipIsSaidOnceEvenWhenTheNextFileCannotBeOpenedEither) {
    start_();
    player.unopenable = {alphaFile(2)};
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), ofServer("https://other.example", "ann", "t2", "Rain"),
                                         localEntry(alphaFile(2), "Calm"), localEntry(lullaby(), "Lullaby")}));
    press(Button::Forward);
    EXPECT_EQ(countOf(speech.said, "Cannot play Rain. Server not set up."), 1);
    EXPECT_TRUE(speech.heard("Cannot open 02 - Calm.mp3"));
    EXPECT_EQ(player.file, lullaby());
}

TEST_F(UnavailableTest, TheSleepTimerAtTheEndOfATrackWaitsForTheTrackThatPlaysAfterASkip) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), ofServer("https://other.example", "ann", "t2", "Rain"),
                                         localEntry(alphaFile(2), "Calm"), localEntry(lullaby(), "Lullaby")}));
    press(Button::Ok); // Player menu
    open("Sleep timer");
    open("End of track");
    ASSERT_TRUE(speech.heard("Sleep timer set to end of track."));
    while (!app->screens().back().isPlayer() && app->screens().size() > 1) press(Button::Home); // out of the menus
    ASSERT_TRUE(app->screens().back().isPlayer());
    press(Button::Forward); // Rain is skipped: Calm plays
    ASSERT_EQ(player.file, alphaFile(2));
    idle(1s);
    EXPECT_NE(app->mode(), App::Mode::Sleeping) << "Calm has only just begun";
    trackEnds();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_TRUE(speech.heard("End of track. Sleep timer ended."));
}

TEST_F(UnavailableTest, PreviousGoesBackOverAnEntryThatCannotPlay) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(lullaby(), "Lullaby"), ofServer("https://other.example", "ann", "t2", "Rain"),
                                         localEntry(alphaFile(2), "Calm")}, 2));
    press(Button::Back);
    EXPECT_TRUE(speech.heard("Cannot play Rain. Server not set up."));
    EXPECT_EQ(player.file, lullaby()) << "the track before the one that was skipped, not Calm again";
    EXPECT_EQ(speech.last(), "Lullaby");
    EXPECT_TRUE(player.playing());
}

TEST_F(UnavailableTest, PreviousWithNothingPlayableBeforeItSaysFirstTrack) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {ofServer("https://other.example", "ann", "t2", "Rain"), localEntry(alphaFile(2), "Calm")}, 1));
    press(Button::Back);
    EXPECT_EQ(speech.last(), "First track.");
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_FALSE(speech.heard("Rain, source unavailable.")) << "nothing is skipped when nothing plays";
}


TEST_F(UnavailableTest, PlayAsksTheServerWhenTheQueueWaitsAtAMissingFileWithServerTracksBehindIt) {
    rainBreaksOff(); // the server is marked down; the queue waits at Rain
    ASSERT_TRUE(app->playQueue("Again", {localEntry(alphaFile(1), "Storm"), localEntry(alphaFile(2), "Calm"),
                                         remoteEntry("t3", "Fog"), remoteEntry("t4", "Mist")}, 0));
    fs::remove(alphaFile(2));
    trackEnds(); // Calm is gone, Fog and Mist are skipped: the queue waits at Calm
    ASSERT_EQ(speech.last(), "Server not reachable. Press play to try again.");
    ASSERT_EQ(app->nowPlaying().chapter, "Calm");
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Checking the server.");
    answerPing();
    EXPECT_EQ(speech.last(), "Loading.") << "Calm is skipped and Fog opens";
    EXPECT_TRUE(speech.heard("Calm, source unavailable."));
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_TRUE(player.playing());
}

TEST_F(UnavailableTest, PlayDoesNotAskTheServerWhenNoServerTrackIsLeftAhead) {
    rainFails(); // the server is marked down
    ASSERT_TRUE(app->playQueue("Again", {localEntry(alphaFile(1), "Storm"), localEntry(alphaFile(2), "Calm")}, 1));
    fs::remove(alphaFile(1));
    trackEnds(); // the end of the queue: it waits at Storm, which is gone
    ASSERT_EQ(speech.last(), "End of queue. Press play to start again.");
    const int asked = pings();
    press(Button::PlayPause);
    EXPECT_EQ(pings(), asked);
    EXPECT_TRUE(speech.heard("Storm, source unavailable."));
    EXPECT_EQ(player.file, alphaFile(2));
}

TEST_F(UnavailableTest, OnlyTracksOfAnotherUserAheadAreNotWorthPinging) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), remoteEntry("t2", "Rain"), localEntry(alphaFile(2), "Calm"),
                                         ofServer("https://music.example", "bob", "t4", "Fog")}));
    player.unopenable = {"fake://stream/t2"};
    trackEnds();
    finishOpen();
    ASSERT_EQ(player.file, alphaFile(2));
    idle(10min);
    EXPECT_EQ(pings(), 0);
}

TEST_F(UnavailableTest, WithoutAServerSetUpNothingIsPinged) {
    rainFails();
    server.isConfigured = false;
    idle(10min);
    EXPECT_EQ(pings(), 0);
}

TEST_F(UnavailableTest, AnotherFailureWhileTheServerIsMarkedDownDoesNotRestartThePings) {
    rainFails();
    idle(10s);
    failPing();
    idle(30s);
    ASSERT_EQ(pings(), 2);
    failPing(); // the next one is 60 s from now
    player.unopenable = {"fake://stream/t4"};
    ASSERT_TRUE(app->playQueue("Again", {remoteEntry("t4", "Mist")})); // the user tries; it fails again
    finishOpen();
    ASSERT_EQ(speech.last(), "Cannot play Mist. Server not reachable.");
    idle(59900ms);
    EXPECT_EQ(pings(), 2) << "not 10 s after that failure";
    idle(100ms);
    EXPECT_EQ(pings(), 3);
}

TEST_F(UnavailableTest, AfterOneFailedOpenTheNextServerTracksAreSkippedWithoutAnOpenAndALocalOnePlays) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), remoteEntry("t2", "Rain"),
                                         remoteEntry("t3", "Fog"), localEntry(alphaFile(2), "Calm")}));
    player.unopenable = {"fake://stream/t2"};
    trackEnds();
    ASSERT_TRUE(player.opening());
    finishOpen();
    EXPECT_EQ(speech.last(), "2 tracks, source unavailable.") << "the one that failed and the one behind it";
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t2"), 1);
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t3"), 0) << "no second wait on the server";
    expectNoUrlSpoken(speech);
}

TEST_F(UnavailableTest, AServerTrackThatFailsAtOnceMarksTheServerDownToo) {
    start_();
    player.holdStreams = false; // the open fails in the call, not in the background
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), remoteEntry("t2", "Rain"),
                                         remoteEntry("t3", "Fog"), localEntry(alphaFile(2), "Calm")}));
    player.unopenable = {"fake://stream/t2"};
    trackEnds();
    EXPECT_EQ(speech.last(), "2 tracks, source unavailable.");
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t3"), 0);
}

TEST_F(UnavailableTest, ADownloadedTrackStillPlaysWhileTheServerIsMarkedDown) {
    cache = base / "cache";
    TrackCache store(diskFiles(), cache, "https://music.example", "ann");
    fs::create_directories(store.listFolder(albumSource("weather")));
    std::vector<CachedTrack> tracks;
    for (std::string id : {"t3", "t4"}) {
        touch(store.listFolder(albumSource("weather")) / (id + ".mp3"));
        tracks.push_back({id, id + ".mp3", id, 100, "", true, 0});
    }
    ASSERT_TRUE(store.save(cachedAlbum("weather", tracks)));
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), remoteEntry("t2", "Rain"),
                                         remoteEntry("t3", "Fog"), remoteEntry("t4", "Mist"),
                                         remoteEntry("t5", "Dusk")}));
    player.unopenable = {"fake://stream/t2"};
    trackEnds();
    finishOpen(); // Rain fails: the server is marked down
    const std::string fog = (store.listFolder(albumSource("weather")) / "t3.mp3").string();
    EXPECT_EQ(player.file, fog) << "downloaded: no wait on the server";
    EXPECT_TRUE(player.playing());
    trackEnds();
    EXPECT_EQ(player.file, (store.listFolder(albumSource("weather")) / "t4.mp3").string());
    trackEnds();
    EXPECT_EQ(speech.last(), "Server not reachable. Press play to try again.") << "Dusk is not downloaded";
}

TEST_F(UnavailableTest, SkippingToAMarkedDownServerTrackWithAKeyPassesItToo) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", mixed()));
    player.unopenable = {"fake://stream/t2"};
    press(Button::Forward); // Rain: tried, because the user asked for it
    finishOpen();
    ASSERT_EQ(speech.last(), "Cannot play Rain. Server not reachable.");
    ASSERT_EQ(app->nowPlaying().chapter, "Storm");
    player.unopenable.clear();
    press(Button::Forward); // Rain again: the server is marked down now, so Calm plays
    EXPECT_TRUE(speech.heard("Rain, source unavailable."));
    EXPECT_EQ(speech.last(), "Calm") << "the track that plays, not the one that was skipped";
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t2"), 1);
}

TEST_F(UnavailableTest, AnAnsweredPingClearsTheMarkAndTheNextServerTrackOpens) {
    rainFails();
    idle(10s);
    ASSERT_EQ(pings(), 1);
    answerPing();
    EXPECT_TRUE(player.playing()) << "the ping does not interrupt what plays";
    EXPECT_EQ(player.file, alphaFile(2));
    trackEnds(); // Calm ends: Dusk is next
    EXPECT_EQ(player.file, "fake://stream/t5") << "no longer skipped";
    finishOpen();
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Dusk");
}

TEST_F(UnavailableTest, WithoutAnAnswerTheNextServerTrackStaysSkipped) {
    rainFails();
    idle(10s);
    failPing();
    trackEnds(); // Calm ends: Dusk is skipped, and nothing else is left
    EXPECT_EQ(speech.last(), "Server not reachable. Press play to try again.");
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Dusk") << "the queue waits at the first entry it skipped";
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t5"), 0);
}

TEST_F(UnavailableTest, TheServerIsPingedAfter10Then30ThenEvery60Seconds) {
    rainFails();
    idle(9900ms);
    EXPECT_EQ(pings(), 0);
    idle(100ms);
    ASSERT_EQ(pings(), 1);
    failPing();
    idle(29900ms);
    EXPECT_EQ(pings(), 1);
    idle(100ms);
    ASSERT_EQ(pings(), 2);
    failPing();
    idle(59900ms);
    EXPECT_EQ(pings(), 2);
    idle(100ms);
    ASSERT_EQ(pings(), 3);
    failPing();
    idle(59900ms);
    EXPECT_EQ(pings(), 3);
    idle(100ms);
    EXPECT_EQ(pings(), 4);
}

TEST_F(UnavailableTest, NoSecondPingIsSentWhileOneWaitsForTheServer) {
    rainFails();
    idle(10s);
    ASSERT_EQ(pings(), 1);
    idle(5min);
    EXPECT_EQ(pings(), 1);
}

TEST_F(UnavailableTest, PingsStopWhenNoServerTrackIsAhead) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", mixed()));
    player.unopenable = {"fake://stream/t2"};
    trackEnds();
    finishOpen();
    ASSERT_EQ(player.file, alphaFile(2)) << "Calm plays; Rain is behind it";
    idle(10min);
    EXPECT_EQ(pings(), 0);
}

TEST_F(UnavailableTest, PingsStartAgainWhenAServerTrackIsAheadAgain) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", mixed()));
    player.unopenable = {"fake://stream/t2"};
    trackEnds();
    finishOpen();
    idle(10min);
    ASSERT_EQ(pings(), 0);
    ASSERT_TRUE(app->playQueue("Again", {localEntry(alphaFile(1), "Storm"), remoteEntry("t3", "Fog")}));
    idle(9900ms);
    EXPECT_EQ(pings(), 0) << "the schedule starts at 10 s again";
    idle(100ms);
    EXPECT_EQ(pings(), 1);
}

TEST_F(UnavailableTest, OnlyServerTracksLeftStopsAtTheFirstOneAndPlayPingsAndPlays) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), remoteEntry("t2", "Rain"),
                                         remoteEntry("t3", "Fog")}));
    player.unopenable = {"fake://stream/t2"};
    trackEnds();
    finishOpen();
    EXPECT_EQ(speech.last(), "Server not reachable. Press play to try again.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_FALSE(player.playing());
    EXPECT_FALSE(player.isOpen());
    EXPECT_EQ(app->nowPlaying().chapter, "Rain") << "the first one that was skipped";
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t3"), 0);
    player.unopenable.clear();
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Checking the server.");
    EXPECT_FALSE(player.playing()) << "it waits for the answer";
    answerPing();
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_TRUE(player.playing());
    trackEnds();
    EXPECT_EQ(player.file, "fake://stream/t3") << "the server is no longer marked down";
}

TEST_F(UnavailableTest, PlayWhenTheServerStillDoesNotAnswerStopsAgainAndKeepsThePlace) {
    rainBreaksOff();
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Checking the server.");
    failPing();
    EXPECT_EQ(speech.last(), "Server not reachable. Press play to try again.");
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
    press(Button::PlayPause);
    answerPing();
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 120) << "where it broke off";
    EXPECT_TRUE(player.playing());
}

TEST_F(UnavailableTest, PlayWhenTheServerDoesNotAnswerGoesOnWithWhatCanPlay) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(2), "Calm"), remoteEntry("t2", "Rain"),
                                         localEntry(alphaFile(1), "Storm")}, 1));
    finishOpen();
    player.pos = 120;
    player.broken = true;
    app->tick();
    press(Button::PlayPause);
    failPing();
    EXPECT_EQ(speech.last(), "Rain, source unavailable.");
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_TRUE(player.playing());
}

TEST_F(UnavailableTest, AStreamThatBreaksOffMarksTheServerDownAndItIsPinged) {
    rainBreaksOff();
    idle(9900ms);
    EXPECT_EQ(pings(), 0);
    idle(100ms);
    ASSERT_EQ(pings(), 1);
    answerPing();
    EXPECT_FALSE(player.playing()) << "an answer does not start the music";
    press(Button::PlayPause);
    EXPECT_NE(speech.last(), "Checking the server.") << "the server answered: Play opens the track";
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 120);
    EXPECT_TRUE(player.playing());
}

TEST_F(UnavailableTest, ChoosingAQueueTriesAMarkedDownServerAndAnOpenThatWorksClearsTheMark) {
    rainFails();
    ASSERT_TRUE(app->playQueue("Again", {remoteEntry("t4", "Mist"), remoteEntry("t5", "Dusk")}));
    EXPECT_EQ(player.file, "fake://stream/t4") << "the user chose it";
    finishOpen();
    ASSERT_TRUE(player.playing());
    trackEnds();
    EXPECT_EQ(player.file, "fake://stream/t5") << "the open worked, so the server is not marked down any more";
    finishOpen();
    EXPECT_TRUE(player.playing());
    idle(10min);
    EXPECT_EQ(pings(), 0);
}

TEST_F(UnavailableTest, AnOpenThatWorksAtOnceClearsTheMarkToo) {
    start_();
    player.holdStreams = false;
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), remoteEntry("t2", "Rain"),
                                         localEntry(alphaFile(2), "Calm")}));
    player.unopenable = {"fake://stream/t2"};
    trackEnds();
    ASSERT_EQ(speech.last(), "Rain, source unavailable.");
    player.unopenable.clear();
    ASSERT_TRUE(app->playQueue("Again", {remoteEntry("t4", "Mist"), remoteEntry("t5", "Dusk")}));
    ASSERT_TRUE(player.playing());
    trackEnds();
    EXPECT_EQ(player.file, "fake://stream/t5");
    EXPECT_TRUE(player.playing());
}

TEST_F(UnavailableTest, TheServerTrackAfterAMarkedDownOneIsNotPreOpened) {
    rainFails();
    player.pos = player.length - 5;
    app->tick();
    EXPECT_TRUE(player.preloads.empty()) << "Dusk is next, but the server is marked down";
}

TEST_F(UnavailableTest, TheServerTrackOfAnotherServerIsNotPreOpened) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), ofServer("https://other.example", "ann", "t2", "Rain")}));
    player.pos = player.length - 5;
    app->tick();
    EXPECT_TRUE(player.preloads.empty());
}

TEST_F(UnavailableTest, TheEndOfAQueueWhoseFirstFileIsMissingWaitsThereAndPlaySkipsIt) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), localEntry(alphaFile(2), "Calm")}, 1));
    fs::remove(alphaFile(1));
    trackEnds();
    EXPECT_EQ(speech.last(), "End of queue. Press play to start again.");
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Storm");
    EXPECT_FALSE(speech.heard("Cannot open 01 - Storm.mp3"));
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Storm, source unavailable.");
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_TRUE(player.playing());
}

TEST_F(UnavailableTest, ChoosingANewQueueWhilePlayAsksTheServerDropsTheAnswer) {
    rainBreaksOff();
    press(Button::PlayPause);
    auto ping = server.lastRequest().id;
    ASSERT_TRUE(app->playQueue("Again", {localEntry(alphaFile(1), "Storm"), localEntry(alphaFile(2), "Calm")}));
    EXPECT_FALSE(server.replyPing(ping, {"navidrome", "0.64.2"})) << "cancelled";
    app->tick();
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Storm");
}

TEST_F(UnavailableTest, SleepWhilePlayAsksTheServerDropsTheAnswer) {
    rainBreaksOff();
    press(Button::PlayPause);
    auto ping = server.lastRequest().id;
    idle(6min); // automatic sleep
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_FALSE(server.replyPing(ping, {"navidrome", "0.64.2"})) << "cancelled";
    app->tick();
    EXPECT_FALSE(player.playing()) << "nothing starts while the device sleeps";
}

TEST_F(UnavailableTest, PowerOffWhilePlayAsksTheServerDropsTheAnswer) {
    rainBreaksOff();
    press(Button::PlayPause);
    auto ping = server.lastRequest().id;
    hold(Button::Power);
    open("Power off");
    ASSERT_TRUE(app->quitRequested());
    EXPECT_FALSE(server.replyPing(ping, {"navidrome", "0.64.2"})) << "cancelled";
    app->tick();
    EXPECT_FALSE(player.playing());
}

TEST_F(UnavailableTest, RestartWhilePlayAsksTheServerDropsTheAnswer) {
    rainBreaksOff();
    press(Button::PlayPause);
    auto ping = server.lastRequest().id;
    hold(Button::Power);
    open("Restart");
    EXPECT_FALSE(server.replyPing(ping, {"navidrome", "0.64.2"})) << "cancelled";
    app->tick();
    EXPECT_FALSE(player.playing()) << "nothing starts by itself after a restart";
}

TEST_F(UnavailableTest, NoPingsWhileTheDeviceSleeps) {
    rainBreaksOff();
    idle(6min); // automatic sleep, before the first ping is due: the tick that sleeps sends none
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    idle(10min);
    EXPECT_EQ(pings(), 0);
}

// ---------------------------------------------------------------- Options: Play now, Play next, Add to queue

class OptionsTest : public MusicTreeTest {
protected:
    void SetUp() override {
        MusicTreeTest::SetUp();
        player.holdStreams = true;
    }
    template <class T> void answer(std::vector<T> items) {
        ASSERT_TRUE(server.reply(server.lastRequest().id, std::move(items)));
        app->tick();
    }
    void finishOpen() {
        ASSERT_TRUE(player.finishOpen()) << "no open pending";
        app->tick();
    }
    // Options on the selected item, then `label` on the Options screen.
    void options(const std::string& label) {
        press(Button::Options);
        ASSERT_EQ(level().title.rfind("Options, ", 0), 0u) << level().title;
        open(label);
    }
    // Home > Music > Albums A to Z > Road Trip, on its first track.
    void openRoadTrip() {
        start();
        open("Music");
        open("Albums A to Z");
        open("Road Trip");
    }
    // Plays Road Trip from Sunrise: the queue is Sunrise, Highway.
    void playRoadTrip() {
        openRoadTrip();
        open("Sunrise");
        ASSERT_EQ(app->mode(), App::Mode::Player);
    }
    // Home > Server > Recently added, with Weather in it (on it).
    void openServerAlbums() {
        open("Server");
        open("Recently added");
        answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    }
    // ... > Weather, with its tracks.
    void openWeather() {
        openServerAlbums();
        open("Weather");
        answer(weather());
    }
    // The titles of the queue, from the Queue list of the Player menu, and "Previous queue" when it has one;
    // ends at Home.
    std::vector<std::string> tracksList() {
        hold(Button::Home);
        open("Now playing");
        press(Button::Ok);
        open("Queue");
        std::vector<std::string> out = queueEntries();
        if (labels().size() > out.size() + 2) out.push_back("Previous queue"); // after Save as playlist, before Clear queue
        hold(Button::Home);
        return out;
    }
    // ... without "Previous queue", which ends that list when there is one.
    std::vector<std::string> queue() {
        std::vector<std::string> out = tracksList();
        if (!out.empty() && out.back() == "Previous queue") out.pop_back();
        return out;
    }
};

TEST_F(OptionsTest, OptionsOnALocalTrackOpensItsActionsAndPlayNowReplacesTheQueue) {
    openRoadTrip();
    press(Button::Forward);
    press(Button::Options);
    EXPECT_EQ(speech.last(), "Options, Highway. Play now, 1 of 3");
    EXPECT_EQ(sounds.last(), Earcon::Open);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play now", "Play next", "Add to queue"}));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Playing Highway.");
    EXPECT_NE(std::find(sounds.played.rbegin(), sounds.played.rbegin() + 3, Earcon::Close), sounds.played.rbegin() + 3)
        << "the Options screen is left with the close sound";
    EXPECT_EQ(level().title, "Road Trip") << "back in the list by itself";
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(queue(), (std::vector<std::string>{"Highway"}));
}

TEST_F(OptionsTest, PlayNextOnALocalTrackPutsItAfterTheCurrentEntry) {
    playRoadTrip();
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Lullaby") press(Button::Forward); // a loose track
    options("Play next");
    EXPECT_EQ(speech.last(), "Added Lullaby. 3 in queue.");
    EXPECT_EQ(level().title, "Albums A to Z");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Lullaby", "Highway"}));
}

TEST_F(OptionsTest, AddToQueueOnALocalTrackPutsItAtTheEnd) {
    playRoadTrip();
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    open("Road Trip");
    press(Button::Options);
    options("Add to queue");
    EXPECT_EQ(speech.last(), "Added Sunrise. 3 in queue.");
    EXPECT_EQ(level().title, "Road Trip");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Highway", "Sunrise"}));
    EXPECT_TRUE(player.playing()) << "what plays goes on";
}

TEST_F(OptionsTest, PlayNowOnALocalAlbumReplacesTheQueueWithAllItsTracks) {
    playRoadTrip();
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Road Trip") press(Button::Forward);
    press(Button::Options);
    EXPECT_EQ(speech.last(), "Options, Road Trip. Play now, 1 of 4");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Playing Road Trip.");
    EXPECT_EQ(level().title, "Albums A to Z");
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Highway"}));
}

TEST_F(OptionsTest, PlayNextAndAddToQueueOnALocalAlbumPutTheTracksAfterTheCurrentOrAtTheEnd) {
    start();
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Lullaby") press(Button::Forward);
    press(Button::Ok); // Lullaby plays
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Road Trip") press(Button::Forward);
    options("Play next");
    EXPECT_EQ(speech.last(), "Added Road Trip, 2 tracks. 3 in queue.");
    EXPECT_EQ(level().title, "Albums A to Z");
    options("Add to queue");
    EXPECT_EQ(speech.last(), "Added Road Trip, 2 tracks. 5 in queue.");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Lullaby", "Sunrise", "Highway", "Sunrise", "Highway"}));
}

TEST_F(OptionsTest, AddToQueueWithShuffleTracksMixesTheNewTracksIntoWhatHasNotPlayed) {
    playRoadTrip(); // Sunrise, Highway
    app->setRandom([](size_t) { return size_t(0); }); // each new track goes right after the current one
    press(Button::Ok);
    open("Shuffle");
    open("tracks");
    hold(Button::Home);
    open("Music");
    ASSERT_EQ(level().items[level().index].label, "Play all");
    options("Add to queue");
    EXPECT_EQ(speech.last(), "Added All music, 3 tracks. 5 in queue.") << "what is spoken is as before";
    // The three new tracks (in the order of All music, Lullaby, Sunrise, Highway, the zeros turn it round) are
    // not all after the old Highway, which had not played yet.
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Highway", "Sunrise", "Lullaby", "Highway"}));
}

TEST_F(OptionsTest, PlayAllHasOptionsToo) {
    playRoadTrip();
    hold(Button::Home);
    open("Music");
    ASSERT_EQ(level().items[level().index].label, "Play all");
    options("Add to queue");
    EXPECT_EQ(speech.last(), "Added All music, 3 tracks. 5 in queue.");
    EXPECT_EQ(level().title, "Music");
}

TEST_F(OptionsTest, ARemoteTrackIsPlayedNowPlayedNextAndAddedAlone) {
    playRoadTrip();
    hold(Button::Home);
    openWeather();
    while (level().items[level().index].label != "Rain") press(Button::Forward);
    options("Play next");
    EXPECT_EQ(speech.last(), "Added Rain. 3 in queue.");
    EXPECT_EQ(level().title, "Weather");
    while (level().items[level().index].label != "Sunshine") press(Button::Forward);
    options("Add to queue");
    EXPECT_EQ(speech.last(), "Added Sunshine. 4 in queue.");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Rain", "Highway", "Sunshine"}));
    hold(Button::Home);
    openWeather();
    while (level().items[level().index].label != "Sunshine") press(Button::Forward);
    options("Play now");
    EXPECT_EQ(speech.last(), "Loading.");
    EXPECT_EQ(level().title, "Weather") << "back in the list at once";
    finishOpen();
    EXPECT_EQ(speech.last(), "Playing Sunshine.");
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunshine"}));
    expectNoUrlSpoken(speech);
}

TEST_F(OptionsTest, ARemoteAlbumIsLoadedFirstThenAddedWithTheCounts) {
    playRoadTrip();
    hold(Button::Home);
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    options("Add to queue");
    EXPECT_EQ(speech.last(), "Loading.");
    EXPECT_EQ(level().title, "Options, Weather") << "waits on the Options screen";
    EXPECT_EQ(server.lastRequest().kind, Kind::AlbumTracks);
    EXPECT_EQ(server.lastRequest().arg, "al1");
    answer(weather());
    EXPECT_EQ(speech.last(), "Added Weather, 3 tracks. 5 in queue.");
    EXPECT_EQ(level().title, "Recently added");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Highway", "Storm Front", "Rain", "Sunshine"}));
}

TEST_F(OptionsTest, PlayNextAndPlayNowOnARemoteAlbum) {
    playRoadTrip();
    hold(Button::Home);
    openServerAlbums();
    options("Play next");
    answer(weather());
    EXPECT_EQ(speech.last(), "Added Weather, 3 tracks. 5 in queue.");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Storm Front", "Rain", "Sunshine", "Highway"}));
    hold(Button::Home);
    openServerAlbums();
    options("Play now");
    answer(weather());
    EXPECT_EQ(speech.last(), "Loading.");
    finishOpen();
    EXPECT_EQ(speech.last(), "Playing Weather.");
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_EQ(level().title, "Recently added");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Storm Front", "Rain", "Sunshine"}));
}

TEST_F(OptionsTest, ARemotePlaylistIsAddedWithItsTracks) {
    indexStore.playlists = {{key(), "p1", "Road songs", 3, 0}};
    playRoadTrip();
    hold(Button::Home);
    open("Music");
    open("Playlists");
    options("Add to queue");
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
    EXPECT_EQ(server.lastRequest().arg, "p1");
    answer(weather());
    EXPECT_EQ(speech.last(), "Added Road songs, 3 tracks. 5 in queue.");
    EXPECT_EQ(level().title, "Playlists");
}

TEST_F(OptionsTest, ARemoteArtistLoadsItsAlbumsThenTheirTracks) {
    playRoadTrip();
    hold(Button::Home);
    open("Server");
    open("Artists");
    answer(std::vector<RemoteArtist>{{"ar1", "Band", 2}});
    options("Add to queue");
    EXPECT_EQ(server.lastRequest().kind, Kind::ArtistAlbums);
    EXPECT_EQ(server.lastRequest().arg, "ar1");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}, {"al2", "Storm", "Band", 1}});
    EXPECT_EQ(server.lastRequest().arg, "al1");
    EXPECT_EQ(level().title, "Options, Band") << "still loading";
    answer(weather());
    EXPECT_EQ(server.lastRequest().arg, "al2");
    answer(std::vector<RemoteTrack>{{"t9", "Thunder", "Band", "Storm", 100, 1}});
    EXPECT_EQ(speech.last(), "Added Band, 4 tracks. 6 in queue.");
    EXPECT_EQ(level().title, "Artists");
}

TEST_F(OptionsTest, HomeWhileARemoteAlbumLoadsAddsNothingAndSaysNothing) {
    playRoadTrip();
    hold(Button::Home);
    openServerAlbums();
    options("Add to queue");
    auto id = server.lastRequest().id;
    press(Button::Home); // leaves the Options screen
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), id), server.cancelled.end());
    EXPECT_FALSE(server.reply(id, weather())) << "the answer is dropped";
    speech.said.clear();
    app->tick();
    EXPECT_TRUE(speech.said.empty());
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Highway"}));
}

TEST_F(OptionsTest, HoldingHomeWhileAnArtistLoadsDropsTheLaterAlbumsToo) {
    playRoadTrip();
    hold(Button::Home);
    open("Server");
    open("Artists");
    answer(std::vector<RemoteArtist>{{"ar1", "Band", 2}});
    options("Play next");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    auto id = server.lastRequest().id;
    hold(Button::Home);
    EXPECT_FALSE(server.reply(id, weather()));
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Highway"}));
}

TEST_F(OptionsTest, PlayNextWithNothingLoadedMakesAQueuePausedAtItsFirstEntry) {
    start();
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Road Trip") press(Button::Forward);
    options("Play next");
    EXPECT_EQ(speech.last(), "Added Road Trip, 2 tracks. Press play.");
    EXPECT_EQ(level().title, "Albums A to Z");
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().title, "Road Trip");
    press(Button::PlayPause);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
}

TEST_F(OptionsTest, AddToQueueWithNothingLoadedOpensNoStreamUntilPlay) {
    start();
    openWeather();
    while (level().items[level().index].label != "Rain") press(Button::Forward);
    options("Add to queue");
    EXPECT_EQ(speech.last(), "Added Rain. Press play.");
    EXPECT_FALSE(player.isOpen());
    EXPECT_FALSE(player.opening());
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Loading.");
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_TRUE(player.playing());
}

TEST_F(OptionsTest, AnAudiobookKeepsPlayingWhileTracksAreAddedAndTheyWaitForContinue) {
    playAlpha();
    ASSERT_TRUE(player.playing());
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Road Trip") press(Button::Forward);
    options("Add to queue");
    EXPECT_EQ(speech.last(), "Added Road Trip, 2 tracks. 2 in queue.");
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(player.file, alphaFile(1)) << "the book goes on";
    hold(Button::Home);
    open("Continue");
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    EXPECT_EQ(app->nowPlaying().chapterCount, 2);
}

TEST_F(OptionsTest, TracksAddedWhileABookPlaysJoinTheQueueThatWaits) {
    playAlpha();
    for (const char* action : {"Add to queue", "Play next"}) {
        hold(Button::Home);
        open("Music");
        open("Albums A to Z");
        while (level().items[level().index].label != "Road Trip") press(Button::Forward);
        options(action);
    }
    EXPECT_EQ(speech.last(), "Added Road Trip, 2 tracks. 4 in queue.");
    hold(Button::Home);
    open("Continue");
    EXPECT_EQ(app->nowPlaying().chapterCount, 4);
}

TEST_F(OptionsTest, AScreenOpenedOverTheOptionsWhileAnAlbumLoadsDropsTheAnswer) {
    state.setString("last_stream.server", "https://music.example");
    state.setString("last_stream.user", "ice");
    state.setString("last_stream.kind", "album");
    state.setString("last_stream.id", "al1");
    state.setString("last_stream.title", "Weather");
    state.setString("last_stream.track", "0");
    state.setString("last_stream.track_id", "t1");
    state.setString("last_stream.pos", "0.0");
    start();
    openServerAlbums();
    options("Add to queue");
    auto add = server.lastRequest().id;
    press(Button::PlayPause); // resumes Weather: its player opens over the Options screen
    answer(weather());
    finishOpen();
    ASSERT_EQ(app->mode(), App::Mode::Player);
    EXPECT_FALSE(server.reply(add, weather())) << "cancelled";
    speech.said.clear();
    app->tick();
    EXPECT_TRUE(speech.said.empty());
}

TEST_F(OptionsTest, ALoadThatFailsSaysSoAndStaysOnTheOptions) {
    playRoadTrip();
    hold(Button::Home);
    openServerAlbums();
    options("Add to queue");
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(speech.last(), "Server not reachable.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(level().title, "Options, Weather");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Highway"}));
}

TEST_F(OptionsTest, AnAlbumWithNoTracksAddsNothing) {
    playRoadTrip();
    hold(Button::Home);
    openServerAlbums();
    options("Play now");
    answer(std::vector<RemoteTrack>{});
    EXPECT_EQ(speech.last(), "Weather has no tracks.");
    EXPECT_EQ(level().title, "Options, Weather");
    EXPECT_TRUE(player.playing()) << "what played goes on";
}

TEST_F(OptionsTest, ChoosingAnotherActionWhileLoadingCancelsTheFirstRequest) {
    playRoadTrip();
    hold(Button::Home);
    openServerAlbums();
    options("Add to queue");
    auto first = server.lastRequest().id;
    press(Button::Back);
    press(Button::Ok); // Play next
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), first), server.cancelled.end());
    answer(weather());
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Storm Front", "Rain", "Sunshine", "Highway"}));
}

TEST_F(OptionsTest, SleepWhileAnAlbumLoadsDropsTheAnswer) {
    playRoadTrip();
    hold(Button::Home);
    openServerAlbums();
    options("Add to queue");
    auto id = server.lastRequest().id;
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_FALSE(server.reply(id, weather())) << "cancelled";
}

TEST_F(OptionsTest, ItemsWithoutActionsStillSayNoOptions) {
    start();
    press(Button::Options);
    EXPECT_EQ(speech.last(), "No options.");
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(level().title, "Home");
    openRoadTrip();
    press(Button::Options);
    ASSERT_EQ(level().title, "Options, Sunrise");
    press(Button::Options);
    EXPECT_EQ(speech.last(), "No options.") << "no second level";
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(level().title, "Options, Sunrise");
}

TEST_F(OptionsTest, OptionsInTheBookPlayerStillSayNoOptions) {
    playAlpha();
    ASSERT_EQ(app->mode(), App::Mode::Player);
    press(Button::Options);
    EXPECT_EQ(speech.last(), "No options.");
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(app->mode(), App::Mode::Player);
}

TEST_F(OptionsTest, TheBeginnerHintNamesOptions) {
    settings.setInt("verbosity", 0);
    openRoadTrip();
    EXPECT_EQ(speech.last(), "Road Trip. Sunrise, 1 of 2. Press OK to play, hold OK for options.");
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Road Trip") press(Button::Forward);
    press(Button::Back);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Road Trip, 2 tracks, 2 of 2. Press OK to open, hold OK for options.");
    hold(Button::Home);
    open("Music");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Artists, 2 of 8. Press OK to open.") << "no options, no mention";
}

TEST_F(OptionsTest, ARemoteTrackAndAlbumHaveTheHintToo) {
    settings.setInt("verbosity", 0);
    start();
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    EXPECT_EQ(speech.last(), "Weather, Band, 1 of 1. Press OK to open, hold OK for options.");
    press(Button::Ok);
    answer(weather());
    while (level().items[level().index].label != "Rain") press(Button::Forward);
    EXPECT_EQ(speech.last(), "Rain, 4 of 5. Press OK to play, hold OK for options.");
}

TEST_F(OptionsTest, AServerOnlyAlbumInMusicHasOptionsToo) {
    start();
    sync({album("al1", "Weather")});
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Weather") press(Button::Forward);
    options("Add to queue");
    EXPECT_EQ(server.lastRequest().arg, "al1");
    answer(weather());
    EXPECT_EQ(speech.last(), "Added Weather, 3 tracks. Press play.");
    EXPECT_EQ(level().title, "Albums A to Z");
}

TEST_F(OptionsTest, ADownloadedTrackInTheQueueProtectsItsListFromDeletion) {
    download("al9", "Weather", 3);
    start();
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al9", "Weather", "Band", 3}});
    options("Add to queue"); // whole on the disk: its tracks come from there, with no question to the server
    hold(Button::Home);
    open("Server");
    open("Downloaded");
    open("Weather");
    open("Delete download");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Cannot delete Weather while it is loaded in the player. Play something else first.");
    EXPECT_TRUE(fs::exists(downloadedFile("al9", 1)));
}

TEST_F(OptionsTest, PlayNowOnAnAlbumOfMusicMarksItAsPlayed) {
    openRoadTrip();
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Road Trip") press(Button::Forward);
    options("Play now");
    hold(Button::Home);
    open("Music");
    open("Recently played");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Road Trip"}));
}

// ---------------------------------------------------------------- Options in the music player, Previous queue

class PlayerOptionsTest : public OptionsTest {
protected:
    // Music > Albums A to Z, on `label`.
    void inAlbums(const std::string& label) {
        hold(Button::Home);
        open("Music");
        open("Albums A to Z");
        while (level().items[level().index].label != label) press(Button::Forward);
    }
    void playNowOn(const std::string& label) {
        inAlbums(label);
        options("Play now");
    }
    void addToQueueOn(const std::string& label) {
        inAlbums(label);
        options("Add to queue");
    }
    // Now playing > Player menu > Queue > `label`.
    void fromTracks(const std::string& label) {
        hold(Button::Home);
        open("Now playing");
        press(Button::Ok);
        open("Queue");
        open(label);
    }
};

TEST_F(PlayerOptionsTest, OptionsInTheMusicPlayerListGoToAlbumAndAlbumInfo) {
    playRoadTrip();
    press(Button::Options);
    EXPECT_EQ(speech.last(), "Options, Sunrise. Go to album, 1 of 2");
    EXPECT_EQ(sounds.last(), Earcon::Open);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Go to album", "Album info"}));
    open("Album info");
    EXPECT_EQ(speech.last().rfind("Road Trip. 2 tracks.", 0), 0u) << speech.last();
    EXPECT_EQ(level().title, "Options, Sunrise") << "it only reports, so the Options screen stays";
    press(Button::Home);
    EXPECT_EQ(app->mode(), App::Mode::Player);
}

TEST_F(PlayerOptionsTest, GoToAlbumOnALocalTrackOpensTheAlbumOnThePlayingTrack) {
    openRoadTrip();
    open("Highway");
    ASSERT_EQ(app->mode(), App::Mode::Player);
    press(Button::Options);
    open("Go to album");
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(level().title, "Road Trip");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Sunrise", "Highway"}));
    EXPECT_EQ(level().index, 1u);
    EXPECT_EQ(speech.last(), "Road Trip. Highway, 2 of 2");
    press(Button::Home);
    EXPECT_EQ(app->mode(), App::Mode::Player) << "back to the player, still playing";
    EXPECT_TRUE(player.playing());
}

TEST_F(PlayerOptionsTest, GoToAlbumOnARemoteTrackLoadsTheAlbumOnThePlayingTrack) {
    start();
    openWeather();
    open("Rain");
    finishOpen();
    ASSERT_EQ(app->mode(), App::Mode::Player);
    press(Button::Options);
    open("Go to album");
    EXPECT_EQ(level().title, "Weather");
    EXPECT_EQ(speech.last(), "Weather. Loading, 1 of 1");
    EXPECT_EQ(server.lastRequest().kind, Kind::AlbumTracks);
    EXPECT_EQ(server.lastRequest().arg, "al1");
    ASSERT_NO_FATAL_FAILURE(answer(weather()));
    EXPECT_EQ(level().items[level().index].label, "Rain");
    EXPECT_EQ(speech.last(), "Rain, 4 of 5");
    EXPECT_EQ(labels().front(), "Play album");
    EXPECT_NE(labels().back(), "Album info");
    expectNoUrlSpoken(speech);
}

TEST_F(PlayerOptionsTest, GoToAlbumOnALooseTrackSaysThereIsNoAlbumAndStaysInOptions) {
    start();
    open("Music");
    open("Albums A to Z");
    open("Lullaby");
    ASSERT_EQ(app->mode(), App::Mode::Player);
    press(Button::Options);
    open("Go to album");
    EXPECT_EQ(speech.last(), "No album for this track.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(level().title, "Options, Lullaby");
}

TEST_F(PlayerOptionsTest, AlbumInfoIsInAnAlbumsOptionsAndAtNoEndOfAList) {
    openRoadTrip();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Sunrise", "Highway"})) << "not at the end of the list";
    hold(Button::Home);
    inAlbums("Road Trip");
    press(Button::Options);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play now", "Play next", "Add to queue", "Album info"}));
    open("Album info");
    EXPECT_EQ(speech.last(), "On the device. 2 tracks.");
    EXPECT_EQ(level().title, "Options, Road Trip");
    press(Button::Home);
    hold(Button::Home);
    inAlbums("Lullaby");
    press(Button::Options);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play now", "Play next", "Add to queue"})) << "a track is not an album";
}

TEST_F(PlayerOptionsTest, ReplacingAnEditedQueueKeepsItAsPreviousQueueAndOkRestoresIt) {
    playRoadTrip();
    addToQueueOn("Lullaby"); // an edit
    fromTracks("Highway");   // the current entry is Highway
    ASSERT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    playNowOn("Lullaby");    // replaces the edited queue
    EXPECT_EQ(queue(), (std::vector<std::string>{"Lullaby"}));
    EXPECT_EQ(tracksList(), (std::vector<std::string>{"Lullaby", "Previous queue"}));
    fromTracks("Previous queue");
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Highway", "Lullaby"})) << "entries and order";
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string()) << "the current entry";
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(tracksList(), (std::vector<std::string>{"Sunrise", "Highway", "Lullaby"}))
        << "the queue it replaced was not edited: nothing to go back to";
}

TEST_F(PlayerOptionsTest, RestoringKeepsTheQueueItReplacedWhenThatWasEditedToo) {
    playRoadTrip();
    addToQueueOn("Lullaby");
    playNowOn("Lullaby");
    addToQueueOn("Road Trip"); // the new queue is edited: Lullaby, Sunrise, Highway
    fromTracks("Previous queue");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Highway", "Lullaby"}));
    EXPECT_EQ(tracksList().back(), "Previous queue");
    fromTracks("Previous queue");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Lullaby", "Sunrise", "Highway"})) << "and back again";
}

TEST_F(PlayerOptionsTest, ReplacingAnUneditedQueueKeepsNothing) {
    playRoadTrip();
    playNowOn("Lullaby");
    EXPECT_EQ(tracksList(), (std::vector<std::string>{"Lullaby"}));
}

TEST_F(PlayerOptionsTest, RestoringThePreviousQueueBringsBackThePositionInTheCurrentEntry) {
    playRoadTrip();
    addToQueueOn("Lullaby");
    fromTracks("Highway");
    player.pos = 42;
    playNowOn("Lullaby");
    ASSERT_EQ(player.pos, 0);
    fromTracks("Previous queue");
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    EXPECT_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
}

TEST_F(PlayerOptionsTest, RestoringWithAnotherCurrentEntryBringsBackThatEntrysPositionAndThePositionOfWhatItReplaced) {
    playRoadTrip();           // Sunrise is current
    addToQueueOn("Lullaby");
    player.pos = 17;
    playNowOn("Lullaby");
    addToQueueOn("Road Trip"); // Lullaby, Sunrise, Highway: edited, current Lullaby
    player.pos = 9;
    fromTracks("Previous queue");
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    EXPECT_EQ(player.pos, 17);
    fromTracks("Previous queue"); // the queue it replaced, at its own position
    EXPECT_EQ(player.file, (music / "Lullaby.mp3").string());
    EXPECT_EQ(player.pos, 9);
}

TEST_F(PlayerOptionsTest, RestoringAQueueWhoseCurrentEntryIsRemoteOpensItAtThePosition) {
    start();
    openWeather();
    open("Rain");
    finishOpen();
    addToQueueOn("Lullaby");
    player.pos = 55;
    playNowOn("Lullaby");
    fromTracks("Previous queue");
    ASSERT_TRUE(player.opening()) << "a stream takes a moment";
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_EQ(player.pos, 55);
    EXPECT_TRUE(player.playing());
}

TEST_F(PlayerOptionsTest, RestoringAQueueThatWasParkedBehindABookBringsBackItsPosition) {
    playRoadTrip();
    addToQueueOn("Lullaby");
    player.pos = 33;
    hold(Button::Home);
    open("Library");
    open("Alpha"); // the book plays; the edited music queue waits
    ASSERT_EQ(player.file, (alpha() / "01 - Storm.mp3").string());
    playNowOn("Lullaby"); // replaces the parked queue
    fromTracks("Previous queue");
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    EXPECT_EQ(player.pos, 33);
}

TEST_F(PlayerOptionsTest, ContinuingAnEditedParkedQueueDoesNotOfferItAsItsOwnPreviousQueue) {
    playRoadTrip();
    addToQueueOn("Lullaby");
    hold(Button::Home);
    open("Library");
    open("Alpha"); // the book plays; the edited music queue waits
    hold(Button::Home);
    ASSERT_EQ(labels()[1], "Continue");
    press(Button::Forward);
    press(Button::Ok);
    ASSERT_EQ(app->screens().back().kind, Screen::Kind::MusicPlayer);
    press(Button::Ok);
    open("Queue");
    EXPECT_FALSE(hasLabel("Previous queue"));
}

TEST_F(PlayerOptionsTest, RestoringThePreviousQueueBringsBackItsShuffleAndRepeat) {
    playRoadTrip();
    app->setRandom([](size_t) { return size_t(0); });
    addToQueueOn("Lullaby"); // an edit: Sunrise, Highway, Lullaby
    auto choose = [this](const std::string& item, const std::string& value) {
        hold(Button::Home);
        open("Now playing");
        press(Button::Ok);
        open(item);
        open(value);
    };
    auto settingValue = [this](const std::string& item) {
        for (const MenuItem& m : level().items)
            if (m.label == item) return valueOf(m);
        return std::string("none");
    };
    choose("Shuffle", "tracks");
    choose("Repeat", "all");
    playNowOn("Lullaby"); // a new queue: both off
    hold(Button::Home);
    open("Now playing");
    press(Button::Ok);
    ASSERT_EQ(settingValue("Shuffle"), "off");
    ASSERT_EQ(settingValue("Repeat"), "off");
    open("Queue");
    open("Previous queue");
    press(Button::Ok);
    EXPECT_EQ(settingValue("Shuffle"), "tracks");
    EXPECT_EQ(settingValue("Repeat"), "all");
    open("Queue");
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"Sunrise", "Lullaby", "Highway"})) << "the shuffled order is back too";
}

// ---------------------------------------------------------------- Shuffle and Repeat in the music Player menu

class ShuffleRepeatTest : public UnavailableTest {
protected:
    std::string sunrise() const { return (roadTrip() / "01 - Sunrise.mp3").string(); }
    std::string highway() const { return (roadTrip() / "02 - Highway.mp3").string(); }
    // Storm, Calm, Lullaby, Sunrise: four local tracks. The random numbers are all 0, which moves the first
    // entry of what is shuffled to its end: the entries after the current one turn left by one place.
    std::vector<QueueEntry> four() const {
        return {localEntry(alphaFile(1), "Storm", "One"), localEntry(alphaFile(2), "Calm", "One"),
                localEntry(lullaby(), "Lullaby", "Two"), localEntry(sunrise(), "Sunrise", "Three")};
    }
    void play(std::vector<QueueEntry> entries, size_t start = 0) {
        start_();
        app->setRandom([](size_t) { return size_t(0); });
        ASSERT_TRUE(app->playQueue("Mixed", std::move(entries), start));
    }
    void toPlayer() {
        for (int i = 0; i < 6 && !level().isPlayer(); ++i) press(Button::Home);
        ASSERT_TRUE(level().isPlayer());
    }
    // From the player: Player menu > `item` > `value`, and back to the player.
    void choose(const std::string& item, const std::string& value) {
        ASSERT_NO_FATAL_FAILURE(toPlayer());
        press(Button::Ok);
        open(item);
        open(value);
        ASSERT_EQ(level().title, "Player menu") << "choosing goes back";
        ASSERT_NO_FATAL_FAILURE(toPlayer());
    }
    // The value spoken after `item` in the Player menu.
    std::string settingValue(const std::string& item) {
        toPlayer();
        press(Button::Ok);
        for (const MenuItem& m : level().items)
            if (m.label == item) return valueOf(m);
        return "none";
    }
    std::vector<std::string> trackList() {
        toPlayer();
        press(Button::Ok);
        open("Queue");
        auto out = queueEntries();
        toPlayer();
        return out;
    }
};

TEST_F(ShuffleRepeatTest, ChoosingShuffleTracksWorksAtOnceAndKeepsTheCurrentEntry) {
    play(four(), 1);
    ASSERT_EQ(app->nowPlaying().chapter, "Calm");
    const size_t opens = player.opened.size();
    ASSERT_NO_FATAL_FAILURE(choose("Shuffle", "tracks"));
    EXPECT_EQ(settingValue("Shuffle"), "tracks");
    EXPECT_EQ(app->nowPlaying().chapter, "Calm") << "the current entry stays current";
    EXPECT_EQ(player.opened.size(), opens) << "and plays on, nothing is opened again";
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(trackList(), (std::vector<std::string>{"Storm", "Calm", "Sunrise", "Lullaby"})) << "what follows is shuffled";
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(player.file, sunrise()) << "the next track is the shuffled one";
}

TEST_F(ShuffleRepeatTest, ShuffleOffGivesBackTheOriginalOrderWithTheSameCurrentEntry) {
    play(four(), 1);
    ASSERT_NO_FATAL_FAILURE(choose("Shuffle", "tracks"));
    press(Button::Forward); // Sunrise, the third in the shuffled order
    ASSERT_EQ(player.file, sunrise());
    ASSERT_NO_FATAL_FAILURE(choose("Shuffle", "off"));
    EXPECT_EQ(app->nowPlaying().chapter, "Sunrise");
    EXPECT_EQ(player.file, sunrise());
    toPlayer();
    press(Button::Ok);
    open("Queue");
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"Storm", "Calm", "Lullaby", "Sunrise"}));
    EXPECT_EQ(level().index, 3u) << "the list opens on the playing track, which is the last one again";
}

TEST_F(ShuffleRepeatTest, TheShuffleListOpensOnTheCurrentValue) {
    play(four());
    ASSERT_NO_FATAL_FAILURE(choose("Shuffle", "albums"));
    press(Button::Ok);
    open("Shuffle");
    EXPECT_EQ(labels(), (std::vector<std::string>{"off", "tracks", "albums"}));
    EXPECT_EQ(level().index, 2u);
    EXPECT_EQ(valueOf(level().items[2]), "current");
    EXPECT_EQ(valueOf(level().items[0]), "");
}

TEST_F(ShuffleRepeatTest, TheRepeatListOpensOnTheCurrentValue) {
    play(four());
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "track"));
    press(Button::Ok);
    open("Repeat");
    EXPECT_EQ(labels(), (std::vector<std::string>{"off", "all", "album", "track"}));
    EXPECT_EQ(level().index, 3u);
    EXPECT_EQ(valueOf(level().items[3]), "current");
    EXPECT_EQ(valueOf(level().items[0]), "");
}

TEST_F(ShuffleRepeatTest, ShuffleAlbumsKeepsEachAlbumsOrderAndTheRestOfTheCurrentAlbumFirst) {
    play({localEntry(alphaFile(1), "Storm", "One"), localEntry(alphaFile(2), "Calm", "One"),
          localEntry(sunrise(), "Sunrise", "Three"), localEntry(highway(), "Highway", "Three"),
          localEntry(lullaby(), "Lullaby", "Two")});
    ASSERT_NO_FATAL_FAILURE(choose("Shuffle", "albums"));
    EXPECT_EQ(trackList(), (std::vector<std::string>{"Storm", "Calm", "Lullaby", "Sunrise", "Highway"}));
}

TEST_F(ShuffleRepeatTest, ANewQueueStartsWithShuffleAndRepeatOff) {
    play(four());
    EXPECT_EQ(settingValue("Shuffle"), "off");
    EXPECT_EQ(settingValue("Repeat"), "off");
    ASSERT_NO_FATAL_FAILURE(choose("Shuffle", "tracks"));
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "all"));
    ASSERT_EQ(settingValue("Repeat"), "all");
    ASSERT_TRUE(app->playQueue("Again", four()));
    EXPECT_EQ(settingValue("Shuffle"), "off");
    EXPECT_EQ(settingValue("Repeat"), "off");
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "track"));
    hold(Button::Home);
    open("Music");
    open("Play all"); // a list from the menus is a new queue too
    EXPECT_EQ(settingValue("Shuffle"), "off");
    EXPECT_EQ(settingValue("Repeat"), "off");
}

TEST_F(ShuffleRepeatTest, TheBookPlayerMenuHasNeitherShuffleNorRepeat) {
    playAlpha();
    press(Button::Ok);
    for (const std::string& label : labels()) {
        EXPECT_NE(label, "Shuffle");
        EXPECT_NE(label, "Repeat");
    }
}

TEST_F(ShuffleRepeatTest, RepeatTrackPlaysTheTrackAgainWhenItEnds) {
    play(four(), 1);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "track"));
    for (int i = 0; i < 2; ++i) {
        const size_t opens = player.opened.size();
        trackEnds();
        EXPECT_EQ(player.file, alphaFile(2));
        EXPECT_EQ(player.opened.size(), opens + 1) << "opened again";
        EXPECT_TRUE(player.playing());
        EXPECT_EQ(app->nowPlaying().chapter, "Calm");
    }
    EXPECT_FALSE(speech.heard("End of queue. Press play to start again."));
}

TEST_F(ShuffleRepeatTest, TheRepeatListSaysAlbumAndChoosingItSticks) {
    play(four());
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "album"));
    press(Button::Ok);
    open("Repeat");
    EXPECT_EQ(level().index, 2u);
    EXPECT_EQ(valueOf(level().items[2]), "current");
    EXPECT_EQ(level().items[2].label, "album");
    EXPECT_EQ(settingValue("Repeat"), "album");
}

TEST_F(ShuffleRepeatTest, RepeatAlbumPlaysTheAlbumsFirstTrackWhenItsLastEnds) {
    play(four(), 1);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "album"));
    trackEnds();
    EXPECT_EQ(player.file, alphaFile(1)) << "Calm is the last of One: Storm plays, not Lullaby";
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Storm");
    trackEnds();
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_FALSE(speech.heard("End of queue. Press play to start again."));
}

TEST_F(ShuffleRepeatTest, RepeatAlbumOnTheQueuesLastTrackRepeatsItWhenItIsAloneInItsAlbum) {
    play(four(), 3);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "album"));
    trackEnds();
    EXPECT_EQ(player.file, sunrise());
    EXPECT_TRUE(player.playing());
    EXPECT_FALSE(speech.heard("End of queue. Press play to start again."));
}

TEST_F(ShuffleRepeatTest, RepeatAlbumStillMovesWithRightAndTheAlbumMovedToRepeats) {
    play({localEntry(alphaFile(1), "Storm", "One"), localEntry(alphaFile(2), "Calm", "One"),
          localEntry(sunrise(), "Sunrise", "Three"), localEntry(highway(), "Highway", "Three")},
         1);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "album"));
    press(Button::Forward);
    ASSERT_EQ(player.file, sunrise());
    trackEnds();
    EXPECT_EQ(player.file, highway());
    trackEnds();
    EXPECT_EQ(player.file, sunrise()) << "Three repeats, One does not come back";
}

TEST_F(ShuffleRepeatTest, RepeatAlbumDoesNotLeaveTheAlbumForATrackThatIsGone) {
    play({localEntry(alphaFile(1), "Storm", "One"), localEntry("/nowhere/gone.mp3", "Calm", "One"),
          localEntry(sunrise(), "Sunrise", "Three")},
         0);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "album"));
    trackEnds();
    EXPECT_EQ(player.file, alphaFile(1)) << "Calm is gone: Storm is the album's next, not Sunrise of Three";
    EXPECT_TRUE(player.playing());
}

TEST_F(ShuffleRepeatTest, RepeatAlbumWrapsToTheAlbumsFirstWhenItsLastTrackIsGone) {
    play({localEntry(alphaFile(1), "Storm", "One"), localEntry(alphaFile(2), "Calm", "One"),
          localEntry("/nowhere/gone.mp3", "Lost", "One"), localEntry(sunrise(), "Sunrise", "Three")},
         1);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "album"));
    trackEnds();
    EXPECT_EQ(player.file, alphaFile(1)) << "Lost is gone and is the last of One: Storm, not Sunrise or the end";
    EXPECT_FALSE(speech.heard("End of queue. Press play to start again."));
}

TEST_F(ShuffleRepeatTest, RepeatAlbumGoesOnInTheAlbumWhenAServerTrackDoesNotOpenAtTheEndOfATrack) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed",
                               {localEntry(alphaFile(1), "Storm", "One"), localEntry(alphaFile(2), "Calm", "One"),
                                remoteEntry("t3", "Rain", "One"), localEntry(sunrise(), "Sunrise", "Three")},
                               1));
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "album"));
    player.unopenable = {"fake://stream/t3"};
    trackEnds();
    finishOpen();
    EXPECT_EQ(player.file, alphaFile(1)) << "Rain does not open and is the last of One: Storm, not Sunrise of Three";
    EXPECT_TRUE(player.playing());
}

TEST_F(ShuffleRepeatTest, PingsGoOnForAServerTrackBehindTheCurrentOneWithRepeatAlbum) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", mixed()));
    player.unopenable = {"fake://stream/t2"};
    trackEnds();
    finishOpen();
    ASSERT_EQ(player.file, alphaFile(2)) << "Calm plays; the server is marked down and Rain is behind it";
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "album"));
    idle(10min);
    EXPECT_EQ(pings(), 1) << "Rain comes up again after Calm: all three are the album Mixed";
}

TEST_F(ShuffleRepeatTest, RepeatTrackStillMovesWithLeftAndRightAndTheTrackMovedToRepeats) {
    play(four(), 1);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "track"));
    press(Button::Forward);
    ASSERT_EQ(player.file, lullaby());
    trackEnds();
    EXPECT_EQ(player.file, lullaby()) << "the track moved to repeats";
    press(Button::Back);
    EXPECT_EQ(player.file, alphaFile(2)) << "and Previous still moves";
}

TEST_F(ShuffleRepeatTest, RepeatTrackPreOpensTheCurrentServerTrackAgain) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", mixed(), 1));
    finishOpen();
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "track"));
    player.pos = player.length - 5;
    app->tick();
    EXPECT_EQ(player.preloads, (std::vector<std::string>{"fake://stream/t2"}));
}

TEST_F(ShuffleRepeatTest, RepeatAllGoesOnFromTheFirstEntryAtTheEnd) {
    play(four(), 3);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "all"));
    trackEnds();
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Storm");
    EXPECT_FALSE(speech.heard("End of queue. Press play to start again."));
}

TEST_F(ShuffleRepeatTest, RepeatAllSkipsUnavailableEntriesAcrossTheWrap) {
    play(four(), 1);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "all"));
    fs::remove(lullaby());
    fs::remove(sunrise());
    trackEnds();
    EXPECT_EQ(speech.said[speech.said.size() - 1], "2 tracks, source unavailable.");
    EXPECT_EQ(player.file, alphaFile(1)) << "the first entry plays after the two that cannot";
    EXPECT_TRUE(player.playing());
    EXPECT_FALSE(speech.heard("End of queue. Press play to start again."));
}

TEST_F(ShuffleRepeatTest, RepeatAllNextOnTheLastTrackGoesToTheFirstOverMissingOnes) {
    play(four(), 1);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "all"));
    fs::remove(lullaby());
    fs::remove(sunrise());
    press(Button::Forward);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_TRUE(speech.heard("2 tracks, source unavailable."));
    EXPECT_EQ(speech.last(), "Storm");
}

TEST_F(ShuffleRepeatTest, RepeatAllPreviousOnTheFirstTrackGoesToTheLastOverMissingOnes) {
    play(four(), 0);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "all"));
    fs::remove(lullaby());
    fs::remove(sunrise());
    press(Button::Back);
    EXPECT_TRUE(speech.heard("2 tracks, source unavailable."));
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_EQ(speech.last(), "Calm");
}

TEST_F(ShuffleRepeatTest, RepeatAllPreviousGoesBackOverMissingOnesPastTheFirstToTheLast) {
    play(four(), 2);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "all"));
    fs::remove(alphaFile(1));
    fs::remove(alphaFile(2));
    press(Button::Back);
    EXPECT_TRUE(speech.heard("2 tracks, source unavailable."));
    EXPECT_EQ(player.file, sunrise());
    EXPECT_EQ(speech.last(), "Sunrise");
}

TEST_F(ShuffleRepeatTest, RepeatAllDoesNotWrapForATrackChosenInTheList) {
    play(four(), 1);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "all"));
    fs::remove(sunrise());
    toPlayer();
    press(Button::Ok);
    open("Queue");
    open("Sunrise"); // the last one, which is gone
    EXPECT_EQ(player.file, alphaFile(2)) << "it does not go on from the first entry";
    EXPECT_EQ(app->nowPlaying().chapter, "Calm");
    EXPECT_TRUE(speech.heard("Sunrise, source unavailable."));
}

TEST_F(ShuffleRepeatTest, ShufflingWhileAServerTrackOpensKeepsTrackOfWhichTrackIsOpening) {
    start_();
    app->setRandom([](size_t) { return size_t(0); });
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), localEntry(lullaby(), "Lullaby"),
                                         localEntry(sunrise(), "Sunrise"), remoteEntry("t4", "Rain")}));
    ASSERT_NO_FATAL_FAILURE(choose("Shuffle", "tracks")); // Storm, Sunrise, Rain, Lullaby
    trackEnds();
    ASSERT_EQ(player.file, sunrise());
    trackEnds();
    ASSERT_EQ(player.file, "fake://stream/t4") << "Rain is opening";
    player.unopenable = {"fake://stream/t4"};
    ASSERT_NO_FATAL_FAILURE(choose("Shuffle", "off")); // Rain is the last entry again
    finishOpen();
    EXPECT_EQ(speech.last(), "Server not reachable. Press play to try again.") << "nothing after Rain plays";
    EXPECT_EQ(countOf(speech.said, "Rain, source unavailable."), 0);
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
}

TEST_F(ShuffleRepeatTest, ShuffleWaitsWhileATrackTheUserChoseIsStillOpening) {
    play({localEntry(alphaFile(1), "Storm"), localEntry(alphaFile(2), "Calm"), remoteEntry("t3", "Rain"),
          localEntry(lullaby(), "Lullaby")}, 1);
    press(Button::Forward);
    ASSERT_TRUE(player.opening()) << "Rain takes a moment";
    press(Button::Ok);
    open("Shuffle");
    open("tracks");
    EXPECT_EQ(speech.last(), "Loading. Try again when it has started.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(level().title, "Shuffle") << "the list stays";
    finishOpen();
    EXPECT_EQ(settingValue("Shuffle"), "off");
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
}

TEST_F(ShuffleRepeatTest, RepeatAllWithNothingThatPlaysEndsTheQueueInsteadOfGoingRoundForEver) {
    play(four(), 1);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "all"));
    for (const QueueEntry& e : four()) fs::remove(e.path);
    trackEnds();
    EXPECT_TRUE(speech.heard("4 tracks, source unavailable."));
    EXPECT_EQ(speech.last(), "End of queue. Press play to start again.");
    EXPECT_FALSE(player.playing());
    press(Button::PlayPause); // still answers
    EXPECT_FALSE(player.playing());
}

TEST_F(ShuffleRepeatTest, RepeatTrackOnATrackThatIsGoneDoesNotHangAndTheQueueGoesOn) {
    play({localEntry(alphaFile(1), "Storm"), localEntry(alphaFile(2), "Calm"), localEntry(lullaby(), "Lullaby")}, 1);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "track"));
    fs::remove(alphaFile(2));
    trackEnds();
    EXPECT_EQ(speech.last(), "Calm, source unavailable.") << "said, then the next one plays and repeats";
    EXPECT_EQ(player.file, lullaby());
    EXPECT_TRUE(player.playing());
    fs::remove(lullaby());
    trackEnds(); // the last track is gone too: nothing is left to repeat
    EXPECT_EQ(speech.last(), "End of queue. Press play to start again.");
    EXPECT_FALSE(player.playing());
}

TEST_F(ShuffleRepeatTest, PingsGoOnForAServerTrackBehindTheCurrentOneWithRepeatAll) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", mixed()));
    player.unopenable = {"fake://stream/t2"};
    trackEnds();
    finishOpen();
    ASSERT_EQ(player.file, alphaFile(2)) << "Calm plays; the server is marked down and Rain is behind it";
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "all"));
    idle(10min);
    EXPECT_EQ(pings(), 1) << "Rain comes up again after Calm";
}

// ---------------------------------------------------------------- the Queue list (queue.md, step 2.7)

class QueueListTest : public UnavailableTest {
protected:
    // T1 ... Tn, all of the server (they open at once: holdStreams is off unless a test sets it).
    std::vector<QueueEntry> tracks(size_t n) const {
        std::vector<QueueEntry> out;
        for (size_t i = 1; i <= n; ++i) out.push_back(remoteEntry("t" + std::to_string(i), "T" + std::to_string(i)));
        return out;
    }
    std::string sunrise() const { return (roadTrip() / "01 - Sunrise.mp3").string(); }
    void play(std::vector<QueueEntry> entries, size_t at = 0) {
        start();
        app->setRandom([](size_t) { return size_t(0); });
        ASSERT_TRUE(app->playQueue("Mixed", std::move(entries), at));
    }
    void toPlayer() {
        for (int i = 0; i < 8 && !level().isPlayer(); ++i) press(Button::Home);
        ASSERT_TRUE(level().isPlayer());
    }
    // The Queue list, from the player.
    void openQueue() {
        ASSERT_NO_FATAL_FAILURE(toPlayer());
        press(Button::Ok);
        open("Queue");
    }
    void select(const std::string& label) {
        for (size_t i = 0; i < level().items.size() && level().items[level().index].label != label; ++i) press(Button::Forward);
        ASSERT_EQ(level().items[level().index].label, label);
    }
    // In the Queue list: Options on the entry `label`, then `item`.
    void optionsOn(const std::string& label, const std::string& item) {
        ASSERT_NO_FATAL_FAILURE(openQueue());
        select(label);
        press(Button::Options);
        ASSERT_EQ(level().title, "Options, " + label);
        open(item);
    }
    // Player menu > Shuffle > tracks.
    void shuffleTracks() {
        ASSERT_NO_FATAL_FAILURE(toPlayer());
        press(Button::Ok);
        open("Shuffle");
        open("tracks");
    }
    // After the queue was edited, a new queue replaces it: the edited one is kept as Previous queue.
    void expectEditedFlag() {
        ASSERT_TRUE(app->playQueue("Other", {remoteEntry("x1", "X1")}));
        ASSERT_NO_FATAL_FAILURE(openQueue());
        EXPECT_TRUE(hasLabel("Previous queue"));
    }
    void expectNoPrevious() {
        ASSERT_TRUE(app->playQueue("Other", {remoteEntry("x1", "X1")}));
        ASSERT_NO_FATAL_FAILURE(openQueue());
        EXPECT_FALSE(hasLabel("Previous queue"));
    }
};

TEST_F(QueueListTest, TheMusicPlayerMenuHasQueueInPlaceOfTracks) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(toPlayer());
    press(Button::Ok);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Queue", "Shuffle", "Repeat", "Go to first track", "Sleep timer"}));
}

TEST_F(QueueListTest, OpensOnTheCurrentEntryAndSaysCurrent) {
    play(tracks(30), 4);
    ASSERT_NO_FATAL_FAILURE(openQueue());
    EXPECT_EQ(speech.last(), "Queue. T5, current, 5 of 32") << "30 entries, Save as playlist and Clear queue";
    EXPECT_EQ(level().index, 4u);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "T6, 6 of 32") << "only the current entry says current";
}

TEST_F(QueueListTest, OkOnAnEntryPlaysIt) {
    play(tracks(5));
    ASSERT_NO_FATAL_FAILURE(openQueue());
    select("T4");
    press(Button::Ok);
    EXPECT_EQ(player.file, "fake://stream/t4");
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "T4");
    EXPECT_EQ(app->mode(), App::Mode::Player);
}

TEST_F(QueueListTest, TheListEndsWithSaveAsPlaylistAndClearQueueAndPreviousQueueOnlyWhenThereIsOne) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(openQueue());
    EXPECT_EQ(labels(), (std::vector<std::string>{"T1", "T2", "T3", "Save as playlist", "Clear queue"}));
    ASSERT_NO_FATAL_FAILURE(optionsOn("T3", "Remove"));
    ASSERT_TRUE(app->playQueue("Other", {remoteEntry("x1", "X1")}));
    ASSERT_NO_FATAL_FAILURE(openQueue());
    EXPECT_EQ(labels(), (std::vector<std::string>{"X1", "Save as playlist", "Previous queue", "Clear queue"}));
    EXPECT_EQ(valueOf(level().items[2]), "2 tracks");
    open("Previous queue");
    EXPECT_EQ(app->mode(), App::Mode::Player);
    ASSERT_NO_FATAL_FAILURE(openQueue());
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1", "T2"}));
}

// ---- Save as playlist (queue.md, step 2.9)

class SavePlaylistTest : public QueueListTest {
protected:
    // 27 October 14:05 of the local time zone: the name does not depend on where the test runs.
    static std::time_t at(int day, int month, int hour, int minute) {
        std::tm t{};
        t.tm_year = 2026 - 1900;
        t.tm_mon = month - 1;
        t.tm_mday = day;
        t.tm_hour = hour;
        t.tm_min = minute;
        t.tm_isdst = -1;
        return std::mktime(&t);
    }
    void SetUp() override {
        QueueListTest::SetUp();
        clock.setWallTime(at(27, 10, 14, 5));
    }
    size_t requestsOf(FakeMusicServer::Kind kind) const {
        return size_t(std::count_if(server.requests.begin(), server.requests.end(),
                                    [kind](const FakeMusicServer::Request& r) { return r.kind == kind; }));
    }
    size_t creates() const {
        return size_t(std::count_if(server.requests.begin(), server.requests.end(),
                                    [](const FakeMusicServer::Request& r) { return r.kind == FakeMusicServer::Kind::CreatePlaylist; }));
    }
    // The createPlaylist App sent last.
    const FakeMusicServer::Request& created() const {
        const auto& r = server.lastRequest();
        EXPECT_EQ(r.kind, FakeMusicServer::Kind::CreatePlaylist);
        return r;
    }
    void save() {
        ASSERT_NO_FATAL_FAILURE(openQueue());
        open("Save as playlist");
    }
    // The server says the playlist is made.
    void answer() {
        ASSERT_TRUE(server.replyCreated(created().id));
        app->tick();
    }
    // A Home press ends the wait; the answer that comes later says nothing.
    void expectLateAnswerDropped() {
        const auto before = speech.said.size();
        EXPECT_FALSE(server.replyCreated(created().id)) << "cancelled: the server object drops it";
        app->tick();
        EXPECT_EQ(speech.said.size(), before);
    }
};

TEST_F(SavePlaylistTest, SendsTheServerTracksInPlayOrderAndSaysItSavedThem) {
    play(tracks(4));
    ASSERT_NO_FATAL_FAILURE(optionsOn("T4", "Play next")); // T1 T4 T2 T3
    ASSERT_NO_FATAL_FAILURE(save());
    EXPECT_EQ(speech.last(), "Saving.");
    EXPECT_EQ(created().arg, "Queue, 27 October 14:05");
    EXPECT_EQ(created().trackIds, (std::vector<std::string>{"t1", "t4", "t2", "t3"}));
    ASSERT_NO_FATAL_FAILURE(answer());
    EXPECT_EQ(speech.last(), "Saved Queue, 27 October 14:05.");
    EXPECT_EQ(sounds.last(), Earcon::Close);
    EXPECT_EQ(level().title, "Queue");
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1", "T4", "T2", "T3"}));
}

TEST_F(SavePlaylistTest, TheSavedPlaylistIsInTheIndexAtOnceWithoutASync) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(save());
    EXPECT_TRUE(indexStore.playlists.empty()) << "not before the server has made it";
    const size_t requests = server.requests.size();
    ASSERT_TRUE(server.replyCreated(created().id, "pl-77"));
    app->tick();
    ASSERT_EQ(indexStore.playlists.size(), 1u);
    const IndexPlaylist& p = indexStore.playlists[0];
    EXPECT_EQ(p.server, TrackCache::serverKey("https://music.example", "ann"));
    EXPECT_EQ(p.id, "pl-77");
    EXPECT_EQ(p.name, "Queue, 27 October 14:05");
    EXPECT_EQ(p.songCount, 3);
    EXPECT_EQ(p.changed, at(27, 10, 14, 5));
    EXPECT_EQ(server.requests.size(), requests) << "no getPlaylists, no sync";
    EXPECT_EQ(requestsOf(FakeMusicServer::Kind::Playlists), 0u);
}

TEST_F(SavePlaylistTest, APlaylistSavedWhileTheSyncAsksForPlaylistsSurvivesAnAnswerThatDoesNotHaveIt) {
    play(tracks(2));
    ASSERT_EQ(server.lastRequest().kind, FakeMusicServer::Kind::CatalogueVersion);
    ASSERT_TRUE(server.replyVersion(server.lastRequest().id, "v1"));
    app->tick();
    ASSERT_EQ(server.lastRequest().kind, FakeMusicServer::Kind::Albums);
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteAlbum>{}));
    app->tick();
    ASSERT_EQ(server.lastRequest().kind, FakeMusicServer::Kind::Playlists) << "the sync waits for getPlaylists";
    const auto playlistsRequest = server.lastRequest().id;
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_TRUE(server.replyCreated(created().id, "pl-77"));
    app->tick();
    ASSERT_TRUE(server.reply(playlistsRequest, std::vector<RemotePlaylist>{remotePlaylist("other", "Made before", 1)}));
    app->tick();
    ASSERT_EQ(indexStore.playlists.size(), 2u);
    EXPECT_EQ(indexStore.playlists[0].id, "other");
    EXPECT_EQ(indexStore.playlists[1].id, "pl-77") << "the answer was asked before the playlist existed";
}

TEST_F(SavePlaylistTest, WhenTheAnswerHasThePlaylistTooItIsTheServersVersionThatCounts) {
    play(tracks(2));
    ASSERT_TRUE(server.replyVersion(server.lastRequest().id, "v1"));
    app->tick();
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteAlbum>{}));
    app->tick();
    const auto playlistsRequest = server.lastRequest().id;
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_TRUE(server.replyCreated(created().id, "pl-77"));
    app->tick();
    ASSERT_TRUE(server.reply(playlistsRequest, std::vector<RemotePlaylist>{remotePlaylist("pl-77", "Renamed on the server", 2)}));
    app->tick();
    ASSERT_EQ(indexStore.playlists.size(), 1u);
    EXPECT_EQ(indexStore.playlists[0].name, "Renamed on the server");
}

TEST_F(SavePlaylistTest, TheIndexCountsOnlyTheTracksThatWereSent) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), remoteEntry("t2", "Rain"), remoteEntry("t5", "Dusk")}));
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_NO_FATAL_FAILURE(answer());
    ASSERT_EQ(indexStore.playlists.size(), 1u);
    EXPECT_EQ(indexStore.playlists[0].songCount, 2);
}

TEST_F(SavePlaylistTest, AFailedSaveAddsNothingToTheIndex) {
    play(tracks(2));
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_TRUE(server.fail(created().id, MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_TRUE(indexStore.playlists.empty());
}

TEST_F(SavePlaylistTest, AnAnswerWithoutAPlaylistIdSavesButAddsNothingToTheIndex) {
    play(tracks(2));
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_TRUE(server.replyCreated(created().id, ""));
    app->tick();
    EXPECT_EQ(speech.last(), "Saved Queue, 27 October 14:05.");
    EXPECT_TRUE(indexStore.playlists.empty());
}

TEST_F(SavePlaylistTest, ASavedPlaylistJoinsTheOnesAlreadyInTheIndex) {
    const std::string key = TrackCache::serverKey("https://music.example", "ann");
    indexStore.playlists = {{key, "old", "Older", 9, 5}};
    play(tracks(2));
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_NO_FATAL_FAILURE(answer());
    ASSERT_EQ(indexStore.playlists.size(), 2u);
    EXPECT_EQ(indexStore.playlists[0].id, "new-playlist");
    EXPECT_EQ(indexStore.playlists[1].id, "old");
}

TEST_F(SavePlaylistTest, SendsTheIdsInThePlayOrderOfAShuffledQueue) {
    play(tracks(6));
    ASSERT_NO_FATAL_FAILURE(shuffleTracks());
    ASSERT_NO_FATAL_FAILURE(openQueue());
    std::vector<std::string> ids;
    for (const std::string& title : queueEntries()) ids.push_back("t" + title.substr(1));
    ASSERT_NE(ids, (std::vector<std::string>{"t1", "t2", "t3", "t4", "t5", "t6"})) << "the shuffle changed the order";
    open("Save as playlist");
    EXPECT_EQ(created().trackIds, ids);
}

TEST_F(SavePlaylistTest, TheNameComesFromTheWallTimeInEnglishWithoutAZeroOnTheDay) {
    clock.setWallTime(at(3, 1, 9, 7));
    play(tracks(2));
    ASSERT_NO_FATAL_FAILURE(save());
    EXPECT_EQ(created().arg, "Queue, 3 January 09:07");
    ASSERT_NO_FATAL_FAILURE(answer());
    EXPECT_EQ(speech.last(), "Saved Queue, 3 January 09:07.");
    expectNoUrlSpoken(speech);
}

TEST_F(SavePlaylistTest, SavesOnlyTheTracksOfTheServerSetUpNowAndSaysHowManyWereLeftOut) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), remoteEntry("t2", "Rain"),
                                         ofServer("https://other.example", "ann", "x3", "Fog"),
                                         ofServer("https://music.example", "bob", "x4", "Mist"),
                                         remoteEntry("t5", "Dusk"), localEntry(alphaFile(2), "Calm")}));
    ASSERT_NO_FATAL_FAILURE(save());
    EXPECT_EQ(created().trackIds, (std::vector<std::string>{"t2", "t5"})) << "local, another server and another user stay out";
    ASSERT_NO_FATAL_FAILURE(answer());
    EXPECT_EQ(speech.last(), "Saved Queue, 27 October 14:05. 4 tracks not saved.");
}

TEST_F(SavePlaylistTest, OneTrackLeftOutIsSpokenInTheSingular) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), remoteEntry("t2", "Rain")}));
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_NO_FATAL_FAILURE(answer());
    EXPECT_EQ(speech.last(), "Saved Queue, 27 October 14:05. 1 track not saved.");
}

TEST_F(SavePlaylistTest, ATrackThatPlaysFromACachedCopyIsStillSaved) {
    cache = base / "cache";
    TrackCache store(diskFiles(), cache, "https://music.example", "ann");
    fs::create_directories(store.listFolder(albumSource("mixed")));
    touch(store.listFolder(albumSource("mixed")) / "t1.mp3");
    ASSERT_TRUE(store.save(cachedAlbum("mixed", {{"t1", "t1.mp3", "t1", 100, "", true, 0}})));
    play(tracks(2));
    ASSERT_NE(player.file, "fake://stream/t1") << "the first track plays from its file";
    ASSERT_NO_FATAL_FAILURE(save());
    EXPECT_EQ(created().trackIds, (std::vector<std::string>{"t1", "t2"}));
    ASSERT_NO_FATAL_FAILURE(answer());
    EXPECT_EQ(speech.last(), "Saved Queue, 27 October 14:05.") << "nothing left out";
}

TEST_F(SavePlaylistTest, WithNoTrackOfTheServerItSaysWhyAndSendsNothing) {
    start_();
    ASSERT_TRUE(app->playQueue("Local", {localEntry(alphaFile(1), "Storm"), localEntry(alphaFile(2), "Calm"),
                                         ofServer("https://other.example", "ann", "x3", "Fog")}));
    ASSERT_NO_FATAL_FAILURE(save());
    EXPECT_EQ(speech.last(), "Only tracks from the server can be saved.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(creates(), 0u);
    EXPECT_EQ(level().title, "Queue");
}

TEST_F(SavePlaylistTest, WithNoServerSetUpItSaysWhyAndSendsNothing) {
    play(tracks(3));
    server.isConfigured = false;
    ASSERT_NO_FATAL_FAILURE(save());
    EXPECT_EQ(speech.last(), "Only tracks from the server can be saved.");
    EXPECT_EQ(creates(), 0u);
}

TEST_F(SavePlaylistTest, AnErrorIsSpokenAndTheQueueStaysAsItWas) {
    play(tracks(3), 1);
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_TRUE(server.fail(created().id, MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(speech.last(), "Server not reachable.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(level().title, "Queue");
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1", "T2", "T3"}));
    EXPECT_EQ(app->nowPlaying().chapter, "T2");
    EXPECT_TRUE(player.playing());
    expectNoPrevious(); // the queue was not edited
}

TEST_F(SavePlaylistTest, AWrongLoginIsSpokenAsSuch) {
    play(tracks(2));
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_TRUE(server.fail(created().id, MusicServer::Error::BadLogin));
    app->tick();
    EXPECT_EQ(speech.last(), "Wrong user name or password.");
}

TEST_F(SavePlaylistTest, SavingTheQueueDoesNotChangeIt) {
    play(tracks(3), 1);
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_NO_FATAL_FAILURE(answer());
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1", "T2", "T3"}));
    EXPECT_EQ(app->nowPlaying().chapter, "T2");
    expectNoPrevious();
}

TEST_F(SavePlaylistTest, PressingItAgainWhileSavingSendsOnlyOneRequest) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(save());
    open("Save as playlist");
    EXPECT_EQ(creates(), 1u);
    EXPECT_EQ(speech.last(), "Saving.");
    ASSERT_NO_FATAL_FAILURE(answer());
    EXPECT_EQ(speech.last(), "Saved Queue, 27 October 14:05.");
}

TEST_F(SavePlaylistTest, ASecondSaveAfterTheAnswerIsSentAgain) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_NO_FATAL_FAILURE(answer());
    open("Save as playlist");
    EXPECT_EQ(creates(), 2u);
}

TEST_F(SavePlaylistTest, HomeWhileSavingDropsTheLateAnswer) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(save());
    press(Button::Home);
    expectLateAnswerDropped();
    EXPECT_FALSE(speech.heard("Saved Queue, 27 October 14:05."));
}

TEST_F(SavePlaylistTest, SleepWhileSavingDropsTheLateAnswer) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(save());
    press(Button::Power);
    expectLateAnswerDropped();
}

TEST_F(SavePlaylistTest, PowerOffWhileSavingDropsTheLateAnswer) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(save());
    hold(Button::Power);
    open("Power off");
    ASSERT_TRUE(app->quitRequested());
    expectLateAnswerDropped();
}

TEST_F(SavePlaylistTest, ALeftAndReopenedQueueListIsNotSpokenTo) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(save());
    const auto id = created().id;
    press(Button::Back); // the Player menu
    ASSERT_NO_FATAL_FAILURE(openQueue());
    EXPECT_FALSE(server.replyCreated(id));
    app->tick();
    EXPECT_NE(speech.last(), "Saved Queue, 27 October 14:05.");
}

TEST_F(SavePlaylistTest, AQueueLongerThanARequestHoldsSavesTheFirstTracksAndSaysHowManyWereLeftOut) {
    play(tracks(MusicServer::kMaxPlaylistTracks + 2));
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_EQ(created().trackIds.size(), MusicServer::kMaxPlaylistTracks);
    EXPECT_EQ(created().trackIds.back(), "t" + std::to_string(MusicServer::kMaxPlaylistTracks));
    ASSERT_NO_FATAL_FAILURE(answer());
    EXPECT_EQ(speech.last(), "Saved Queue, 27 October 14:05. 2 tracks not saved.");
}

TEST_F(QueueListTest, PlayNextPutsAnEntryRightAfterTheCurrentOne) {
    play(tracks(5), 1);
    ASSERT_NO_FATAL_FAILURE(optionsOn("T5", "Play next"));
    EXPECT_EQ(speech.last(), "T5 will play next.");
    EXPECT_EQ(sounds.last(), Earcon::Close);
    EXPECT_EQ(level().title, "Queue") << "an action that finishes a task goes back by itself";
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1", "T2", "T5", "T3", "T4"}));
    EXPECT_EQ(level().index, 2u);
    trackEnds();
    EXPECT_EQ(player.file, "fake://stream/t5");
}

TEST_F(QueueListTest, PlayNextOfAnEntryBeforeTheCurrentOneKeepsTheCurrentOnePlaying) {
    play(tracks(5), 2);
    const size_t opens = player.opened.size();
    ASSERT_NO_FATAL_FAILURE(optionsOn("T1", "Play next"));
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T2", "T3", "T1", "T4", "T5"}));
    EXPECT_EQ(level().index, 2u);
    EXPECT_EQ(app->nowPlaying().chapter, "T3");
    EXPECT_EQ(player.opened.size(), opens);
    EXPECT_TRUE(player.playing());
    trackEnds();
    EXPECT_EQ(player.file, "fake://stream/t1");
}

TEST_F(QueueListTest, PlayNextOnTheCurrentEntryOnlyReportsAndStaysInTheOptions) {
    play(tracks(3), 1);
    ASSERT_NO_FATAL_FAILURE(optionsOn("T2", "Play next"));
    EXPECT_EQ(speech.last(), "T2 is the current entry.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(level().title, "Options, T2");
}

TEST_F(QueueListTest, MoveOpensOnTheEntrysOwnPlaceAndMovingToThreeSaysSoAndChangesTheOrder) {
    play(tracks(30));
    ASSERT_NO_FATAL_FAILURE(openQueue());
    select("T30");
    press(Button::Options);
    open("Move");
    EXPECT_EQ(speech.last(), "Move. 30, T30, 30 of 30");
    EXPECT_EQ(level().index, 29u);
    open("3");
    EXPECT_EQ(speech.last(), "Moved to 3 of 30.");
    EXPECT_EQ(level().title, "Queue") << "back in the list by itself";
    EXPECT_EQ(level().index, 2u);
    auto entries = queueEntries();
    ASSERT_EQ(entries.size(), 30u);
    EXPECT_EQ((std::vector<std::string>(entries.begin(), entries.begin() + 4)), (std::vector<std::string>{"T1", "T2", "T30", "T3"}));
    EXPECT_EQ(entries.back(), "T29");
    trackEnds();
    EXPECT_EQ(player.file, "fake://stream/t2");
    trackEnds();
    EXPECT_EQ(player.file, "fake://stream/t30") << "the order is what plays";
}

TEST_F(QueueListTest, MovingTheCurrentEntryKeepsItCurrentAndPlaying) {
    play(tracks(5), 1);
    const size_t opens = player.opened.size();
    ASSERT_NO_FATAL_FAILURE(optionsOn("T2", "Move"));
    open("4");
    EXPECT_EQ(speech.last(), "Moved to 4 of 5.");
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1", "T3", "T4", "T2", "T5"}));
    EXPECT_EQ(valueOf(level().items[3]), "current");
    EXPECT_EQ(app->nowPlaying().chapter, "T2");
    EXPECT_EQ(player.opened.size(), opens);
    EXPECT_TRUE(player.playing());
    trackEnds();
    EXPECT_EQ(player.file, "fake://stream/t5") << "what follows T2 in the new order";
}

TEST_F(QueueListTest, MovingToItsOwnPlaceMovesNothingAndIsNoEdit) {
    play(tracks(5), 1);
    ASSERT_NO_FATAL_FAILURE(optionsOn("T3", "Move"));
    open("3");
    EXPECT_EQ(speech.last(), "Not moved.");
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1", "T2", "T3", "T4", "T5"}));
    expectNoPrevious();
}

TEST_F(QueueListTest, RemoveTakesAnEntryOutAndTheOthersKeepPlaying) {
    play(tracks(5));
    ASSERT_NO_FATAL_FAILURE(optionsOn("T4", "Remove"));
    EXPECT_EQ(speech.last(), "Removed T4. 4 in queue.");
    EXPECT_EQ(level().title, "Queue");
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1", "T2", "T3", "T5"}));
    EXPECT_EQ(level().index, 3u) << "on the entry that took its place";
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "T1");
}

TEST_F(QueueListTest, RemovingTheLastEntryLeavesTheCursorOnTheNewLastEntryNotOnSaveAsPlaylist) {
    play(tracks(5));
    ASSERT_NO_FATAL_FAILURE(optionsOn("T5", "Remove"));
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1", "T2", "T3", "T4"}));
    EXPECT_EQ(level().items[level().index].label, "T4");
}

TEST_F(QueueListTest, PingsStopWhenTheLastServerTrackAheadIsRemoved) {
    rainFails(); // Calm plays, Dusk (server) is ahead and the server is marked down
    idle(1s);
    ASSERT_NO_FATAL_FAILURE(optionsOn("Dusk", "Remove"));
    idle(10min);
    EXPECT_EQ(pings(), 0) << "nothing of the server is ahead any more";
}

TEST_F(QueueListTest, RemovingTheCurrentEntryStopsItAndMakesTheNextOneCurrentPaused) {
    play(tracks(5), 1);
    ASSERT_NO_FATAL_FAILURE(optionsOn("T2", "Remove"));
    EXPECT_EQ(speech.last(), "Removed T2. T3 is current, paused.");
    EXPECT_FALSE(player.playing());
    EXPECT_FALSE(player.isOpen());
    EXPECT_EQ(app->nowPlaying().chapter, "T3");
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1", "T3", "T4", "T5"}));
    EXPECT_EQ(level().index, 1u);
    EXPECT_EQ(valueOf(level().items[1]), "current");
    press(Button::PlayPause);
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_TRUE(player.playing());
}

TEST_F(QueueListTest, RemovingTheCurrentLastEntryMakesTheFirstOneCurrent) {
    play(tracks(3), 2);
    ASSERT_NO_FATAL_FAILURE(optionsOn("T3", "Remove"));
    EXPECT_EQ(speech.last(), "Removed T3. T1 is current, paused.");
    EXPECT_EQ(level().index, 0u);
    EXPECT_EQ(app->nowPlaying().chapter, "T1");
    EXPECT_FALSE(player.playing());
}

TEST_F(QueueListTest, RemovingTheOnlyEntryLeavesNothingLoadedAndGoesHome) {
    play(tracks(1));
    ASSERT_NO_FATAL_FAILURE(optionsOn("T1", "Remove"));
    EXPECT_EQ(speech.last().rfind("Removed T1. The queue is empty. Library", 0), 0u) << speech.last();
    EXPECT_EQ(app->screens().size(), 1u);
    EXPECT_FALSE(player.playing());
    EXPECT_FALSE(app->nowPlaying().loaded);
    EXPECT_FALSE(hasLabel("Now playing"));
}

TEST_F(QueueListTest, RemoveSetsTheEditedFlag) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(optionsOn("T3", "Remove"));
    expectEditedFlag();
}

TEST_F(QueueListTest, MoveSetsTheEditedFlag) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(optionsOn("T3", "Move"));
    open("1");
    expectEditedFlag();
}

TEST_F(QueueListTest, PlayNextFromTheListSetsTheEditedFlag) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(optionsOn("T3", "Play next"));
    expectEditedFlag();
}

TEST_F(QueueListTest, ClearQueueOpensOnCancelAndCancelChangesNothing) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(openQueue());
    select("Clear queue");
    press(Button::Ok);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Cancel", "Clear queue"}));
    EXPECT_EQ(level().index, 0u);
    EXPECT_EQ(speech.last(), "Clear queue. Cancel, 1 of 2");
    press(Button::Ok);
    EXPECT_EQ(level().title, "Queue");
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1", "T2", "T3"}));
    EXPECT_TRUE(player.playing());
}

TEST_F(QueueListTest, ClearQueueStopsTheMusicAndLeavesNothingLoaded) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(openQueue());
    select("Clear queue");
    press(Button::Ok);
    open("Clear queue");
    EXPECT_EQ(speech.last().rfind("Queue cleared. Library", 0), 0u) << speech.last();
    EXPECT_EQ(app->screens().size(), 1u);
    EXPECT_FALSE(player.playing());
    EXPECT_FALSE(player.isOpen());
    EXPECT_FALSE(app->nowPlaying().loaded);
    EXPECT_FALSE(hasLabel("Now playing")) << "Home no longer offers it";
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Nothing to play. Choose a book in the library.");
}

TEST_F(QueueListTest, ClearingAnEditedQueueKeepsItAsPreviousQueue) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(optionsOn("T3", "Remove"));
    select("Clear queue");
    press(Button::Ok);
    open("Clear queue");
    ASSERT_TRUE(app->playQueue("Other", {remoteEntry("x1", "X1")}));
    ASSERT_NO_FATAL_FAILURE(openQueue());
    open("Previous queue");
    ASSERT_NO_FATAL_FAILURE(openQueue());
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1", "T2"})) << "the undo";
}

TEST_F(QueueListTest, ClearingAQueueThatWasNotEditedKeepsNothing) {
    play(tracks(3));
    ASSERT_NO_FATAL_FAILURE(openQueue());
    select("Clear queue");
    press(Button::Ok);
    open("Clear queue");
    expectNoPrevious();
}

TEST_F(QueueListTest, MovingUnderShuffleKeepsTheCurrentTrackPlayingAndTheOrderIsWhatPlays) {
    play({localEntry(alphaFile(1), "Storm"), localEntry(alphaFile(2), "Calm"), localEntry(lullaby(), "Lullaby"),
          localEntry(sunrise(), "Sunrise")}, 1);
    ASSERT_NO_FATAL_FAILURE(shuffleTracks()); // Storm, Calm, Sunrise, Lullaby
    const size_t opens = player.opened.size();
    ASSERT_NO_FATAL_FAILURE(optionsOn("Storm", "Move"));
    open("4");
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"Calm", "Sunrise", "Lullaby", "Storm"}));
    EXPECT_EQ(app->nowPlaying().chapter, "Calm");
    EXPECT_EQ(player.opened.size(), opens);
    EXPECT_TRUE(player.playing());
    trackEnds();
    EXPECT_EQ(player.file, sunrise());
    EXPECT_EQ(app->nowPlaying().chapter, "Sunrise");
}

TEST_F(QueueListTest, RemovingUnderShuffleKeepsTheOriginalOrderForShuffleOff) {
    play({localEntry(alphaFile(1), "Storm"), localEntry(alphaFile(2), "Calm"), localEntry(lullaby(), "Lullaby"),
          localEntry(sunrise(), "Sunrise")}, 1);
    ASSERT_NO_FATAL_FAILURE(shuffleTracks()); // Storm, Calm, Sunrise, Lullaby
    ASSERT_NO_FATAL_FAILURE(optionsOn("Storm", "Remove"));
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"Calm", "Sunrise", "Lullaby"}));
    EXPECT_EQ(app->nowPlaying().chapter, "Calm");
    ASSERT_NO_FATAL_FAILURE(toPlayer());
    press(Button::Ok);
    open("Shuffle");
    open("off");
    EXPECT_EQ(app->nowPlaying().chapter, "Calm");
    ASSERT_NO_FATAL_FAILURE(openQueue());
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"Calm", "Lullaby", "Sunrise"}));
}

TEST_F(QueueListTest, RemovingTheCurrentEntryUnderShuffleMakesTheNextInTheShuffledOrderCurrent) {
    play({localEntry(alphaFile(1), "Storm"), localEntry(alphaFile(2), "Calm"), localEntry(lullaby(), "Lullaby"),
          localEntry(sunrise(), "Sunrise")}, 1);
    ASSERT_NO_FATAL_FAILURE(shuffleTracks()); // Storm, Calm, Sunrise, Lullaby
    ASSERT_NO_FATAL_FAILURE(optionsOn("Calm", "Remove"));
    EXPECT_EQ(speech.last(), "Removed Calm. Sunrise is current, paused.");
    press(Button::PlayPause);
    EXPECT_EQ(player.file, sunrise());
}

TEST_F(QueueListTest, MovingWhileAServerTrackOpensKeepsTrackOfWhichTrackIsOpening) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), localEntry(lullaby(), "Lullaby"),
                                         localEntry(sunrise(), "Sunrise"), remoteEntry("t4", "Rain")}, 2));
    trackEnds();
    ASSERT_TRUE(player.opening()) << "Rain is opening";
    ASSERT_NO_FATAL_FAILURE(optionsOn("Storm", "Move"));
    open("4"); // Lullaby, Sunrise, Rain, Storm
    player.unopenable = {"fake://stream/t4"};
    finishOpen();
    EXPECT_EQ(player.file, alphaFile(1)) << "Rain failed; the entry after it in the new order plays";
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Storm");
}

TEST_F(QueueListTest, RemovingWhileAServerTrackOpensKeepsTrackOfWhichTrackIsOpening) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), localEntry(lullaby(), "Lullaby"),
                                         remoteEntry("t3", "Rain"), localEntry(sunrise(), "Sunrise")}, 1));
    trackEnds();
    ASSERT_TRUE(player.opening()) << "Rain is opening";
    ASSERT_NO_FATAL_FAILURE(optionsOn("Storm", "Remove")); // Rain moves one place up
    player.unopenable = {"fake://stream/t3"};
    finishOpen();
    EXPECT_EQ(player.file, sunrise()) << "Rain failed; Sunrise follows it";
    EXPECT_TRUE(player.playing());
}

TEST_F(QueueListTest, RemovingTheEntryThatIsOpeningDropsTheOpen) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(lullaby(), "Lullaby"), remoteEntry("t2", "Rain"),
                                         localEntry(sunrise(), "Sunrise")}));
    trackEnds();
    ASSERT_TRUE(player.opening()) << "Rain is opening";
    ASSERT_NO_FATAL_FAILURE(optionsOn("Rain", "Remove"));
    EXPECT_EQ(speech.last(), "Removed Rain. Sunrise is current, paused.");
    EXPECT_FALSE(player.opening());
    app->tick();
    EXPECT_EQ(speech.last(), "Removed Rain. Sunrise is current, paused.") << "the dropped open says nothing, and fails nothing";
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Sunrise");
    press(Button::PlayPause);
    EXPECT_EQ(player.file, sunrise());
    EXPECT_TRUE(player.playing());
}

TEST_F(QueueListTest, RemovingTheLastEntryOfAnEditedQueueKeepsItAsPreviousQueue) {
    play(tracks(2));
    ASSERT_NO_FATAL_FAILURE(optionsOn("T2", "Remove"));
    ASSERT_NO_FATAL_FAILURE(optionsOn("T1", "Remove")); // the last one
    ASSERT_TRUE(app->playQueue("Other", {remoteEntry("x1", "X1")}));
    ASSERT_NO_FATAL_FAILURE(openQueue());
    EXPECT_EQ(valueOf(level().items[2]), "1 track") << labels()[2];
    open("Previous queue");
    ASSERT_NO_FATAL_FAILURE(openQueue());
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"T1"}));
}

TEST_F(QueueListTest, TheOrderCannotChangeWhileATrackChosenInTheListIsOpening) {
    start_();
    app->setRandom([](size_t) { return size_t(0); });
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(lullaby(), "Lullaby"), remoteEntry("t2", "Rain"), localEntry(sunrise(), "Sunrise")}));
    ASSERT_NO_FATAL_FAILURE(openQueue());
    select("Rain");
    press(Button::Ok);
    ASSERT_TRUE(player.opening());
    press(Button::Options);
    open("Remove");
    EXPECT_EQ(speech.last(), "Loading. Try again when it has started.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
    EXPECT_EQ(level().title, "Options, Rain");
    press(Button::Home);
    ASSERT_EQ(level().title, "Queue");
    ASSERT_NO_FATAL_FAILURE(finishOpen());
    EXPECT_EQ(player.file, "fake://stream/t2");
    ASSERT_NO_FATAL_FAILURE(openQueue());
    EXPECT_EQ(queueEntries(), (std::vector<std::string>{"Lullaby", "Rain", "Sunrise"}));
}

TEST_F(QueueListTest, MovingAnEntryKeepsTheEndOfTrackTimerOnTheTrackItWasSetFor) {
    play(tracks(5), 1);
    press(Button::Ok); // Player menu
    open("Sleep timer");
    open("End of track");
    ASSERT_TRUE(speech.heard("Sleep timer set to end of track."));
    ASSERT_NO_FATAL_FAILURE(optionsOn("T5", "Move"));
    open("1"); // T2 is now the third entry
    idle(1s);
    EXPECT_NE(app->mode(), App::Mode::Sleeping) << "T2 has only just begun";
    trackEnds();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
}

TEST_F(QueueListTest, RemovingTheCurrentEntryCancelsTheEndOfTrackTimerSetForIt) {
    play(tracks(5), 1);
    press(Button::Ok); // Player menu
    open("Sleep timer");
    open("End of track");
    ASSERT_NO_FATAL_FAILURE(optionsOn("T2", "Remove"));
    ASSERT_NO_FATAL_FAILURE(toPlayer());
    press(Button::Ok);
    open("Sleep timer");
    EXPECT_EQ(valueOf(level().items[level().index]), "current");
    EXPECT_EQ(level().items[level().index].label, "Off") << "the timer was for a track that is gone";
}

TEST_F(ResumeStreamTest, ClearingTheQueueForgetsTheStreamSoHomeDoesNotOfferItAfterARestart) {
    playFromAlbum("Rain");
    reboot(); // the queue is saved
    press(Button::Ok); // Now playing
    ASSERT_EQ(app->mode(), App::Mode::Player);
    press(Button::Ok); // Player menu
    open("Queue");
    open("Clear queue");
    open("Clear queue");
    EXPECT_FALSE(hasLabel("Now playing"));
    reboot();
    EXPECT_EQ(labels().front(), "Library");
    EXPECT_FALSE(hasLabel("Now playing"));
}

// ---------------------------------------------------------------- keeping the queue (queue.md, step 2.8)

class KeepQueueTest : public QueueListTest {
protected:
    void SetUp() override {
        QueueListTest::SetUp();
        settings.setString("server.password", "hunter2");
    }
    // Exit, and a new App on what was written, as after a reboot. `offline` runs between the two.
    void restart(const std::function<void()>& offline = {}) {
        app->shutdown();
        app.reset();
        if (offline) offline();
        state = Settings(&stateFile);
        state.load();
        player = FakePlayer{};
        speech.said.clear();
        sounds.played.clear();
        start();
    }
    // The queue.tsv on disk, empty if there is none.
    std::string queueText() const { return diskFiles().read(queueFile).value_or(""); }
    std::optional<SavedQueue> savedQueue() const { return parseQueue(queueText()); }
    // The entries of the Queue list, from the player.
    std::vector<std::string> listedQueue() {
        if (!openQueue_()) return {};
        std::vector<std::string> out = queueEntries();
        press(Button::Home);
        press(Button::Home);
        return out;
    }
    bool openQueue_() {
        openQueue();
        return level().title == "Queue";
    }
    // Player menu > `setting` > `value`.
    void choose(const std::string& setting, const std::string& value) {
        ASSERT_NO_FATAL_FAILURE(toPlayer());
        press(Button::Ok);
        open(setting);
        open(value);
    }
    std::string setting(const std::string& name) {
        toPlayer();
        press(Button::Ok);
        for (const MenuItem& item : level().items)
            if (item.label == name) {
                std::string v = valueOf(item);
                press(Button::Home);
                return v;
            }
        return "?";
    }
    std::vector<QueueEntry> mix() {
        QueueEntry storm = localEntry(alphaFile(1), "Storm");
        QueueEntry calm = localEntry(alphaFile(2), "Calm");
        storm.artist = "Local Band";
        return {storm, remoteEntry("t2", "Rain"), calm, remoteEntry("t4", "Fog"), remoteEntry("t5", "Dusk")};
    }
};

TEST_F(KeepQueueTest, SleepThenRestartKeepsTheMixedShuffledQueueItsPlaceAndRepeat) {
    play(mix(), 1);
    ASSERT_NO_FATAL_FAILURE(choose("Shuffle", "tracks"));
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "all"));
    // Move on in the shuffled order: the current entry is whatever comes next.
    ASSERT_NO_FATAL_FAILURE(toPlayer());
    const int prior = app->nowPlaying().chapterIndex;
    press(Button::Forward);
    ASSERT_EQ(app->nowPlaying().chapterIndex, (prior + 1) % app->nowPlaying().chapterCount);
    player.pos = 77;
    const std::vector<std::string> before = listedQueue();
    const std::string playing = app->nowPlaying().chapter;
    ASSERT_EQ(before.size(), 5u);
    ASSERT_NO_FATAL_FAILURE(toPlayer());
    press(Button::Power); // sleep writes the queue
    ASSERT_EQ(files.queueWrites, 1);

    restart();
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(labels().front(), "Now playing");
    EXPECT_EQ(valueOf(level().items[0]), "Mixed");
    EXPECT_TRUE(player.opened.empty()) << "nothing plays until it is chosen";
    press(Button::Ok);
    EXPECT_TRUE(player.playing());
    EXPECT_DOUBLE_EQ(player.pos, 77);
    EXPECT_EQ(app->nowPlaying().chapter, playing);
    EXPECT_EQ(listedQueue(), before) << "the same entries in the same play order";
    EXPECT_EQ(setting("Shuffle"), "tracks");
    EXPECT_EQ(setting("Repeat"), "all");
}

TEST_F(KeepQueueTest, ARestoredQueueIsTheOneTheUserEditedAndKeepsItsPreviousQueueRule) {
    play(tracks(4), 0);
    ASSERT_NO_FATAL_FAILURE(optionsOn("T3", "Remove")); // an edit
    restart();
    press(Button::Ok);
    ASSERT_EQ(app->mode(), App::Mode::Player);
    ASSERT_TRUE(app->playQueue("Other", {remoteEntry("x1", "X1")}));
    ASSERT_NO_FATAL_FAILURE(openQueue());
    EXPECT_TRUE(hasLabel("Previous queue")) << "the edited flag was kept";
}

TEST_F(KeepQueueTest, ExitWritesTheQueueAndASecondExitWithNoChangeWritesNothing) {
    play(mix(), 1);
    player.pos = 20;
    hold(Button::Power);
    open("Power off");
    ASSERT_TRUE(app->quitRequested());
    EXPECT_EQ(files.queueWrites, 1);
    ASSERT_TRUE(savedQueue());
    EXPECT_EQ(savedQueue()->queue.size(), 5u);
    app->shutdown(); // the front end calls it too
    EXPECT_EQ(files.queueWrites, 1) << "nothing changed since";
    app->shutdown();
    EXPECT_EQ(files.queueWrites, 1);
}

TEST_F(KeepQueueTest, AnExitAfterASleepWithNoChangeWritesNothing) {
    play(mix(), 1);
    player.pos = 20;
    press(Button::Power);
    ASSERT_EQ(files.queueWrites, 1);
    press(Button::Ok); // wake
    hold(Button::Power);
    open("Power off");
    app->shutdown();
    EXPECT_EQ(files.queueWrites, 1);
    ASSERT_TRUE(savedQueue());
    EXPECT_DOUBLE_EQ(savedQueue()->position, 20);
}

TEST_F(KeepQueueTest, AnotherShutdownAfterShutdownDoesNotWriteThePlaceAsZero) {
    play(mix(), 1);
    player.pos = 20;
    app->shutdown();
    app->shutdown(); // the front end's SIGTERM handler and the window closing
    EXPECT_EQ(files.queueWrites, 1);
    ASSERT_TRUE(savedQueue());
    EXPECT_DOUBLE_EQ(savedQueue()->position, 20);
}

TEST_F(KeepQueueTest, AChangeAfterTheLastWriteIsWrittenAtTheNextExit) {
    play(mix(), 1);
    player.pos = 20;
    press(Button::Power);
    ASSERT_EQ(files.queueWrites, 1);
    press(Button::Ok); // wake
    press(Button::Forward); // another track
    hold(Button::Power);
    open("Power off");
    EXPECT_EQ(files.queueWrites, 2);
    EXPECT_EQ(savedQueue()->queue.position(), 2u);
}

TEST_F(KeepQueueTest, ALostWriteIsTriedAgainAtTheNextExit) {
    play(mix(), 1);
    files.failQueueWrites = true;
    press(Button::Power);
    EXPECT_EQ(files.queueWrites, 0);
    EXPECT_FALSE(savedQueue());
    files.failQueueWrites = false;
    press(Button::Ok); // wake
    app->shutdown();
    EXPECT_EQ(files.queueWrites, 1);
    EXPECT_TRUE(savedQueue());
}

TEST_F(KeepQueueTest, NothingInQueueTsvIsAStreamUrlATokenOrThePassword) {
    play(mix(), 1);
    app->shutdown();
    const std::string text = queueText();
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("t2"), std::string::npos) << "the track id is kept";
    for (const char* secret : {"fake://", "stream", "hunter2", "t=", "s=", "u=", "p=", "password", "token"})
        EXPECT_EQ(text.find(secret), std::string::npos) << secret;
    for (const auto& [key, value] : stateFile.file) EXPECT_EQ(value.find("hunter2"), std::string::npos) << key;
}

TEST_F(KeepQueueTest, AQueueOfMoreThan5000EntriesIsCutAroundTheCurrentOne) {
    std::vector<QueueEntry> many;
    for (size_t i = 0; i < kMaxSavedEntries + 700; ++i) many.push_back(remoteEntry("t" + std::to_string(i), "T" + std::to_string(i)));
    play(many, 3000);
    app->shutdown();
    ASSERT_TRUE(savedQueue());
    const Queue q = savedQueue()->queue;
    ASSERT_EQ(q.size(), kMaxSavedEntries);
    EXPECT_EQ(q.current()->trackId, "t3000");
    EXPECT_EQ(q.at(0).trackId, "t" + std::to_string(3000 - kMaxSavedEntries / 2));

    restart();
    press(Button::Ok);
    EXPECT_EQ(player.file, "fake://stream/t3000");
}

TEST_F(KeepQueueTest, ACorruptQueueFileStartsWithoutAQueueAndIsReplaced) {
    const std::string good = [&] {
        play(mix(), 1);
        app->shutdown();
        return queueText();
    }();
    ASSERT_FALSE(good.empty());
    const std::vector<std::string> bad = {"garbage", std::string("\0\xff\xfe\t\t\n\n", 8), good.substr(0, good.size() / 2) + "\tR\tx",
                                          "oap-queue\t1\nL\t/a\n", "oap-queue\t9\n" + good.substr(good.find('\n') + 1)};
    for (const std::string& text : bad) {
        app.reset();
        ASSERT_TRUE(diskFiles().writeAtomically(queueFile, text));
        files.queueWrites = 0;
        player = FakePlayer{};
        state = Settings(&stateFile);
        state.load();
        start();
        EXPECT_FALSE(hasLabel("Now playing")) << text;
        EXPECT_EQ(labels().front(), "Library");
        press(Button::Ok); // the app answers its keys
        app->shutdown();
        EXPECT_EQ(files.queueWrites, 1) << "the damaged file is replaced";
        EXPECT_FALSE(savedQueue());
    }
}

TEST_F(KeepQueueTest, ABrokenOrderAndPositionStillGiveAQueue) {
    const std::string text = "oap-queue\t1\ntitle\tHand made\nshuffle\ttracks\nposition\t9\norder\t0\t0\t7\n"
                             "R\thttps://music.example\tann\tt1\t\tOne\tAlbum\tBand\t10\n"
                             "R\thttps://music.example\tann\tt2\t\tTwo\tAlbum\tBand\t10\n";
    ASSERT_TRUE(diskFiles().writeAtomically(queueFile, text));
    start();
    ASSERT_EQ(labels().front(), "Now playing");
    press(Button::Ok);
    EXPECT_EQ(player.file, "fake://stream/t1") << "the first entry";
    EXPECT_EQ(listedQueue(), (std::vector<std::string>{"One", "Two"}));
    EXPECT_EQ(setting("Shuffle"), "off");
}

TEST_F(KeepQueueTest, AFileTooLongToReadIsNotReadAndIsReplaced) {
    fs::create_directories(queueFile.parent_path());
    {
        std::ofstream(queueFile) << "x";
    }
    fs::resize_file(queueFile, kMaxQueueFileBytes + 1); // sparse: nothing is written
    start();
    EXPECT_EQ(files.queueReads, 0) << "a file that long is not read";
    EXPECT_EQ(labels().front(), "Library");
    app->shutdown();
    EXPECT_LT(fs::file_size(queueFile), 10u) << "replaced by an empty file";
}

TEST_F(KeepQueueTest, TitlesWithTabsAndLineBreaksComeBack) {
    std::vector<QueueEntry> entries = {remoteEntry("t1", "Wind\tand\nRain"), remoteEntry("t2", "Back\\slash \r end")};
    entries[0].album = "Al\tbum";
    play(entries, 0);
    restart();
    press(Button::Ok);
    EXPECT_EQ(listedQueue(), (std::vector<std::string>{"Wind\tand\nRain", "Back\\slash \r end"}));
}

TEST_F(KeepQueueTest, ClearQueueThenExitLeavesNoQueueAfterARestart) {
    play(mix(), 1);
    press(Button::Power);
    press(Button::Ok); // wake
    ASSERT_TRUE(savedQueue());
    ASSERT_NO_FATAL_FAILURE(openQueue());
    open("Clear queue");
    open("Clear queue");
    app->shutdown();
    EXPECT_FALSE(savedQueue());
    restart();
    EXPECT_FALSE(hasLabel("Now playing"));
    EXPECT_EQ(labels().front(), "Library");
}

TEST_F(KeepQueueTest, ARemovedLastEntryLeavesNoQueueAfterARestart) {
    play(tracks(1), 0);
    press(Button::Power);
    press(Button::Ok);
    ASSERT_TRUE(savedQueue());
    ASSERT_NO_FATAL_FAILURE(optionsOn("T1", "Remove"));
    restart();
    EXPECT_FALSE(hasLabel("Now playing"));
}

TEST_F(KeepQueueTest, AParkedMusicQueueSurvivesARestartWhileABookPlays) {
    play(mix(), 1);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "track"));
    player.pos = 33;
    hold(Button::Home);
    open("Library");
    open("Alpha");
    player.pos = 12;
    const std::string alpha1 = alphaFile(1);
    restart();
    ASSERT_EQ(app->mode(), App::Mode::Player) << "the book played last";
    EXPECT_EQ(player.file, alpha1);
    EXPECT_DOUBLE_EQ(player.pos, 12);
    hold(Button::Home);
    EXPECT_EQ(labels()[0], "Now playing");
    EXPECT_EQ(valueOf(level().items[0]), "Alpha");
    ASSERT_EQ(labels()[1], "Continue");
    EXPECT_EQ(valueOf(level().items[1]), "Mixed");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 33);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(setting("Repeat"), "track");
    EXPECT_EQ(listedQueue().size(), 5u);
}

TEST_F(KeepQueueTest, TheQueueOfTheBookThatIsNotLoadedYetIsNotLost) {
    play(mix(), 1);
    app->shutdown();
    // A restart where nothing is played, twice: the second one still has the queue.
    for (int i = 0; i < 2; ++i) {
        files.queueWrites = 0;
        restart();
        ASSERT_TRUE(hasLabel("Now playing")) << i;
        app->shutdown();
        EXPECT_EQ(files.queueWrites, 0) << "nothing changed: the file is as it was";
    }
}

TEST_F(KeepQueueTest, OkOnNowPlayingThatCannotOpenSaysSoAndKeepsTheQueue) {
    play(tracks(3), 1);
    player.pos = 40;
    restart();
    player.unopenable = {"fake://stream/t2"};
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Cannot play T2. Server not reachable.");
    EXPECT_EQ(labels().front(), "Now playing") << "it can be tried again";
    player.unopenable.clear();
    press(Button::Ok); // Now playing again: it plays from where it was
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 40);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(app->mode(), App::Mode::Player);
}

TEST_F(KeepQueueTest, FactoryResetForgetsTheQueue) {
    play(mix(), 1);
    press(Button::Power);
    press(Button::Ok);
    ASSERT_TRUE(savedQueue());
    hold(Button::Home);
    open("System");
    open("Factory reset");
    open("Erase all settings");
    EXPECT_FALSE(savedQueue());
    EXPECT_FALSE(hasLabel("Now playing"));
}

TEST_F(KeepQueueTest, OldLastStreamKeysBecomeAQueueAndAreRemoved) {
    settings.setString("server.user", "ann");
    state.setString("last_stream.server", "https://music.example");
    state.setString("last_stream.user", "ann");
    state.setString("last_stream.kind", "album");
    state.setString("last_stream.id", "al1");
    state.setString("last_stream.title", "Weather");
    state.setString("last_stream.track", "1");
    state.setString("last_stream.track_id", "t2");
    state.setString("last_stream.pos", "42.0");
    start();
    EXPECT_EQ(speech.last(), "Home. Now playing, Weather, 1 of 8");
    press(Button::Ok); // the list is loaded once, as before
    EXPECT_EQ(speech.last(), "Loading.");
    ASSERT_EQ(server.lastRequest().kind, Kind::AlbumTracks);
    ASSERT_TRUE(server.reply(server.lastRequest().id, weather()));
    app->tick();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());

    app->shutdown();
    for (const auto& [key, value] : stateFile.file) EXPECT_NE(key.rfind("last_stream.", 0), 0u) << key << ": removed";
    ASSERT_TRUE(savedQueue());
    EXPECT_EQ(savedQueue()->title, "Weather");
    EXPECT_EQ(savedQueue()->queue.size(), 3u);
    EXPECT_EQ(savedQueue()->queue.current()->trackId, "t2");
    EXPECT_DOUBLE_EQ(savedQueue()->position, 42);

    restart(); // from the queue now: nothing is loaded
    const size_t requests = server.requests.size();
    press(Button::Ok);
    EXPECT_EQ(server.requests.size(), requests);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
}

TEST_F(KeepQueueTest, OldLastStreamKeysAreRemovedWhenAnotherQueuePlaysInstead) {
    state.setString("last_stream.server", "https://music.example");
    state.setString("last_stream.user", "ann");
    state.setString("last_stream.kind", "album");
    state.setString("last_stream.id", "al1");
    state.setString("last_stream.title", "Weather");
    state.setString("last_stream.track_id", "t1");
    start();
    ASSERT_TRUE(app->playQueue("Mixed", tracks(2)));
    app->shutdown();
    for (const auto& [key, value] : stateFile.file) EXPECT_NE(key.rfind("last_stream.", 0), 0u) << key;
    restart();
    EXPECT_EQ(valueOf(level().items[0]), "Mixed");
}

TEST_F(KeepQueueTest, TheQueueWinsOverOldLastStreamKeys) {
    play(tracks(3), 2);
    app->shutdown();
    app.reset();
    stateFile.file["last_stream.server"] = "https://music.example";
    stateFile.file["last_stream.user"] = "ann";
    stateFile.file["last_stream.kind"] = "album";
    stateFile.file["last_stream.id"] = "al1";
    stateFile.file["last_stream.title"] = "Weather";
    stateFile.file["last_stream.track_id"] = "t1";
    state = Settings(&stateFile);
    state.load();
    start();
    EXPECT_EQ(valueOf(level().items[0]), "Mixed");
    app->shutdown();
    for (const auto& [key, value] : stateFile.file) EXPECT_NE(key.rfind("last_stream.", 0), 0u) << key;
}

// ---------------------------------------------------------------- playing from the right server

// Two servers: the fixture's `server` (music.example, user ann) and `second` (books.example, user ben, which
// streams as "fake://books/<id>"). Both are given tracks with the same ids, so only the server tells them apart.
class TwoServerPlayTest : public MusicQueueTest {
protected:
    void SetUp() override {
        MusicQueueTest::SetUp();
        cache = base / "cache";
        clock.setWallTime(kNow);
        player.length = 200;
        second.streamHost = "books";
        moreLinks.push_back(secondLink());
    }
    static constexpr std::time_t kNow = 1'800'000'000;
    ServerLink secondLink() {
        ServerConfig config;
        config.name = "Audiobooks";
        config.url = "https://books.example";
        config.user = "ben";
        config.password = "hunter2";
        return ServerLink{config, TrackCache::serverKey(config.url, config.user), "Audiobooks", second, secondDownloader};
    }
    QueueEntry onFirst(const std::string& id, const std::string& title) const { return remoteEntry(id, title); }
    QueueEntry onSecond(const std::string& id, const std::string& title) const {
        QueueEntry e = remoteEntry(id, title);
        e.serverUrl = "https://books.example";
        e.user = "ben";
        return e;
    }
    std::string firstKey() const { return TrackCache::serverKey("https://music.example", "ann"); }
    std::string secondKey() const { return TrackCache::serverKey("https://books.example", "ben"); }
    // Exit, and a new App on what was written, as after a reboot; `offline` runs between the two.
    void reboot(const std::function<void()>& offline = {}) {
        app->shutdown();
        app.reset();
        if (offline) offline();
        state = Settings(&stateFile);
        state.load();
        player = FakePlayer{};
        player.length = 200;
        speech.said.clear();
        sounds.played.clear();
        start_();
    }
    size_t submissionsTo(FakeMusicServer& fake) const {
        return size_t(std::count_if(fake.scrobbles.begin(), fake.scrobbles.end(), [](const auto& s) { return s.submission; }));
    }
    // Everything the App wrote that survives a restart: no URL of a stream and no password.
    void expectNothingSavedHoldsAUrl() const {
        std::vector<std::string> texts;
        for (const auto& [key, value] : stateFile.file) texts.push_back(key + "=" + value);
        for (const fs::path& file : {queueFile, scrobbleFile}) texts.push_back(diskFiles().read(file).value_or(""));
        for (const std::string& text : texts) {
            EXPECT_EQ(text.find("fake://"), std::string::npos) << text;
            EXPECT_EQ(text.find("hunter2"), std::string::npos) << text;
        }
    }
    // Track `id` of the second server's album b1 is in its cache folder; returns the cache and the file.
    std::pair<TrackCache, fs::path> cacheOnSecond(const std::string& id) {
        TrackCache booksCache(diskFiles(), cache, "https://books.example", "ben");
        fs::create_directories(booksCache.listFolder(albumSource("b1")));
        const fs::path file = booksCache.listFolder(albumSource("b1")) / (id + ".mp3");
        touch(file);
        EXPECT_TRUE(booksCache.save(CachedList{albumSource("b1"), "Dune", "Herbert", {{id, id + ".mp3", "Part one", 100, "", true, 0}}}));
        return {booksCache, file};
    }
    // Player menu > Queue > Options on `label` > Remove.
    void removeFromQueue(const std::string& label) {
        press(Button::Ok);
        open("Queue");
        for (size_t i = 0; i < level().items.size() && level().items[level().index].label != label; ++i) press(Button::Forward);
        ASSERT_EQ(level().items[level().index].label, label);
        press(Button::Options);
        open("Remove");
    }
    int pingsOf(FakeMusicServer& fake) const {
        return int(std::count_if(fake.requests.begin(), fake.requests.end(),
                                 [](const FakeMusicServer::Request& r) { return r.kind == FakeMusicServer::Kind::Ping; }));
    }
    FakeMusicServer second;
    FakeDownloader secondDownloader;
};

TEST_F(TwoServerPlayTest, QueueSyncSendsOnlyCurrentServersTracksAndDefersTheOtherServer) {
    server.queueSync = true;
    second.queueSync = true;
    auto lastQueue = [](const FakeMusicServer& fake) {
        for (auto it = fake.requests.rbegin(); it != fake.requests.rend(); ++it)
            if (it->kind == FakeMusicServer::Kind::PlayQueue) return it->id;
        return FakeMusicServer::RequestId{};
    };
    start();
    auto initial = lastQueue(server);
    ASSERT_NE(initial, 0u);
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("a", "A"), onSecond("b", "B"), onFirst("a", "A again")}, 0));
    app->tick();
    RemoteQueue old; old.changed = (kNow - 1) * 1000;
    ASSERT_TRUE(server.replyQueue(initial, old)); app->tick();
    ASSERT_NE(lastQueue(second), 0u);
    ASSERT_TRUE(second.replyQueue(lastQueue(second), old)); app->tick();
    EXPECT_TRUE(second.queueSaves.empty());
    clock.advance(31s); app->tick();
    ASSERT_TRUE(server.replyQueue(lastQueue(server), old)); app->tick();
    ASSERT_EQ(server.queueSaves.size(), 1u);
    ASSERT_EQ(server.queueSaves[0].tracks.size(), 2u);
    EXPECT_EQ(server.queueSaves[0].tracks[0].id, "a");
    EXPECT_EQ(server.queueSaves[0].tracks[1].id, "a");
    EXPECT_EQ(server.queueSaves[0].current, 0u);
    EXPECT_TRUE(second.queueSaves.empty());
}

TEST_F(TwoServerPlayTest, ATrackFromEachServerOpensItsStreamFromItsOwnServer) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t1", "Storm"), onSecond("t1", "Dune"), onFirst("t2", "Rain")}));
    EXPECT_EQ(player.file, "fake://stream/t1");
    finishOpen();
    trackEnds();
    EXPECT_EQ(player.file, "fake://books/t1") << "the same id, the other server";
    finishOpen();
    trackEnds();
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
    expectNoUrlSpoken(speech);
}

TEST_F(TwoServerPlayTest, ATrackChosenFromTheSecondServersListPlaysFromIt) {
    start_();
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"b1", "Dune", "Herbert", 2}}, false));
    app->tick();
    open("Dune");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteTrack>{{"c1", "Part one", "Herbert", "Dune", 300, 1},
                                                                               {"c2", "Part two", "Herbert", "Dune", 300, 2}},
                             false));
    app->tick();
    open("Part two");
    finishOpen();
    EXPECT_EQ(player.file, "fake://books/c2");
    app->shutdown();
    const SavedQueue saved = parseQueue(diskFiles().read(queueFile).value_or("")).value_or(SavedQueue{});
    ASSERT_EQ(saved.queue.size(), 2u);
    EXPECT_EQ(saved.queue.current()->serverUrl, "https://books.example");
    EXPECT_EQ(saved.queue.current()->user, "ben");
    expectNoUrlSpoken(speech);
}

TEST_F(TwoServerPlayTest, AScrobbleGoesToTheServerThatPlayedTheTrack) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onSecond("t1", "Dune")}));
    finishOpen();
    listen(101s);
    ASSERT_EQ(second.scrobbles.size(), 2u);
    EXPECT_EQ(second.scrobbles[0], (FakeMusicServer::Scrobble{"t1", false})) << "now playing";
    EXPECT_EQ(second.scrobbles[1], (FakeMusicServer::Scrobble{"t1", true}));
    EXPECT_TRUE(server.scrobbles.empty()) << "the other server heard nothing";
    EXPECT_TRUE(parseScrobbles(diskFiles().read(scrobbleFile).value_or("")).empty()) << "its server took it";
}

TEST_F(TwoServerPlayTest, ATrackOfTheFirstServerIsScrobbledOnlyThere) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t1", "Storm")}));
    finishOpen();
    listen(101s);
    EXPECT_EQ(submissionsTo(server), 1u);
    EXPECT_TRUE(second.scrobbles.empty());
}

TEST_F(TwoServerPlayTest, AScrobbleKeptForASecondServerIsSentToItAtStartUp) {
    ASSERT_TRUE(diskFiles().writeAtomically(scrobbleFile, formatScrobbles({{secondKey(), "b7", 7000}, {firstKey(), "t8", 8000}})));
    start_();
    app->tick(); // the answers arrive and the next entry goes out
    ASSERT_EQ(submissionsTo(server), 1u);
    EXPECT_EQ(server.scrobbles[0].trackId, "t8");
    EXPECT_EQ(server.scrobbles[0].timeMs, 8000);
    ASSERT_EQ(submissionsTo(second), 1u);
    EXPECT_EQ(second.scrobbles[0].trackId, "b7");
    EXPECT_EQ(second.scrobbles[0].timeMs, 7000);
    app->tick();
    EXPECT_TRUE(parseScrobbles(diskFiles().read(scrobbleFile).value_or("")).empty());
}

TEST_F(TwoServerPlayTest, AScrobbleWaitsWhileItsServerIsMarkedDownAndGoesWhenItAnswers) {
    player.length = 200;
    second.scrobbleError = MusicServer::Error::Unreachable; // its answer keeps the entry
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onSecond("b1", "Dune"), onSecond("b2", "Arrakis"), onFirst("t3", "Rain"), onSecond("b4", "Sand")}));
    finishOpen();
    listen(101s);
    ASSERT_EQ(submissionsTo(second), 1u);
    ASSERT_EQ(parseScrobbles(diskFiles().read(scrobbleFile).value_or("")).size(), 1u) << "kept: the server did not take it";
    player.unopenable = {"fake://books/b2"};
    trackEnds();
    finishOpen(); // the second server cannot be reached: it is marked down, Rain plays from the first
    finishOpen();
    ASSERT_EQ(player.file, "fake://stream/t3");
    listen(101s);
    EXPECT_EQ(submissionsTo(server), 1u) << "the first server's entry goes";
    EXPECT_EQ(submissionsTo(second), 1u) << "nothing is sent to a server that is marked down";
    ASSERT_EQ(parseScrobbles(diskFiles().read(scrobbleFile).value_or("")).size(), 1u);
    second.scrobbleError = MusicServer::Error::None;
    ASSERT_EQ(second.lastRequest().kind, FakeMusicServer::Kind::Ping) << "its pings are running";
    ASSERT_TRUE(second.replyPing(second.lastRequest().id, {"abs", "2.30"}));
    app->tick();
    app->tick();
    EXPECT_EQ(submissionsTo(second), 2u) << "it answers: the kept entry is sent";
    EXPECT_TRUE(parseScrobbles(diskFiles().read(scrobbleFile).value_or("")).empty());
}

TEST_F(TwoServerPlayTest, TheNextTrackIsPreOpenedOnItsOwnServer) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t1", "Storm"), onSecond("t1", "Dune")}));
    finishOpen();
    player.pos = player.length - 5;
    app->tick();
    EXPECT_EQ(player.preloads, (std::vector<std::string>{"fake://books/t1"}));
    trackEnds();
    EXPECT_EQ(player.file, "fake://books/t1");
    EXPECT_EQ(player.adopted, 1);
}

TEST_F(TwoServerPlayTest, APreOpenedTrackOfOneServerIsNotTakenForTheSameIdOnTheOther) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t0", "Intro"), onSecond("t1", "Dune"), onFirst("t1", "Storm")}));
    finishOpen();
    player.pos = player.length - 5;
    app->tick();
    ASSERT_EQ(player.preloads, (std::vector<std::string>{"fake://books/t1"})); // Dune is next
    press(Button::Ok);
    open("Queue");
    press(Button::Forward);
    press(Button::Forward);
    press(Button::Ok); // Storm, the first server's t1
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t1") << "not the stream pre-opened for the other server's t1";
    EXPECT_EQ(app->nowPlaying().chapter, "Storm");
}

TEST_F(TwoServerPlayTest, ATrackThatFailsOnOneServerMarksOnlyThatServerDown) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t1", "Storm"), onSecond("t2", "Dune"), onSecond("t3", "Arrakis"), onFirst("t4", "Rain")}));
    finishOpen();
    player.unopenable = {"fake://books/t2"};
    trackEnds();
    finishOpen();
    EXPECT_EQ(speech.last(), "2 tracks, source unavailable.") << "the second server's two tracks";
    EXPECT_EQ(player.file, "fake://stream/t4") << "the first server is not marked down";
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://books/t3"), 0) << "not even tried";
}

TEST_F(TwoServerPlayTest, AServerMarkedDownIsPingedAndNoOtherServerIsAsked) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t1", "Storm"), onSecond("t2", "Dune"), onSecond("t3", "Arrakis")}));
    finishOpen();
    player.unopenable = {"fake://books/t2"};
    trackEnds();
    finishOpen(); // marked down; the queue waits at Dune
    const size_t before = server.requests.size();
    idle(11s);
    ASSERT_FALSE(second.requests.empty());
    EXPECT_EQ(second.lastRequest().kind, FakeMusicServer::Kind::Ping);
    EXPECT_EQ(server.requests.size(), before) << "the first server is not asked";
    ASSERT_TRUE(second.replyPing(second.lastRequest().id, {"abs", "2.30"}));
    app->tick();
    player.unopenable.clear();
    press(Button::PlayPause);
    finishOpen();
    EXPECT_EQ(player.file, "fake://books/t2") << "the mark is gone";
}

TEST_F(TwoServerPlayTest, TheMixedQueueResumesAfterARestartOnBothServers) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t1", "Storm"), onSecond("t1", "Dune"), onFirst("t3", "Rain")}, 1));
    finishOpen();
    player.pos = 42;
    reboot();
    ASSERT_EQ(labels().front(), "Now playing");
    press(Button::Ok);
    finishOpen();
    EXPECT_EQ(player.file, "fake://books/t1") << "the entry it stopped at, on its own server";
    EXPECT_DOUBLE_EQ(player.pos, 42);
    trackEnds();
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t3");
    press(Button::Back);
    finishOpen();
    press(Button::Back);
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t1") << "the first entry plays from the first server";
    expectNoUrlSpoken(speech);
    expectNothingSavedHoldsAUrl();
}

TEST_F(TwoServerPlayTest, TheQueueFileNamesTheServerOfEachEntryByUrlAndUser) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t1", "Storm"), onSecond("t1", "Dune")}));
    finishOpen();
    app->shutdown();
    const SavedQueue saved = parseQueue(diskFiles().read(queueFile).value_or("")).value_or(SavedQueue{});
    ASSERT_EQ(saved.queue.size(), 2u);
    EXPECT_EQ(saved.queue.at(0).serverUrl, "https://music.example");
    EXPECT_EQ(saved.queue.at(0).user, "ann");
    EXPECT_EQ(saved.queue.at(1).serverUrl, "https://books.example");
    EXPECT_EQ(saved.queue.at(1).user, "ben");
    expectNothingSavedHoldsAUrl();
}

TEST_F(TwoServerPlayTest, EntriesOfAServerRemovedFromTheConfigAreSkippedAndTheOthersPlay) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onSecond("t1", "Dune"), onSecond("t2", "Arrakis"), onFirst("t3", "Rain")}));
    finishOpen();
    reboot([this] { moreLinks.clear(); });
    ASSERT_EQ(labels().front(), "Now playing");
    press(Button::Ok);
    EXPECT_TRUE(speech.heard("Cannot play 2 tracks. Server not set up."));
    EXPECT_GE(std::count(sounds.played.begin(), sounds.played.end(), Earcon::Error), 1);
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://books/t1"), 0);
    expectNoUrlSpoken(speech);
}

TEST_F(TwoServerPlayTest, ASingleEntryOfARemovedServerIsNamedInTheSkip) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t0", "Intro"), onSecond("t1", "Dune"), onFirst("t3", "Rain")}));
    finishOpen();
    reboot([this] { moreLinks.clear(); });
    press(Button::Ok);
    finishOpen();
    trackEnds();
    EXPECT_TRUE(speech.heard("Cannot play Dune. Server not set up."));
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_FALSE(speech.heard("Dune, source unavailable."));
}

TEST_F(TwoServerPlayTest, TheQueueOfARemovedServerIsKeptAndPlaysAgainWhenTheServerIsBack) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onSecond("t1", "Dune"), onFirst("t3", "Rain")}));
    finishOpen();
    reboot([this] { moreLinks.clear(); });
    reboot([this] { moreLinks.push_back(secondLink()); });
    press(Button::Ok);
    finishOpen();
    EXPECT_EQ(player.file, "fake://books/t1");
}

TEST_F(TwoServerPlayTest, AStreamedTrackIsCachedThroughItsOwnServersDownloaderIntoItsOwnFolder) {
    start_();
    ASSERT_TRUE(app->playRemote("Dune", {{"t1", "Part one", "Herbert", "Dune", 200, 1, "mp3", 5}}, 0, albumSource("b1"), 0, "Herbert", 1));
    finishOpen();
    listen(11s);
    ASSERT_EQ(secondDownloader.requests.size(), 1u);
    EXPECT_TRUE(downloader.requests.empty()) << "the first server's downloader is not used";
    const TrackCache booksCache(diskFiles(), cache, "https://books.example", "ben");
    EXPECT_EQ(secondDownloader.lastRequest().trackId, "t1");
    EXPECT_EQ(secondDownloader.lastRequest().dest, (booksCache.listFolder(albumSource("b1")) / "t1.mp3").string());
    ASSERT_TRUE(secondDownloader.finish(secondDownloader.lastRequest().id, Downloader::Result::Ok, true));
    app->tick();
    EXPECT_TRUE(booksCache.find("t1")) << "in the second server's manifest";
    EXPECT_FALSE(TrackCache(diskFiles(), cache, "https://music.example", "ann").find("t1")) << "not in the first's";
}

TEST_F(TwoServerPlayTest, ACachedTrackOfOneServerIsNotPlayedForTheSameIdOnTheOther) {
    // The second server's t1 is in the cache; the first server's t1 is not.
    const auto [booksCache, booksFile] = cacheOnSecond("t1");
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t1", "Storm"), onSecond("t1", "Part one")}));
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t1") << "the first server's t1 streams";
    trackEnds();
    EXPECT_EQ(player.file, booksFile.string()) << "the second's plays from its file";
}

TEST_F(TwoServerPlayTest, ThePlayOfACachedTrackIsWrittenToTheManifestOfItsOwnServer) {
    const auto [booksCache, booksFile] = cacheOnSecond("t1");
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onSecond("t1", "Part one")}));
    listen(30s);
    app->shutdown();
    EXPECT_EQ(booksCache.load(albumSource("b1")).tracks.at(0).lastPlayed, kNow);
}

TEST_F(TwoServerPlayTest, GoToAlbumOfATrackOfTheSecondServerLoadsItFromThatServer) {
    QueueEntry track = onSecond("t1", "Part one");
    track.albumId = "b1";
    track.album = "Dune";
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {track}));
    finishOpen();
    const size_t asked = server.requests.size();
    press(Button::Options);
    open("Go to album");
    ASSERT_EQ(second.lastRequest().kind, FakeMusicServer::Kind::AlbumTracks);
    EXPECT_EQ(second.lastRequest().arg, "b1");
    EXPECT_EQ(server.requests.size(), asked) << "the first server is not asked";
}

TEST_F(TwoServerPlayTest, NothingSpokenOrSavedHoldsAUrlAfterPlayingFromBothServers) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t1", "Storm"), onSecond("t2", "Dune")}));
    finishOpen();
    listen(101s);
    trackEnds();
    finishOpen();
    listen(101s);
    app->shutdown();
    expectNoUrlSpoken(speech);
    expectNothingSavedHoldsAUrl();
}

TEST_F(TwoServerPlayTest, ServerStatusSaysNotReachableOnlyForTheServerThatIsMarkedDown) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t1", "Storm"), onSecond("t2", "Dune"), onFirst("t3", "Rain")}));
    finishOpen();
    player.unopenable = {"fake://books/t2"};
    trackEnds();
    finishOpen();
    finishOpen();
    hold(Button::Home);
    open("Audiobooks");
    EXPECT_EQ(valueOf(level().items.back()), "not reachable");
    hold(Button::Home);
    open("Server");
    ASSERT_EQ(labels().back(), "Server status");
    EXPECT_EQ(valueOf(level().items.back()), "") << "the first server is fine";
    hold(Button::Home);
    open("Audiobooks");
    open("Server status");
    ASSERT_EQ(second.lastRequest().kind, FakeMusicServer::Kind::Ping);
    ASSERT_TRUE(second.replyPing(second.lastRequest().id, {"abs", "2.30"}));
    app->tick();
    EXPECT_EQ(valueOf(level().items.back()), "") << "an answer from that server clears its mark";
}

TEST_F(TwoServerPlayTest, ADownloadedTrackOfAServerThatIsMarkedDownStillPlaysFromItsFile) {
    const auto [booksCache, booksFile] = cacheOnSecond("b9");
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t1", "Storm"), onSecond("b2", "Dune"), onSecond("b9", "Part one")}));
    finishOpen();
    player.unopenable = {"fake://books/b2"};
    trackEnds();
    finishOpen(); // the second server is marked down
    EXPECT_EQ(player.file, booksFile.string()) << "its downloaded track plays whatever the server does";
    EXPECT_FALSE(speech.heard("Part one, source unavailable."));
}

TEST_F(TwoServerPlayTest, TheAutomaticCacheOfASecondServerCountsItsBytesAgainstTheLimit) {
    TrackCache booksCache(diskFiles(), cache, "https://books.example", "ben");
    fs::create_directories(booksCache.listFolder(albumSource("b1")));
    std::ofstream(booksCache.listFolder(albumSource("b1")) / "x.mp3", std::ios::binary) << std::string(5, 'x');
    ASSERT_TRUE(booksCache.save(CachedList{albumSource("b1"), "Dune", "Herbert", {{"x", "x.mp3", "Old", 100, "", false, 100}}}));
    start_();
    app->setCacheGigabyte(12); // a limit of 12 bytes
    const std::vector<RemoteTrack> tracks = {{"t1", "One", "Herbert", "Dune", 200, 1, "mp3", 5}, {"t2", "Two", "Herbert", "Dune", 200, 2, "mp3", 5}};
    ASSERT_TRUE(app->playRemote("Dune", tracks, 0, albumSource("b1"), 0, "Herbert", 1));
    finishOpen();
    listen(11s);
    ASSERT_EQ(secondDownloader.requests.size(), 1u);
    ASSERT_TRUE(secondDownloader.finish(secondDownloader.lastRequest().id, Downloader::Result::Ok, true));
    app->tick();
    ASSERT_TRUE(booksCache.find("x")) << "10 bytes of 12: nothing goes yet";
    press(Button::Forward);
    finishOpen();
    listen(11s);
    ASSERT_EQ(secondDownloader.requests.size(), 2u);
    ASSERT_TRUE(secondDownloader.finish(secondDownloader.lastRequest().id, Downloader::Result::Ok, true));
    app->tick();
    EXPECT_FALSE(booksCache.find("x")) << "15 bytes: the track played longest ago goes";
    EXPECT_TRUE(booksCache.find("t2"));
}

TEST_F(TwoServerPlayTest, ExitCancelsTheTransferOnTheDownloaderItRunsOn) {
    start_();
    ASSERT_TRUE(app->playRemote("Dune", {{"t1", "One", "Herbert", "Dune", 200, 1, "mp3", 5}}, 0, albumSource("b1"), 0, "Herbert", 1));
    finishOpen();
    listen(11s);
    ASSERT_EQ(secondDownloader.requests.size(), 1u);
    const auto id = secondDownloader.lastRequest().id;
    app->shutdown();
    EXPECT_EQ(secondDownloader.cancelled, (std::vector<Downloader::RequestId>{id}));
    EXPECT_TRUE(downloader.cancelled.empty());
}

TEST_F(TwoServerPlayTest, ASecondServerWhoseDownloaderIsNotSetUpCachesNothing) {
    secondDownloader.isConfigured = false;
    start_();
    ASSERT_TRUE(app->playRemote("Dune", {{"t1", "One", "Herbert", "Dune", 200, 1, "mp3", 5}}, 0, albumSource("b1"), 0, "Herbert", 1));
    finishOpen();
    listen(11s);
    EXPECT_TRUE(secondDownloader.requests.empty());
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(TwoServerPlayTest, ASecondServerCachesEvenWhenTheFirstServersDownloaderIsNotSetUp) {
    downloader.isConfigured = false;
    start_();
    ASSERT_TRUE(app->playRemote("Dune", {{"t1", "One", "Herbert", "Dune", 200, 1, "mp3", 5}}, 0, albumSource("b1"), 0, "Herbert", 1));
    finishOpen();
    listen(11s);
    EXPECT_EQ(secondDownloader.requests.size(), 1u);
}

TEST_F(TwoServerPlayTest, WhenTheNextTrackChangesToTheSameIdOnTheOtherServerThePreOpenIsRedone) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t0", "Intro"), onSecond("t1", "Dune"), onFirst("t1", "Storm")}));
    finishOpen();
    player.pos = player.length - 5;
    app->tick();
    ASSERT_EQ(player.preloads, (std::vector<std::string>{"fake://books/t1"}));
    ASSERT_NO_FATAL_FAILURE(removeFromQueue("Dune")); // Storm is next now
    app->tick();
    EXPECT_EQ(player.preloads, (std::vector<std::string>{"fake://books/t1", "fake://stream/t1"}));
}

TEST_F(TwoServerPlayTest, PlayPingsTheServerOfTheEntryItWaitsAtBeforeAnotherServerThatIsDown) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), onFirst("t2", "Rain"), onSecond("b1", "Dune"),
                                         onFirst("t4", "Fog")}));
    player.unopenable = {"fake://books/b1", "fake://stream/t2"};
    trackEnds();
    finishOpen();
    finishOpen();
    ASSERT_EQ(speech.last(), "Server not reachable. Press play to try again.") << "both servers are down; the queue waits at Dune";
    const int firstBefore = pingsOf(server), secondBefore = pingsOf(second);
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Checking the server.");
    EXPECT_EQ(pingsOf(second), secondBefore + 1) << "the server of the entry it waits at";
    EXPECT_EQ(pingsOf(server), firstBefore);
}

TEST_F(TwoServerPlayTest, TheTrackThatPlaysIsNotEvictedFromItsOwnServersCacheOnlyBecauseTheOtherHasTheSameId) {
    TrackCache firstCache(diskFiles(), cache, "https://music.example", "ann");
    TrackCache booksCache(diskFiles(), cache, "https://books.example", "ben");
    for (TrackCache* store : {&firstCache, &booksCache}) {
        const RemoteSource source = albumSource(store == &firstCache ? "al1" : "b1");
        fs::create_directories(store->listFolder(source));
        std::ofstream(store->listFolder(source) / "x.mp3", std::ios::binary) << std::string(5, 'x');
        ASSERT_TRUE(store->save(CachedList{source, "Album", "Band", {{"x", "x.mp3", "X", 100, "", false, 100}}}));
    }
    start_();
    app->setCacheGigabyte(1); // a limit of one byte: nothing unpinned may stay unless it is open
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("x", "X")}));
    ASSERT_TRUE(player.playing());
    app->shutdown();
    EXPECT_TRUE(firstCache.find("x")) << "the file that is open stays";
    EXPECT_FALSE(booksCache.find("x")) << "the other server's x is only a cached copy over the limit";
}

TEST_F(TwoServerPlayTest, ATrackOfTheOtherServerWithTheSameIdAsOneOfTheQueuesListIsNotCachedIntoThatList) {
    start_();
    ASSERT_TRUE(app->playRemote("Weather", {{"x1", "Storm", "Band", "Weather", 200, 1, "flac", 5}, {"t1", "Rain", "Band", "Weather", 180, 2, "mp3", 5}},
                                0, albumSource("al1"), 0, "Band"));
    finishOpen();
    hold(Button::Home);
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"b1", "Dune", "Herbert", 1}}, false));
    app->tick();
    press(Button::Options);
    open("Play next");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteTrack>{{"t1", "Part one", "Herbert", "Dune", 300, 1, "mp3", 5}}, false));
    app->tick();
    trackEnds();
    finishOpen();
    ASSERT_EQ(player.file, "fake://books/t1");
    listen(30s);
    EXPECT_TRUE(downloader.requests.empty());
    EXPECT_TRUE(secondDownloader.requests.empty()) << "t1 of the other server is not a track of Weather";
}

TEST_F(TwoServerPlayTest, AServerThatCannotBeReachedDoesNotHoldBackTheScrobblesOfTheOther) {
    ASSERT_TRUE(diskFiles().writeAtomically(scrobbleFile, formatScrobbles({{firstKey(), "t8", 8000}, {secondKey(), "b7", 7000}})));
    server.scrobbleError = MusicServer::Error::Unreachable; // the first server never takes one, and is not marked down
    start_();
    app->tick();
    EXPECT_EQ(submissionsTo(server), 1u) << "tried once";
    EXPECT_EQ(submissionsTo(second), 1u) << "the second server's entry goes anyway";
    const auto kept = parseScrobbles(diskFiles().read(scrobbleFile).value_or(""));
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0].trackId, "t8") << "the unreachable server's entry stays for later";
}

// ---------------------------------------------------------------- downloads of each server

// Two servers: the fixture's `server` ("Server", music.example, user bob) and `second` ("Audiobooks", books.example,
// user ben), each with its own downloader and its own cache folder.
class TwoServerDownloadTest : public DownloadedTest {
protected:
    void SetUp() override {
        DownloadedTest::SetUp();
        second.streamHost = "books";
        ServerConfig config;
        config.name = "Audiobooks";
        config.url = "https://books.example";
        config.user = "ben";
        moreLinks.push_back(ServerLink{config, TrackCache::serverKey(config.url, config.user), "Audiobooks", second, secondDownloader});
    }
    FakeMusicServer second;
    FakeDownloader secondDownloader;
    TrackCache secondStore() const { return TrackCache(diskFiles(), cache, "https://books.example", "ben"); }
    // Home > Audiobooks > Recently added > <name> (album `id`), on its first item.
    void openSecondAlbum(const std::string& id, const std::string& name, std::vector<RemoteTrack> tracks = sized()) {
        start();
        open("Audiobooks");
        open("Recently added");
        ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{id, name, "Herbert", int(tracks.size())}}, false));
        app->tick();
        open(name);
        ASSERT_TRUE(second.reply(second.lastRequest().id, std::move(tracks), false));
        app->tick();
    }
    void finishSecond() {
        ASSERT_TRUE(secondDownloader.finish(secondDownloader.lastRequest().id, Result::Ok, true));
        app->tick();
    }
    // Home > <link> > Downloaded
    void openDownloadedOf(const std::string& link) {
        if (level().title != "Home") hold(Button::Home);
        open(link);
        open("Downloaded");
    }
    // Home > `link` > Downloaded > `list` > Delete download > Yes, delete
    void deleteDownloadOf(const std::string& link, const std::string& list) {
        openDownloadedOf(link);
        open(list);
        open("Delete download");
        open("Yes, delete");
    }
    // The first server's Weather and the second's Dune, both as album "al1".
    void seedBoth() {
        seed(albumSource("al1"), "Weather", "Band", {{"t1", "Storm Front"}, {"t2", "Rain"}, {"t3", "Sunshine"}});
        seed(albumSource("al1"), "Dune", "Herbert", {{"t1", "Part one"}, {"t2", "Part two"}}, secondStore());
    }
    std::string secondFile(const RemoteSource& source, const std::string& id) const {
        return (secondStore().listFolder(source) / (id + ".mp3")).string();
    }
};

TEST_F(TwoServerDownloadTest, DownloadingAnAlbumOnTheSecondServerCallsOnlyItsDownloaderAndWritesUnderItsKey) {
    ASSERT_NO_FATAL_FAILURE(openSecondAlbum("b1", "Dune"));
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play album", "Download album", "Storm Front", "Rain", "Sunshine"}));
    open("Download album");
    EXPECT_TRUE(downloader.requests.empty()) << "not the first server's";
    ASSERT_EQ(secondDownloader.requests.size(), 1u);
    EXPECT_EQ(secondDownloader.lastRequest().trackId, "t1");
    EXPECT_EQ(secondDownloader.lastRequest().dest, (secondStore().listFolder(albumSource("b1")) / "t1.flac").string());
    EXPECT_NE(secondDownloader.lastRequest().dest.find(TrackCache::serverKey("https://books.example", "ben")), std::string::npos);
    finishSecond();
    ASSERT_EQ(secondDownloader.requests.size(), 2u) << "the next track goes to the same downloader";
    finishSecond();
    finishSecond();
    EXPECT_TRUE(downloader.requests.empty());
    CachedList list = secondStore().load(albumSource("b1"));
    EXPECT_EQ(list.name, "Dune");
    ASSERT_EQ(list.tracks.size(), 3u);
    EXPECT_TRUE(list.tracks[0].pinned);
    for (const CachedTrack& t : list.tracks) EXPECT_EQ(t.changed, "") << "the download is marked whole in its own manifest";
    EXPECT_TRUE(store().lists(false).empty()) << "nothing under the first server's key";
    EXPECT_FALSE(fs::exists(store().listFolder(albumSource("b1"))));
    EXPECT_EQ(speech.last(), "Download finished, Dune.");
}

TEST_F(TwoServerDownloadTest, ASecondServersAlbumOffersDownloadInItsOptionsToo) {
    start();
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"b1", "Dune", "Herbert", 3}}, false));
    app->tick();
    press(Button::Options);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play now", "Play next", "Add to queue", "Download album"}));
    open("Download album");
    EXPECT_EQ(second.lastRequest().kind, Kind::AlbumTracks);
    ASSERT_TRUE(second.reply(second.lastRequest().id, sized(), false));
    app->tick();
    ASSERT_EQ(secondDownloader.requests.size(), 1u);
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(TwoServerDownloadTest, ASecondServerWithoutADownloaderOffersNoDownload) {
    secondDownloader.isConfigured = false;
    ASSERT_NO_FATAL_FAILURE(openSecondAlbum("b1", "Dune"));
    EXPECT_EQ(labels().at(1), "Storm Front");
}

TEST_F(TwoServerDownloadTest, EachServersDownloadedListsItsOwnDownloads) {
    seed(albumSource("al1"), "Weather", "Band", {{"t1", "Storm Front"}});
    seed(albumSource("b1"), "Dune", "Herbert", {{"t1", "Part one"}}, secondStore());
    start();
    openDownloadedOf("Server");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Weather"}));
    openDownloadedOf("Audiobooks");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Dune"}));
}

TEST_F(TwoServerDownloadTest, ADownloadedListOfTheSecondServerPlaysFromItsFolder) {
    seedBoth();
    start();
    openDownloadedOf("Audiobooks");
    open("Dune");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play album", "Delete download", "Part one", "Part two"}));
    open("Part two");
    EXPECT_EQ(player.file, secondFile(albumSource("al1"), "t2")) << "not the first server's t2";
    EXPECT_TRUE(second.requests.empty() || second.lastRequest().kind != Kind::AlbumTracks);
}

TEST_F(TwoServerDownloadTest, AnAlbumDownloadedWholeOnTheSecondServerPlaysFromItsFilesWithoutAskingTheServer) {
    seedBoth();
    start();
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"al1", "Dune", "Herbert", 2}}, false));
    app->tick();
    const size_t asked = second.requests.size();
    press(Button::Options);
    open("Play now");
    EXPECT_EQ(second.requests.size(), asked);
    EXPECT_EQ(player.file, secondFile(albumSource("al1"), "t1"));
}

TEST_F(TwoServerDownloadTest, AListThatFailedOnASecondServerOffersItsOwnDownloaded) {
    seed(albumSource("b1"), "Dune", "Herbert", {{"t1", "Part one"}}, secondStore());
    start();
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.fail(second.lastRequest().id, MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not reachable", "Try again", "Downloaded"}));
    open("Downloaded");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Dune"}));
}

TEST_F(TwoServerDownloadTest, StartingADownloadOnTheSecondServerWhileTheFirstDownloadsSaysAnotherIsRunning) {
    start();
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(sized());
    startDownload();
    ASSERT_EQ(downloader.requests.size(), 1u);
    hold(Button::Home);
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"al1", "Dune", "Herbert", 3}}, false));
    app->tick();
    open("Dune");
    ASSERT_TRUE(second.reply(second.lastRequest().id, sized(), false));
    app->tick();
    // The same id on the other server is another list: not the progress of the one that runs.
    open("Download album");
    EXPECT_EQ(speech.last(), "Another download is running.");
    EXPECT_TRUE(secondDownloader.requests.empty());
    EXPECT_EQ(downloader.requests.size(), 1u);
}

TEST_F(TwoServerDownloadTest, DownloadFromTheOptionsOfTheFirstServerWhileTheSecondDownloadsSaysAnotherIsRunning) {
    ASSERT_NO_FATAL_FAILURE(openSecondAlbum("al1", "Dune"));
    open("Download album");
    ASSERT_EQ(secondDownloader.requests.size(), 1u);
    hold(Button::Home);
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    press(Button::Options);
    EXPECT_EQ(labels().back(), "Download album") << "offered: the album that downloads is the other server's";
    open("Download album");
    EXPECT_EQ(speech.last(), "Another download is running.");
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(TwoServerDownloadTest, TheSameAlbumIdOnTheOtherServerIsNotWholeBecauseTheFirstOneIsDownloaded) {
    seed(albumSource("al1"), "Weather", "Band", {{"t1", "Storm Front"}, {"t2", "Rain"}, {"t3", "Sunshine"}});
    start();
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"al1", "Dune", "Herbert", 3}}, false));
    app->tick();
    press(Button::Options);
    EXPECT_EQ(labels().back(), "Download album") << "the first server's download of al1 is not this one";
}

TEST_F(TwoServerDownloadTest, DeletingADownloadOfTheSecondServerLeavesTheFirstServersListWithTheSameId) {
    seedBoth();
    start();
    ASSERT_NO_FATAL_FAILURE(deleteDownloadOf("Audiobooks", "Dune"));
    EXPECT_EQ(speech.last().rfind("Deleted Dune.", 0), 0u) << speech.last();
    EXPECT_TRUE(secondStore().load(albumSource("al1")).tracks.empty());
    EXPECT_EQ(store().load(albumSource("al1")).tracks.size(), 3u);
    EXPECT_TRUE(fs::exists(fileOf(albumSource("al1"), "t1")));
}

TEST_F(TwoServerDownloadTest, DeletingTheRunningDownloadOfTheSecondServerCancelsItOnItsDownloader) {
    ASSERT_NO_FATAL_FAILURE(openSecondAlbum("b1", "Dune"));
    open("Download album");
    const auto id = secondDownloader.lastRequest().id;
    finishSecond(); // t1 is in, t2 is fetched
    const auto running = secondDownloader.lastRequest().id;
    ASSERT_NE(id, running);
    ASSERT_NO_FATAL_FAILURE(deleteDownloadOf("Audiobooks", "Dune"));
    EXPECT_EQ(secondDownloader.cancelled, (std::vector<Downloader::RequestId>{running}));
    EXPECT_TRUE(downloader.cancelled.empty());
    EXPECT_FALSE(fs::exists(secondStore().listFolder(albumSource("b1"))));
}

TEST_F(TwoServerDownloadTest, DeletingTheFirstServersListLeavesTheSecondServersDownloadOfTheSameIdRunning) {
    seed(albumSource("al1"), "Weather", "Band", {{"t1", "Storm Front"}});
    ASSERT_NO_FATAL_FAILURE(openSecondAlbum("al1", "Dune"));
    open("Download album");
    ASSERT_EQ(secondDownloader.requests.size(), 1u);
    ASSERT_NO_FATAL_FAILURE(deleteDownloadOf("Server", "Weather"));
    EXPECT_TRUE(secondDownloader.cancelled.empty()) << "the download that runs is of the other server";
    finishSecond();
    EXPECT_EQ(secondDownloader.requests.size(), 2u);
}

TEST_F(TwoServerDownloadTest, AListPlayingFromTheFirstServerDoesNotBlockDeletingTheSecondServersListWithTheSameId) {
    seedBoth();
    start();
    openDownloadedOf("Server");
    open("Weather");
    open("Storm Front");
    ASSERT_TRUE(player.playing());
    ASSERT_NO_FATAL_FAILURE(deleteDownloadOf("Audiobooks", "Dune"));
    EXPECT_TRUE(secondStore().load(albumSource("al1")).tracks.empty());
}

TEST_F(TwoServerDownloadTest, AListPlayingFromTheSecondServerBlocksDeletingItsDownloadButNotTheFirstServersOfTheSameId) {
    seedBoth();
    start();
    openDownloadedOf("Audiobooks");
    open("Dune");
    open("Part one");
    ASSERT_TRUE(player.playing());
    ASSERT_NO_FATAL_FAILURE(deleteDownloadOf("Audiobooks", "Dune"));
    EXPECT_EQ(speech.last(), "Cannot delete Dune while it is loaded in the player. Play something else first.");
    EXPECT_EQ(secondStore().load(albumSource("al1")).tracks.size(), 2u);
    ASSERT_NO_FATAL_FAILURE(deleteDownloadOf("Server", "Weather"));
    EXPECT_TRUE(store().load(albumSource("al1")).tracks.empty());
}

TEST_F(TwoServerDownloadTest, DeletingTheSecondServersListLeavesTheFirstServersAutomaticTransferOfTheSameIdRunning) {
    clock.setWallTime(1'800'000'000);
    seed(albumSource("al1"), "Dune", "Herbert", {{"t1", "Part one"}}, secondStore());
    start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    for (int i = 0; i < 11; ++i) idle(1s);
    ASSERT_EQ(downloader.requests.size(), 1u) << "the track is being cached on the first server";
    press(Button::Forward);
    for (int i = 0; i < 11; ++i) idle(1s); // Rain waits behind Storm Front
    ASSERT_NO_FATAL_FAILURE(deleteDownloadOf("Audiobooks", "Dune"));
    EXPECT_TRUE(downloader.cancelled.empty());
    ASSERT_TRUE(downloader.finish(downloader.requests.front().id, Result::Ok, true));
    app->tick();
    ASSERT_EQ(downloader.requests.size(), 2u) << "Rain is still fetched";
    EXPECT_EQ(downloader.lastRequest().trackId, "t2");
}

TEST_F(TwoServerDownloadTest, ADownloadDeletedOnTheSecondServerIsStreamedAgainAfterItsFilesWereIndexed) {
    seed(albumSource("b1"), "Dune", "Herbert", {{"t1", "Part one"}, {"t2", "Part two"}}, secondStore());
    start();
    ASSERT_TRUE(app->playRemote("Dune", sized(), 0, albumSource("b1"), 0, "Herbert", 1));
    EXPECT_EQ(player.file, secondFile(albumSource("b1"), "t1")) << "from its file: the index of the second server is made";
    ASSERT_TRUE(app->playRemote("Other", {{"o1", "One", "Duo", "Other", 100, 1, "mp3", 5}}, 0, albumSource("al2")));
    ASSERT_NO_FATAL_FAILURE(deleteDownloadOf("Audiobooks", "Dune"));
    ASSERT_TRUE(app->playRemote("Dune", sized(), 0, albumSource("b1"), 0, "Herbert", 1));
    EXPECT_EQ(player.file, "fake://books/t1");
    player.preloads.clear();
    player.pos = player.length - 5;
    app->tick();
    EXPECT_EQ(player.preloads, (std::vector<std::string>{"fake://books/t2"})) << "t2 is not taken for a file: it is gone with the list";
}

TEST_F(TwoServerDownloadTest, ALastStreamOfTheSecondServerResumesFromItsServer) {
    state.setString("last_player", "music");
    state.setString("last_stream.server", "https://books.example");
    state.setString("last_stream.user", "ben");
    state.setString("last_stream.kind", "album");
    state.setString("last_stream.id", "b1");
    state.setString("last_stream.title", "Dune");
    state.setString("last_stream.track", "1");
    state.setString("last_stream.track_id", "t2");
    state.setString("last_stream.pos", "42");
    start();
    const size_t asked = server.requests.size();
    open("Now playing");
    ASSERT_FALSE(second.requests.empty());
    EXPECT_EQ(second.lastRequest().kind, Kind::AlbumTracks);
    EXPECT_EQ(second.lastRequest().arg, "b1");
    EXPECT_EQ(server.requests.size(), asked) << "the first server is not asked";
    ASSERT_TRUE(second.reply(second.lastRequest().id, sized(), false));
    app->tick();
    EXPECT_EQ(player.file, "fake://books/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
}

TEST_F(TwoServerDownloadTest, ALastStreamOfTheSecondServerPlaysFromItsDownloadWhenItIsWhole) {
    state.setString("last_player", "music");
    state.setString("last_stream.server", "https://books.example");
    state.setString("last_stream.user", "ben");
    state.setString("last_stream.kind", "album");
    state.setString("last_stream.id", "al1");
    state.setString("last_stream.title", "Dune");
    state.setString("last_stream.track", "1");
    state.setString("last_stream.track_id", "t2");
    seedBoth();
    start();
    open("Now playing");
    EXPECT_TRUE(second.requests.empty() || second.lastRequest().kind != Kind::AlbumTracks) << "no question to the server";
    EXPECT_EQ(player.file, secondFile(albumSource("al1"), "t2"));
}

TEST_F(TwoServerDownloadTest, ALastStreamOfTheSecondServerDoesNotPlayFromTheFirstServersDownloadOfTheSameId) {
    state.setString("last_player", "music");
    state.setString("last_stream.server", "https://books.example");
    state.setString("last_stream.user", "ben");
    state.setString("last_stream.kind", "album");
    state.setString("last_stream.id", "al1");
    state.setString("last_stream.title", "Dune");
    state.setString("last_stream.track", "1");
    state.setString("last_stream.track_id", "t2");
    seed(albumSource("al1"), "Weather", "Band", {{"t1", "Storm Front"}, {"t2", "Rain"}, {"t3", "Sunshine"}}); // the first server's
    start();
    open("Now playing");
    ASSERT_FALSE(second.requests.empty());
    EXPECT_EQ(second.lastRequest().kind, Kind::AlbumTracks) << "the first server's files are not this list";
    EXPECT_TRUE(player.opened.empty());
}

TEST_F(TwoServerDownloadTest, ALastStreamOfAnotherUserOnTheSameServerIsNotResumed) {
    state.setString("last_player", "music");
    state.setString("last_stream.server", "https://books.example");
    state.setString("last_stream.user", "someone-else");
    state.setString("last_stream.kind", "album");
    state.setString("last_stream.id", "b1");
    state.setString("last_stream.title", "Dune");
    start();
    EXPECT_NE(labels().front(), "Now playing");
}

TEST_F(TwoServerDownloadTest, LeavingTheScreenCancelsTheResumeOnTheSecondServer) {
    state.setString("last_player", "music");
    state.setString("last_stream.server", "https://books.example");
    state.setString("last_stream.user", "ben");
    state.setString("last_stream.kind", "album");
    state.setString("last_stream.id", "b1");
    state.setString("last_stream.title", "Dune");
    start();
    open("Now playing");
    const auto id = second.lastRequest().id;
    press(Button::Forward); // a new screen or a move: the request is dropped
    open("Library");
    EXPECT_NE(std::find(second.cancelled.begin(), second.cancelled.end(), id), second.cancelled.end());
    EXPECT_TRUE(server.cancelled.empty());
}

TEST_F(TwoServerDownloadTest, ALastStreamOfAServerThatIsNotSetUpIsNotResumed) {
    state.setString("last_player", "music");
    state.setString("last_stream.server", "https://gone.example");
    state.setString("last_stream.user", "ben");
    state.setString("last_stream.kind", "album");
    state.setString("last_stream.id", "b1");
    state.setString("last_stream.title", "Dune");
    start();
    EXPECT_NE(labels().front(), "Now playing");
}

TEST_F(TwoServerDownloadTest, ALinkWithNoCacheOfItsOwnHasNoDownloadedWhileTheFirstServerHasOne) {
    moreLinks.clear();
    moreLinks.push_back(ServerLink{ServerConfig{}, "other-1", "Server 2", second, secondDownloader}); // no url: its cache stores nothing
    start();
    open("Server 2");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Recently added", "Albums A to Z", "Artists", "Random albums", "Starred", "Server status"}));
    hold(Button::Home);
    open("Server");
    EXPECT_EQ(labels().at(5), "Downloaded");
}

TEST_F(TwoServerDownloadTest, TheOptionsOfASecondServersAlbumOfferNoDownloadWithoutItsDownloader) {
    secondDownloader.isConfigured = false;
    start();
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"b1", "Dune", "Herbert", 3}}, false));
    app->tick();
    press(Button::Options);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play now", "Play next", "Add to queue"}));
}

// The first server's al1 downloads; the second's al1 is another list, here whole on the disk.
TEST_F(TwoServerDownloadTest, TheSameIdAsTheRunningDownloadOnTheOtherServerIsAnotherListInTheOptions) {
    seed(albumSource("al1"), "Dune", "Herbert", {{"t1", "Part one"}}, secondStore());
    start();
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(sized());
    startDownload();
    ASSERT_EQ(downloader.requests.size(), 1u);
    hold(Button::Home);
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"al1", "Dune", "Herbert", 1}}, false));
    app->tick();
    press(Button::Options);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play now", "Play next", "Add to queue"})) << "whole on the second server: nothing to download";
}

TEST_F(TwoServerDownloadTest, ADownloadFromTheOptionsOfTheSecondServerWhileTheFirstDownloadsTheSameIdSaysAnotherIsRunning) {
    start();
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    open("Weather");
    answer(sized());
    startDownload();
    hold(Button::Home);
    open("Audiobooks");
    open("Recently added");
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteAlbum>{{"al1", "Dune", "Herbert", 3}}, false));
    app->tick();
    press(Button::Options);
    open("Download album");
    EXPECT_EQ(speech.last(), "Another download is running.") << "not the progress of the first server's al1";
    EXPECT_TRUE(second.requests.size() == 1u) << "nothing is loaded for it";
    EXPECT_TRUE(secondDownloader.requests.empty());
}

TEST_F(TwoServerDownloadTest, ShutdownCancelsTheTransferOfTheSecondServerOnItsDownloader) {
    ASSERT_NO_FATAL_FAILURE(openSecondAlbum("b1", "Dune"));
    open("Download album");
    const auto id = secondDownloader.lastRequest().id;
    app->shutdown();
    EXPECT_EQ(secondDownloader.cancelled, (std::vector<Downloader::RequestId>{id}));
    EXPECT_TRUE(downloader.cancelled.empty());
}

TEST_F(TwoServerDownloadTest, ThePartOfAFailedDownloadOfTheSecondServerIsKeptForTheNextTry) {
    ASSERT_NO_FATAL_FAILURE(openSecondAlbum("b1", "Dune"));
    app->setCacheGigabyte(5);
    open("Download album");
    ASSERT_TRUE(secondDownloader.finish(secondDownloader.lastRequest().id, Result::Unreachable, true, "", "0123456789"));
    app->tick();
    const fs::path part = secondStore().listFolder(albumSource("b1")) / "t1.flac.part";
    ASSERT_TRUE(fs::exists(part));
    app->shutdown(); // 10 bytes with the part: over the limit of 5
    EXPECT_TRUE(fs::exists(part)) << "OK on Download continues from it";
}

TEST_F(TwoServerDownloadTest, TheRestOfARunningDownloadOfTheSecondServerKeepsItsParts) {
    ASSERT_NO_FATAL_FAILURE(openSecondAlbum("b1", "Dune"));
    app->setCacheGigabyte(5);
    open("Download album");
    const fs::path folder = secondStore().listFolder(albumSource("b1"));
    fs::create_directories(folder);
    std::ofstream(folder / "t1.flac.part", std::ios::binary) << std::string(10, 'p');
    std::ofstream(folder / "t2.mp3.part", std::ios::binary) << std::string(10, 'p');
    app->shutdown(); // saves and trims before it cancels
    EXPECT_TRUE(fs::exists(folder / "t1.flac.part"));
    EXPECT_TRUE(fs::exists(folder / "t2.mp3.part"));
}

TEST_F(TwoServerDownloadTest, ATrackTheServerNoLongerListsIsDroppedFromTheSecondServersDownload) {
    seed(albumSource("b1"), "Dune", "Herbert", {{"t1", "Part one"}, {"t2", "Part two"}, {"t3", "Part three"}}, secondStore());
    ASSERT_TRUE(fs::exists(secondFile(albumSource("b1"), "t3")));
    std::vector<RemoteTrack> now = {{"t1", "Part one", "Herbert", "Dune", 100, 1}, {"t2", "Part two", "Herbert", "Dune", 100, 2}};
    ASSERT_NO_FATAL_FAILURE(openSecondAlbum("b1", "Dune", now));
    open("Download album");
    EXPECT_EQ(speech.last(), "Already downloaded.");
    EXPECT_FALSE(fs::exists(secondFile(albumSource("b1"), "t3"))) << "its file went from the second server's folder";
    EXPECT_EQ(secondStore().load(albumSource("b1")).tracks.size(), 2u);
}

TEST_F(TwoServerDownloadTest, AFinishedFileOfTheSecondServerWhoseManifestCannotBeWrittenIsNotLeftUnlisted) {
    ReadOnlyFiles readOnly;
    textFiles = &readOnly;
    ASSERT_NO_FATAL_FAILURE(openSecondAlbum("b1", "Dune"));
    open("Download album");
    const std::string dest = secondDownloader.lastRequest().dest;
    finishSecond();
    EXPECT_EQ(speech.last(), "Cannot download Storm Front. Cannot write the file.");
    EXPECT_FALSE(fs::exists(dest));
    app.reset(); // before readOnly goes
}

TEST_F(TwoServerDownloadTest, AListPlayingForAnotherUserOfTheSameUrlDoesNotBlockDeletingTheListOfTheSecondServer) {
    FakeMusicServer third;
    FakeDownloader thirdDownloader;
    ServerConfig config;
    config.name = "Carl";
    config.url = "https://books.example"; // the second server's url, another user: another server, another cache folder
    config.user = "carl";
    moreLinks.push_back(ServerLink{config, TrackCache::serverKey(config.url, config.user), "Carl", third, thirdDownloader});
    seedBoth();
    seed(albumSource("al1"), "Emma", "Austen", {{"t1", "One"}, {"t2", "Two"}}, TrackCache(diskFiles(), cache, config.url, config.user));
    start();
    openDownloadedOf("Carl");
    open("Emma");
    open("One");
    ASSERT_TRUE(player.playing());
    ASSERT_NO_FATAL_FAILURE(deleteDownloadOf("Audiobooks", "Dune"));
    EXPECT_TRUE(secondStore().load(albumSource("al1")).tracks.empty());
    app.reset(); // before the fakes go
}

// ---------------------------------------------------------------- catalogue sync per server (audiobookshelf.md 1.7)

// Two servers: the first is the fixture's `server` ("Music", user ice), the second a fake of its own ("Audiobooks", user ben).
// The sync asks one server at a time, in config order: the second is asked once the first is done.
class TwoServerCatalogueTest : public CatalogueTest {
protected:
    void SetUp() override {
        CatalogueTest::SetUp();
        settings.setString("server.name", "Music");
        second.streamHost = "books";
        moreLinks.push_back(secondLink());
    }
    ServerLink secondLink(const std::string& folder = "") {
        ServerConfig config;
        config.name = "Audiobooks";
        config.url = "https://books.example";
        config.user = "ben";
        config.audiobookFolder = folder;
        return ServerLink{config, secondKey(), "Audiobooks", second, secondDownloader};
    }
    std::string secondKey() const { return TrackCache::serverKey("https://books.example", "ben"); }
    std::string secondRef(const std::string& id) const { return ref(id, secondKey()); }
    void seedOnSecond(const std::string& id, const std::string& title) { seedServerAlbum(id, title, secondKey()); }

    static MusicServer::RequestId newest(FakeMusicServer& fake, FakeMusicServer::Kind kind) {
        for (auto it = fake.requests.rbegin(); it != fake.requests.rend(); ++it)
            if (it->kind == kind) return it->id;
        ADD_FAILURE() << "no such request";
        return 0;
    }
    static size_t asked(const FakeMusicServer& fake, FakeMusicServer::Kind kind) {
        return size_t(std::count_if(fake.requests.begin(), fake.requests.end(), [kind](const auto& r) { return r.kind == kind; }));
    }
    // One server's whole sync: its version, its albums (when it changes) and its playlists.
    void syncOf(FakeMusicServer& fake, const std::string& version, std::vector<RemoteAlbum> albums,
                std::vector<RemotePlaylist> playlists = {}) {
        ASSERT_TRUE(fake.replyVersion(newest(fake, Kind::CatalogueVersion), version));
        app->tick();
        if (fake.requests.back().kind == Kind::Albums) {
            ASSERT_TRUE(fake.reply(fake.requests.back().id, std::move(albums), false));
            app->tick();
        }
        ASSERT_EQ(fake.requests.back().kind, Kind::Playlists);
        ASSERT_TRUE(fake.reply(fake.requests.back().id, std::move(playlists)));
        app->tick();
    }
    // The first server has no audiobook folder (album a1 "Storm"); the second has "Books" (book b1 "Dune"). Both synced.
    void syncWithBooksOnSecond() {
        settings.setString("server.audiobook_folder", "");
        moreLinks.clear();
        moreLinks.push_back(secondLink("Books"));
        start();
        ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {album("a1", "Storm")}));
        ASSERT_TRUE(second.replyVersion(newest(second, Kind::CatalogueVersion), "w1"));
        app->tick();
        ASSERT_EQ(second.requests.back().kind, Kind::MusicFolders);
        ASSERT_TRUE(second.reply(second.requests.back().id, std::vector<RemoteFolder>{{"1", "Music"}, {"2", "Books"}}));
        app->tick();
        ASSERT_EQ(second.requests.back().kind, Kind::Albums);
        EXPECT_EQ(second.requests.back().folder, "2");
        ASSERT_TRUE(second.reply(second.requests.back().id, std::vector<RemoteAlbum>{album("b1", "Dune")}, false));
        app->tick();
        ASSERT_TRUE(second.reply(second.requests.back().id, std::vector<RemoteAlbum>{album("b1", "Dune")}, false));
        app->tick();
        ASSERT_TRUE(second.reply(second.requests.back().id, std::vector<RemotePlaylist>{}));
        app->tick();
    }
    void failVersionOf(FakeMusicServer& fake, MusicServer::Error error) {
        ASSERT_TRUE(fake.fail(newest(fake, Kind::CatalogueVersion), error));
        app->tick();
    }
    // The state of a sync that already ran for the server named by `serverKey`.
    void seedSynced(const std::string& serverKey, const std::string& version) { state.setString(versionKey(serverKey), version); }

    FakeMusicServer second;
    FakeDownloader secondDownloader;
};

TEST_F(TwoServerCatalogueTest, MusicListsTheAlbumsOfBothServers) {
    start();
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {album("a1", "Storm")}));
    ASSERT_NO_FATAL_FAILURE(syncOf(second, "w1", {album("b1", "Calm")}));
    EXPECT_TRUE(indexStore.items.count(ref("a1")));
    EXPECT_TRUE(indexStore.items.count(secondRef("b1")));
    open("Music");
    open("Albums A to Z");
    EXPECT_TRUE(hasLabel("Storm"));
    EXPECT_TRUE(hasLabel("Calm"));
    EXPECT_EQ(state.getString(versionKey()), "v1");
    EXPECT_EQ(state.getString(versionKey(secondKey())), "w1");
}

TEST_F(TwoServerCatalogueTest, TheServersAreAskedOneAfterTheOther) {
    start();
    EXPECT_EQ(asked(server, Kind::CatalogueVersion), 1u);
    EXPECT_TRUE(second.requests.empty()) << "the first has not answered";
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {album("a1", "Storm")}));
    EXPECT_EQ(asked(second, Kind::CatalogueVersion), 1u);
}

TEST_F(TwoServerCatalogueTest, AlbumOfEachServerOpensItsTracksFromThatServer) {
    start();
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {album("a1", "Storm")}));
    ASSERT_NO_FATAL_FAILURE(syncOf(second, "w1", {album("b1", "Calm")}));
    open("Music");
    open("Albums A to Z");
    open("Calm");
    EXPECT_EQ(asked(second, Kind::AlbumTracks), 1u);
    EXPECT_EQ(asked(server, Kind::AlbumTracks), 0u);
    EXPECT_EQ(second.requests.back().arg, "b1");
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    open("Storm");
    EXPECT_EQ(asked(server, Kind::AlbumTracks), 1u);
    EXPECT_EQ(server.requests.back().arg, "a1");
    EXPECT_EQ(asked(second, Kind::AlbumTracks), 1u);
}

TEST_F(TwoServerCatalogueTest, OptionsOfAnAlbumOfTheSecondServerInMusicLoadItFromThatServer) {
    start();
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {album("a1", "Storm")}));
    ASSERT_NO_FATAL_FAILURE(syncOf(second, "w1", {album("b1", "Calm")}));
    open("Music");
    open("Albums A to Z");
    for (size_t i = 0; i < level().items.size() && level().items[level().index].label != "Calm"; ++i) press(Button::Forward);
    ASSERT_EQ(level().items[level().index].label, "Calm");
    press(Button::Options);
    open("Play now");
    EXPECT_EQ(asked(second, Kind::AlbumTracks), 1u);
    EXPECT_EQ(asked(server, Kind::AlbumTracks), 0u);
    EXPECT_EQ(second.requests.back().arg, "b1");
}

TEST_F(TwoServerCatalogueTest, AChangedVersionOnOneServerFetchesOnlyThatOne) {
    seedServerAlbum("a1", "Storm");
    seedOnSecond("b1", "Calm");
    seedVersion("v1");
    seedSynced(secondKey(), "w1");
    start();
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {}));
    ASSERT_NO_FATAL_FAILURE(syncOf(second, "w2", {album("b1", "Calm"), album("b2", "Quiet")}));
    EXPECT_EQ(asked(server, Kind::Albums), 0u) << "unchanged";
    EXPECT_EQ(asked(second, Kind::Albums), 1u);
    EXPECT_TRUE(indexStore.items.count(secondRef("b2")));
    EXPECT_EQ(state.getString(versionKey(secondKey())), "w2");
    EXPECT_EQ(state.getString(versionKey()), "v1");
}

TEST_F(TwoServerCatalogueTest, AServerRemovedFromTheConfigLosesItsItemsOnTheNextSync) {
    seedServerAlbum("a1", "Storm");
    seedOnSecond("b1", "Calm");
    seedVersion("v1");
    seedSynced(secondKey(), "w1");
    state.setString(folderKey(secondKey()), "Books");
    const std::string ghost = TrackCache::serverKey("https://ghost.example", "ice"); // in no item, only in the state
    seedSynced(ghost, "g1");
    state.setString(folderKey(ghost), "Books");
    moreLinks.clear(); // the second server is gone from config.yaml
    start();
    EXPECT_FALSE(indexStore.items.count(secondRef("b1")));
    EXPECT_FALSE(storeHolds("books.example"));
    EXPECT_EQ(state.values().count(versionKey(secondKey())), 0u) << "and the version it was synced at";
    EXPECT_EQ(state.values().count(folderKey(secondKey())), 0u) << "and its audiobook folder";
    EXPECT_EQ(state.values().count(versionKey(ghost)), 0u) << "also of a server the index holds nothing of";
    EXPECT_EQ(state.values().count(folderKey(ghost)), 0u);
    EXPECT_TRUE(indexStore.items.count(ref("a1"))) << "the server that stays keeps its items";
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {}));
    EXPECT_TRUE(indexStore.items.count(ref("a1")));
    EXPECT_EQ(state.getString(versionKey()), "v1");
}

TEST_F(TwoServerCatalogueTest, ItemsOfEveryConfiguredServerAreKeptWhenOneIsSynced) {
    seedServerAlbum("a1", "Storm");
    seedOnSecond("b1", "Calm");
    start();
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {album("a2", "Wind")}));
    EXPECT_FALSE(indexStore.items.count(ref("a1"))) << "replaced by the server's own list";
    EXPECT_TRUE(indexStore.items.count(secondRef("b1"))) << "the other server has not answered yet";
}

TEST_F(TwoServerCatalogueTest, OneServerFailingAtStartUpKeepsItsSavedItemsAndSaysNothing) {
    seedServerAlbum("a1", "Storm");
    seedOnSecond("b1", "Calm");
    seedVersion("v1");
    seedSynced(secondKey(), "w1");
    start();
    const size_t spoken = speech.said.size();
    ASSERT_NO_FATAL_FAILURE(failVersionOf(server, MusicServer::Error::Unreachable));
    EXPECT_EQ(asked(second, Kind::CatalogueVersion), 1u) << "the others are still synced";
    ASSERT_NO_FATAL_FAILURE(syncOf(second, "w2", {album("b2", "Quiet")}));
    EXPECT_EQ(speech.said.size(), spoken);
    EXPECT_TRUE(indexStore.items.count(ref("a1"))) << "the saved items of the server that failed";
    EXPECT_EQ(state.getString(versionKey()), "v1") << "and its version: the next start asks again";
    EXPECT_TRUE(indexStore.items.count(secondRef("b2"))) << "the other one went on";
    EXPECT_FALSE(indexStore.items.count(secondRef("b1")));
}

TEST_F(TwoServerCatalogueTest, RefreshWithOneServerFailingSaysTheCountAndWhichServerFailed) {
    start();
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {}));
    ASSERT_NO_FATAL_FAILURE(syncOf(second, "w1", {album("b1", "Calm")}));
    refresh();
    EXPECT_EQ(speech.last(), "Updating the library.");
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    ASSERT_NO_FATAL_FAILURE(failVersionOf(server, MusicServer::Error::Unreachable));
    EXPECT_EQ(speech.last(), "Updating the library.") << "Audiobooks has not answered";
    ASSERT_NO_FATAL_FAILURE(syncOf(second, "w1", {}));
    EXPECT_EQ(speech.last(), "Library updated, 3 albums. Music: server not reachable.");
}

TEST_F(TwoServerCatalogueTest, RefreshNamesTheSecondServerToo) {
    start();
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {}));
    ASSERT_NO_FATAL_FAILURE(syncOf(second, "w1", {}));
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {}));
    ASSERT_NO_FATAL_FAILURE(failVersionOf(second, MusicServer::Error::BadLogin));
    EXPECT_EQ(speech.last(), "Library updated, 2 albums. Audiobooks: wrong user name or password.");
}

TEST_F(TwoServerCatalogueTest, RefreshWithEveryServerFailingSaysOnlyTheErrors) {
    start();
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {}));
    ASSERT_NO_FATAL_FAILURE(syncOf(second, "w1", {}));
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    ASSERT_NO_FATAL_FAILURE(failVersionOf(server, MusicServer::Error::Unreachable));
    ASSERT_NO_FATAL_FAILURE(failVersionOf(second, MusicServer::Error::Server));
    EXPECT_EQ(speech.last(), "Music: server not reachable. Audiobooks: server error.");
}

TEST_F(TwoServerCatalogueTest, RefreshOfAServerThatIsNotConfiguredIsNotAFailure) {
    second.isConfigured = false;
    start();
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {}));
    EXPECT_TRUE(second.requests.empty());
    refresh();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {}));
    EXPECT_EQ(speech.last(), "Library updated, 2 albums.");
}

TEST_F(TwoServerCatalogueTest, TheOldCatalogueKeysAreGoneAfterASyncAndTheCatalogueIsFetchedAgain) {
    seedServerAlbum("a1", "Storm");
    state.setString("catalogue.server", key());
    state.setString("catalogue.version", "v1");
    state.setString("catalogue.folder", "Books");
    start();
    EXPECT_EQ(state.values().count("catalogue.server"), 0u);
    EXPECT_EQ(state.values().count("catalogue.version"), 0u);
    EXPECT_EQ(state.values().count("catalogue.folder"), 0u);
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {album("a1", "Storm")}));
    EXPECT_EQ(asked(server, Kind::Albums), 1u) << "the same version, but nothing says it was synced: one extra fetch";
    EXPECT_EQ(state.getString(versionKey()), "v1");
}

// The first server has no audiobook folder and syncs; the second has "Books" and syncs its book Dune.
TEST_F(TwoServerCatalogueTest, TheAudiobookFolderIsPerServer) {
    ASSERT_NO_FATAL_FAILURE(syncWithBooksOnSecond());
    EXPECT_EQ(asked(server, Kind::MusicFolders), 0u) << "no folder set for the first server";
    EXPECT_EQ(indexStore.items.at(secondRef("b1")).kind, IndexItem::Kind::Book);
    EXPECT_EQ(indexStore.items.at(ref("a1")).kind, IndexItem::Kind::Music);
    EXPECT_EQ(state.getString(folderKey(secondKey())), "Books");
    EXPECT_EQ(state.values().count(folderKey()), 0u);
}

TEST_F(TwoServerCatalogueTest, ABookOfTheSecondServerLoadsAndPlaysFromIt) {
    ASSERT_NO_FATAL_FAILURE(syncWithBooksOnSecond());
    open("Library");
    open("Dune");
    EXPECT_EQ(asked(second, Kind::AlbumTracks), 1u);
    EXPECT_EQ(asked(server, Kind::AlbumTracks), 0u);
    ASSERT_TRUE(second.reply(second.requests.back().id, std::vector<RemoteTrack>{{"c1", "Part one", "Herbert", "Dune", 100, 1}}));
    app->tick();
    EXPECT_EQ(player.file, "fake://books/c1") << "the book plays from the server it is of";
}

TEST_F(TwoServerCatalogueTest, LeavingABookOfTheSecondServerWhileItLoadsCancelsItsRequest) {
    ASSERT_NO_FATAL_FAILURE(syncWithBooksOnSecond());
    open("Library");
    open("Dune");
    const auto id = second.requests.back().id;
    hold(Button::Home);
    EXPECT_FALSE(second.reply(id, std::vector<RemoteTrack>{{"c1", "Part one", "Herbert", "Dune", 100, 1}})) << "cancelled on the server it was asked of";
}

TEST_F(TwoServerCatalogueTest, ADownloadOfEachServerKeepsTheReferenceOfItsOwnServer) {
    cache = base / "cache";
    clock.setWallTime(1'800'000'000);
    for (const auto& [url, user, id] : {std::tuple<std::string, std::string, std::string>{"https://music.example", "ice", "a1"},
                                         {"https://books.example", "ben", "b1"}}) {
        TrackCache store(diskFiles(), cache, url, user);
        RemoteSource source{RemoteSource::Kind::Album, id};
        fs::create_directories(store.listFolder(source));
        touch(store.listFolder(source) / "t1.mp3");
        ASSERT_TRUE(store.save(CachedList{source, "Down " + id, "Band", {{"t1", "t1.mp3", "Song", 100, "", true, 0}}}));
    }
    start();
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {album("a1", "Storm")}));
    ASSERT_NO_FATAL_FAILURE(syncOf(second, "w1", {album("b1", "Calm")}));
    ASSERT_TRUE(indexStore.items.count("cached:" + ref("a1")));
    ASSERT_TRUE(indexStore.items.count("cached:" + secondRef("b1")));
    EXPECT_EQ(indexStore.items.at("cached:" + ref("a1")).seenOn, (std::vector<std::string>{ref("a1")}));
    EXPECT_EQ(indexStore.items.at("cached:" + secondRef("b1")).seenOn, (std::vector<std::string>{secondRef("b1")}));
    EXPECT_FALSE(indexStore.items.count("cached:" + ref("b1"))) << "not under the other server's key";
}

TEST_F(TwoServerCatalogueTest, ALocalAlbumThatIsAnAlbumOfTheSecondServerGetsItsReference) {
    for (const char* f : {"01 - Sunrise.mp3", "02 - Highway.mp3"}) {
        TrackTags t;
        t.album = "Road Trip";
        t.albumArtist = "Band";
        t.title = f;
        tags.tags[roadTrip() / f] = t;
    }
    start();
    RemoteAlbum onServer = album("rt", "Road Trip");
    onServer.songCount = 2;
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {}));
    ASSERT_NO_FATAL_FAILURE(syncOf(second, "w1", {onServer}));
    bool linked = false;
    for (const auto& [id, item] : indexStore.items)
        if (item.source == IndexItem::Source::Local && item.title == "Road Trip")
            linked = std::count(item.seenOn.begin(), item.seenOn.end(), secondRef("rt")) == 1 && item.seenOn.size() == 1;
    EXPECT_TRUE(linked) << "the local album is the second server's, and only its";
}

TEST_F(TwoServerCatalogueTest, MusicPlaylistsListThoseOfBothServersAndOpenFromTheirOwn) {
    start();
    ASSERT_NO_FATAL_FAILURE(syncOf(server, "v1", {}, {{"p1", "Road", 3, ""}}));
    ASSERT_NO_FATAL_FAILURE(syncOf(second, "w1", {}, {{"p9", "Night", 2, ""}}));
    open("Music");
    open("Playlists");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Night", "Road"}));
    open("Night");
    EXPECT_EQ(asked(second, Kind::PlaylistTracks), 1u);
    EXPECT_EQ(asked(server, Kind::PlaylistTracks), 0u);
    EXPECT_EQ(second.requests.back().arg, "p9");
}

TEST_F(TwoServerPlayTest, SaveAsPlaylistGoesToTheServerOfTheQueuesFirstServerTrack) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {onFirst("t1", "Storm"), onSecond("t7", "Dune"), onSecond("t8", "Part two")}));
    finishOpen();
    press(Button::Ok);
    open("Queue");
    open("Save as playlist");
    ASSERT_EQ(server.lastRequest().kind, FakeMusicServer::Kind::CreatePlaylist) << "the first track is the first server's";
    EXPECT_EQ(server.lastRequest().trackIds, (std::vector<std::string>{"t1"}));
    EXPECT_TRUE(second.requests.empty() || second.requests.back().kind != FakeMusicServer::Kind::CreatePlaylist);
    ASSERT_TRUE(server.replyCreated(server.lastRequest().id));
    app->tick();
    EXPECT_NE(speech.last().find("2 tracks not saved"), std::string::npos) << speech.last();
}

TEST_F(TwoServerPlayTest, SaveAsPlaylistOfATrackFromTheSecondServerIsMadeOnTheSecondServer) {
    start_();
    ASSERT_TRUE(app->playQueue("Dune", {onSecond("t7", "Part one"), onSecond("t8", "Part two")}));
    finishOpen();
    press(Button::Ok);
    open("Queue");
    open("Save as playlist");
    ASSERT_FALSE(second.requests.empty());
    EXPECT_EQ(second.requests.back().kind, FakeMusicServer::Kind::CreatePlaylist);
    EXPECT_EQ(second.requests.back().trackIds, (std::vector<std::string>{"t7", "t8"}));
    for (const auto& r : server.requests) EXPECT_NE(r.kind, FakeMusicServer::Kind::CreatePlaylist);
    ASSERT_TRUE(second.replyCreated(second.requests.back().id, "np"));
    app->tick();
    ASSERT_EQ(indexStore.playlists.size(), 1u);
    EXPECT_EQ(indexStore.playlists.front().server, secondKey());
}

TEST_F(TwoServerPlayTest, LeavingTheQueueWhileSavingToTheSecondServerCancelsItsRequest) {
    start_();
    ASSERT_TRUE(app->playQueue("Dune", {onSecond("t7", "Part one")}));
    finishOpen();
    press(Button::Ok);
    open("Queue");
    open("Save as playlist");
    ASSERT_FALSE(second.requests.empty());
    const auto id = second.requests.back().id;
    hold(Button::Home);
    EXPECT_FALSE(second.replyCreated(id)) << "cancelled on the server it was asked of";
}

// ---------------------------------------------------------------- the Books server menu

namespace {
RemoteTrack bookFile(const std::string& id, double seconds) { return {id, id, "Gibson", "Neuromancer", seconds, 0, "mp3", 5}; }
} // namespace

class BooksMenuTest : public DownloadTest {
protected:
    void SetUp() override {
        DownloadTest::SetUp();
        server.catalogueKind = MusicServer::Catalogue::Books;
        settings.setString("server.type", "audiobookshelf");
    }
    static RemoteAlbum book(const std::string& id, const std::string& title, const std::string& author = "Gibson") {
        return {id, title, author, 0};
    }
    std::string bookRef(const std::string& id) const {
        return LibraryIndex::seenRef(TrackCache::serverKey("https://music.example", "bob"), SeenKind::Album, id);
    }
    // Two files (200 s and 180 s) and three chapters: Intro and Middle in file 1, End in file 2 at 30 s.
    static RemoteBook threeChapters() {
        RemoteBook b;
        b.tracks = {bookFile("f1", 200), bookFile("f2", 180)};
        b.chapters = {{"Intro", 0}, {"Middle", 100}, {"End", 230}};
        return b;
    }
    // Home > Server > Recently added > Neuromancer; the book has loaded.
    void openNeuromancer(RemoteBook remote = threeChapters()) {
        openList("Recently added");
        answer(std::vector<RemoteAlbum>{book("b1", "Neuromancer")});
        open("Neuromancer");
        ASSERT_EQ(server.lastRequest().kind, Kind::Book);
        ASSERT_TRUE(server.replyBook(server.lastRequest().id, std::move(remote)));
        app->tick();
    }
};

TEST_F(BooksMenuTest, HomeOpensTheBooksMenuNotStreaming) {
    openServer();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Continue listening", "Recently added", "Books A to Z", "Authors", "Downloaded", "Server status"}));
}

TEST_F(BooksMenuTest, WithoutACacheFolderThereIsNoDownloaded) {
    cache.clear();
    openServer();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Continue listening", "Recently added", "Books A to Z", "Authors", "Server status"}));
}

TEST_F(BooksMenuTest, AMusicServerStillOpensStreaming) {
    server.catalogueKind = MusicServer::Catalogue::Music;
    openServer();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Recently added", "Albums A to Z", "Artists", "Random albums", "Starred", "Downloaded", "Server status"}));
}

TEST_F(BooksMenuTest, RecentlyAddedIsOnePageOf500NewestBooksSpokenWithTheirAuthor) {
    openList("Recently added");
    EXPECT_EQ(speech.last(), "Recently added. Loading, 1 of 1");
    EXPECT_EQ(server.lastRequest().kind, Kind::Albums);
    EXPECT_EQ(server.lastRequest().arg, "newest");
    EXPECT_EQ(server.lastRequest().size, 500);
    EXPECT_EQ(server.lastRequest().folder, "");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Still loading.");
    answer(std::vector<RemoteAlbum>{book("b1", "Neuromancer"), book("b2", "Dune", "Herbert")}, true);
    EXPECT_EQ(speech.last(), "Neuromancer, Gibson, 1 of 2");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Neuromancer", "Dune"}));
    EXPECT_EQ(requestsOf(Kind::Albums), 1u) << "one page only, whatever `more` says";
}

TEST_F(BooksMenuTest, RecentlyAddedErrorOffersTryAgain) {
    openList("Recently added");
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not reachable", "Try again"}));
    EXPECT_EQ(speech.last(), "Server not reachable, 1 of 2");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Loading, 1 of 1");
    answer(std::vector<RemoteAlbum>{book("b1", "Neuromancer")});
    EXPECT_EQ(speech.last(), "Neuromancer, Gibson, 1 of 1");
}

TEST_F(BooksMenuTest, RecentlyAddedEmptySaysNoBooks) {
    openList("Recently added");
    answer(std::vector<RemoteAlbum>{});
    EXPECT_EQ(labels(), (std::vector<std::string>{"No books"}));
    EXPECT_EQ(speech.last(), "No books, 1 of 1");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "The server has no books yet. Add books to the server, then come back.");
}

TEST_F(BooksMenuTest, BooksAToZLoadsLoadingThenTheFirstBook) {
    openList("Books A to Z");
    EXPECT_EQ(speech.last(), "Books A to Z. Loading, 1 of 1");
    EXPECT_EQ(server.lastRequest().arg, "alphabeticalByName");
    answer(std::vector<RemoteAlbum>{book("b1", "Dune", "Herbert")});
    EXPECT_EQ(speech.last(), "Dune, Herbert, 1 of 1");
}

TEST_F(BooksMenuTest, BooksAToZLoadsMoreOnlyWhenChosen) {
    openList("Books A to Z");
    answer(std::vector<RemoteAlbum>{book("b1", "A1"), book("b2", "A2")}, true);
    ASSERT_EQ(requestsOf(Kind::Albums), 1u);
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "Load more, 3 of 3. More items are available. Load more to continue.");
    press(Button::Ok);
    ASSERT_EQ(requestsOf(Kind::Albums), 2u);
    EXPECT_EQ(server.lastRequest().offset, 2);
    EXPECT_EQ(server.lastRequest().size, 500);
    answer(std::vector<RemoteAlbum>{book("b3", "B1")}, false);
    EXPECT_EQ(labels(), (std::vector<std::string>{"A1", "A2", "B1"}));
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "B1, Gibson, 3 of 3");
    EXPECT_EQ(requestsOf(Kind::Albums), 2u);
}

TEST_F(BooksMenuTest, BooksAToZErrorAndEmpty) {
    openList("Books A to Z");
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::BadLogin));
    app->tick();
    EXPECT_EQ(labels().front(), "Wrong user name or password");
    EXPECT_EQ(labels()[1], "Try again");
    press(Button::Forward);
    press(Button::Ok);
    answer(std::vector<RemoteAlbum>{});
    EXPECT_EQ(labels(), (std::vector<std::string>{"No books"}));
}

TEST_F(BooksMenuTest, AuthorsListLoadsAndAnAuthorListsOnlyTheirBooks) {
    openList("Authors");
    EXPECT_EQ(speech.last(), "Authors. Loading, 1 of 1");
    EXPECT_EQ(server.lastRequest().kind, Kind::Artists);
    answer(std::vector<RemoteArtist>{{"a1", "Gibson", 2}, {"a2", "Herbert", 1}});
    EXPECT_EQ(speech.last(), "Gibson, 2 books, 1 of 2");
    open("Gibson");
    EXPECT_EQ(speech.last(), "Gibson. Loading, 1 of 1");
    ASSERT_EQ(server.lastRequest().kind, Kind::ArtistAlbums);
    EXPECT_EQ(server.lastRequest().arg, "a1");
    answer(std::vector<RemoteAlbum>{book("b1", "Neuromancer"), book("b3", "Count Zero")});
    EXPECT_EQ(labels(), (std::vector<std::string>{"Neuromancer", "Count Zero"}));
    EXPECT_EQ(speech.last(), "Neuromancer, Gibson, 1 of 2");
}

TEST_F(BooksMenuTest, AuthorsErrorAndEmpty) {
    openList("Authors");
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Server));
    app->tick();
    EXPECT_EQ(labels().front(), "Server error");
    press(Button::Forward);
    press(Button::Ok);
    answer(std::vector<RemoteArtist>{});
    EXPECT_EQ(labels(), (std::vector<std::string>{"No authors"}));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "The server has no authors yet. Add books to the server, then come back.");
}

TEST_F(BooksMenuTest, AnAuthorWithNoBooksSaysNoBooks) {
    openList("Authors");
    answer(std::vector<RemoteArtist>{{"a1", "Gibson", 0}});
    open("Gibson");
    answer(std::vector<RemoteAlbum>{});
    EXPECT_EQ(labels(), (std::vector<std::string>{"No books"}));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "The server has no books by Gibson.");
}

TEST_F(BooksMenuTest, ABookOpensOnPlayBookDownloadBookAndItsChapters) {
    openList("Recently added");
    answer(std::vector<RemoteAlbum>{book("b1", "Neuromancer")});
    open("Neuromancer");
    EXPECT_EQ(speech.last(), "Neuromancer. Loading, 1 of 1");
    EXPECT_EQ(server.lastRequest().kind, Kind::Book);
    EXPECT_EQ(server.lastRequest().arg, "b1");
    EXPECT_EQ(requestsOf(Kind::AlbumTracks), 0u);
    ASSERT_TRUE(server.replyBook(server.lastRequest().id, threeChapters()));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play book", "Download book", "Intro", "Middle", "End"}));
    EXPECT_EQ(speech.last(), "Play book, 3 chapters, 1 of 5");
}

TEST_F(BooksMenuTest, ABookWithoutServerChaptersListsItsFiles) {
    RemoteBook b;
    b.tracks = {bookFile("f1", 200), bookFile("f2", 180)};
    b.tracks[0].title = "Part one";
    b.tracks[1].title = "";
    ASSERT_NO_FATAL_FAILURE(openNeuromancer(b));
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play book", "Download book", "Part one", "Chapter 2"}));
}

TEST_F(BooksMenuTest, NoDownloadBookWithoutACacheFolder) {
    cache.clear();
    ASSERT_NO_FATAL_FAILURE(openNeuromancer());
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play book", "Intro", "Middle", "End"}));
}

TEST_F(BooksMenuTest, ABookThatFailsToLoadOffersTryAgain) {
    openList("Recently added");
    answer(std::vector<RemoteAlbum>{book("b1", "Neuromancer")});
    open("Neuromancer");
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(labels().front(), "Server not reachable");
    press(Button::Forward);
    press(Button::Ok);
    EXPECT_EQ(requestsOf(Kind::Book), 2u);
    ASSERT_TRUE(server.replyBook(server.lastRequest().id, threeChapters()));
    app->tick();
    EXPECT_EQ(labels().front(), "Play book");
}

TEST_F(BooksMenuTest, ABookWithNoFilesSaysNoChapters) {
    ASSERT_NO_FATAL_FAILURE(openNeuromancer(RemoteBook{}));
    EXPECT_EQ(labels(), (std::vector<std::string>{"No chapters"}));
}

TEST_F(BooksMenuTest, PlayBookStartsAtTheStartWithNoSavedPlace) {
    ASSERT_NO_FATAL_FAILURE(openNeuromancer());
    press(Button::Ok);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(app->screens().back().kind, Screen::Kind::BookPlayer);
    EXPECT_EQ(player.file, "fake://stream/f1");
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

TEST_F(BooksMenuTest, PlayBookResumesTheSavedPlaceOfTheLibraryBook) {
    state.setString("pos:" + bookRef("b1"), "1:42.5");
    ASSERT_NO_FATAL_FAILURE(openNeuromancer());
    press(Button::Ok);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, "fake://stream/f2");
    EXPECT_DOUBLE_EQ(player.pos, 42.5);
    EXPECT_EQ(app->nowPlaying().title, "Neuromancer");
}

TEST_F(BooksMenuTest, PlayBookOnTheBookThatIsPlayingKeepsTheProgressMadeSinceItOpened) {
    ASSERT_NO_FATAL_FAILURE(openNeuromancer());
    press(Button::Ok);
    ASSERT_EQ(app->mode(), App::Mode::Player);
    player.pos = 50;
    press(Button::Home); // back to the book's page; the book plays on
    ASSERT_EQ(level().title, "Neuromancer");
    press(Button::Ok);
    EXPECT_EQ(player.file, "fake://stream/f1");
    EXPECT_DOUBLE_EQ(player.pos, 50);
}

TEST_F(BooksMenuTest, OkOnAChapterPlaysTheBookFromThatChapter) {
    ASSERT_NO_FATAL_FAILURE(openNeuromancer());
    press(Button::Forward);
    press(Button::Forward);
    press(Button::Forward);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "End, 5 of 5");
    press(Button::Ok);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, "fake://stream/f2");
    EXPECT_DOUBLE_EQ(player.pos, 30);
    EXPECT_EQ(app->nowPlaying().chapter, "End");
}

TEST_F(BooksMenuTest, OkOnTheThirdChapterOfThreeInOneFileSeeksWithinIt) {
    RemoteBook b;
    b.tracks = {bookFile("f1", 600)};
    b.chapters = {{"One", 0}, {"Two", 200}, {"Three", 400}};
    ASSERT_NO_FATAL_FAILURE(openNeuromancer(b));
    open("Two");
    EXPECT_EQ(player.file, "fake://stream/f1");
    EXPECT_DOUBLE_EQ(player.pos, 200);
}

TEST_F(BooksMenuTest, ThePlaceKeptByPlayBookIsTheOneLibraryResumes) {
    ASSERT_NO_FATAL_FAILURE(openNeuromancer());
    open("End");
    player.pos = 50;
    app->shutdown();
    EXPECT_EQ(state.getString("pos:" + bookRef("b1")), "1:50.0");
}

TEST_F(BooksMenuTest, TheBookPageIsNotRebuiltWhenYouComeBackFromThePlayer) {
    ASSERT_NO_FATAL_FAILURE(openNeuromancer());
    press(Button::Forward);
    press(Button::Forward); // Intro
    const size_t asked = server.requests.size();
    press(Button::Ok);
    ASSERT_EQ(app->mode(), App::Mode::Player);
    press(Button::Home);
    ASSERT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(level().title, "Neuromancer");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play book", "Download book", "Intro", "Middle", "End"}));
    EXPECT_EQ(server.requests.size(), asked) << "no new request";
}

TEST_F(BooksMenuTest, LeavingABookPageWhileItLoadsDropsTheLateAnswer) {
    openList("Recently added");
    answer(std::vector<RemoteAlbum>{book("b1", "Neuromancer")});
    open("Neuromancer");
    auto id = server.lastRequest().id;
    press(Button::Home);
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), id), server.cancelled.end());
    EXPECT_FALSE(server.replyBook(id, threeChapters()));
    speech.said.clear();
    app->tick();
    EXPECT_TRUE(speech.said.empty());
}

TEST_F(BooksMenuTest, LeavingABooksListWhileItLoadsCancelsTheRequest) {
    openList("Books A to Z");
    auto id = server.lastRequest().id;
    press(Button::Home);
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), id), server.cancelled.end());
    EXPECT_FALSE(server.reply(id, std::vector<RemoteAlbum>{book("b1", "Late")}));
}

TEST_F(BooksMenuTest, AListIsNotRebuiltWhenYouComeBackToIt) {
    openList("Recently added");
    answer(std::vector<RemoteAlbum>{book("b1", "Neuromancer")});
    open("Neuromancer");
    press(Button::Home); // up one level
    EXPECT_EQ(level().title, "Recently added");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Neuromancer"}));
    EXPECT_EQ(requestsOf(Kind::Albums), 1u);
}

TEST_F(BooksMenuTest, DownloadBookCountsFilesAndFetchesEveryFileOfTheBook) {
    ASSERT_NO_FATAL_FAILURE(openNeuromancer());
    open("Download book");
    EXPECT_EQ(speech.last(), "Downloading, 0 of 2 files.");
    ASSERT_EQ(downloader.requests.size(), 1u);
    EXPECT_EQ(downloader.lastRequest().trackId, "f1");
    open("Download book");
    EXPECT_EQ(speech.last(), "Downloading, 0 of 2 files.") << "OK again says the same, with the count so far";
    finishLast();
    ASSERT_EQ(downloader.requests.size(), 2u);
    EXPECT_EQ(downloader.lastRequest().trackId, "f2");
}

TEST_F(BooksMenuTest, ADownloadOfAnAlbumStillCountsTracks) {
    server.catalogueKind = MusicServer::Catalogue::Music;
    openAlbum();
    startDownload();
    EXPECT_EQ(speech.last(), "Downloading, 0 of 3 tracks.");
}

TEST_F(BooksMenuTest, ServerStatusStillReportsOnTheBooksMenu) {
    openServer();
    open("Server status");
    EXPECT_EQ(speech.last(), "Checking the server.");
    ASSERT_TRUE(server.replyPing(server.lastRequest().id, {"Audiobookshelf", "2.37.2"}));
    app->tick();
    EXPECT_EQ(speech.last(), "Connected to Audiobookshelf 2.37.2.");
}

// ---------------------------------------------------------------- download a book (docs/audiobookshelf.md 2.8)

class BookDownloadTest : public BooksServerTest {
protected:
    using Result = Downloader::Result;
    static RemoteTrack part(const std::string& id, double seconds, const std::string& title = "") {
        RemoteTrack t = file(id, seconds);
        t.title = title;
        t.suffix = "mp3";
        t.size = 5;
        return t;
    }
    // Two files (200 s and 180 s) and three chapters, as in BooksServerTest: Middle at 100 s goes into file 2, End is at 30 s there.
    static RemoteBook twoParts(const std::string& title1 = "", const std::string& title2 = "") {
        RemoteBook b;
        b.tracks = {part("t1", 200, title1), part("t2", 180, title2)};
        b.chapters = {{"Intro", 0}, {"Middle", 100}, {"End", 230}};
        return b;
    }
    // `count` files of 100 s, 24 chapters of them.
    static RemoteBook manyParts(int count) {
        RemoteBook b;
        for (int i = 1; i <= count; ++i) {
            b.tracks.push_back(part("t" + std::to_string(i), 100, "Part " + std::to_string(i)));
            b.chapters.push_back({"Chapter " + std::to_string(i), 100.0 * (i - 1)});
        }
        return b;
    }
    // Home > Server > Recently added > Dune, with its page loaded from `remote`.
    void openPage(RemoteBook remote, bool boot = true) {
        if (boot) {
            start();
            syncBooks({book("b1", "Dune")});
        }
        open("Server");
        open("Recently added");
        ASSERT_EQ(server.lastRequest().kind, Kind::Albums);
        ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteAlbum>{book("b1", "Dune")}, false));
        app->tick();
        open("Dune");
        ASSERT_EQ(server.lastRequest().kind, Kind::Book);
        ASSERT_TRUE(server.replyBook(server.lastRequest().id, std::move(remote)));
        app->tick();
    }
    void finishFile(Result result = Result::Ok) {
        ASSERT_FALSE(downloader.requests.empty());
        ASSERT_TRUE(downloader.finish(downloader.lastRequest().id, result, result == Result::Ok));
        app->tick();
    }
    // The book is downloaded whole (every file arrives).
    void downloadWhole(RemoteBook remote = twoParts()) {
        const size_t files = remote.tracks.size();
        ASSERT_NO_FATAL_FAILURE(openPage(std::move(remote)));
        open("Download book");
        for (size_t i = 0; i < files; ++i) ASSERT_NO_FATAL_FAILURE(finishFile());
        EXPECT_EQ(speech.last(), "Download finished, Dune.");
    }
    // The device is switched off and on again with no network: the sync cannot reach the server.
    void restartOffline() {
        app->shutdown();
        app.reset();
        player = FakePlayer{};
        speech.said.clear();
        sounds.played.clear();
        start(false);
        ASSERT_NO_FATAL_FAILURE(deliverScan());
        ASSERT_NO_FATAL_FAILURE(fail(Kind::CatalogueVersion, MusicServer::Error::Unreachable));
        asked = server.requests.size();
    }
    std::string fileOf(int n) const { // the n-th file of Dune in its download
        return (store().listFolder(albumSource("b1")) / ("t" + std::to_string(n) + ".mp3")).string();
    }
    void expectNothingAskedAndNoLoading() {
        EXPECT_EQ(server.requests.size(), asked) << "nothing is asked of the server";
        EXPECT_FALSE(speech.heard("Loading."));
        for (const std::string& said : speech.said) EXPECT_EQ(said.find("not reachable"), std::string::npos) << said;
    }
    std::string bookKey(const std::string& id) const { return "pos:" + ref(id); }
    size_t asked = 0;
};

TEST_F(BookDownloadTest, BooksRemainOriginalWhenDownloadQualityIsSet) {
    settings.setInt("downloads.bitrate", 128);
    openPage(twoParts());
    open("Download book");
    ASSERT_FALSE(downloader.requests.empty());
    EXPECT_EQ(downloader.lastRequest().bitrate, 0);
    EXPECT_FALSE(downloader.lastRequest().dest.ends_with("~128.mp3"));
    finishFile();
    auto saved = store().load(albumSource("b1"));
    ASSERT_EQ(saved.tracks.size(), 1u);
    EXPECT_EQ(saved.tracks[0].bitrate, 0);
    EXPECT_EQ(downloader.lastRequest().bitrate, 0);
}

TEST_F(BookDownloadTest, TheDownloaderGetsTheFilesInOrderIntoTheServersCacheFolderPinned) {
    ASSERT_NO_FATAL_FAILURE(openPage(manyParts(24)));
    open("Download book");
    EXPECT_EQ(speech.last(), "Downloading, 0 of 24 files.");
    for (int i = 1; i <= 3; ++i) {
        ASSERT_EQ(downloader.requests.size(), size_t(i)) << "one at a time";
        EXPECT_EQ(downloader.lastRequest().trackId, "t" + std::to_string(i));
        EXPECT_EQ(downloader.lastRequest().dest, fileOf(i));
        ASSERT_NO_FATAL_FAILURE(finishFile());
    }
    ASSERT_EQ(downloader.requests.size(), 4u);
    EXPECT_EQ(downloader.lastRequest().trackId, "t4");
    const CachedList cached = store().load(albumSource("b1"));
    ASSERT_EQ(cached.tracks.size(), 3u);
    for (const CachedTrack& t : cached.tracks) EXPECT_TRUE(t.pinned) << t.id;
    EXPECT_EQ(cached.name, "Dune");
    EXPECT_EQ(cached.artist, "Author");
}

TEST_F(BookDownloadTest, WhereAmISaysHowManyFilesAreDownloaded) {
    ASSERT_NO_FATAL_FAILURE(openPage(manyParts(24)));
    open("Download book");
    for (int i = 0; i < 3; ++i) ASSERT_NO_FATAL_FAILURE(finishFile());
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Home, Server, Recently added, Dune. Download book, 2 of 26. Downloading, 3 of 24 files.");
}

TEST_F(BookDownloadTest, WhereAmIInThePlayerSaysTheFilesToo) {
    ASSERT_NO_FATAL_FAILURE(openPage(manyParts(24)));
    open("Download book");
    for (int i = 0; i < 3; ++i) ASSERT_NO_FATAL_FAILURE(finishFile());
    press(Button::Back);
    press(Button::Back); // Play book
    press(Button::Ok);
    ASSERT_EQ(app->mode(), App::Mode::Player);
    hold(Button::PlayPause);
    EXPECT_NE(speech.last().find("Downloading, 3 of 24 files."), std::string::npos) << speech.last();
}

TEST_F(BookDownloadTest, TheManifestKeepsTheServerChaptersFromTheFirstFileOn) {
    ASSERT_NO_FATAL_FAILURE(openPage(twoParts()));
    open("Download book");
    ASSERT_NO_FATAL_FAILURE(finishFile());
    const CachedList cached = store().load(albumSource("b1"));
    ASSERT_EQ(cached.chapters.size(), 3u) << "a download that fails after one file keeps them";
    EXPECT_EQ(cached.chapters[1].title, "Middle");
    EXPECT_DOUBLE_EQ(cached.chapters[1].start, 100);
    EXPECT_EQ(cached.chapters[2].title, "End");
    EXPECT_DOUBLE_EQ(cached.chapters[2].start, 230);
}

TEST_F(BookDownloadTest, AWholeDownloadPlaysFromLibraryWithTheServerDownAndNoLoadingWithItsServerChapters) {
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    ASSERT_NO_FATAL_FAILURE(restartOffline());
    open("Library");
    open("Dune");
    expectNothingAskedAndNoLoading();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, fileOf(1));
    EXPECT_EQ(app->nowPlaying().chapterCount, 3) << "the server's chapters, not one per file";
    press(Button::Forward); // Middle: 100 s into file 1
    EXPECT_EQ(player.file, fileOf(1));
    EXPECT_DOUBLE_EQ(player.pos, 100);
    press(Button::Forward); // End: 30 s into file 2
    EXPECT_EQ(player.file, fileOf(2));
    EXPECT_DOUBLE_EQ(player.pos, 30);
    app->shutdown();
    EXPECT_EQ(state.getString("last_book"), ref("b1"));
    EXPECT_EQ(state.getString(bookKey("b1")), "1:30.0");
}

TEST_F(BookDownloadTest, TheBooksMenuDownloadedListsTheBookAndPlaysItFromItsFilesWithItsChapters) {
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    ASSERT_NO_FATAL_FAILURE(restartOffline());
    open("Server");
    open("Downloaded");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Dune"}));
    ASSERT_TRUE(level().items[0].value);
    EXPECT_EQ(level().items[0].value(), "Author");
    open("Dune");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play book", "Delete download", "Intro", "Middle", "End"}))
        << "the chapters of the server, no Download book";
    EXPECT_EQ(speech.last(), "Dune. Play book, 3 chapters, 1 of 5");
    open("End");
    expectNothingAskedAndNoLoading();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, fileOf(2));
    EXPECT_DOUBLE_EQ(player.pos, 30);
}

TEST_F(BookDownloadTest, PlayBookInDownloadedKeepsTheSamePlaceAsLibrary) {
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    ASSERT_NO_FATAL_FAILURE(restartOffline());
    open("Server");
    open("Downloaded");
    open("Dune");
    press(Button::Ok); // Play book
    press(Button::Forward);
    ASSERT_DOUBLE_EQ(player.pos, 100);
    player.pos = 77.5;
    press(Button::Home);
    hold(Button::Home);
    open("Library");
    open("Dune");
    expectNothingAskedAndNoLoading();
    EXPECT_EQ(player.file, fileOf(1));
    EXPECT_DOUBLE_EQ(player.pos, 77.5) << "the Library place, `pos:" << ref("b1") << "`";
    app->shutdown();
    EXPECT_EQ(state.getString(bookKey("b1")), "0:77.5");
    EXPECT_EQ(state.getString(bookKey("b1")).find("http"), std::string::npos);
}

TEST_F(BookDownloadTest, NowPlayingAfterARestartResumesTheDownloadedBookOfflineWithItsChapters) {
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    hold(Button::Home);
    open("Library");
    open("Dune");
    press(Button::Forward);
    press(Button::Forward); // End
    player.pos = 42;
    ASSERT_NO_FATAL_FAILURE(restartOffline());
    ASSERT_EQ(level().items[0].label, "Now playing");
    press(Button::Ok);
    expectNothingAskedAndNoLoading();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, fileOf(2));
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_EQ(app->nowPlaying().chapterCount, 3);
    press(Button::Back); // Previous chapter well into End: its start
    EXPECT_DOUBLE_EQ(player.pos, 30);
    press(Button::Back);
    EXPECT_EQ(player.file, fileOf(1)) << "Middle begins in file 1";
    EXPECT_DOUBLE_EQ(player.pos, 100);
}

TEST_F(BookDownloadTest, ABookDownloadedOnlyPartlyAsksTheServerAndSaysItIsNotReachable) {
    ASSERT_NO_FATAL_FAILURE(openPage(twoParts()));
    open("Download book");
    ASSERT_NO_FATAL_FAILURE(finishFile());
    ASSERT_NO_FATAL_FAILURE(finishFile(Result::Unreachable));
    ASSERT_NO_FATAL_FAILURE(restartOffline());
    open("Library");
    open("Dune");
    EXPECT_EQ(speech.last(), "Loading.");
    ASSERT_NO_FATAL_FAILURE(fail(Kind::Book, MusicServer::Error::Unreachable));
    EXPECT_EQ(speech.last(), "Server not reachable.");
    EXPECT_EQ(level().title, "Library");
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(player.file, "");
}

TEST_F(BookDownloadTest, ABookDownloadedOnlyPartlyOpensFromDownloadedAsOnTheServerAndOffersTryAgain) {
    ASSERT_NO_FATAL_FAILURE(openPage(twoParts()));
    open("Download book");
    ASSERT_NO_FATAL_FAILURE(finishFile());
    ASSERT_NO_FATAL_FAILURE(finishFile(Result::Unreachable));
    ASSERT_NO_FATAL_FAILURE(restartOffline());
    open("Server");
    open("Downloaded");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Dune"}));
    open("Dune");
    EXPECT_EQ(speech.last(), "Dune. Loading, 1 of 1");
    ASSERT_NO_FATAL_FAILURE(fail(Kind::Book, MusicServer::Error::Unreachable));
    EXPECT_EQ(labels().front(), "Server not reachable");
    press(Button::Forward);
    press(Button::Ok);
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoParts()));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play book", "Download book", "Delete download", "Intro", "Middle", "End"}))
        << "Download book goes on with the missing file";
}

TEST_F(BookDownloadTest, AWholeDownloadsPageOnTheServerHasNoDownloadBookButCanBeDeleted) {
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    hold(Button::Home);
    ASSERT_NO_FATAL_FAILURE(openPage(twoParts(), false));
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play book", "Delete download", "Intro", "Middle", "End"}));
}

TEST_F(BookDownloadTest, FilesThatVanishedFromTheDiskLeaveAMessageNotAnEmptyMenu) {
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    ASSERT_NO_FATAL_FAILURE(restartOffline());
    open("Server");
    open("Downloaded");
    fs::remove(fileOf(1));
    fs::remove(fileOf(2));
    open("Dune");
    EXPECT_EQ(labels(), (std::vector<std::string>{"No tracks"}));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "The files of this download are gone from the disk.");
}

TEST_F(BookDownloadTest, AFileTheServerAddedSinceOffersDownloadBookAgainAndGoesOnWithIt) {
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    hold(Button::Home);
    RemoteBook grown = twoParts();
    grown.tracks.push_back(part("t3", 100));
    grown.chapters.push_back({"Epilogue", 380});
    ASSERT_NO_FATAL_FAILURE(openPage(grown, false));
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play book", "Download book", "Delete download", "Intro", "Middle", "End", "Epilogue"}));
    open("Download book");
    EXPECT_EQ(speech.last(), "Downloading, 2 of 3 files.");
    ASSERT_EQ(downloader.requests.size(), 3u);
    EXPECT_EQ(downloader.lastRequest().trackId, "t3") << "only the new file";
    ASSERT_NO_FATAL_FAILURE(finishFile());
    EXPECT_EQ(store().load(albumSource("b1")).chapters.size(), 4u) << "and the chapters the server has now";
}

TEST_F(BookDownloadTest, ABookNotDownloadedHasNoDeleteDownload) {
    ASSERT_NO_FATAL_FAILURE(openPage(twoParts()));
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play book", "Download book", "Intro", "Middle", "End"}));
}

TEST_F(BookDownloadTest, DeletingADownloadedBookGoesBackToDownloadedAndRemovesTheFiles) {
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    ASSERT_NO_FATAL_FAILURE(restartOffline());
    open("Server");
    open("Downloaded");
    open("Dune");
    open("Delete download");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Cancel", "Yes, delete"}));
    open("Yes, delete");
    EXPECT_EQ(level().title, "Downloaded");
    EXPECT_EQ(labels(), (std::vector<std::string>{"No downloads"}));
    EXPECT_FALSE(fs::exists(store().listFolder(albumSource("b1"))));
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Nothing is downloaded. Choose Download book in a book.");
}

TEST_F(BookDownloadTest, ABookThatIsLoadedInThePlayerCannotBeDeleted) {
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    hold(Button::Home);
    open("Library");
    open("Dune"); // plays from the disk
    ASSERT_EQ(app->mode(), App::Mode::Player);
    hold(Button::Home);
    open("Server");
    open("Downloaded");
    open("Dune");
    open("Delete download");
    open("Yes, delete");
    EXPECT_NE(speech.last().find("Cannot delete Dune while it is loaded in the player"), std::string::npos) << speech.last();
    EXPECT_TRUE(fs::exists(fileOf(1)));
}

TEST_F(BookDownloadTest, ADownloadedBookStaysAbookAndIsNeverListedInMusicEvenWhenTheServerDroppedIt) {
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    app->shutdown();
    app.reset();
    start(false);
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    answerVersion("");
    answerAlbums({}); // the server has no books any more
    for (const auto& [id, item] : indexStore.items) EXPECT_NE(item.source, IndexItem::Source::Cached) << id;
    open("Music");
    open("Albums A to Z");
    for (const auto& label : labels()) EXPECT_NE(label, "Dune");
    hold(Button::Home);
    open("Server");
    open("Downloaded");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Dune"})) << "still there to play and to delete";
}

TEST_F(BookDownloadTest, AFailedFileOfABookIsNamedByItsTitleOrElseAsAFile) {
    RemoteBook remote = twoParts("Part one", "");
    ASSERT_NO_FATAL_FAILURE(openPage(remote));
    open("Download book");
    ASSERT_NO_FATAL_FAILURE(finishFile(Result::Unreachable));
    EXPECT_EQ(speech.last(), "Cannot download Part one. Server not reachable.");
    open("Download book");
    ASSERT_NO_FATAL_FAILURE(finishFile());
    ASSERT_NO_FATAL_FAILURE(finishFile(Result::Server));
    EXPECT_EQ(speech.last(), "Cannot download file 2. Server error.") << "no title: the file, not a track";
}

TEST_F(BookDownloadTest, NothingSpokenOrSavedByABookDownloadHoldsAUrl) {
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    ASSERT_NO_FATAL_FAILURE(restartOffline());
    open("Library");
    open("Dune");
    app->shutdown();
    for (const std::string& said : speech.said)
        for (const char* bad : {"http", "://", "t=", "sesame", "music.example"}) EXPECT_EQ(said.find(bad), std::string::npos) << said;
    for (const auto& [k, v] : state.values())
        for (const char* bad : {"http", "://", "sesame", "music.example"}) {
            EXPECT_EQ(k.find(bad), std::string::npos) << k;
            EXPECT_EQ(v.find(bad), std::string::npos) << k << " = " << v;
        }
    std::ifstream manifest(store().manifestFile(albumSource("b1")));
    std::string text((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
    for (const char* bad : {"http", "://", "token", "sesame"}) EXPECT_EQ(text.find(bad), std::string::npos) << text;
}

// Progress pushes are observed at the server and through the saved state, not
// through App internals. The fake wall clock deliberately stays in one second.
class BookProgressPushTest : public BooksServerTest {
protected:
    void SetUp() override { BooksServerTest::SetUp(); server.progressSync = true; }
    void restartFromDisk() {
        app.reset();
        player = FakePlayer{};
        state.load();
        start(false);
    }
    void answerRetry() {
        ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters()));
        app->tick();
    }
};

TEST_F(BookProgressPushTest, PausePushesTheFileOffsetAndWritesNoDiskUntilExit) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 150;
    press(Button::Forward); // file 2, thirty seconds in
    press(Button::PlayPause);
    ASSERT_EQ(server.progressSaves.size(), 1u);
    EXPECT_EQ(server.progressSaves[0].albumId, "b1");
    EXPECT_DOUBLE_EQ(server.progressSaves[0].progress.seconds, 230);
    EXPECT_DOUBLE_EQ(server.progressSaves[0].progress.duration, 380);
    EXPECT_FALSE(server.progressSaves[0].progress.finished);
    EXPECT_EQ(stateFile.saves, 0);
    app->tick(); // acknowledgement remains in memory
    EXPECT_EQ(state.getString("pushed:" + ref("b1")), state.getString("posat:" + ref("b1")));
    EXPECT_EQ(stateFile.saves, 0);
    app->shutdown();
    EXPECT_EQ(stateFile.onDisk("pos:" + ref("b1")), "1:30.0");
    EXPECT_EQ(stateFile.onDisk("posat:" + ref("b1")), "1700000000");
    EXPECT_EQ(stateFile.onDisk("pushed:" + ref("b1")), "1700000000");
}

TEST_F(BookProgressPushTest, AConflictingOfflineSaveStaysUnacknowledgedUntilANewLocalPlace) {
    server.progressSaveError = MusicServer::Error::Conflict;
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 40; press(Button::PlayPause); app->tick();
    EXPECT_EQ(state.getString("progressconflict:" + ref("b1")), "1700000000");
    EXPECT_EQ(state.getString("pushed:" + ref("b1")), "");
    EXPECT_EQ(state.getString("pushdirty:" + ref("b1")), "1");
    ASSERT_EQ(server.lastRequest().kind, Kind::Progress);
    ASSERT_TRUE(server.replyProgress(lastOf(Kind::Progress), RemoteProgress{250, 380, false, 1700000010000}));
    app->tick(); EXPECT_EQ(speech.last(), "The server has a later place, chapter 3. It is in Options.");
    app->shutdown();
    EXPECT_EQ(stateFile.onDisk("progressconflict:" + ref("b1")), "1700000000");
    const auto requests = server.requests.size();
    restartFromDisk();
    for (size_t i = requests; i < server.requests.size(); ++i) EXPECT_NE(server.requests[i].kind, Kind::Book);
    EXPECT_EQ(state.getString("pushed:" + ref("b1")), "");
    server.progressSaveError = MusicServer::Error::None;
    open("Now playing");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters())); app->tick();
    clock.setWallTime(1700000020); player.pos = 60; press(Button::PlayPause);
    app->tick(); EXPECT_EQ(state.getString("progressconflict:" + ref("b1")), "");
    EXPECT_EQ(state.getString("pushed:" + ref("b1")), "1700000020");
}

TEST_F(BookProgressPushTest, PeriodicPushWaitsSixtyPlaybackSecondsAndFiveSecondsOfMovement) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 59;
    idle(59s);
    EXPECT_TRUE(server.progressSaves.empty());
    player.pos = 60;
    idle(1s);
    ASSERT_EQ(server.progressSaves.size(), 1u);
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 60);
    app->tick();
    player.pos = 64;
    idle(60s);
    EXPECT_EQ(server.progressSaves.size(), 1u);
    player.pos = 65;
    idle(1s);
    EXPECT_EQ(server.progressSaves.size(), 2u);
    EXPECT_EQ(stateFile.saves, 0);
}

TEST_F(BookProgressPushTest, FailedPushRetriesAfterRestartButAnAcknowledgedPushDoesNot) {
    server.progressSaveError = MusicServer::Error::Unreachable;
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 42;
    press(Button::PlayPause);
    app->tick();
    EXPECT_NE(state.getString("pushed:" + ref("b1")), state.getString("posat:" + ref("b1")));
    app->shutdown();
    restartFromDisk();
    server.progressSaveError = MusicServer::Error::None;
    ASSERT_NO_FATAL_FAILURE(answerRetry());
    ASSERT_EQ(server.progressSaves.size(), 3u); // pause, exit retry, startup retry
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 42);
    app->tick();
    app->shutdown();
    const size_t books = countOf(Kind::Book);
    restartFromDisk();
    EXPECT_EQ(countOf(Kind::Book), books) << "acknowledged saved place needs no metadata request";
}

TEST_F(BookProgressPushTest, LeavingForAnotherBookAndSleepAndExitPushTheirLatestPlace) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 18;
    hold(Button::Home);
    open("Library");
    open("Alpha");
    ASSERT_EQ(server.progressSaves.size(), 1u);
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 18);
    hold(Button::Home);
    open("Library");
    open("Dune");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters()));
    app->tick();
    player.pos = 35;
    press(Button::Power);
    ASSERT_EQ(server.progressSaves.size(), 2u);
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 35);
    EXPECT_GT(stateFile.saves, 0);
    app->tick();
    EXPECT_EQ(stateFile.onDisk("pushed:" + ref("b1")), stateFile.onDisk("posat:" + ref("b1")));
    press(Button::Power); // wake
    press(Button::PlayPause);
    player.pos = 50;
    app->shutdown();
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 50);
    server.poll(); // final acknowledgement, while App is alive
    EXPECT_EQ(stateFile.onDisk("pushed:" + ref("b1")), stateFile.onDisk("posat:" + ref("b1")));
}

TEST_F(BookProgressPushTest, EndOfBookPushesFinishedAndAnOfflineCompletionSurvivesRestart) {
    server.progressSaveError = MusicServer::Error::Unreachable;
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.atEnd = true;
    app->tick(); // next file
    player.pos = 180;
    player.atEnd = true;
    app->tick();
    ASSERT_FALSE(server.progressSaves.empty());
    EXPECT_TRUE(server.progressSaves.back().progress.finished);
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 380);
    EXPECT_EQ(state.getString("pos:" + ref("b1")), "0:0.0");
    app->tick();
    app->shutdown();
    const auto savesBeforeRetry = server.progressSaves.size();
    restartFromDisk();
    ASSERT_NO_FATAL_FAILURE(answerRetry());
    ASSERT_EQ(server.progressSaves.size(), savesBeforeRetry + 1);
    EXPECT_TRUE(server.progressSaves.back().progress.finished);
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 380);
}

TEST_F(BookProgressPushTest, LateAcknowledgementCannotMarkANewerSameSecondPlaceAsPushed) {
    server.holdProgressSaves = true;
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 10;
    press(Button::PlayPause);
    press(Button::PlayPause);
    player.pos = 20;
    press(Button::PlayPause);
    EXPECT_EQ(server.progressSaves.size(), 1u) << "saves for one book are serial";
    ASSERT_TRUE(server.answerProgressSave());
    app->tick();
    EXPECT_NE(state.getString("pushed:" + ref("b1")), state.getString("posat:" + ref("b1")));
    ASSERT_EQ(server.progressSaves.size(), 2u);
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 20);
    ASSERT_TRUE(server.answerProgressSave());
    app->tick();
    EXPECT_EQ(state.getString("pushed:" + ref("b1")), state.getString("posat:" + ref("b1")));
    player.pos = 25; // seek while paused, in the very same wall second
    app->shutdown();
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 25);
    EXPECT_EQ(stateFile.onDisk("pushdirty:" + ref("b1")), "1");
    app.reset();
    const auto saved = state.values();
    ASSERT_TRUE(server.answerProgressSave());
    server.poll(); // a destroyed App is never touched
    EXPECT_EQ(state.values(), saved);
}

TEST_F(BookProgressPushTest, ReturningToTheOutstandingPlaceDropsAnOlderQueuedSave) {
    server.holdProgressSaves = true;
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 10;
    press(Button::PlayPause); // A is in flight
    press(Button::PlayPause);
    player.pos = 20;
    press(Button::PlayPause); // B waits behind A
    press(Button::PlayPause);
    player.pos = 10;
    press(Button::PlayPause); // latest place is A again, in the same wall second
    ASSERT_EQ(server.progressSaves.size(), 1u);
    ASSERT_TRUE(server.answerProgressSave());
    app->tick();
    EXPECT_EQ(server.progressSaves.size(), 1u) << "the queued place at 20 is no longer current";
    EXPECT_EQ(state.getString("pushed:" + ref("b1")), state.getString("posat:" + ref("b1")));
    EXPECT_TRUE(state.getString("pushdirty:" + ref("b1")).empty());
    app->shutdown();
    EXPECT_EQ(stateFile.onDisk("pos:" + ref("b1")), "0:10.0");
    EXPECT_EQ(server.progressSaves.size(), 1u);
}

TEST_F(BookProgressPushTest, RestartKeepsNewerProgressBehindAnOutstandingSave) {
    server.holdProgressSaves = true;
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 10;
    press(Button::PlayPause);
    press(Button::PlayPause);
    player.pos = 20;
    press(Button::PlayPause);
    hold(Button::Power);
    open("Restart");
    ASSERT_NO_FATAL_FAILURE(answerRetry());
    EXPECT_EQ(server.progressSaves.size(), 1u) << "restart must not overlap saves for the same book";
    ASSERT_TRUE(server.answerProgressSave());
    app->tick();
    ASSERT_EQ(server.progressSaves.size(), 2u);
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 20);
    ASSERT_TRUE(server.answerProgressSave());
    app->tick();
    EXPECT_TRUE(state.getString("pushdirty:" + ref("b1")).empty());
    app->shutdown();
    EXPECT_EQ(stateFile.onDisk("pos:" + ref("b1")), "0:20.0");
    EXPECT_EQ(stateFile.onDisk("pushed:" + ref("b1")), stateFile.onDisk("posat:" + ref("b1")));
}

TEST_F(BookProgressPushTest, FactoryResetDropsOutstandingAndQueuedProgressAcknowledgements) {
    server.holdProgressSaves = true;
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 10;
    press(Button::PlayPause);
    press(Button::PlayPause);
    player.pos = 20;
    press(Button::PlayPause);
    hold(Button::Home);
    open("System");
    open("Factory reset");
    open("Erase all settings");
    const auto reset = state.values();
    ASSERT_TRUE(server.answerProgressSave());
    app->tick();
    EXPECT_EQ(state.values(), reset);
    EXPECT_EQ(server.progressSaves.size(), 1u) << "erased progress must not be sent after reset";
    EXPECT_TRUE(state.getString("pos:" + ref("b1")).empty());
    EXPECT_TRUE(state.getString("pushed:" + ref("b1")).empty());
}

TEST_F(BookProgressPushTest, RetryDoesNotSendAPlaceForAChangedTrackOrder) {
    state.setString("pos:" + ref("b1"), "1:30.0");
    state.setString("postrack:" + ref("b1"), "old-track");
    state.setString("posat:" + ref("b1"), "1700000000");
    start();
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters())); app->tick();
    EXPECT_TRUE(server.progressSaves.empty());
    EXPECT_EQ(state.getString("pushed:" + ref("b1")), "");
}

TEST_F(BookProgressPushTest, StaleStartupMetadataNeverOverwritesANewerPlayedPlace) {
    state.setString("pos:" + ref("b1"), "0:10.0");
    state.setString("posat:" + ref("b1"), "1699999990");
    state.setString("pushed:" + ref("b1"), "1699999989");
    start();
    const auto retry = lastOf(Kind::Book);
    syncBooks({book("b1", "Dune")});
    open("Library");
    open("Dune");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters()));
    app->tick();
    player.pos = 50;
    press(Button::PlayPause);
    ASSERT_TRUE(server.replyBook(retry, twoFilesThreeChapters()));
    app->tick();
    ASSERT_EQ(server.progressSaves.size(), 1u);
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 50);
}

TEST_F(BookProgressPushTest, ABackwardWallClockKeepsTheLastAcknowledgementAndRetriesDirtyProgress) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 10;
    press(Button::PlayPause);
    app->tick();
    ASSERT_EQ(state.getString("pushed:" + ref("b1")), "1700000000");
    server.progressSaveError = MusicServer::Error::Unreachable;
    clock.setWallTime(1699999900);
    player.pos = 20;
    app->shutdown();
    server.poll();
    EXPECT_EQ(stateFile.onDisk("pushed:" + ref("b1")), "1700000000");
    EXPECT_EQ(stateFile.onDisk("posat:" + ref("b1")), "1699999900");
    EXPECT_EQ(stateFile.onDisk("pushdirty:" + ref("b1")), "1");
    restartFromDisk();
    const size_t saves = server.progressSaves.size();
    ASSERT_NO_FATAL_FAILURE(answerRetry());
    EXPECT_EQ(server.progressSaves.size(), saves + 1);
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 20);
}

TEST_F(BookProgressPushTest, AServerThatDoesNotSyncNeverGetsProgressOrTimestampKeys) {
    server.progressSync = false;
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 42;
    idle(60s);
    press(Button::PlayPause);
    app->shutdown();
    EXPECT_TRUE(server.progressSaves.empty());
    EXPECT_TRUE(stateFile.onDisk("posat:" + ref("b1")).empty());
    EXPECT_EQ(stateFile.onDisk("pos:" + ref("b1")), "0:42.0");
}

TEST_F(BookProgressPushTest, PlaybackAfterCompletionClearsFinishedAndPushesTheNewPlace) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.atEnd = true;
    app->tick();
    player.pos = 180;
    player.atEnd = true;
    app->tick();
    app->tick();
    ASSERT_TRUE(server.progressSaves.back().progress.finished);
    press(Button::PlayPause);
    player.pos = 12;
    press(Button::PlayPause);
    EXPECT_FALSE(server.progressSaves.back().progress.finished);
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 12);
    EXPECT_TRUE(state.getString("finished:" + ref("b1")).empty());
}

TEST_F(BookProgressPushTest, PausedTimeDoesNotCountTowardsThePeriodicPlaybackInterval) {
    settings.setInt("power.auto_sleep_min", 0);
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 30;
    idle(30s);
    press(Button::PlayPause);
    app->tick();
    const size_t saves = server.progressSaves.size();
    idle(10min);
    press(Button::PlayPause);
    app->tick(); // establish the resumed playback interval
    player.pos = 59;
    idle(29s);
    EXPECT_EQ(server.progressSaves.size(), saves);
    player.pos = 60;
    idle(1s);
    EXPECT_EQ(server.progressSaves.size(), saves + 1);
}

TEST_F(BookProgressPushTest, StartupMetadataFailureIsSilentAndLeavesProgressPending) {
    state.setString("pos:" + ref("b1"), "1:30.0");
    state.setString("posat:" + ref("b1"), "1700000000");
    start();
    speech.said.clear();
    ASSERT_TRUE(server.fail(lastOf(Kind::Book), MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_TRUE(server.progressSaves.empty());
    EXPECT_TRUE(speech.said.empty());
    EXPECT_TRUE(state.getString("pushed:" + ref("b1")).empty());
}

TEST_F(BookProgressPushTest, ExitWithAnOlderSaveOutstandingKeepsTheLatestPlaceForRestart) {
    server.holdProgressSaves = true;
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 10;
    press(Button::PlayPause);
    player.pos = 20;
    app->shutdown();
    ASSERT_EQ(server.progressSaves.size(), 1u);
    ASSERT_TRUE(server.answerProgressSave());
    server.poll();
    EXPECT_EQ(server.progressSaves.size(), 1u) << "no follow-up after the exit transport stops";
    EXPECT_EQ(stateFile.onDisk("pos:" + ref("b1")), "0:20.0");
    EXPECT_EQ(stateFile.onDisk("pushdirty:" + ref("b1")), "1");
    restartFromDisk();
    ASSERT_NO_FATAL_FAILURE(answerRetry());
    ASSERT_EQ(server.progressSaves.size(), 2u);
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 20);
}

// Pulls start after local playback, and are observed through speech, file opens,
// seeks, and saved places. The fake answers only when explicitly told to.
class BookProgressPullTest : public BookProgressPushTest {
protected:
    static RemoteProgress later(double seconds = 250) { return {seconds, 380, false, 1700000010000}; }
    void localPlace(double seconds = 10) {
        state.setString("pos:" + ref("b1"), "0:" + std::to_string(seconds));
        state.setString("posat:" + ref("b1"), "1699999990");
        state.setString("pushed:" + ref("b1"), "1699999990");
    }
    void answerPlace(std::optional<RemoteProgress> place) {
        ASSERT_TRUE(server.replyProgress(lastOf(Kind::Progress), place));
        app->tick();
    }
    void expectNoOptions() {
        press(Button::Options);
        EXPECT_EQ(app->mode(), App::Mode::Player);
        EXPECT_EQ(speech.last(), "No options.");
    }
};

TEST_F(BookProgressPullTest, NoLocalPlacePlaysAtOnceThenMovesToTheServersFileAndSaysTheChapter) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    EXPECT_TRUE(player.playing());
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_EQ(server.requests.back().kind, Kind::Progress);
    EXPECT_EQ(server.requests.back().arg, "b1");
    speech.said.clear();
    ASSERT_NO_FATAL_FAILURE(answerPlace(later()));
    EXPECT_NE(player.opened.back().find("t2"), std::string::npos);
    EXPECT_DOUBLE_EQ(player.pos, 50);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(speech.last(), "End.");
    EXPECT_EQ(state.getString("pos:" + ref("b1")), "1:50.0");
    EXPECT_EQ(state.getString("posat:" + ref("b1")), "1700000010");
    EXPECT_EQ(stateFile.saves, 0);
    ASSERT_NO_FATAL_FAILURE(expectNoOptions());
}

TEST_F(BookProgressPullTest, AChangedDownloadedTrackOrderDoesNotApplyServerPlace) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    RemoteProgress place = later();
    place.trackIds = {"t2", "t1"}; place.trackDurations = {180, 200};
    speech.said.clear(); ASSERT_NO_FATAL_FAILURE(answerPlace(place));
    EXPECT_DOUBLE_EQ(player.pos, 0); EXPECT_TRUE(speech.said.empty());
    EXPECT_NE(player.file.find("t1"), std::string::npos);
}

TEST_F(BookProgressPullTest, ChangedDownloadedDurationDoesNotApplyServerPlace) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    RemoteProgress place = later();
    place.trackIds = {"t1", "t2"}; place.trackDurations = {300, 180};
    speech.said.clear(); ASSERT_NO_FATAL_FAILURE(answerPlace(place));
    EXPECT_DOUBLE_EQ(player.pos, 0); EXPECT_TRUE(speech.said.empty());
    EXPECT_NE(player.file.find("t1"), std::string::npos);
}

TEST_F(BookProgressPullTest, ALaterServerPlaceIsOfferedOnceInOptionsAndOkSeeksWithoutChangingPause) {
    localPlace();
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    EXPECT_DOUBLE_EQ(player.pos, 10);
    ASSERT_NO_FATAL_FAILURE(answerPlace(later()));
    EXPECT_EQ(speech.last(), "The server has a later place, chapter 3. It is in Options.");
    EXPECT_DOUBLE_EQ(player.pos, 10);
    const auto spoken = speech.said.size();
    app->tick();
    EXPECT_EQ(speech.said.size(), spoken);
    press(Button::PlayPause);
    press(Button::Options);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Go to the server's place, chapter 3, 4 minutes 10 seconds"}));
    press(Button::Ok);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_DOUBLE_EQ(player.pos, 50);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "End.");
    ASSERT_NO_FATAL_FAILURE(expectNoOptions());
}

TEST_F(BookProgressPullTest, AnOlderOrNearbyOrMissingServerPlaceAndFailuresAreSilent) {
    for (int scenario = 0; scenario < 5; ++scenario) {
        SCOPED_TRACE(scenario);
        app.reset(); player = FakePlayer{}; state.clear();
        localPlace();
        ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
        speech.said.clear();
        if (scenario == 0) {
            auto progress = later(); progress.updated = 1699999990000;
            ASSERT_NO_FATAL_FAILURE(answerPlace(progress));
        } else if (scenario == 1) {
            ASSERT_NO_FATAL_FAILURE(answerPlace(later(40))); // exactly 30 seconds away
        } else if (scenario == 2) {
            ASSERT_NO_FATAL_FAILURE(answerPlace(std::nullopt));
        } else {
            ASSERT_TRUE(server.fail(lastOf(Kind::Progress), scenario == 3 ? MusicServer::Error::Unreachable : MusicServer::Error::BadLogin));
            app->tick();
        }
        EXPECT_TRUE(speech.said.empty());
        EXPECT_DOUBLE_EQ(player.pos, 10);
        EXPECT_TRUE(player.playing());
        ASSERT_NO_FATAL_FAILURE(expectNoOptions());
    }
}

TEST_F(BookProgressPullTest, AOneFileBookUsesItsChaptersAndSeeksInsideThePlayingFile) {
    RemoteBook remote = bookOf({file("m4b", 1000)});
    remote.chapters = {{"Beginning", 0}, {"Second", 100}, {"Third", 700}};
    ASSERT_NO_FATAL_FAILURE(playBook(remote));
    const size_t opens = player.opened.size();
    ASSERT_NO_FATAL_FAILURE(answerPlace(later(720)));
    EXPECT_DOUBLE_EQ(player.pos, 720);
    EXPECT_EQ(player.opened.size(), opens);
    EXPECT_EQ(speech.last(), "Third.");
}

TEST_F(BookProgressPullTest, LeavingThePlayerDropsItsQueuedAnswerAndItsOptionsOffer) {
    localPlace();
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    const auto request = lastOf(Kind::Progress);
    ASSERT_TRUE(server.replyProgress(request, later())); // queued, not delivered
    // Home is processed on keyUp; use a gesture directly without a tick between
    // the queued answer and leaving the screen.
    app->keyDown(Button::Home);
    clock.advance(50ms);
    app->keyUp(Button::Home);
    speech.said.clear();
    app->tick();
    EXPECT_TRUE(speech.said.empty());
    EXPECT_DOUBLE_EQ(player.pos, 10);
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), request), server.cancelled.end());
    hold(Button::Home);
    open("Now playing");
    ASSERT_EQ(app->mode(), App::Mode::Player);
    ASSERT_NO_FATAL_FAILURE(expectNoOptions());
    ASSERT_NO_FATAL_FAILURE(answerPlace(later()));
    press(Button::Options);
    EXPECT_NE(labels().front().find("Go to the server's place"), std::string::npos);
    hold(Button::Home);
    open("Now playing");
    ASSERT_NO_FATAL_FAILURE(expectNoOptions());
}

TEST_F(BookProgressPullTest, SleepShutdownPowerOffAndRestartDropPendingPullsWithoutMovingOrAnnouncing) {
    for (int action = 0; action < 4; ++action) {
        SCOPED_TRACE(action);
        app.reset(); player = FakePlayer{}; state.clear();
        ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
        const auto request = lastOf(Kind::Progress);
        if (action == 1) app->shutdown();
        else {
            hold(Button::Power);
            if (action == 0) press(Button::Ok);
            else open(action == 2 ? "Power off" : "Restart");
        }
        EXPECT_FALSE(server.replyProgress(request, later()));
        speech.said.clear();
        app->tick();
        EXPECT_TRUE(speech.said.empty());
        EXPECT_DOUBLE_EQ(player.pos, 0);
    }
}

TEST_F(BookProgressPullTest, ANewerLocalSaveWhileThePullWaitsPreventsAnAutomaticJump) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 75;
    press(Button::PlayPause);
    speech.said.clear();
    auto progress = later(); progress.updated = 1699999999000;
    ASSERT_NO_FATAL_FAILURE(answerPlace(progress));
    EXPECT_TRUE(speech.said.empty());
    EXPECT_DOUBLE_EQ(player.pos, 75);
    ASSERT_NO_FATAL_FAILURE(expectNoOptions());
}

TEST_F(BookProgressPullTest, BooksMenuPlayAndNowPlayingAfterRestartBothAskForProgress) {
    start(); syncBooks({book("b1", "Dune")});
    open("Server"); open("Recently added");
    answerAlbums({book("b1", "Dune")});
    open("Dune");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters())); app->tick();
    open("Play book");
    EXPECT_EQ(server.requests.back().kind, Kind::Progress);
    ASSERT_NO_FATAL_FAILURE(answerPlace(later()));
    EXPECT_DOUBLE_EQ(player.pos, 50);
    app->shutdown();
    restartFromDisk();
    open("Now playing");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters())); app->tick();
    EXPECT_EQ(server.requests.back().kind, Kind::Progress);
    EXPECT_DOUBLE_EQ(player.pos, 50);
    auto newest = later(100); newest.updated += 1000;
    ASSERT_NO_FATAL_FAILURE(answerPlace(newest));
    EXPECT_EQ(speech.last(), "The server has a later place, chapter 2. It is in Options.");
}

TEST_F(BookProgressPullTest, AcceptingTheServerPlaceQueuesItAfterAnOlderOutstandingLocalSave) {
    localPlace(); server.holdProgressSaves = true;
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    ASSERT_NO_FATAL_FAILURE(answerPlace(later()));
    press(Button::PlayPause); // older local position is now on its way to the server
    ASSERT_EQ(server.progressSaves.size(), 1u);
    press(Button::Options); press(Button::Ok);
    EXPECT_DOUBLE_EQ(player.pos, 50);
    ASSERT_TRUE(server.answerProgressSave()); app->tick();
    ASSERT_EQ(server.progressSaves.size(), 2u);
    EXPECT_DOUBLE_EQ(server.progressSaves.back().progress.seconds, 250);
    ASSERT_TRUE(server.answerProgressSave()); app->tick();
    EXPECT_EQ(state.getString("pushdirty:" + ref("b1")), "");
}

TEST_F(BookProgressPullTest, ABookWithoutServerChaptersUsesTheFilesAndUnknownTimelinesAreIgnored) {
    ASSERT_NO_FATAL_FAILURE(playBook(bookOf({file("one", 200), file("two", 180)})));
    ASSERT_NO_FATAL_FAILURE(answerPlace(later()));
    EXPECT_DOUBLE_EQ(player.pos, 50);
    EXPECT_EQ(speech.last(), "two.");
    app.reset(); player = FakePlayer{}; state.clear();
    ASSERT_NO_FATAL_FAILURE(playBook(bookOf({file("unknown", 0), file("two", 180)})));
    speech.said.clear();
    ASSERT_NO_FATAL_FAILURE(answerPlace(later()));
    EXPECT_TRUE(speech.said.empty());
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

TEST_F(BookProgressPullTest, ReplacingTheBookCancelsItsPendingPullAndUnsupportedServersNeverAsk) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    const auto request = lastOf(Kind::Progress);
    hold(Button::Home); open("Library"); open("Alpha");
    EXPECT_FALSE(server.replyProgress(request, later()));
    EXPECT_EQ(player.file, alphaFile(1));
    app.reset(); player = FakePlayer{}; state.clear(); server.progressSync = false;
    const auto requests = countOf(Kind::Progress);
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    EXPECT_EQ(countOf(Kind::Progress), requests);
    ASSERT_NO_FATAL_FAILURE(expectNoOptions());
}

TEST_F(BookDownloadTest, ProgressPullFailureDoesNotDelayOrAnnounceOverAnOfflineDownloadedBook) {
    server.progressSync = true;
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    ASSERT_NO_FATAL_FAILURE(restartOffline());
    open("Library");
    speech.said.clear();
    open("Dune");
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(player.file, fileOf(1));
    EXPECT_EQ(server.lastRequest().kind, Kind::Progress);
    EXPECT_FALSE(speech.heard("Loading."));
    speech.said.clear();
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable)); app->tick();
    EXPECT_TRUE(speech.said.empty());
    EXPECT_TRUE(player.playing());
}

TEST_F(BookProgressPullTest, AProgressReplyForADestroyedAppCannotAffectANewAppInTheSameStorage) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    const auto request = lastOf(Kind::Progress);
    // Reuse valid storage for another App of the same type. Without the weak
    // lifetime guard, equal book/session generations would let the old answer
    // change the new player. This avoids relying on freed memory to crash.
    App* storage = app.release();
    storage->~App();
    app.reset(new (storage) App(Services{speech, player, sounds, volume, serverLinks(), indexStore, scanner, settings, state, clock, textFiles}, root, music, cache, queueFile, scrobbleFile));
    app->start();
    ASSERT_NO_FATAL_FAILURE(deliverScan());
    syncBooks({book("b1", "Dune")});
    open("Library"); open("Dune");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters())); app->tick();
    speech.said.clear();
    ASSERT_TRUE(server.replyProgress(request, later()));
    server.poll();
    EXPECT_TRUE(speech.said.empty());
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_TRUE(state.getString("pos:" + ref("b1")).empty());
}

TEST_F(BookProgressPullTest, ContinuingAParkedServerBookAlsoOffersTheLaterServerPlace) {
    ASSERT_NO_FATAL_FAILURE(playBook(twoFilesThreeChapters()));
    player.pos = 10;
    hold(Button::Home); open("Music"); open("Albums A to Z"); open("Road Trip"); open("Sunrise");
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    hold(Button::Home); open("Continue");
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 10);
    EXPECT_EQ(server.requests.back().kind, Kind::Progress);
    ASSERT_NO_FATAL_FAILURE(answerPlace(later()));
    EXPECT_EQ(speech.last(), "The server has a later place, chapter 3. It is in Options.");
}

TEST_F(BookProgressPullTest, ASlowStreamStartsThePullOnlyAfterItHasOpenedAndKeepsPlayingWhileItWaits) {
    player.holdStreams = true;
    start(); syncBooks({book("b1", "Dune")}); open("Library"); open("Dune");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters())); app->tick();
    EXPECT_TRUE(player.opening());
    EXPECT_EQ(countOf(Kind::Progress), 0u);
    ASSERT_TRUE(player.finishOpen()); app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(server.requests.back().kind, Kind::Progress);
    ASSERT_NO_FATAL_FAILURE(answerPlace(later(100)));
    EXPECT_DOUBLE_EQ(player.pos, 100);
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(speech.last(), "Middle.");
}

TEST_F(BookProgressPullTest, ExplicitlyChoosingTheFirstChapterKeepsThatChoiceWhenServerProgressArrives) {
    start(); syncBooks({book("b1", "Dune")});
    open("Server"); open("Recently added"); answerAlbums({book("b1", "Dune")}); open("Dune");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters())); app->tick();
    open("Intro");
    ASSERT_EQ(app->mode(), App::Mode::Player);
    EXPECT_DOUBLE_EQ(player.pos, 0);
    ASSERT_NO_FATAL_FAILURE(answerPlace(later()));
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_EQ(speech.last(), "The server has a later place, chapter 3. It is in Options.");
}


TEST_F(BooksMenuTest, ContinueListeningLoadsItsFirstBookWithAuthorAndKeepsTheListOnReturn) {
    openList("Continue listening");
    EXPECT_EQ(server.lastRequest().kind, Kind::InProgress);
    EXPECT_EQ(speech.last(), "Continue listening. Loading, 1 of 1");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Still loading.");
    answer(std::vector<RemoteAlbum>{book("b1", "Neuromancer"), book("b2", "Dune", "Herbert")});
    EXPECT_EQ(labels(), (std::vector<std::string>{"Neuromancer", "Dune"}));
    EXPECT_EQ(speech.last(), "Neuromancer, Gibson, 1 of 2");
    press(Button::Forward);
    press(Button::Ok);
    ASSERT_TRUE(server.replyBook(server.lastRequest().id, threeChapters())); app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    press(Button::Home);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Neuromancer", "Dune"}));
    EXPECT_NE(speech.last().find("Dune, Herbert, 2 of 2"), std::string::npos);
    EXPECT_EQ(requestsOf(Kind::InProgress), 1u);
}

TEST_F(BooksMenuTest, ContinueListeningErrorRetriesAndEmptyExplainsNothingInProgress) {
    openList("Continue listening");
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable)); app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not reachable", "Try again"}));
    EXPECT_EQ(speech.last(), "Server not reachable, 1 of 2");
    open("Try again");
    EXPECT_EQ(speech.last(), "Loading, 1 of 1");
    EXPECT_EQ(requestsOf(Kind::InProgress), 2u);
    answer(std::vector<RemoteAlbum>{});
    EXPECT_EQ(speech.last(), "Nothing in progress, 1 of 1");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "There are no books in progress in this library.");
}

TEST_F(BooksMenuTest, LeavingContinueListeningDropsEvenAnAlreadyQueuedAnswer) {
    openList("Continue listening");
    const auto request = server.lastRequest().id;
    ASSERT_TRUE(server.reply(request, std::vector<RemoteAlbum>{book("b1", "Neuromancer")}));
    hold(Button::Home);
    speech.said.clear(); app->tick();
    EXPECT_TRUE(speech.said.empty());
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), request), server.cancelled.end());
    EXPECT_EQ(requestsOf(Kind::Book), 0u);
}

TEST_F(BookProgressPullTest, ContinueListeningOpensAtTheServerPlaceEvenWithANewerNearbyLocalPlace) {
    localPlace();
    start(); syncBooks({book("b1", "Dune")});
    open("Server"); open("Continue listening");
    EXPECT_EQ(server.requests.back().kind, Kind::InProgress);
    ASSERT_TRUE(server.reply(server.requests.back().id, std::vector<RemoteAlbum>{book("b1", "Dune")})); app->tick();
    open("Dune");
    EXPECT_EQ(speech.last(), "Loading.");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters())); app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_TRUE(player.playing());
    EXPECT_DOUBLE_EQ(player.pos, 10);
    ASSERT_NO_FATAL_FAILURE(answerPlace(RemoteProgress{15, 380, false, 1699999980000LL}));
    EXPECT_DOUBLE_EQ(player.pos, 15);
    EXPECT_EQ(speech.last(), "Intro.");
    EXPECT_EQ(state.getString("pos:" + ref("b1")), "0:15.0");
    ASSERT_NO_FATAL_FAILURE(expectNoOptions());
}

TEST_F(BookProgressPullTest, ContinueListeningMapsTheServerTimelineAcrossFilesAndDropsRepliesAfterLeaving) {
    start(); syncBooks({book("b1", "Dune")});
    open("Server"); open("Continue listening");
    ASSERT_TRUE(server.reply(server.requests.back().id, std::vector<RemoteAlbum>{book("b1", "Dune")})); app->tick();
    open("Dune");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters())); app->tick();
    ASSERT_NO_FATAL_FAILURE(answerPlace(later()));
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 50);
    EXPECT_EQ(speech.last(), "End.");
    hold(Button::Home); open("Server"); open("Continue listening");
    ASSERT_TRUE(server.reply(server.requests.back().id, std::vector<RemoteAlbum>{book("b1", "Dune")})); app->tick();
    open("Dune");
    ASSERT_TRUE(server.replyBook(lastOf(Kind::Book), twoFilesThreeChapters())); app->tick();
    const auto request = lastOf(Kind::Progress);
    hold(Button::Home);
    EXPECT_FALSE(server.replyProgress(request, later(100)));
    EXPECT_DOUBLE_EQ(player.pos, 50);
}

TEST_F(BookDownloadTest, ContinueListeningUsesDownloadedFilesAndStillFollowsTheServerPlace) {
    server.progressSync = true;
    ASSERT_NO_FATAL_FAILURE(downloadWhole());
    state.setString(bookKey("b1"), "0:10.0");
    state.setString("posat:" + ref("b1"), "1700000000");
    hold(Button::Home); open("Server"); open("Continue listening");
    ASSERT_TRUE(server.reply(server.requests.back().id, std::vector<RemoteAlbum>{book("b1", "Dune")})); app->tick();
    speech.said.clear();
    const size_t books = countOf(Kind::Book);
    open("Dune");
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, fileOf(1));
    EXPECT_DOUBLE_EQ(player.pos, 10);
    EXPECT_EQ(countOf(Kind::Book), books);
    EXPECT_FALSE(speech.heard("Loading."));
    ASSERT_TRUE(server.replyProgress(lastOf(Kind::Progress), RemoteProgress{210, 380, false, 1699999990000LL})); app->tick();
    EXPECT_EQ(player.file, fileOf(2));
    EXPECT_DOUBLE_EQ(player.pos, 10);
}

TEST_F(AppTest, InactiveKeysDoNotInterruptSpeakMoveOrChangePlayback) {
    start();
    for (bool inPlayer : {false, true}) {
        if (inPlayer) { open("Library"); open("Alpha"); }
        const auto said = speech.said.size();
        const auto stopped = speech.stops;
        const auto played = sounds.played.size();
        const auto file = player.file;
        const auto index = level().index;
        player.pos = 42;
        for (Button key : {Button::Up, Button::Down, Button::Talk}) { press(key); hold(key); }
        EXPECT_EQ(speech.said.size(), said);
        EXPECT_EQ(speech.stops, stopped);
        EXPECT_EQ(sounds.played.size(), played);
        EXPECT_EQ(level().index, index);
        EXPECT_EQ(player.file, file);
        EXPECT_EQ(player.pos, 42);
        EXPECT_EQ(player.playing(), inPlayer);
    }
}

TEST_F(AppTest, HomeReturnsToLoadedPlayingOrPausedPlayerWithoutResumingIt) {
    playAlpha();
    for (bool paused : {false, true}) {
        if (paused) press(Button::PlayPause);
        player.pos = 42;
        hold(Button::Home);
        ASSERT_EQ(level().title, "Home");
        hold(Button::Home);
        EXPECT_EQ(level().title, "Home");
        press(Button::Home);
        EXPECT_EQ(app->mode(), App::Mode::Player);
        EXPECT_EQ(player.playing(), !paused);
        EXPECT_EQ(player.file, alphaFile(1));
        EXPECT_EQ(player.pos, 42);
    }
}

TEST_F(AppTest, HomeWithoutLoadedPlayerStaysHomeWithFeedback) {
    start();
    speech.said.clear();
    press(Button::Home);
    EXPECT_EQ(level().title, "Home");
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Home. Library, 1 of 7");
    hold(Button::Home);
    EXPECT_EQ(level().title, "Home");
    EXPECT_TRUE(player.opened.empty());
}

TEST_F(AppTest, HeldOkOpensItemAndPlayerOptionsWithoutActivationOrNesting) {
    start();
    open("Music"); open("Albums A to Z");
    hold(Button::Ok); // Lullaby's Options, without starting it
    ASSERT_EQ(level().title, "Options, Lullaby");
    EXPECT_TRUE(player.opened.empty());
    const auto depth = app->screens().size();
    hold(Button::Ok);
    EXPECT_EQ(speech.last(), "No options.");
    EXPECT_EQ(app->screens().size(), depth);
    press(Button::Options);
    EXPECT_EQ(speech.last(), "No options.");
    EXPECT_EQ(app->screens().size(), depth);
    press(Button::Home);
    open("Road Trip"); open("Sunrise");
    hold(Button::Ok);
    EXPECT_EQ(level().title, "Options, Sunrise");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Go to album", "Album info"}));
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    press(Button::Home);
    press(Button::Ok);
    EXPECT_EQ(level().title, "Player menu");
}

TEST_F(StreamingTest, LoadingAndErrorBoundaryJumpsKeepTheirOwnAnnouncements) {
    openList("Albums A to Z");
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "Loading, 1 of 1");
    answer(someAlbums("A", 2), true);
    hold(Button::Back);
    EXPECT_EQ(speech.last(), "A 1, Band, 1 of 3. More items are available. Load more to continue.");
    open("Load more");
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "Loading, 3 of 3");
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Server));
    app->tick();
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "Try again, 4 of 4");
    hold(Button::Back);
    EXPECT_EQ(speech.last(), "A 1, Band, 1 of 4");
    EXPECT_EQ(requestsOf(Kind::Albums), 2u);
}

TEST_F(StreamingTest, EmptyNextPageStopsOfferingLoadMoreEvenWhenServerSaysMore) {
    openList("Albums A to Z");
    answer(someAlbums("A", 1), true);
    open("Load more");
    answer(std::vector<RemoteAlbum>{}, true);
    EXPECT_EQ(labels(), (std::vector<std::string>{"A 1"}));
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "A 1, Band, 1 of 1");
    EXPECT_EQ(requestsOf(Kind::Albums), 2u);
}

TEST_F(AppTest, LightHoldSeeksAtTheConfiguredThresholdAndRepeatsWithoutSpeech) {
    settings.setInt("buttons.long_press_ms", 900);
    playAlpha();
    player.pos = 40;
    const auto said = speech.said.size();
    sounds.played.clear();
    app->keyDown(Button::Forward);
    idle(899ms);
    EXPECT_EQ(player.pos, 40);
    idle(1ms);
    EXPECT_EQ(player.pos, 45);
    idle(499ms);
    EXPECT_EQ(player.pos, 45);
    idle(1ms);
    EXPECT_EQ(player.pos, 50);
    idle(500ms);
    EXPECT_EQ(player.pos, 55);
    EXPECT_EQ(speech.said.size(), said);
    EXPECT_EQ(sounds.played, (std::vector<Earcon>{Earcon::Move}));
    app->keyUp(Button::Forward);
    EXPECT_EQ(speech.last(), "Position, 55 seconds.");
    idle(1s);
    EXPECT_EQ(player.pos, 55);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, SeekingChangesStageWithoutAnExtraStepOrDeadlineReset) {
    playAlpha();
    player.pos = 100;
    sounds.played.clear();
    app->keyDown(Button::Forward);
    idle(600ms);
    EXPECT_EQ(player.pos, 105);
    idle(100ms);
    app->keyDown(Button::ForwardDeep);
    EXPECT_EQ(player.pos, 105);
    idle(399ms);
    EXPECT_EQ(player.pos, 105);
    idle(1ms);
    EXPECT_EQ(player.pos, 135);
    idle(100ms);
    app->keyUp(Button::ForwardDeep);
    EXPECT_EQ(player.pos, 135);
    idle(399ms);
    EXPECT_EQ(player.pos, 135);
    idle(1ms);
    EXPECT_EQ(player.pos, 140);
    app->keyUp(Button::Forward);
    EXPECT_EQ(speech.last(), "Position, 2 minutes 20 seconds.");
    EXPECT_EQ(sounds.played, (std::vector<Earcon>{Earcon::Move}));
}

TEST_F(AppTest, DeepFirstRemainsFastAfterLongAndEasesBeforeTheHoldThreshold) {
    playAlpha();
    player.pos = 100;
    app->keyDown(Button::Back);
    app->keyDown(Button::BackDeep);
    EXPECT_EQ(player.pos, 70);
    idle(500ms);
    EXPECT_EQ(player.pos, 40);
    idle(100ms); // Long arrives without changing the deep rate
    EXPECT_EQ(player.pos, 40);
    idle(400ms);
    EXPECT_EQ(player.pos, 10);
    app->keyUp(Button::BackDeep);
    app->keyUp(Button::Back);
    EXPECT_EQ(speech.last(), "Position, 10 seconds.");
    player.pos = 100;
    app->keyDown(Button::ForwardDeep); // reversed contact order also works
    EXPECT_EQ(player.pos, 130);
    app->keyDown(Button::Forward);
    idle(100ms);
    app->keyUp(Button::ForwardDeep); // ease before Long
    idle(400ms);
    EXPECT_EQ(player.pos, 135);
    app->keyUp(Button::Forward);
    EXPECT_EQ(speech.last(), "Position, 2 minutes 15 seconds.");
}

TEST_F(AppTest, SeekingPreservesPausedStateAtBothRatesAndInBothDirections) {
    playAlpha();
    press(Button::PlayPause);
    player.pos = 200;
    for (Button b : {Button::Back, Button::Forward}) {
        const double from = player.pos;
        const int direction = b == Button::Back ? -1 : 1;
        app->keyDown(b);
        idle(600ms);
        EXPECT_EQ(player.pos, from + direction * 5);
        idle(500ms);
        EXPECT_EQ(player.pos, from + direction * 10);
        app->keyUp(b);
        EXPECT_FALSE(player.playing());
        deep(b);
        EXPECT_EQ(player.pos, from + direction * 40);
        EXPECT_FALSE(player.playing());
    }
    EXPECT_EQ(player.file, alphaFile(1));
}

TEST_F(AppTest, SeekingClampsTheCurrentFileWithOneEdgeAndNoRepeatedBoundaryRequests) {
    playAlpha();
    for (Button b : {Button::Back, Button::Forward}) {
        player.pos = b == Button::Back ? 2 : 598;
        player.seeks.clear();
        sounds.played.clear();
        const Button second = b == Button::Back ? Button::BackDeep : Button::ForwardDeep;
        app->keyDown(b);
        app->keyDown(second);
        EXPECT_EQ(player.pos, b == Button::Back ? 0 : 600);
        player.atEnd = true;
        idle(1500ms);
        EXPECT_EQ(player.file, alphaFile(1));
        EXPECT_EQ(player.seeks.size(), 1u);
        EXPECT_EQ(sounds.played, (std::vector<Earcon>{Earcon::Move, Earcon::Edge}));
        app->keyUp(b); // first contact may release first
        EXPECT_EQ(player.file, alphaFile(1));
        app->keyUp(second);
        EXPECT_EQ(player.file, alphaFile(1));
        EXPECT_EQ(player.pos, b == Button::Back ? 0 : 600);
        player.atEnd = false;
    }
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, UnknownDurationAllowsForwardSeekingAndClampsBackAtZero) {
    playAlpha();
    player.length = 0;
    player.pos = 700;
    deep(Button::Forward);
    EXPECT_EQ(player.pos, 730);
    EXPECT_EQ(speech.last(), "Position, 12 minutes 10 seconds.");
    player.pos = 2;
    sounds.played.clear();
    app->keyDown(Button::Back);
    idle(600ms);
    EXPECT_EQ(player.pos, 0);
    idle(500ms);
    EXPECT_EQ(player.pos, 0);
    EXPECT_EQ(sounds.played, (std::vector<Earcon>{Earcon::Move, Earcon::Edge}));
    app->keyUp(Button::Back);
    EXPECT_EQ(speech.last(), "Position, 0 seconds.");
}

TEST_F(AppTest, SeekingAccumulatesTargetsWhenDecoderFeedbackLags) {
    playAlpha();
    player.pos = 100;
    player.deferSeeks = true;
    player.seeks.clear();
    app->keyDown(Button::Forward);
    idle(600ms);
    idle(500ms);
    app->keyDown(Button::ForwardDeep);
    idle(500ms);
    EXPECT_EQ(player.seeks, (std::vector<double>{105, 110, 140}));
    app->keyUp(Button::ForwardDeep);
    app->keyUp(Button::Forward);
    EXPECT_EQ(speech.last(), "Position, 2 minutes 20 seconds.");
}

TEST_F(AppTest, SeekingCancellationOnLockSleepScreenAndShutdownHasNoLateAction) {
    playAlpha();
    for (int cancel : {0, 1, 2, 3}) {
        app->keyDown(Button::Forward);
        app->keyDown(Button::ForwardDeep);
        const auto at = player.pos;
        if (cancel == 0) { app->setHold(true); app->setHold(false); }
        if (cancel == 1) { press(Button::Power); press(Button::Power); }
        if (cancel == 2) { press(Button::Ok); press(Button::Home); }
        if (cancel == 3) app->shutdown();
        const auto said = speech.said.size();
        const auto stopped = speech.stops;
        idle(1s);
        app->keyUp(Button::ForwardDeep);
        app->keyUp(Button::Forward);
        EXPECT_EQ(player.file, alphaFile(1));
        EXPECT_EQ(player.pos, at);
        EXPECT_EQ(speech.said.size(), said);
        EXPECT_EQ(speech.stops, stopped);
    }
}

TEST_F(AppTest, OldUnitKeysAndSkipSettingsDoNotAffectShortsOrHoldsAndAreNotMigrated) {
    stateFile.file["unit.book"] = "2";
    stateFile.file["unit.music"] = "999";
    configFile.file["skip.back"] = "60";
    configFile.file["skip.forward"] = "120";
    settings.load();
    state.load();
    playAlpha();
    EXPECT_EQ(stateFile.saves, 0);
    EXPECT_EQ(configFile.saves, 0);
    press(Button::Forward);
    EXPECT_EQ(player.file, alphaFile(2));
    player.pos = 40;
    hold(Button::Back);
    EXPECT_EQ(player.pos, 35);
    hold(Button::Home);
    open("Settings");
    EXPECT_FALSE(hasLabel("Skip back"));
    EXPECT_FALSE(hasLabel("Skip forward"));
    open("Speech rate"); open("250 words per minute");
    EXPECT_EQ(configFile.onDisk("skip.back"), "60");
    hold(Button::Home);
    open("Music"); open("Albums A to Z"); open("Road Trip"); open("Sunrise");
    press(Button::Forward);
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    app->shutdown();
    EXPECT_EQ(stateFile.onDisk("unit.book"), "2");
    EXPECT_EQ(stateFile.onDisk("unit.music"), "999");
}

TEST_F(AppTest, SeekingToEofDoesNotAdvanceOnReleaseOrTheFollowingTick) {
    playAlpha();
    player.pos = 599;
    deep(Button::Forward);
    EXPECT_EQ(player.pos, 600);
    player.atEnd = true; // the decoder drained at exactly duration, as the real Player does
    idle(1s);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_TRUE(player.playing());
    EXPECT_EQ(speech.last(), "Position, 10 minutes.");
    hold(Button::Back);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_EQ(player.pos, 595);
    player.atEnd = false; // this seek has now reached the decoder
    press(Button::Forward);
    EXPECT_EQ(player.file, alphaFile(2));
}

TEST_F(AsyncOpenTest, AHeldGestureDuringAPendingOpenCannotBeginSeekingAfterItCompletes) {
    playWeather();
    press(Button::Forward);
    ASSERT_TRUE(player.opening());
    player.seeks.clear();
    app->keyDown(Button::Back);
    app->keyDown(Button::BackDeep);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_TRUE(player.seeks.empty());
    finishOpen();
    const auto said = speech.said.size();
    idle(1s);
    app->keyDown(Button::BackDeep); // duplicate contact also stays cancelled
    app->keyUp(Button::BackDeep);
    app->keyUp(Button::Back);
    EXPECT_TRUE(player.seeks.empty());
    EXPECT_EQ(speech.said.size(), said);
    EXPECT_EQ(player.file, "fake://stream/t2");
    deep(Button::Forward);
    EXPECT_EQ(player.pos, 30) << "fresh gesture works";
}

TEST_F(AsyncOpenTest, FailedAndClosedPlayersCannotStartSeekingOrAnnounceAPosition) {
    playWeather();
    player.broken = true;
    const auto said = speech.said.size();
    app->keyDown(Button::ForwardDeep); // failure visible before tick sees it
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_TRUE(player.seeks.empty());
    app->tick();
    EXPECT_FALSE(player.isOpen());
    app->keyUp(Button::ForwardDeep);
    const auto failedSpeech = speech.said.size();
    hold(Button::Back);
    EXPECT_EQ(speech.said.size(), failedSpeech);
    EXPECT_GT(failedSpeech, said);
    EXPECT_TRUE(player.seeks.empty());
    EXPECT_FALSE(player.playing());
}

TEST_F(AppTest, NaturalFileChangeBeforeTheHoldThresholdCancelsThatGesture) {
    playAlpha();
    app->keyDown(Button::Forward);
    player.atEnd = true;
    idle(100ms);
    ASSERT_EQ(player.file, alphaFile(2));
    idle(1s);
    app->keyUp(Button::Forward);
    EXPECT_EQ(player.pos, 0);
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_NE(speech.last().find("Position"), 0u);
}

TEST_F(AppTest, ShortBackUsesTheThreeSecondRuleWithinAnEmbeddedChapter) {
    player.embedded = {{"Intro", 0}, {"Middle", 100}, {"End", 200}};
    start(); open("Library"); open("Beta");
    for (double seconds : {102.99, 103.0, 103.01}) {
        player.pos = seconds;
        press(Button::Back);
        EXPECT_EQ(player.pos, seconds > 103 ? 100 : 0);
        EXPECT_EQ(player.file, beta().string());
    }
}

TEST_F(AppTest, ChoosingAChapterAfterSeekingToEofRestoresNaturalEndHandling) {
    player.embedded = {{"Intro", 0}, {"Middle", 100}};
    start(); open("Library"); open("Beta");
    player.pos = 599;
    deep(Button::Forward);
    player.atEnd = true;
    press(Button::Ok); open("Chapters"); open("Intro");
    EXPECT_EQ(player.pos, 0);
    player.atEnd = true; // playback later reaches the natural end again
    idle(1ms);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "End of book. Press play to start again.");
}

TEST_F(AsyncOpenTest, AStreamFailureCancelsActiveSeekingAndItsReleaseSpeech) {
    playWeather();
    app->keyDown(Button::Forward);
    app->keyDown(Button::ForwardDeep);
    ASSERT_EQ(player.pos, 30);
    player.broken = true;
    idle(500ms);
    EXPECT_FALSE(player.isOpen());
    const auto seeks = player.seeks;
    const auto said = speech.said.size();
    idle(1s);
    app->keyUp(Button::ForwardDeep);
    app->keyUp(Button::Forward);
    EXPECT_EQ(player.seeks, seeks);
    EXPECT_EQ(speech.said.size(), said);
    EXPECT_EQ(speech.last(), "Server not reachable. Press play to try again.");
    press(Button::PlayPause);
    answerPing();
    finishOpen();
    player.seeks.clear();
    idle(1s);
    EXPECT_TRUE(player.seeks.empty()) << "a failed gesture cannot revive after recovery";
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, ExplicitPlayAfterSeekingToEofAllowsPlaybackToAdvanceAgain) {
    playAlpha();
    player.pos = 599;
    deep(Button::Forward);
    player.atEnd = true;
    press(Button::PlayPause);
    press(Button::PlayPause);
    idle(1ms);
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, FileChangesCancelNavigationButPreserveHeldVolumeAndPlay) {
    playAlpha();
    app->keyDown(Button::VolumeUp);
    app->keyDown(Button::PlayPause);
    press(Button::Forward);
    EXPECT_EQ(player.file, alphaFile(2));
    idle(400ms);
    EXPECT_EQ(volume.volume, 0.80f);
    idle(150ms);
    EXPECT_NE(speech.last().find("Book player. Alpha."), std::string::npos);
    app->keyUp(Button::PlayPause);
    press(Button::Back);
    EXPECT_EQ(player.file, alphaFile(1));
    player.atEnd = true;
    idle(150ms);
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_GT(volume.volume, 0.80f);
    const float before = volume.volume;
    idle(150ms);
    EXPECT_GT(volume.volume, before);
    app->keyUp(Button::VolumeUp);
}

TEST_F(AppTest, PausedSeekingCountsAsActivityUntilTheButtonIsReleased) {
    settings.setInt("power.auto_sleep_min", 1);
    playAlpha();
    press(Button::PlayPause);
    app->keyDown(Button::Forward);
    idle(600ms);
    for (int i = 0; i < 122; ++i) idle(500ms);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(player.file, alphaFile(1));
    app->keyUp(Button::Forward);
    idle(61s);
    EXPECT_EQ(app->mode(), App::Mode::Sleeping) << "inactivity sleep returns after release";
}

TEST_F(AsyncOpenTest, ALightPressDuringAnOpenIsCancelledWhenTheFileArrives) {
    playWeather();
    press(Button::Forward);
    ASSERT_TRUE(player.opening());
    player.seeks.clear();
    app->keyDown(Button::Back);
    idle(100ms); // still below the hold threshold when the new file arrives
    finishOpen();
    player.pos = 100;
    const auto said = speech.said.size();
    idle(1s);
    app->keyUp(Button::Back);
    EXPECT_TRUE(player.seeks.empty());
    EXPECT_EQ(player.pos, 100);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_EQ(speech.said.size(), said);
    deep(Button::Forward);
    EXPECT_EQ(player.pos, 130) << "a fresh gesture still works";
}

TEST_F(AsyncOpenTest, ALightPressDuringAnOpenCannotSkipWhenReleasedAfterCompletion) {
    playWeather();
    press(Button::Forward);
    ASSERT_TRUE(player.opening());
    app->keyDown(Button::Back);
    idle(100ms);
    finishOpen();
    player.pos = 100;
    player.seeks.clear();
    const auto said = speech.said.size();
    app->keyUp(Button::Back); // release below the hold threshold
    EXPECT_TRUE(player.seeks.empty());
    EXPECT_EQ(player.pos, 100);
    EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_EQ(speech.said.size(), said);
}

TEST_F(PlayerOptionsTest, StarsLoadAuthoritativelyAndMutationsReturnToThePlayer) {
    start(); openWeather(); open("Rain"); finishOpen();
    for (bool starred : {false, true}) {
        press(Button::Options);
        EXPECT_TRUE(hasLabel("Go to album"));
        EXPECT_TRUE(hasLabel("Loading"));
        ASSERT_EQ(server.lastRequest().kind, Kind::TrackDetails);
        EXPECT_EQ(server.lastRequest().arg, "t2");
        RemoteTrack t = weather()[1]; t.starred = starred;
        answer(std::vector<RemoteTrack>{t});
        EXPECT_TRUE(hasLabel(starred ? "Unstar" : "Star"));
        EXPECT_FALSE(hasLabel(starred ? "Star" : "Unstar"));
        open(starred ? "Unstar" : "Star");
        EXPECT_EQ(speech.last(), "Saving.");
        EXPECT_EQ(server.lastRequest().kind, starred ? Kind::Unstar : Kind::Star);
        EXPECT_EQ(server.lastRequest().arg, "t2");
        ASSERT_TRUE(server.replyCreated(server.lastRequest().id)); app->tick();
        EXPECT_EQ(app->mode(), App::Mode::Player);
        EXPECT_EQ(speech.last(), starred ? "Unstarred." : "Starred.");
    }
    press(Button::Ok);
    EXPECT_FALSE(hasLabel("Star")); EXPECT_FALSE(hasLabel("Unstar"));
    expectNoUrlSpoken(speech);
}

TEST_F(PlayerOptionsTest, StarErrorsAreSpokenAndRetriedWithoutOptimisticState) {
    start(); openWeather(); open("Rain"); finishOpen(); press(Button::Options);
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable)); app->tick();
    EXPECT_NE(speech.last().find("Server not reachable"), std::string::npos);
    EXPECT_TRUE(hasLabel("Go to album")); open("Try again");
    answer(std::vector<RemoteTrack>{weather()[0]}); // wrong track: do not offer a guessed action
    EXPECT_FALSE(hasLabel("Star")); EXPECT_FALSE(hasLabel("Unstar"));
    EXPECT_EQ(sounds.last(), Earcon::Error);
    open("Try again");
    answer(std::vector<RemoteTrack>{weather()[1]}); open("Star");
    auto id = server.lastRequest().id;
    open("Star"); EXPECT_EQ(server.lastRequest().id, id); EXPECT_EQ(speech.last(), "Still saving.");
    ASSERT_TRUE(server.fail(id, MusicServer::Error::BadLogin)); app->tick();
    EXPECT_TRUE(hasLabel("Star")); EXPECT_FALSE(hasLabel("Unstar"));
    EXPECT_EQ(sounds.last(), Earcon::Error); EXPECT_EQ(speech.last(), "Wrong user name or password");
    open("Star"); ASSERT_TRUE(server.replyCreated(server.lastRequest().id)); app->tick();
    EXPECT_EQ(speech.last(), "Starred.");
}

TEST_F(PlayerOptionsTest, LeavingStarOptionsCancelsReadsAndMutationsIncludingQueuedReplies) {
    start(); openWeather(); open("Rain"); finishOpen();
    press(Button::Options); auto read = server.lastRequest().id;
    ASSERT_TRUE(server.reply(read, std::vector<RemoteTrack>{weather()[1]}));
    app->keyDown(Button::Home); app->keyUp(Button::Home); app->tick(); EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_NE(std::find(server.cancelled.begin(), server.cancelled.end(), read), server.cancelled.end());
    press(Button::Options); answer(std::vector<RemoteTrack>{weather()[1]}); open("Star");
    auto write = server.lastRequest().id; press(Button::Home);
    EXPECT_FALSE(server.replyCreated(write)); app->tick();
    EXPECT_FALSE(speech.heard("Starred."));
}

TEST_F(PlayerOptionsTest, LocalAndUnsupportedTracksOfferNoStars) {
    playRoadTrip(); press(Button::Options);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Go to album", "Album info"}));
    press(Button::Home); hold(Button::Home);
    openWeather(); open("Rain"); finishOpen(); server.starsSupported = false;
    auto n = server.requests.size(); press(Button::Options);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Go to album", "Album info"}));
    EXPECT_EQ(server.requests.size(), n);
}

TEST_F(StreamingTest, StarredLoadsPlaysAndKeepsAlbumIdentityAcrossRestart) {
    settings.setString("server.url", "https://music.example");
    openList("Starred"); EXPECT_EQ(speech.last(), "Starred. Loading, 1 of 1");
    ASSERT_EQ(server.lastRequest().kind, Kind::StarredTracks);
    auto tracks = weather(); for (auto& t : tracks) { t.albumId = "al1"; t.album = "Weather"; }
    answer(tracks); EXPECT_EQ(labels(), (std::vector<std::string>{"Storm Front", "Rain", "Sunshine"}));
    open("Rain"); EXPECT_EQ(player.file, "fake://stream/t2");
    player.pos = 42; app->shutdown();
    const auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved); ASSERT_EQ(saved->queue.size(), 3u);
    EXPECT_EQ(saved->queue.at(1).albumId, "al1");
    EXPECT_EQ(saved->queue.at(1).title, "Rain");
    start(); open("Now playing"); EXPECT_EQ(player.file, "fake://stream/t2");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    press(Button::Options); open("Go to album");
    EXPECT_EQ(server.lastRequest().kind, Kind::AlbumTracks); EXPECT_EQ(server.lastRequest().arg, "al1");
    expectNoUrlSpoken(speech);
}

TEST_F(StreamingTest, StarredErrorsEmptyAndCancellationUseOrdinaryListFeedback) {
    settings.setString("server.url", "https://music.example");
    openList("Starred"); ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable)); app->tick();
    EXPECT_NE(speech.last().find("Server not reachable"), std::string::npos); open("Try again");
    answer(std::vector<RemoteTrack>{}); EXPECT_EQ(labels(), (std::vector<std::string>{"No starred tracks"}));
    open("No starred tracks"); EXPECT_EQ(speech.last(), "Star a track from Options in the music player, then come back.");
    press(Button::Home); open("Starred"); auto id = server.lastRequest().id; press(Button::Home);
    EXPECT_FALSE(server.reply(id, weather())); app->tick(); EXPECT_EQ(level().title, "Server");
}

TEST_F(PlayerOptionsTest, TrackChangeSleepAndShutdownCancelStarCallbacks) {
    start(); openWeather(); open("Rain"); finishOpen(); press(Button::Options);
    auto read = server.lastRequest().id;
    player.atEnd = true; app->tick(); finishOpen();
    EXPECT_FALSE(server.reply(read, std::vector<RemoteTrack>{weather()[1]}));
    EXPECT_EQ(app->mode(), App::Mode::Player);
    press(Button::Options); answer(std::vector<RemoteTrack>{weather()[2]}); open("Star");
    auto write = server.lastRequest().id;
    hold(Button::Power); open("Sleep");
    EXPECT_FALSE(server.replyCreated(write));
    press(Button::Power); // wake
    press(Button::Home); // leave Power menu if it remains
    hold(Button::Home); open("Now playing"); press(Button::Options);
    auto exitRead = server.lastRequest().id;
    app->shutdown(); EXPECT_FALSE(server.reply(exitRead, std::vector<RemoteTrack>{weather()[2]}));
}

TEST_F(TwoServerPlayTest, StarsReadAndWriteOnlyThePlayingTracksServer) {
    start_(); ASSERT_TRUE(app->playQueue("Mixed", {onSecond("t1", "Dune")})); finishOpen();
    auto firstRequests = server.requests.size(); press(Button::Options);
    ASSERT_EQ(second.lastRequest().kind, Kind::TrackDetails); EXPECT_EQ(second.lastRequest().arg, "t1");
    RemoteTrack track; track.id = "t1"; track.starred = true;
    ASSERT_TRUE(second.reply(second.lastRequest().id, std::vector<RemoteTrack>{track})); app->tick();
    open("Unstar"); EXPECT_EQ(second.lastRequest().kind, Kind::Unstar); EXPECT_EQ(second.lastRequest().arg, "t1");
    ASSERT_TRUE(second.replyCreated(second.lastRequest().id)); app->tick();
    EXPECT_EQ(speech.last(), "Unstarred."); EXPECT_EQ(server.requests.size(), firstRequests);
}

TEST_F(PlayerOptionsTest, AnotherScreenCancelsStarOptionsAndReopeningFetchesFreshState) {
    start(); openWeather(); open("Rain"); finishOpen(); press(Button::Options);
    answer(std::vector<RemoteTrack>{weather()[1]}); open("Star");
    auto write = server.lastRequest().id; hold(Button::Power);
    EXPECT_FALSE(server.replyCreated(write)); app->tick();
    EXPECT_EQ(level().title, "Power");
    press(Button::Home); EXPECT_EQ(app->mode(), App::Mode::Player);
    press(Button::Options); EXPECT_EQ(server.lastRequest().kind, Kind::TrackDetails);
    RemoteTrack t = weather()[1]; t.starred = true;
    answer(std::vector<RemoteTrack>{t}); EXPECT_TRUE(hasLabel("Unstar")); EXPECT_FALSE(hasLabel("Star"));
}

TEST_F(PlayerOptionsTest, AutoSleepCancelsPendingStarMutation) {
    settings.setInt("power.auto_sleep_min", 1);
    start(); openWeather(); open("Rain"); finishOpen(); press(Button::Options);
    answer(std::vector<RemoteTrack>{weather()[1]}); open("Star");
    auto id = server.lastRequest().id;
    press(Button::PlayPause); // paused so automatic sleep is allowed
    idle(2min); ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_FALSE(server.replyCreated(id)); app->tick(); EXPECT_FALSE(speech.heard("Starred."));
}

class LinkedLocalStarTest : public AppTest {
protected:
    void SetUp() override {
        AppTest::SetUp();
        settings.setString("server.url", "https://music.example");
        settings.setString("server.user", "ann");
        ServerConfig c;
        c.url = "https://second.example"; c.user = "ben"; c.name = "Second";
        secondKey = TrackCache::serverKey(c.url, c.user);
        firstKey = TrackCache::serverKey("https://music.example", "ann");
        moreLinks.push_back({c, secondKey, "Second", second, secondDownloader});
        path = (roadTrip() / "01 - Sunrise.mp3").string();
    }
    void seed(std::vector<std::string> refs, IndexItem::Source source = IndexItem::Source::Local) {
        IndexItem item = savedItem("linked-local", IndexItem::Kind::Music, "Road Trip", {path, (roadTrip() / "02 - Highway.mp3").string()});
        item.source = source;
        item.tracks[0].seenOn = std::move(refs);
        // Neither an album-level link nor a different track's link identifies the playing track.
        item.seenOn = {LibraryIndex::seenRef(firstKey, SeenKind::Album, "album-id")};
        item.tracks[1].seenOn = {LibraryIndex::seenRef(firstKey, SeenKind::Track, "other-song")};
        indexStore.items[item.id] = std::move(item);
    }
    void playLocal() {
        start(false);
        QueueEntry e; e.kind = QueueEntry::Kind::Local; e.path = path; e.title = "Sunrise"; e.album = "Road Trip";
        ASSERT_TRUE(app->playQueue("Road Trip", {e}));
        ASSERT_EQ(player.file, path);
    }
    void acknowledge(FakeMusicServer& target, const std::string& id, bool starred) {
        ASSERT_EQ(target.lastRequest().kind, Kind::TrackDetails);
        EXPECT_EQ(target.lastRequest().arg, id);
        RemoteTrack track; track.id = id; track.starred = starred;
        ASSERT_TRUE(target.reply(target.lastRequest().id, std::vector<RemoteTrack>{track})); app->tick();
        open(starred ? "Unstar" : "Star");
        EXPECT_EQ(target.lastRequest().kind, starred ? Kind::Unstar : Kind::Star);
        EXPECT_EQ(target.lastRequest().arg, id);
        ASSERT_TRUE(target.replyCreated(target.lastRequest().id)); app->tick();
        EXPECT_EQ(speech.last(), starred ? "Unstarred." : "Starred.");
        EXPECT_EQ(player.file, path); EXPECT_TRUE(player.playing());
    }
    FakeMusicServer second;
    FakeDownloader secondDownloader;
    std::string path, firstKey, secondKey;
};

TEST_F(LinkedLocalStarTest, LinkedLocalPlaybackStarsAndUnstarsTheExactTrackAndStaysLocal) {
    seed({LibraryIndex::seenRef(firstKey, SeenKind::Track, "server-song")});
    playLocal();
    const auto opens = player.opened.size();
    for (bool starred : {false, true}) {
        press(Button::Options);
        ASSERT_NO_FATAL_FAILURE(acknowledge(server, "server-song", starred));
        EXPECT_EQ(app->mode(), App::Mode::Player);
    }
    EXPECT_EQ(player.opened.size(), opens);
    app->shutdown();
    auto saved = parseQueue(diskFiles().read(queueFile).value_or(""));
    ASSERT_TRUE(saved); ASSERT_EQ(saved->queue.size(), 1u);
    EXPECT_EQ(saved->queue.at(0).kind, QueueEntry::Kind::Local);
    EXPECT_EQ(saved->queue.at(0).path, path); EXPECT_TRUE(saved->queue.at(0).trackId.empty());
}

TEST_F(LinkedLocalStarTest, CachedLocalFileUsesItsTrackReferenceAndSkipsNonTrackReferences) {
    path = (base / "cache" / "download.mp3").string(); touch(path);
    seed({LibraryIndex::seenRef(firstKey, SeenKind::Album, "album-id"),
          LibraryIndex::seenRef(firstKey, SeenKind::Playlist, "playlist-id"),
          firstKey + ":track:", firstKey + ":track", firstKey + "extra:track:wrong-server",
          LibraryIndex::seenRef(firstKey, SeenKind::Track, "downloaded-song")}, IndexItem::Source::Cached);
    playLocal(); press(Button::Options);
    ASSERT_NO_FATAL_FAILURE(acknowledge(server, "downloaded-song", false));
}

TEST_F(LinkedLocalStarTest, InvalidTrackReferencesNeverBorrowAlbumOrSiblingTrackIdentity) {
    seed({LibraryIndex::seenRef(firstKey, SeenKind::Album, "album-id"),
          LibraryIndex::seenRef(firstKey, SeenKind::Playlist, "playlist-id"),
          firstKey + ":track:", firstKey + ":track", firstKey + "extra:track:wrong",
          "removed-server:track:gone"});
    playLocal(); const auto requests = server.requests.size() + second.requests.size(); press(Button::Options);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Go to album", "Album info"}));
    EXPECT_EQ(server.requests.size() + second.requests.size(), requests);
}

TEST_F(LinkedLocalStarTest, FirstEligibleConfiguredServerWinsRegardlessOfReferenceOrder) {
    seed({LibraryIndex::seenRef(secondKey, SeenKind::Track, "second-song"),
          LibraryIndex::seenRef(firstKey, SeenKind::Track, "first-song")});
    playLocal(); const auto otherRequests = second.requests.size(); press(Button::Options);
    ASSERT_NO_FATAL_FAILURE(acknowledge(server, "first-song", false));
    EXPECT_EQ(second.requests.size(), otherRequests);
}

TEST_F(LinkedLocalStarTest, UnsupportedFirstServerIsSkippedAndSecondServerReceivesBothOperations) {
    server.starsSupported = false;
    seed({LibraryIndex::seenRef(firstKey, SeenKind::Track, "first-song"),
          LibraryIndex::seenRef(secondKey, SeenKind::Track, "second-song")});
    playLocal(); const auto firstRequests = server.requests.size();
    for (bool starred : {false, true}) {
        press(Button::Options);
        ASSERT_NO_FATAL_FAILURE(acknowledge(second, "second-song", starred));
    }
    EXPECT_EQ(server.requests.size(), firstRequests);
}

TEST_F(LinkedLocalStarTest, UnconfiguredFirstServerIsSkippedAndLocalCallbackCancelsOnLeave) {
    server.isConfigured = false;
    seed({LibraryIndex::seenRef(firstKey, SeenKind::Track, "first-song"),
          LibraryIndex::seenRef(secondKey, SeenKind::Track, "second-song")});
    playLocal(); press(Button::Options);
    ASSERT_EQ(second.lastRequest().kind, Kind::TrackDetails); EXPECT_EQ(second.lastRequest().arg, "second-song");
    auto id = second.lastRequest().id; press(Button::Home);
    RemoteTrack track; track.id = "second-song"; track.title = "Sunrise";
    EXPECT_FALSE(second.reply(id, std::vector<RemoteTrack>{track}));
    EXPECT_EQ(player.file, path); EXPECT_TRUE(player.playing());
}

TEST(HalEventsTest, PostsInOrderAndWakesOutsideTheLock) {
    int wakes = 0;
    HalEvents* queue = nullptr;
    HalEvents events([&] { ++wakes; if (wakes == 1) queue->post(HalError{"second"}); });
    queue = &events;
    events.post(HalError{"first"});
    EXPECT_EQ(wakes, 2);
    const auto drained = events.drain();
    ASSERT_EQ(drained.size(), 2u);
    EXPECT_EQ(std::get<HalError>(drained[0]).message, "first");
    EXPECT_EQ(std::get<HalError>(drained[1]).message, "second");
    EXPECT_TRUE(events.drain().empty());
}

TEST(HalEventsTest, ConcurrentPostsAreNotLost) {
    constexpr int workers = 4, count = 1000;
    std::atomic<int> wakes = 0;
    HalEvents events([&] { ++wakes; });
    std::vector<std::thread> threads;
    for (int i = 0; i < workers; ++i)
        threads.emplace_back([&, i] {
            for (int j = 0; j < count; ++j) events.post(HalError{std::to_string(i * count + j)});
        });
    for (auto& thread : threads) thread.join();
    const auto drained = events.drain();
    ASSERT_EQ(drained.size(), workers * count);
    EXPECT_EQ(wakes.load(), workers * count);
    std::set<std::string> unique;
    for (const auto& event : drained) unique.insert(std::get<HalError>(event).message);
    EXPECT_EQ(unique.size(), workers * count);
}

TEST_F(AppTest, HalEventsAreSpokenOnTickAndIncludedInStatus) {
    HalEvents events;
    app = std::make_unique<App>(Services{speech, player, sounds, volume, serverLinks(), indexStore,
        scanner, settings, state, clock, textFiles, &events}, root, music, cache, queueFile, scrobbleFile);
    app->start();
    speech.said.clear();
    events.post(HalError{"First hardware error."});
    events.post(HalError{"Second hardware error."});
    EXPECT_TRUE(speech.said.empty());
    app->tick();
    ASSERT_GE(speech.said.size(), 2u);
    EXPECT_EQ(speech.said[0], "First hardware error.");
    EXPECT_EQ(speech.said[1], "Second hardware error.");
    open("System");
    open("Status");
    EXPECT_NE(speech.last().find("First hardware error."), std::string::npos);
    EXPECT_NE(speech.last().find("Second hardware error."), std::string::npos);
}

TEST_F(AppTest, StatusUsesCurrentPowerAndStorageSnapshotsAndOmitsMissingParts) {
    FakePower power;
    FakeStorage storage;
    power.snapshot.percent = 73;
    storage.snapshot.freeBytes = 9ULL * 1024 * 1024 * 1024;
    auto make = [&](Power* p, Storage* s) {
        app = std::make_unique<App>(Services{speech, player, sounds, volume, serverLinks(), indexStore,
            scanner, settings, state, clock, textFiles, nullptr, p, s}, root, music, cache, queueFile, scrobbleFile);
        app->start();
        open("System"); open("Status");
    };
    make(&power, &storage);
    EXPECT_NE(speech.last().find("Battery 73 percent."), std::string::npos);
    EXPECT_NE(speech.last().find("9 gigabytes free."), std::string::npos);
    EXPECT_NE(speech.last().find("Wi-Fi connected."), std::string::npos);
    power.snapshot.percent = 24;
    storage.snapshot.freeBytes = 3ULL * 1024 * 1024 * 1024;
    press(Button::Ok);
    EXPECT_NE(speech.last().find("Battery 24 percent."), std::string::npos);
    EXPECT_NE(speech.last().find("3 gigabytes free."), std::string::npos);
    make(nullptr, &storage);
    EXPECT_EQ(speech.last().find("Battery"), std::string::npos);
    EXPECT_NE(speech.last().find("3 gigabytes free."), std::string::npos);
    make(&power, nullptr);
    EXPECT_NE(speech.last().find("Battery 24 percent."), std::string::npos);
    EXPECT_EQ(speech.last().find("gigabytes free"), std::string::npos);
    make(nullptr, nullptr);
    EXPECT_EQ(speech.last().find("Battery"), std::string::npos);
    EXPECT_EQ(speech.last().find("gigabytes free"), std::string::npos);
    EXPECT_NE(speech.last().find("Wi-Fi connected."), std::string::npos);
}

TEST_F(AppTest, BatteryWarningsAndChargingEdgesRearmOnce) {
    HalEvents events;
    FakePower power;
    app = std::make_unique<App>(Services{speech, player, sounds, volume, serverLinks(), indexStore,
        scanner, settings, state, clock, textFiles, &events, &power}, root, music, cache, queueFile, scrobbleFile);
    app->start();
    speech.said.clear();
    auto post = [&](int percent, bool charging = false, bool full = false) {
        events.post(BatteryChanged{{percent, charging, charging, full}});
        app->tick();
    };
    post(21); EXPECT_TRUE(speech.said.empty());
    post(20); EXPECT_EQ(speech.last(), "Battery 20 percent.");
    post(20); post(19);
    post(10); EXPECT_EQ(speech.last(), "Battery 10 percent.");
    post(9);
    int stopsBeforeCritical = speech.stops;
    post(5); EXPECT_EQ(speech.last(), "Battery 5 percent.");
    EXPECT_EQ(speech.stops, stopsBeforeCritical + 1);
    post(4);
    EXPECT_EQ(speech.said, (std::vector<std::string>{"Battery 20 percent.", "Battery 10 percent.", "Battery 5 percent."}));
    post(4, true); post(5, true); post(100, true, true); post(100, true, true);
    EXPECT_EQ(std::count(speech.said.begin(), speech.said.end(), "Charging started."), 1);
    EXPECT_EQ(std::count(speech.said.begin(), speech.said.end(), "Battery full."), 1);
    post(20); post(10); post(5);
    EXPECT_EQ(std::count(speech.said.begin(), speech.said.end(), "Battery 20 percent."), 2);
    EXPECT_EQ(std::count(speech.said.begin(), speech.said.end(), "Battery 10 percent."), 2);
    EXPECT_EQ(std::count(speech.said.begin(), speech.said.end(), "Battery 5 percent."), 2);
}

TEST_F(AppTest, CriticalBatterySavesBeforePowerOffOrWarnsWhenUnsupported) {
    HalEvents events;
    FakePower power;
    power.capabilities.powerOff = true;
    app = std::make_unique<App>(Services{speech, player, sounds, volume, serverLinks(), indexStore,
        scanner, settings, state, clock, textFiles, &events, &power}, root, music, cache, queueFile, scrobbleFile);
    app->start();
    state.setInt("battery_test", 42);
    power.onPowerOff = [&] { EXPECT_EQ(stateFile.file["battery_test"], "42"); };
    events.post(BatteryChanged{{3, false, false, false}});
    app->tick();
    EXPECT_EQ(power.powerOffRequests, 1);
    events.post(BatteryChanged{{2, false, false, false}});
    app->tick();
    EXPECT_EQ(power.powerOffRequests, 1);
    power.capabilities.powerOff = false;
    events.post(BatteryChanged{{4, true, true, false}});
    events.post(BatteryChanged{{3, false, false, false}});
    app->tick();
    EXPECT_EQ(power.powerOffRequests, 1);
    EXPECT_EQ(speech.last(), "Battery critical.");
}

class PlaylistEditingTest : public MusicQueueTest {
protected:
    void SetUp() override {
        MusicQueueTest::SetUp();
        localPlaylists = &playlists;
    }
    FakeLocalPlaylists playlists;
};

TEST_F(PlaylistEditingTest, LocalCreateRemoveAndDeleteConfirmation) {
    start();
    open("Music");
    open("Playlists");
    open("Create playlist");
    EXPECT_EQ(labels().front(), "On the device");
    press(Button::Ok);
    ASSERT_EQ(playlists.lists.size(), 1u);
    EXPECT_TRUE(speech.last().find("Created Playlist, ") == 0);
    const auto name = playlists.lists.front().name;
    EXPECT_TRUE(name.starts_with("Playlist, "));
    playlists.lists.front().tracks = {roadTrip() / "01 - Sunrise.mp3", roadTrip() / "02 - Highway.mp3"};
    open(name);
    press(Button::Forward);
    press(Button::Options);
    open("Remove from playlist");
    ASSERT_EQ(playlists.lists.front().tracks.size(), 1u);
    EXPECT_EQ(playlists.lists.front().tracks.front(), roadTrip() / "02 - Highway.mp3");
    EXPECT_EQ(speech.last(), "Removed from playlist.");
    open("Delete playlist");
    EXPECT_EQ(level().items[level().index].label, "Cancel");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Cancelled.");
    ASSERT_EQ(playlists.lists.size(), 1u);
    open("Delete playlist");
    open("Yes, delete");
    EXPECT_TRUE(playlists.lists.empty());
    EXPECT_EQ(speech.last(), "Playlist deleted.");
}

TEST_F(PlaylistEditingTest, AddLocalTrackRemembersLastEligiblePlaylistAndReportsWriteFailure) {
    playlists.lists = {{"a", "Apple", {}}, {"z", "Zebra", {}}};
    start();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm")}));
    press(Button::Ok);
    open("Add to playlist");
    open("Zebra");
    EXPECT_EQ(speech.last(), "Added to playlist.");
    ASSERT_EQ(playlists.lists[1].tracks.size(), 1u);
    EXPECT_EQ(playlists.lists[1].tracks[0], alphaFile(1));
    open("Add to playlist");
    EXPECT_EQ(level().items[level().index].label, "Zebra");
    playlists.failWrites = true;
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Could not save playlist.");
    EXPECT_EQ(playlists.lists[1].tracks.size(), 1u);
}

TEST_F(PlaylistEditingTest, ServerCreateAddRemoveAndDeleteHandleErrorsAndCancellation) {
    indexStore.playlists.push_back({TrackCache::serverKey("https://music.example", "ann"), "p", "Remote mix", 2, 0});
    start();
    open("Music");
    open("Playlists");
    open("Create playlist");
    open("Server");
    EXPECT_EQ(speech.last(), "Saving.");
    auto create = server.lastRequest();
    EXPECT_EQ(create.kind, Kind::CreatePlaylist);
    ASSERT_TRUE(create.trackIds.empty());
    ASSERT_TRUE(server.replyCreated(create.id, "new")); app->tick();
    EXPECT_TRUE(speech.last().find("Created Playlist, ") == 0);
    EXPECT_TRUE(hasLabel("Remote mix"));
    open("Remote mix");
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "First"}, {"t2", "Second"}})); app->tick();
    open("First");
    press(Button::Ok);
    open("Add to playlist");
    EXPECT_EQ(labels(), (std::vector<std::string>{create.arg, "Remote mix"}));
    open("Remote mix");
    auto add = server.lastRequest();
    EXPECT_EQ(add.kind, Kind::AddPlaylistTrack);
    EXPECT_EQ(add.arg, "p");
    EXPECT_EQ(add.trackIds, (std::vector<std::string>{"t1"}));
    ASSERT_TRUE(server.fail(add.id, MusicServer::Error::Unreachable)); app->tick();
    EXPECT_EQ(speech.last(), "Server not reachable.");
    press(Button::Ok);
    auto retry = server.lastRequest();
    ASSERT_TRUE(server.replyCreated(retry.id)); app->tick();
    EXPECT_EQ(speech.last(), "Added to playlist.");
    auto stored = std::find_if(indexStore.playlists.begin(), indexStore.playlists.end(), [](const IndexPlaylist& p) { return p.id == "p"; });
    ASSERT_NE(stored, indexStore.playlists.end()); EXPECT_EQ(stored->songCount, 3);
    press(Button::Home); // player
    press(Button::Home); // playlist
    press(Button::Forward); // First
    press(Button::Options);
    open("Remove from playlist");
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "First"}, {"t2", "Second"}})); app->tick();
    open("Second");
    auto remove = server.lastRequest();
    EXPECT_EQ(remove.kind, Kind::RemovePlaylistTrack);
    EXPECT_EQ(remove.trackIndex, 1u);
    ASSERT_TRUE(server.replyCreated(remove.id)); app->tick();
    EXPECT_EQ(speech.last(), "Removed from playlist.");
    stored = std::find_if(indexStore.playlists.begin(), indexStore.playlists.end(), [](const IndexPlaylist& p) { return p.id == "p"; });
    ASSERT_NE(stored, indexStore.playlists.end()); EXPECT_EQ(stored->songCount, 2);
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "First"}})); app->tick();
    open("Delete playlist");
    EXPECT_EQ(level().items[level().index].label, "Cancel");
    open("Yes, delete");
    auto deletion = server.lastRequest();
    EXPECT_EQ(deletion.kind, Kind::DeletePlaylist);
    press(Button::Home);
    EXPECT_FALSE(server.replyCreated(deletion.id));
    open("Delete playlist");
    open("Yes, delete");
    auto again = server.lastRequest();
    ASSERT_TRUE(server.replyCreated(again.id)); app->tick();
    EXPECT_EQ(speech.last(), "Playlist deleted.");
    EXPECT_FALSE(hasLabel("Remote mix"));
}

TEST_F(PlaylistEditingTest, RemoteTrackCannotBeAddedToLocalPlaylist) {
    playlists.lists = {{"local", "Local list", {}}};
    indexStore.playlists.push_back({TrackCache::serverKey("https://music.example", "ann"), "p", "Remote mix", 1, 0});
    start();
    ASSERT_TRUE(app->playQueue("Remote", {remoteEntry("t1", "First")}));
    press(Button::Ok);
    open("Add to playlist");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Remote mix"}));
    open("Remote mix");
    EXPECT_EQ(server.lastRequest().kind, Kind::AddPlaylistTrack);
    EXPECT_EQ(server.lastRequest().trackIds, (std::vector<std::string>{"t1"}));
    ASSERT_TRUE(server.replyCreated(server.lastRequest().id)); app->tick();
    EXPECT_TRUE(playlists.lists[0].tracks.empty());
}

class DownloadedPlaylistEditingTest : public MusicPlaylistsTest {
protected:
    void SetUp() override {
        MusicPlaylistsTest::SetUp();
        localPlaylists = &playlists;
    }
    FakeLocalPlaylists playlists;
};

TEST_F(DownloadedPlaylistEditingTest, KeepOfflinePreservesOrderedDuplicatesAndIndependentDownloads) {
    indexPlaylist("p1", "Road Trip", 3);
    auto manual = seedRoad();
    openPlaylists();
    open("Road Trip");
    open("Keep offline");
    EXPECT_EQ(speech.last(), "Keeping offline.");
    app->tick();
    auto request = server.lastRequest();
    ASSERT_EQ(request.kind, Kind::PlaylistTracks);
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 0; });
    ASSERT_TRUE(server.reply(request.id, std::vector<RemoteTrack>{{"t1", "Sunrise"}, {"t1", "Sunrise"}, {"t2", "Highway"}}));
    app->tick();
    EXPECT_TRUE(downloader.requests.empty()) << "existing files are shared without another transfer or space";
    RemoteSource kept{RemoteSource::Kind::KeptPlaylist, "p1"};
    auto snapshot = store().load(kept);
    EXPECT_EQ(snapshot.order, (std::vector<std::string>{"t1", "t1", "t2"}));
    ASSERT_EQ(snapshot.tracks.size(), 2u);
    EXPECT_TRUE(snapshot.tracks[0].pinned);
    EXPECT_TRUE(snapshot.tracks[1].pinned);
    EXPECT_TRUE(fs::equivalent(store().listFolder(kept) / snapshot.tracks[0].file, store().listFolder(manual) / "t1.mp3"));
    press(Button::Home);
    open("Road Trip");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play playlist", "Download playlist", "Sunrise", "Sunrise", "Highway", "Stop keeping offline", "Delete playlist"}));
    open("Stop keeping offline");
    EXPECT_EQ(speech.last(), "Stopped keeping offline.");
    snapshot = store().load(kept);
    EXPECT_FALSE(snapshot.tracks[0].pinned);
    EXPECT_TRUE(fs::exists(store().listFolder(kept) / snapshot.tracks[0].file));
    EXPECT_TRUE(store().load(manual).tracks[0].pinned);
    ASSERT_TRUE(store().remove(manual));
    EXPECT_TRUE(fs::exists(store().listFolder(kept) / snapshot.tracks[0].file));
}

TEST_F(DownloadedPlaylistEditingTest, KeptPlaylistRefreshSurvivesRestartAndFailedFetch) {
    indexPlaylist("p1", "Road Trip", 2);
    RemoteSource kept{RemoteSource::Kind::KeptPlaylist, "p1"};
    seed(kept, "Road Trip", "", {{"t1", "Sunrise"}, {"t2", "Highway"}});
    auto original = store().load(kept);
    original.order = {"t1", "t2"};
    ASSERT_TRUE(store().save(original));
    auto key = "playlist.keep:" + TrackCache::safeName(TrackCache::serverKey("https://music.example", "bob")) + ":p1";
    state.setInt(key, 1);
    state.setInt("playlist.keep.ready:" + TrackCache::safeName(TrackCache::serverKey("https://music.example", "bob")) + ":p1", 0);
    start();
    syncWith({{"p1", "Road Trip", 2}});
    auto request = server.lastRequest();
    ASSERT_EQ(request.kind, Kind::PlaylistTracks);
    ASSERT_TRUE(server.fail(request.id, MusicServer::Error::Unreachable)); app->tick();
    EXPECT_EQ(store().load(kept).order, original.order);
    app->shutdown();
    app.reset();
    state = Settings(&stateFile);
    state.load();
    ASSERT_EQ(state.getInt(key, 0), 1);
    start();
    syncWith({{"p1", "Road Trip", 2}});
    request = server.lastRequest();
    ASSERT_EQ(request.kind, Kind::PlaylistTracks);
    ASSERT_TRUE(server.reply(request.id, std::vector<RemoteTrack>{{"t2", "Highway", "", "", 100, 1, "mp3", 5}, {"t3", "New", "", "", 100, 2, "mp3", 5}})); app->tick();
    ASSERT_EQ(downloader.lastRequest().trackId, "t3");
    finishLast(Downloader::Result::Unreachable);
    auto failed = store().load(kept);
    EXPECT_EQ(failed.order, original.order);
    EXPECT_TRUE(failed.tracks[0].pinned);
    EXPECT_TRUE(failed.tracks[1].pinned);
    app->shutdown(); app.reset();
    state = Settings(&stateFile); state.load();
    start();
    syncWith({{"p1", "Road Trip", 2}});
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t2", "Highway", "", "", 100, 1, "mp3", 5}, {"t3", "New", "", "", 100, 2, "mp3", 5}})); app->tick();
    ASSERT_EQ(downloader.lastRequest().trackId, "t3");
    finishLast();
    auto result = store().load(kept);
    EXPECT_EQ(result.order, (std::vector<std::string>{"t2", "t3"}));
    auto removed = std::find_if(result.tracks.begin(), result.tracks.end(), [](const CachedTrack& t) { return t.id == "t1"; });
    ASSERT_NE(removed, result.tracks.end());
    EXPECT_FALSE(removed->pinned);
    EXPECT_TRUE(fs::exists(store().listFolder(kept) / removed->file));
    open("Music"); open("Playlists"); open("Road Trip");
    EXPECT_TRUE(hasLabel("Highway"));
    EXPECT_TRUE(hasLabel("New"));
    EXPECT_FALSE(hasLabel("Sunrise"));
}

TEST_F(DownloadedPlaylistEditingTest, KeepOfflineReportsFetchFailureAndInsufficientSpaceAndCanRetry) {
    indexPlaylist("p1", "Road Trip", 1);
    seedRoad();
    openPlaylists(); open("Road Trip"); open("Keep offline"); app->tick();
    auto request = server.lastRequest();
    ASSERT_EQ(request.kind, Kind::PlaylistTracks);
    ASSERT_TRUE(server.fail(request.id, MusicServer::Error::Unreachable)); app->tick();
    EXPECT_EQ(speech.last(), "Server not reachable.");
    EXPECT_TRUE(downloader.requests.empty());
    open("Stop keeping offline");
    open("Keep offline"); app->tick();
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 0; });
    request = server.lastRequest();
    ASSERT_TRUE(server.reply(request.id, std::vector<RemoteTrack>{{"t3", "New", "", "", 100, 1, "mp3", 5}})); app->tick();
    EXPECT_EQ(speech.last(), "Not enough space.");
    EXPECT_TRUE(downloader.requests.empty());
    open("Stop keeping offline");
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 100; });
    open("Keep offline"); app->tick();
    request = server.lastRequest();
    ASSERT_TRUE(server.reply(request.id, std::vector<RemoteTrack>{{"t3", "New", "", "", 100, 1, "mp3", 5}})); app->tick();
    ASSERT_EQ(downloader.lastRequest().trackId, "t3");
    finishLast();
    EXPECT_TRUE(store().load({RemoteSource::Kind::KeptPlaylist, "p1"}).tracks[0].pinned);
}

TEST_F(DownloadedPlaylistEditingTest, StoppingAndDeletingKeptPlaylistReleaseOnlyItsPins) {
    indexPlaylist("p1", "Road Trip", 2);
    auto manual = seedRoad();
    RemoteSource kept{RemoteSource::Kind::KeptPlaylist, "p1"};
    seed(kept, "Road Trip", "", {{"t1", "Sunrise"}});
    auto saved = store().load(kept);
    saved.order = {"t1"};
    ASSERT_TRUE(store().save(saved));
    auto key = "playlist.keep:" + TrackCache::safeName(TrackCache::serverKey("https://music.example", "bob")) + ":p1";
    state.setInt(key, 1);
    openPlaylists(); open("Road Trip");
    open("Stop keeping offline");
    EXPECT_FALSE(store().load(kept).tracks[0].pinned);
    open("Keep offline"); app->tick();
    auto pending = server.lastRequest().id;
    open("Stop keeping offline");
    EXPECT_FALSE(server.reply(pending, std::vector<RemoteTrack>{{"t2", "Highway"}}));
    open("Keep offline"); app->tick();
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "Sunrise"}})); app->tick();
    EXPECT_TRUE(store().load(kept).tracks[0].pinned);
    open("Delete playlist"); open("Yes, delete");
    ASSERT_TRUE(server.replyCreated(server.lastRequest().id)); app->tick();
    EXPECT_EQ(speech.last(), "Playlist deleted.");
    EXPECT_FALSE(store().load(kept).tracks[0].pinned);
    EXPECT_TRUE(store().load(manual).tracks[0].pinned);
}

TEST_F(DownloadedPlaylistEditingTest, AManualDownloadReusesTheKeptPlaylistsFile) {
    indexPlaylist("p1", "Road Trip", 1);
    seed(playlistSource("p1"), "Old download", "", {{"t1", "Sunrise", false}}); // manifest survived; its file did not
    RemoteSource kept{RemoteSource::Kind::KeptPlaylist, "p1"};
    seed(kept, "Road Trip", "", {{"t1", "Sunrise"}});
    auto saved = store().load(kept);
    saved.order = {"t1"};
    ASSERT_TRUE(store().save(saved));
    auto key = TrackCache::safeName(TrackCache::serverKey("https://music.example", "bob")) + ":p1";
    state.setInt("playlist.keep:" + key, 1);
    state.setInt("playlist.keep.ready:" + key, 0);
    openPlaylists(); open("Road Trip");
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 0; });
    open("Download playlist");
    EXPECT_TRUE(downloader.requests.empty());
    auto manual = store().load(playlistSource("p1"));
    ASSERT_EQ(manual.tracks.size(), 1u);
    EXPECT_TRUE(manual.tracks[0].pinned);
    EXPECT_TRUE(fs::equivalent(store().listFolder(playlistSource("p1")) / manual.tracks[0].file,
                               store().listFolder(kept) / saved.tracks[0].file));
    ASSERT_TRUE(store().remove(playlistSource("p1")));
    CachedTrack low{"t1", "low.mp3", "Sunrise", 100, "", true, 0, 128};
    touch(store().listFolder(playlistSource("p1")) / low.file);
    ASSERT_TRUE(store().save(CachedList{playlistSource("p1"), "Road Trip", "", {low}}));
    open("Download playlist");
    EXPECT_TRUE(downloader.requests.empty()) << "a suitable shared original replaces the lower-quality file";
    manual = store().load(playlistSource("p1"));
    ASSERT_EQ(manual.tracks.size(), 1u);
    EXPECT_EQ(manual.tracks[0].bitrate, 0);
    EXPECT_FALSE(fs::exists(store().listFolder(playlistSource("p1")) / low.file));
    EXPECT_TRUE(fs::equivalent(store().listFolder(playlistSource("p1")) / manual.tracks[0].file,
                               store().listFolder(kept) / saved.tracks[0].file));
}

TEST_F(DownloadedPlaylistEditingTest, AKeptSnapshotDoesNotMakeAnOlderDownloadCurrent) {
    indexPlaylist("p1", "Road Trip", 1);
    seedRoad();
    RemoteSource kept{RemoteSource::Kind::KeptPlaylist, "p1"};
    seed(kept, "Road Trip", "", {{"t3", "New"}});
    auto snapshot = store().load(kept);
    snapshot.order = {"t3"};
    ASSERT_TRUE(store().save(snapshot));
    auto key = TrackCache::safeName(TrackCache::serverKey("https://music.example", "bob")) + ":p1";
    state.setInt("playlist.keep:" + key, 1);
    state.setInt("playlist.keep.revision:" + key, 1);
    state.setInt("playlist.keep.ready:" + key, 0);
    state.setInt("playlist.stale:" + key, 1);
    start();
    syncWith({{"p1", "Road Trip", 1}});
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t3", "New"}})); app->tick();
    open("Music"); open("Playlists"); open("Road Trip");
    EXPECT_TRUE(hasLabel("New"));
    open("Stop keeping offline");
    press(Button::Home);
    auto count = requestsOf(Kind::PlaylistTracks);
    open("Road Trip");
    EXPECT_EQ(requestsOf(Kind::PlaylistTracks), count + 1);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Loading"}));
}

TEST_F(DownloadedPlaylistEditingTest, EmptyKeptPlaylistOpensOffline) {
    indexPlaylist("p1", "Empty", 0);
    RemoteSource kept{RemoteSource::Kind::KeptPlaylist, "p1"};
    ASSERT_TRUE(store().save(CachedList{kept, "Empty", "", {}}));
    auto key = TrackCache::safeName(TrackCache::serverKey("https://music.example", "bob")) + ":p1";
    state.setInt("playlist.keep:" + key, 1);
    state.setInt("playlist.keep.ready:" + key, 0);
    openPlaylists();
    auto count = requestsOf(Kind::PlaylistTracks);
    open("Empty");
    EXPECT_EQ(requestsOf(Kind::PlaylistTracks), count);
    EXPECT_TRUE(hasLabel("Play playlist"));
    EXPECT_TRUE(hasLabel("Stop keeping offline"));
    open("Play playlist");
    EXPECT_EQ(speech.last(), "No tracks.");
}

TEST_F(DownloadedPlaylistEditingTest, ServerDeletionReleasesKeptPinsButKeepsIndependentDownload) {
    indexPlaylist("p1", "Road Trip", 1);
    auto manual = seedRoad();
    RemoteSource kept{RemoteSource::Kind::KeptPlaylist, "p1"};
    seed(kept, "Road Trip", "", {{"t1", "Sunrise"}});
    auto snapshot = store().load(kept);
    snapshot.order = {"t1"};
    ASSERT_TRUE(store().save(snapshot));
    auto key = "playlist.keep:" + TrackCache::safeName(TrackCache::serverKey("https://music.example", "bob")) + ":p1";
    state.setInt(key, 1);
    start();
    syncWith({});
    EXPECT_EQ(state.getInt(key, 0), 0);
    EXPECT_FALSE(store().load(kept).tracks[0].pinned);
    EXPECT_TRUE(store().load(manual).tracks[0].pinned);
    EXPECT_TRUE(fs::exists(store().listFolder(kept) / snapshot.tracks[0].file));
}

TEST_F(DownloadedPlaylistEditingTest, RefreshReportsInsufficientSpaceForAKeptPlaylist) {
    indexPlaylist("p1", "Road Trip", 1);
    auto key = "playlist.keep:" + TrackCache::safeName(TrackCache::serverKey("https://music.example", "bob")) + ":p1";
    state.setInt(key, 1);
    start();
    syncWith({{"p1", "Road Trip", 1}});
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable)); app->tick();
    open("Music");
    app->setFreeSpace([](const fs::path&) -> std::optional<std::uintmax_t> { return 0; });
    open("Refresh library");
    syncWith({{"p1", "Road Trip", 1}});
    ASSERT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t3", "New", "", "", 100, 1, "mp3", 5}})); app->tick();
    EXPECT_EQ(speech.last(), "Not enough space.");
    EXPECT_TRUE(downloader.requests.empty());
}

TEST_F(DownloadedPlaylistEditingTest, RemovalFetchesServerOrderAndHandlesFailureAndDuplicates) {
    indexPlaylist("p1", "Road Trip", 3);
    seedRoad(); // the snapshot has Sunrise and Highway only
    openPlaylists();
    open("Road Trip");
    EXPECT_TRUE(hasLabel("Highway"));
    while (level().items[level().index].label != "Highway") press(Button::Forward);
    press(Button::Options);
    open("Remove from playlist");
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
    auto first = server.lastRequest().id;
    ASSERT_TRUE(server.fail(first, MusicServer::Error::Unreachable)); app->tick();
    EXPECT_TRUE(hasLabel("Server not reachable"));
    EXPECT_EQ(requestsOf(Kind::RemovePlaylistTrack), 0u);
    open("Try again");
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "Sunrise"}, {"t1", "Sunrise"}, {"t2", "Highway"}})); app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Sunrise", "Sunrise", "Highway"}));
    open("Highway");
    auto remove = server.lastRequest();
    EXPECT_EQ(remove.kind, Kind::RemovePlaylistTrack);
    EXPECT_EQ(remove.trackIndex, 2u);
    ASSERT_TRUE(server.replyCreated(remove.id)); app->tick();
    EXPECT_EQ(speech.last(), "Removed from playlist.");
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "Sunrise"}, {"t1", "Sunrise"}})); app->tick();
    open("Download playlist");
    EXPECT_EQ(speech.last(), "Already downloaded.");
    press(Button::Home);
    auto asked = requestsOf(Kind::PlaylistTracks);
    open("Road Trip");
    EXPECT_EQ(requestsOf(Kind::PlaylistTracks), asked + 1) << "the older download is not the current server list";
    EXPECT_TRUE(hasLabel("Loading"));
}

class LinkedLocalPlaylistTest : public LinkedLocalStarTest {
protected:
    void SetUp() override {
        LinkedLocalStarTest::SetUp();
        localPlaylists = &playlists;
        indexStore.playlists = {{firstKey, "p1", "First server list", 0, 0},
                                {secondKey, "p2", "Second server list", 0, 0}};
    }
    FakeLocalPlaylists playlists;
};

TEST_F(LinkedLocalPlaylistTest, AddsOnlyToServersWithAReferenceForTheCurrentLocalTrack) {
    seed({LibraryIndex::seenRef(firstKey, SeenKind::Album, "album"),
          LibraryIndex::seenRef(firstKey, SeenKind::Track, "server-song")});
    playLocal();
    press(Button::Ok);
    open("Add to playlist");
    EXPECT_EQ(labels(), (std::vector<std::string>{"First server list"}));
    open("First server list");
    EXPECT_EQ(server.lastRequest().kind, Kind::AddPlaylistTrack);
    EXPECT_EQ(server.lastRequest().trackIds, (std::vector<std::string>{"server-song"}));
    ASSERT_TRUE(server.replyCreated(server.lastRequest().id)); app->tick();
    EXPECT_EQ(player.file, path);
}

TEST_F(PlaylistEditingTest, UnsupportedServerIsNotAPlaylistDestination) {
    server.playlistEditingSupported = false;
    start();
    open("Music"); open("Playlists"); open("Create playlist");
    EXPECT_EQ(labels(), (std::vector<std::string>{"On the device"}));
}

TEST_F(DownloadedPlaylistEditingTest, InFlightCatalogueCannotResurrectADeletedPlaylist) {
    indexPlaylist("p1", "Road Trip", 1);
    openPlaylists();
    open("Road Trip");
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "Sunrise"}})); app->tick();
    open("Delete playlist"); open("Yes, delete");
    ASSERT_TRUE(server.replyCreated(server.lastRequest().id)); app->tick();
    EXPECT_FALSE(hasLabel("Road Trip"));
    syncWith({{"p1", "Road Trip", 1}}); // an older response was already in flight
    press(Button::Home); open("Playlists");
    EXPECT_FALSE(hasLabel("Road Trip"));
}

TEST_F(PlaylistEditingTest, ServerCreateFinishingUnderPowerMenuUpdatesIndexSilently) {
    start();
    open("Music"); open("Playlists"); open("Create playlist"); open("Server");
    auto request = server.lastRequest();
    hold(Button::Power);
    ASSERT_EQ(level().title, "Power");
    const auto count = speech.said.size();
    ASSERT_TRUE(server.replyCreated(request.id, "new")); app->tick();
    EXPECT_EQ(level().title, "Power");
    EXPECT_EQ(speech.said.size(), count);
    auto found = std::find_if(indexStore.playlists.begin(), indexStore.playlists.end(), [](const IndexPlaylist& p) { return p.id == "new"; });
    EXPECT_NE(found, indexStore.playlists.end());
    press(Button::Home);
    EXPECT_EQ(level().items[level().index].label, "Playlist created");
    press(Button::Ok);
    EXPECT_EQ(level().title, "Playlists");
}

TEST_F(DownloadedPlaylistEditingTest, RefreshingEditedUniquePlaylistRestoresOfflineSnapshot) {
    indexPlaylist("p1", "Road Trip", 2);
    seedRoad();
    openPlaylists(); open("Road Trip");
    while (level().items[level().index].label != "Highway") press(Button::Forward);
    press(Button::Options); open("Remove from playlist");
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "Sunrise"}, {"t2", "Highway"}})); app->tick();
    open("Highway");
    ASSERT_TRUE(server.replyCreated(server.lastRequest().id)); app->tick();
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "Sunrise"}})); app->tick();
    open("Download playlist");
    EXPECT_EQ(speech.last(), "Already downloaded.");
    press(Button::Home); // the playlist list
    auto asked = requestsOf(Kind::PlaylistTracks);
    open("Road Trip");
    EXPECT_EQ(requestsOf(Kind::PlaylistTracks), asked);
    EXPECT_TRUE(hasLabel("Sunrise"));
    EXPECT_FALSE(hasLabel("Highway"));
}

TEST_F(DownloadedPlaylistEditingTest, EditingWhileDownloadRunsKeepsItsSnapshotStale) {
    indexPlaylist("p1", "Road Trip", 2);
    seedRoad();
    openPlaylists(); open("Road Trip");
    while (level().items[level().index].label != "Highway") press(Button::Forward);
    press(Button::Options); open("Remove from playlist");
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "Sunrise"}, {"t2", "Highway"}})); app->tick();
    open("Highway"); ASSERT_TRUE(server.replyCreated(server.lastRequest().id)); app->tick();
    std::vector<RemoteTrack> revised{{"t1", "Sunrise", "", "", 1, 1, "mp3", 5}, {"t3", "New", "", "", 1, 2, "mp3", 5}};
    ASSERT_TRUE(server.reply(server.lastRequest().id, revised)); app->tick();
    open("Download playlist");
    ASSERT_FALSE(downloader.requests.empty());
    ASSERT_TRUE(app->playRemote("Road Trip", revised, 0, playlistSource("p1")));
    press(Button::Ok); open("Add to playlist"); open("Road Trip");
    ASSERT_TRUE(server.replyCreated(server.lastRequest().id)); app->tick();
    finishLast();
    press(Button::Home); // player
    press(Button::Home); // playlist page
    press(Button::Home); // playlist list
    open("Road Trip");
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
    EXPECT_TRUE(hasLabel("Loading"));
}

TEST_F(DownloadedPlaylistEditingTest, ShutdownCancelsPendingRemovalListFetch) {
    indexPlaylist("p1", "Road Trip", 2);
    seedRoad(); openPlaylists(); open("Road Trip");
    while (level().items[level().index].label != "Highway") press(Button::Forward);
    press(Button::Options); open("Remove from playlist");
    const auto request = server.lastRequest().id;
    app->shutdown();
    EXPECT_FALSE(server.reply(request, std::vector<RemoteTrack>{{"t2", "Highway"}}));
}

TEST_F(DownloadedPlaylistEditingTest, InFlightCatalogueCannotDropAnEditedPlaylist) {
    indexPlaylist("p1", "Road Trip", 1);
    openPlaylists(); open("Road Trip");
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "Sunrise"}})); app->tick();
    open("Sunrise"); press(Button::Ok); open("Add to playlist"); open("Road Trip");
    ASSERT_TRUE(server.replyCreated(server.lastRequest().id)); app->tick();
    syncWith({}); // an older reply did not yet include the playlist
    press(Button::Home); press(Button::Home); press(Button::Home);
    EXPECT_TRUE(hasLabel("Road Trip"));
}

TEST_F(DownloadedPlaylistEditingTest, RemovalFinishingUnderPowerMenuRefreshesSilently) {
    indexPlaylist("p1", "Road Trip", 2); seedRoad();
    openPlaylists(); open("Road Trip");
    while (level().items[level().index].label != "Highway") press(Button::Forward);
    press(Button::Options); open("Remove from playlist");
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "Sunrise"}, {"t2", "Highway"}})); app->tick();
    open("Highway"); auto remove = server.lastRequest().id;
    hold(Button::Power); ASSERT_EQ(level().title, "Power");
    const auto spoken = speech.said.size();
    ASSERT_TRUE(server.replyCreated(remove)); app->tick();
    EXPECT_EQ(level().title, "Power"); EXPECT_EQ(speech.said.size(), spoken);
    EXPECT_EQ(server.lastRequest().kind, Kind::PlaylistTracks);
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteTrack>{{"t1", "Sunrise"}})); app->tick();
    press(Button::Home);
    EXPECT_EQ(level().items[level().index].label, "Removed from playlist");
    press(Button::Ok); press(Button::Home);
    EXPECT_TRUE(hasLabel("Sunrise")); EXPECT_FALSE(hasLabel("Highway"));
}
