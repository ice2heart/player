#include "core/app.h"
#include "core/queue_file.h"
#include "core/track_cache.h"
#include "fakes.h"
#include "frontend/threaded_scanner.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <memory>
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

    // Starts the App and, unless `scan` is false, lets its background scan finish.
    void start(bool scan = true) {
        app = std::make_unique<App>(speech, player, sounds, volume, server, downloader, indexStore, scanner, settings, state, clock, root, music, cache, textFiles, queueFile);
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
            press(Button::Down);
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
    FakeDownloader downloader;
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

TEST_F(AppTest, UpAndDownMoveThroughTheMenuAndWrap) {
    start();
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Music, 2 of 7");
    EXPECT_EQ(sounds.last(), Earcon::Move);
    press(Button::Up);
    press(Button::Up);
    EXPECT_EQ(speech.last(), "Help, 7 of 7");
    EXPECT_EQ(sounds.last(), Earcon::Wrap);
}

TEST_F(AppTest, NoWrapWhenWrapAroundIsOff) {
    settings.setInt("menu.wrap", 0);
    start();
    press(Button::Up);
    EXPECT_EQ(level().index, 0u);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Library, 1 of 7");
}

TEST_F(AppTest, LongPressDownJumpsToLastItemOnce) {
    start();
    hold(Button::Down);
    EXPECT_EQ(level().index, 6u);
    EXPECT_EQ(speech.last(), "Help, 7 of 7");
    const auto& usage = app->buttonUsage()[size_t(Button::Down)];
    EXPECT_EQ(usage[size_t(Gesture::Long)], 1);
    EXPECT_EQ(usage[size_t(Gesture::Short)], 0);
}

TEST_F(AppTest, EveryKeyInterruptsSpeech) {
    start();
    int before = speech.stops;
    press(Button::Down);
    EXPECT_EQ(speech.stops, before + 1);
}

TEST_F(AppTest, ExpertVerbosityLeavesOutThePosition) {
    settings.setInt("verbosity", 2);
    start();
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Music");
}

TEST_F(AppTest, BeginnerVerbosityAddsAHint) {
    settings.setInt("verbosity", 0);
    start();
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Music, 2 of 7. Press OK to open.");
}

TEST_F(AppTest, OkOpensSubmenuAndLongOkGoesBack) {
    start();
    press(Button::Ok);
    EXPECT_EQ(level().title, "Library");
    EXPECT_EQ(speech.last(), "Library. Alpha, 2 parts, 1 of 2");
    hold(Button::Ok);
    EXPECT_EQ(app->screens().size(), 1u);
    EXPECT_EQ(sounds.last(), Earcon::Close);
}

TEST_F(AppTest, ASettingOpensOnItsCurrentValue) {
    start();
    open("Settings");
    open("Volume");
    EXPECT_EQ(level().title, "Volume");
    EXPECT_EQ(speech.last(), "Volume. 70 percent, current, 15 of 21");
    press(Button::Down);
    EXPECT_EQ(speech.last(), "75 percent, 16 of 21");
}

TEST_F(AppTest, ChoosingAValueFromTheListSavesItAppliesItAndGoesBack) {
    start();
    open("Settings");
    open("Speech rate");
    press(Button::Down);
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
    press(Button::Down);
    press(Button::Home);
    EXPECT_EQ(level().title, "Settings");
    EXPECT_EQ(settings.getInt("volume", 70), 70);
    EXPECT_EQ(speech.last(), "Volume, 70 percent, 2 of 12");
}

TEST_F(AppTest, RightInSettingsChangesNoSetting) {
    start();
    open("Settings"); // Speech rate, then Volume, ... 12 items in all
    press(Button::Forward);
    press(Button::Down);
    press(Button::Back);
    press(Button::Forward);
    EXPECT_EQ(settings.getInt("speech.rate", 220), 220);
    EXPECT_EQ(settings.getInt("volume", 70), 70);
    EXPECT_EQ(speech.rate, 220);
    EXPECT_FLOAT_EQ(volume.volume, 0.7f);
    press(Button::Down);
    EXPECT_EQ(configFile.saves, 0) << "nothing was changed, so nothing is written";
    EXPECT_EQ(stateFile.saves, 0);
}

TEST_F(AppTest, ASettingIsOnlyChangedInItsList) {
    start();
    open("Settings"); // Speech rate
    press(Button::Forward);
    press(Button::Back);
    ASSERT_EQ(level().index, 0u) << "Left from 10 is back at the first item";
    press(Button::Ok);
    EXPECT_EQ(level().title, "Speech rate");
    press(Button::Down);
    press(Button::Ok);
    EXPECT_EQ(settings.getInt("speech.rate", 0), 250);
    EXPECT_EQ(speech.rate, 250);
    EXPECT_EQ(configFile.onDisk("speech.rate"), "250");
}

TEST_F(AppTest, RightInAListThatIsNotSortedByNameMovesTenItems) {
    start();
    open("Settings"); // 12 items
    press(Button::Forward);
    EXPECT_EQ(level().index, 10u);
    EXPECT_EQ(sounds.last(), Earcon::Move);
    EXPECT_EQ(speech.last(), "Cache size, 1 gigabyte, 11 of 12") << "no letter: the list is not sorted by name";
    press(Button::Forward);
    EXPECT_EQ(level().index, 11u) << "stops at the last item";
    EXPECT_EQ(sounds.last(), Earcon::Move);
    press(Button::Back);
    EXPECT_EQ(level().index, 1u);
    press(Button::Back);
    EXPECT_EQ(level().index, 0u) << "stops at the first item";
    EXPECT_EQ(sounds.last(), Earcon::Move);
}

TEST_F(AppTest, TheTenItemJumpHasEdgesAndWrapsWhenWrapAroundIsOn) {
    settings.setInt("menu.wrap", 0);
    start();
    open("Settings");
    press(Button::Back);
    EXPECT_EQ(level().index, 0u);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Speech rate, 220 words per minute, 1 of 12");
    hold(Button::Down); // last
    press(Button::Forward);
    EXPECT_EQ(level().index, 11u);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Sleep timer, off, 12 of 12");

    hold(Button::Up);
    open("Menu wrap around"); // turn wrapping on in the list
    press(Button::Down);
    press(Button::Ok);
    ASSERT_EQ(settings.getInt("menu.wrap", 0), 1);
    hold(Button::Down);
    press(Button::Forward);
    EXPECT_EQ(level().index, 0u) << "wrapped to the first item";
    EXPECT_EQ(sounds.last(), Earcon::Wrap);
    press(Button::Back);
    EXPECT_EQ(level().index, 11u) << "and back to the last";
    EXPECT_EQ(sounds.last(), Earcon::Wrap);
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
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(app->screens().size(), 1u);
}

TEST_F(AppTest, HoldingOkGoesBackLikeHome) {
    playAlpha();
    hold(Button::Ok);
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(level().title, "Library");
}

TEST_F(AppTest, ReturningToAListWithRepeatedLabelsKeepsThePlace) {
    touch(music / "Twins" / "01 - Song.mp3");
    touch(music / "Twins" / "02 - Song.mp3");
    start();
    open("Music");
    open("Albums A to Z");
    open("Twins");
    press(Button::Down);
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
    EXPECT_EQ(speech.last().rfind("This player has no screen. Up and Down move through a menu.", 0), 0u)
        << speech.last();
    EXPECT_NE(speech.last().find("Left and Right jump in a long list, to the next letter or ten items on."), std::string::npos)
        << speech.last();
    EXPECT_EQ(speech.last().find("change a setting"), std::string::npos) << "Left and Right no longer change settings";
}

TEST_F(AppTest, HelpButtonsDescribesEachButton) {
    start();
    open("Help");
    open("Buttons");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play pause", "Left", "Right", "Up", "Down", "OK", "Home",
                                                  "Volume up", "Volume down", "Options", "Talk", "Power"}));
    open("Left");
    EXPECT_EQ(speech.last(), "Left. In menus, jumps to the previous letter, or back 10 items. In player, moves back by the step unit. Hold, or press deeper, for previous chapter or track.");
    open("Right");
    EXPECT_EQ(speech.last(), "Right. In menus, jumps to the next letter, or forward 10 items. In player, moves forward by the step unit. Hold, or press deeper, for next chapter or track.");
    EXPECT_FALSE(app->keyDescriber());
}

TEST_F(AppTest, TalkSaysNotAvailable) {
    start();
    press(Button::Talk);
    EXPECT_EQ(speech.last(), "Voice input is not available yet.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
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

TEST_F(AppTest, SkipForwardAndBack) {
    playAlpha();
    player.pos = 100;
    press(Button::Forward);
    EXPECT_DOUBLE_EQ(player.pos, 130);
    press(Button::Back);
    EXPECT_DOUBLE_EQ(player.pos, 120);
}

TEST_F(AppTest, SkipBackClampsAtStart) {
    playAlpha();
    player.pos = 3;
    press(Button::Back);
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
}

TEST_F(AppTest, SkipWhilePausedIsAnnounced) {
    playAlpha();
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Paused.");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Forward 30 seconds.");
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

// ---------------------------------------------------------------- the step unit

TEST_F(AppTest, UpAndDownInAPlayerDoNotChangeTheVolume) {
    playAlpha();
    press(Button::Down);
    press(Button::Up);
    press(Button::Down);
    hold(Button::Down);
    EXPECT_EQ(settings.getInt("volume", -1), -1);
    EXPECT_FLOAT_EQ(volume.volume, 0.7f);
}

TEST_F(AppTest, SkipIsTheFirstUnitAndUpFromItPlaysTheEdgeSound) {
    playAlpha();
    player.pos = 100;
    press(Button::Up);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Skip");
    press(Button::Forward);
    EXPECT_DOUBLE_EQ(player.pos, 130) << "still the skip of Settings";
}

TEST_F(AppTest, DownFromSkipSaysTrackAndRightStartsTheNextTrack) {
    start();
    open("Music");
    open("Albums A to Z");
    open("Road Trip");
    open("Sunrise");
    player.pos = 40;
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Track");
    EXPECT_EQ(sounds.last(), Earcon::Move);
    press(Button::Forward);
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    EXPECT_EQ(speech.last(), "Highway");
    player.pos = 40;
    press(Button::Back);
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string()) << "well into it: back to its start";
    EXPECT_DOUBLE_EQ(player.pos, 0);
    player.pos = 0;
    press(Button::Back);
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
}

TEST_F(AppTest, UpGoesBackToThePreviousUnit) {
    start();
    open("Music");
    open("Albums A to Z");
    open("Road Trip");
    open("Sunrise");
    press(Button::Down);
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Album");
    press(Button::Up);
    EXPECT_EQ(speech.last(), "Track");
    EXPECT_EQ(sounds.last(), Earcon::Move);
}

TEST_F(AppTest, TheMusicUnitsEndAtAlbum) {
    start();
    open("Music");
    open("Albums A to Z");
    open("Road Trip");
    open("Sunrise");
    press(Button::Down);
    press(Button::Down);
    press(Button::Down);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Album") << "heard again, and not moved on";
    press(Button::Up);
    EXPECT_EQ(speech.last(), "Track");
}

TEST_F(AppTest, TheBookUnitsAreSkipChapterAndTenMinutes) {
    playAlpha();
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Chapter");
    press(Button::Down);
    EXPECT_EQ(speech.last(), "10 minutes");
    press(Button::Down);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "10 minutes");
}

TEST_F(AppTest, HoldingUpOrDownInAPlayerMovesOneUnitOnly) {
    playAlpha();
    hold(Button::Down);
    EXPECT_EQ(speech.last(), "Chapter") << "not a jump to the last unit";
    hold(Button::Up);
    EXPECT_EQ(speech.last(), "Skip");
}

TEST_F(AppTest, ChapterUnitMovesABookToTheNextPart) {
    playAlpha();
    press(Button::Down);
    press(Button::Forward);
    EXPECT_EQ(player.file, alphaFile(2));
    press(Button::Forward);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Last chapter.");
}

TEST_F(AppTest, TenMinutesMovesABookSixHundredSeconds) {
    playAlpha();
    player.length = 3000;
    player.pos = 100;
    press(Button::Down);
    press(Button::Down);
    press(Button::Forward);
    EXPECT_DOUBLE_EQ(player.pos, 700);
    press(Button::Back);
    EXPECT_DOUBLE_EQ(player.pos, 100);
    press(Button::Back);
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
}

TEST_F(AppTest, TenMinutesPastTheEndOfAFilePlaysTheEdgeSound) {
    playAlpha();
    player.length = 500;
    player.pos = 100;
    press(Button::Down);
    press(Button::Down);
    press(Button::Forward);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_LT(player.pos, 500);
}

TEST_F(AppTest, AnnouncesTenMinutesWhilePaused) {
    playAlpha();
    player.length = 3000;
    press(Button::Down);
    press(Button::Down);
    press(Button::PlayPause);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Forward 10 minutes.");
}

TEST_F(AppTest, AlbumUnitGoesToTheFirstTrackOfTheNextAlbumOfPlayAll) {
    start();
    open("Music");
    open("Play all");
    ASSERT_EQ(player.file, (music / "Lullaby.mp3").string());
    press(Button::Down);
    press(Button::Down);
    ASSERT_EQ(speech.last(), "Album");
    press(Button::Forward);
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    EXPECT_EQ(speech.last(), "Road Trip");
    EXPECT_EQ(sounds.last(), Earcon::Move);
    app->tick();
    press(Button::Forward);
    EXPECT_EQ(sounds.last(), Earcon::Edge) << "no album after the last";
    EXPECT_EQ(speech.last(), "Last album.");
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    press(Button::Back);
    EXPECT_EQ(player.file, (music / "Lullaby.mp3").string());
    press(Button::Back);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "First album.");
}

TEST_F(AppTest, AlbumUnitFromTheMiddleOfAnAlbumGoesToTheNextAlbumsFirstTrack) {
    start();
    open("Music");
    open("Play all");
    player.atEnd = true;
    app->tick(); // Sunrise
    player.atEnd = true;
    app->tick(); // Highway
    ASSERT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    press(Button::Down);
    press(Button::Down);
    press(Button::Back);
    EXPECT_EQ(player.file, (music / "Lullaby.mp3").string());
}

TEST_F(AppTest, AlbumUnitInAOneAlbumListPlaysTheEdgeSound) {
    start();
    open("Music");
    open("Albums A to Z");
    open("Road Trip");
    open("Sunrise");
    press(Button::Down);
    press(Button::Down);
    press(Button::Forward);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Last album.");
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    press(Button::Back);
    EXPECT_EQ(speech.last(), "First album.");
}

TEST_F(AppTest, DeepPressChangesTrackWhateverTheUnit) {
    playAlpha();
    press(Button::Down);
    press(Button::Down); // 10 minutes
    player.pos = 100;
    app->keyDown(Button::Forward);
    app->keyUp(Button::Forward);
    hold(Button::Forward);
    EXPECT_EQ(player.file, alphaFile(2)) << "a hold goes to the next part, not 600 s on";
}

TEST_F(AppTest, EachPlayerKeepsItsOwnUnit) {
    playAlpha();
    press(Button::Down); // book: Chapter
    hold(Button::Home);
    open("Music");
    open("Play all");
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Track") << "the music unit started at Skip";
    EXPECT_EQ(state.getString("unit.music"), "1");
    EXPECT_EQ(state.getString("unit.book"), "1");
}

TEST_F(AppTest, TheUnitSurvivesSleepAndARestart) {
    playAlpha();
    press(Button::Down);
    press(Button::Down); // book: 10 minutes
    press(Button::Power);
    EXPECT_EQ(stateFile.onDisk("unit.book"), "2");
    start(false);
    ASSERT_EQ(app->mode(), App::Mode::Player);
    player.length = 3000;
    player.pos = 100;
    press(Button::Forward);
    EXPECT_DOUBLE_EQ(player.pos, 700) << "the 10 minutes unit came back";
}

TEST_F(AppTest, ASavedUnitOutOfRangeIsTheNearestUnit) {
    state.setString("unit.book", "9");
    playAlpha();
    press(Button::Down);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "10 minutes");
    state.setString("unit.book", "-4");
    press(Button::Up);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Skip");
}

TEST_F(AppTest, NoUnitIsWrittenUntilOneIsChosen) {
    playAlpha();
    app->shutdown();
    EXPECT_EQ(stateFile.onDisk("unit.book"), "");
    EXPECT_EQ(stateFile.onDisk("unit.music"), "");
}

TEST_F(AppTest, KeyDescriberNamesTheStepUnitForUpAndDown) {
    start();
    open("Help");
    open("Buttons");
    open("Down");
    EXPECT_EQ(speech.last(), "Down. In menus, next item. Hold for last item. In player, next step unit: skip, track or chapter, album or ten minutes.");
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

TEST_F(AppTest, AKeyThatWokeTheDeviceAndNeverCameUpWorksAgainOnTheNextPress) {
    start();
    press(Button::Power);
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    app->keyDown(Button::VolumeUp); // wakes; the release is lost
    app->keyDown(Button::VolumeUp);
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
    EXPECT_NE(speech.last().find("Options shows what can be done"), std::string::npos) << speech.last();
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

TEST_F(AppTest, DeepPressJumpsToTheNextChapterAtOnce) {
    playAlpha();
    player.pos = 42;
    app->keyDown(Button::Forward); // first stage
    app->keyDown(Button::ForwardDeep);
    EXPECT_EQ(player.file, alphaFile(2)) << "no waiting for a long press";
    EXPECT_EQ(speech.last(), "Calm");
    clock.advance(700ms);
    app->tick();
    app->keyUp(Button::ForwardDeep);
    app->keyUp(Button::Forward);
    EXPECT_EQ(speech.last(), "Calm") << "releasing must not also skip or jump again";
    EXPECT_EQ(player.pos, 0);
}

TEST_F(AppTest, PushingDeeperAfterALongPressDoesNotJumpAgain) {
    start();
    open("Settings");
    app->keyDown(Button::Forward);
    clock.advance(700ms);
    app->tick(); // long press: one jump
    app->keyDown(Button::ForwardDeep);
    app->keyUp(Button::ForwardDeep);
    app->keyUp(Button::Forward);
    EXPECT_EQ(level().index, 10u) << "a second jump would have reached the last item";
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
    EXPECT_EQ(level().index, 10u);
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
    hold(Button::Back);
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_DOUBLE_EQ(player.pos, 0);
    EXPECT_EQ(speech.last(), "Calm");
    hold(Button::Back);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_EQ(speech.last(), "Storm");
    hold(Button::Back);
    EXPECT_EQ(speech.last(), "First chapter.");
    EXPECT_EQ(sounds.last(), Earcon::Edge);
}

TEST_F(AppTest, EmbeddedChaptersInASingleFileBook) {
    player.embedded = {{"Intro", 0}, {"Middle", 100}, {"End", 200}};
    start();
    press(Button::Ok);   // Library
    press(Button::Down); // Beta
    press(Button::Ok);
    ASSERT_EQ(player.file, beta().string());
    player.pos = 150;
    hold(Button::Forward);
    EXPECT_DOUBLE_EQ(player.pos, 200);
    EXPECT_EQ(speech.last(), "End");
    hold(Button::Forward);
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
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Road Trip, 2 tracks, 2 of 2");
    press(Button::Up);
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
    hold(Button::Forward);
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
    hold(Button::Down);
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
    press(Button::Down);
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
    press(Button::Down);
    press(Button::Ok);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(app->screens().back().kind, Screen::Kind::BookPlayer);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_DOUBLE_EQ(player.pos, 20);
    EXPECT_TRUE(player.playing());
    hold(Button::Home);
    EXPECT_EQ(valueOf(level().items[0]), "Alpha") << "the player used last comes first";
    EXPECT_EQ(labels()[1], "Continue");
    press(Button::Down);
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
    press(Button::Down);
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
    press(Button::Down);
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
    press(Button::Down);
    press(Button::Down);
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
    press(Button::Down);
    press(Button::Ok);
    EXPECT_EQ(player.file, (music / "Big Album CD2" / "b.mp3").string());
}

TEST_F(AppTest, AFolderOfUntaggedFilesIsOneAlbumWithTheNameOfTheFolder) {
    touch(music / "Mix" / "disc 1" / "x.mp3");
    touch(music / "Mix" / "disc 2" / "y.mp3");
    touch(music / "Mix" / "z.flac");
    start();
    open("Music");
    open("Albums A to Z");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Lullaby", "Mix", "Road Trip"}));
    press(Button::Down);
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
    hold(Button::Forward); // next track
    EXPECT_EQ(lastSaidAtOpen.back(), "Loading.");
    EXPECT_EQ(speech.last(), "Rain");
    player.unopenable = {"fake://stream/t3"};
    hold(Button::Forward); // fails: Rain is opened again on Play
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
    hold(Button::Forward);
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
    hold(Button::Forward);
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
    hold(Button::Forward); // fails: Storm Front stays current, closed
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
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "Rain");
    EXPECT_EQ(player.file, "fake://stream/t2");
    player.pos = 100;
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Music player. Weather, by Band. Track 2 of 3, Rain. 8 minutes 20 seconds left in track. Playing.");
    player.pos = 1;
    hold(Button::Back);
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
    hold(Button::Forward);
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
    hold(Button::Forward); // fails: Storm Front is to be opened again on Play
    hold(Button::Home);
    open("Library");
    open("Alpha");
    ASSERT_EQ(player.file, alphaFile(1));
    player.unopenable = {alphaFile(2)};
    hold(Button::Forward); // Calm cannot be opened: nothing is open now
    ASSERT_EQ(speech.last(), "Cannot open 02 - Calm.mp3");
    player.opened.clear();
    press(Button::PlayPause);
    EXPECT_TRUE(player.opened.empty()) << "no retry at the remote track's place";
}

TEST_F(AppTest, ChoosingTheCurrentRemoteTrackAfterAFailureOpensIt) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.unopenable = {"fake://stream/t2"};
    hold(Button::Forward); // fails: Storm Front stays the current track, closed
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
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "Rain");
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "Sunshine");
    finishOpen();
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1", "fake://stream/t2", "fake://stream/t3"}));
    EXPECT_EQ(player.file, "fake://stream/t3");
    EXPECT_TRUE(player.playing()) << "it was playing before the first skip";
}

TEST_F(AsyncOpenTest, PauseWhileATrackOpensKeepsItPausedOnceOpen) {
    playWeather();
    hold(Button::Forward); // Rain opens, to play
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
    hold(Button::Back);       // Storm Front opens, paused like Rain
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Loading.");
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_TRUE(player.playing());
}

// A skip or Play belongs to no menu: the user may go on browsing while it opens.
TEST_F(AsyncOpenTest, ASkipStillOpensWhenTheUserLeavesThePlayer) {
    playWeather();
    hold(Button::Forward);
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
    hold(Button::Forward); // Rain fails: Storm Front is opened again on Play
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
    hold(Button::Forward); // Rain opens
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
    hold(Button::Forward);
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
    hold(Button::Forward);    // Rain opens, paused
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
    hold(Button::Forward); // Rain
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
    for (int i = 0; i < 5; ++i) press(Button::Forward); // 150 s on
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
    hold(Button::Back); // more than 3 s in: back to its start
    ASSERT_EQ(player.file, "fake://stream/t1");
    ASSERT_DOUBLE_EQ(player.pos, 0);
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
    hold(Button::Forward); // Rain fails: Storm Front opens again on Play
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
    hold(Button::Back); // the start of Intro, the first chapter
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
    hold(Button::Back); // back to its start
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
    start();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Library", "Music", "Home server", "Podcasts", "Settings", "System",
                                                  "Help"}));
    open("Home server");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Recently added", "Albums A to Z", "Artists",
                                                  "Random albums", "Server status"}));
    EXPECT_TRUE(server.requests.empty()) << "nothing is asked before a list is opened";
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

TEST_F(StreamingTest, AlbumsAToZLoadsTheNextPageWhileTheServerHasMore) {
    openList("Albums A to Z");
    EXPECT_EQ(server.lastRequest().arg, "alphabeticalByName");
    EXPECT_EQ(server.lastRequest().size, 500);
    answer(someAlbums("A", 500), /*more=*/true);
    EXPECT_EQ(speech.last(), "A 1, Band, 1 of 500");
    ASSERT_EQ(requestsOf(Kind::Albums), 2u) << "the next page, in the background";
    EXPECT_EQ(server.lastRequest().arg, "alphabeticalByName");
    EXPECT_EQ(server.lastRequest().offset, 500);
    EXPECT_EQ(server.lastRequest().size, 500);
    press(Button::Down);
    speech.said.clear();
    answer(someAlbums("B", 1), /*more=*/false);
    EXPECT_TRUE(speech.said.empty()) << "a later page is added silently";
    EXPECT_EQ(labels().size(), 501u);
    EXPECT_EQ(labels().back(), "B 1");
    EXPECT_EQ(level().index, 1u) << "the user stays where they are";
    EXPECT_EQ(requestsOf(Kind::Albums), 2u) << "no more pages";
    hold(Button::Down);
    EXPECT_EQ(speech.last(), "B 1, Band, 501 of 501");
}

TEST_F(StreamingTest, TheNextPageStartsAfterWhatCame) {
    openList("Albums A to Z");
    answer(someAlbums("A", 2), /*more=*/true); // a server that sends shorter pages
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
    auto page2 = server.lastRequest().id;
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
    press(Button::Down);
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
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Server));
    speech.said.clear();
    app->tick();
    EXPECT_TRUE(speech.said.empty()) << "the user is on a loaded item";
    EXPECT_EQ(labels(), (std::vector<std::string>{"A 1", "A 2", "Server error", "Try again"}));
    hold(Button::Down);
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

TEST_F(StreamingTest, AServerThisPlayerCannotUseSaysSo) {
    server.isConfigured = false; // the front end could not make a server from it
    settings.setString("server.type", "audiobookshelf");
    settings.setString("server.url", "https://books.example");
    openServer();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Server not supported"}));
    press(Button::Ok);
    EXPECT_NE(speech.last().find("Subsonic"), std::string::npos) << speech.last();
    // Also what a build without network support says: it must not blame the type alone.
    EXPECT_NE(speech.last().find("network support"), std::string::npos) << speech.last();
    EXPECT_EQ(speech.last().find("https://"), std::string::npos) << "the address is not spoken";
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
    for (int i = 0; i < 4; ++i) press(Button::Down);
    EXPECT_EQ(speech.last(), "Server status, not reachable, 5 of 5");
    press(Button::Ok);
    ASSERT_TRUE(server.replyPing(server.lastRequest().id, {"navidrome", "0.64.2"}));
    app->tick();
    EXPECT_EQ(speech.last(), "Connected to Navidrome 0.64.2.");
    press(Button::Up);
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Server status, 5 of 5") << "the answer cleared the mark";
}

TEST_F(StreamingTest, ServerStatusStaysNotReachableWhenThePingDoesNotAnswer) {
    start();
    player.unopenable = {"fake://stream/t1"};
    EXPECT_FALSE(app->playRemote("Weather", weather(), 0));
    press(Button::Home);
    open("Server");
    for (int i = 0; i < 4; ++i) press(Button::Down);
    press(Button::Ok);
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable));
    app->tick();
    EXPECT_EQ(speech.last(), "Server not reachable.");
    press(Button::Up);
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Server status, not reachable, 5 of 5");
}

TEST_F(StreamingTest, ServerStatusSaysNothingExtraWhileTheServerIsNotMarkedDown) {
    openServer();
    for (int i = 0; i < 4; ++i) press(Button::Down);
    EXPECT_EQ(speech.last(), "Server status, 5 of 5");
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
    press(Button::Down);
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
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Library, 2 of 8");
    press(Button::Up);
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
    press(Button::Up); // the second Rain, the last item
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
    hold(Button::Forward);
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
    press(Button::Down);
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
    press(Button::Down);
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
    press(Button::Down);
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
    press(Button::Down);
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
    EXPECT_TRUE(speech.heard("2 tracks, source unavailable.")) << "skipped, as in any queue";
    EXPECT_TRUE(player.opened.empty());
}

TEST_F(ResumeStreamTest, AQueueOfAnotherUserIsKeptButItsTracksAreUnavailable) {
    playFromAlbum("Rain");
    reboot([this] { settings.setString("server.user", "guest"); });
    EXPECT_EQ(labels().front(), "Now playing");
    press(Button::Ok);
    EXPECT_TRUE(speech.heard("2 tracks, source unavailable."));
    EXPECT_TRUE(player.opened.empty());
}

TEST_F(ResumeStreamTest, WithoutAServerTheQueueIsKeptButItsTracksAreUnavailable) {
    playFromAlbum("Rain");
    reboot([this] { server.isConfigured = false; });
    EXPECT_EQ(labels().front(), "Now playing");
    press(Button::Ok);
    EXPECT_TRUE(speech.heard("2 tracks, source unavailable."));
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
    hold(Button::Forward);    // a track change
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
    hold(Button::Forward);
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

// ---------------------------------------------------------------- hold switch and power

TEST_F(AppTest, HoldSwitchBlocksEverythingButPower) {
    start();
    app->setHold(true);
    EXPECT_EQ(speech.last(), "Buttons locked.");
    press(Button::Down);
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
    EXPECT_DOUBLE_EQ(player.pos, 130) << "works again once let go";
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
    press(Button::Down); // Sleep timer
    press(Button::Ok);
    press(Button::Down); // 1 minute
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
    hold(Button::Down);
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
    hold(Button::Down);
    press(Button::Ok); // Sleep timer
    EXPECT_EQ(labels(), (std::vector<std::string>{"Off", "1 minute", "15 minutes", "30 minutes", "45 minutes", "1 hour"}));
    hold(Button::Home);
    open("Library");
    open("Alpha");
    hold(Button::Home);
    open("Settings");
    hold(Button::Down);
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
    press(Button::Down); // Power off
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
    press(Button::Down);
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
    press(Button::Forward); // 30 s on would be past the end of Rain's 180 s
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_DOUBLE_EQ(player.pos, 179);
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
    app = std::make_unique<App>(speech, player, sounds, volume, server, downloader, indexStore, scanner, reloaded, state, clock, root, music);
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

TEST_F(DownloadTest, AlbumAndPlaylistOfferDownloadRightAfterPlay) {
    indexPlaylist("p1", "Road", 3);
    openAlbum();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play album", "Download album", "Storm Front", "Rain", "Sunshine"}));
    hold(Button::Home);
    open("Music");
    open("Playlists");
    open("Road");
    answer(sized());
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play playlist", "Download playlist", "Storm Front", "Rain", "Sunshine"}));
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
    app = std::make_unique<App>(speech, player, sounds, volume, server, downloader, indexStore, scanner, settings, state, clock, root, music, cache);
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
    press(Button::Down);
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

TEST_F(DownloadedTest, TheServerMenuOffersDownloadedOnlyWithACacheFolder) {
    openServer();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Recently added", "Albums A to Z", "Artists",
                                                  "Random albums", "Downloaded", "Server status"}));
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
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Mixed, Band, 2 of 4");
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Road songs, playlist, 3 of 4");
    press(Button::Down);
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
    press(Button::Down);
    press(Button::Home);
    EXPECT_TRUE(fs::exists(store().listFolder(al) / "t1.mp3"));
}

TEST_F(DownloadedTest, ConfirmingDeletesTheFolderAndTheListUpdates) {
    RemoteSource al = seedWeather();
    seed(albumSource("al2"), "Zodiac", "Band", {{"z1", "Fish"}});
    openDownload("Weather");
    open("Delete download");
    press(Button::Down);
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
    press(Button::Down);
    press(Button::Ok);
    EXPECT_EQ(labels(), (std::vector<std::string>{"No downloads"}));
    EXPECT_EQ(speech.last(), "Deleted Weather. No downloads, 1 of 1");
}

TEST_F(DownloadedTest, DeletingAPlaylistKeepsTheAlbumWithTheSameId) {
    seed(albumSource("7"), "Album seven", "Band", {{"a1", "One"}});
    RemoteSource pl = seed(playlistSource("7"), "Playlist seven", "", {{"p1", "Two"}});
    openDownload("Playlist seven");
    open("Delete download");
    press(Button::Down);
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
    press(Button::Down);
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
    press(Button::Down);
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
    press(Button::Down);
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
    press(Button::Down);
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
    press(Button::Down);
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
    press(Button::Down);
    press(Button::Down);
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
    press(Button::Down);
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
    press(Button::Down); // another screen or item: the user has moved on
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
    press(Button::Down);
    press(Button::Down);
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
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play playlist", "Delete download", "Sunrise", "Highway"}));
    EXPECT_FALSE(speech.heard("Loading."));
    open("Highway");
    EXPECT_EQ(player.file, fileOf(pl, "t2"));
    EXPECT_FALSE(anyStreamOpened());
    EXPECT_EQ(server.requests.size(), asked) << "the server is never asked";
    EXPECT_FALSE(spokeUnreachable());
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
    press(Button::Down);
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

TEST_F(MusicPlaylistsTest, LeftAndRightJumpByLetter) {
    indexPlaylist("p1", "Alpha", 1);
    indexPlaylist("p2", "Beta", 1);
    indexPlaylist("p3", "Bravo", 1);
    indexPlaylist("p4", "Delta", 1);
    openPlaylists();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "B. Beta, 1 track, 2 of 4");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "D. Delta, 1 track, 4 of 4");
    press(Button::Back);
    EXPECT_EQ(speech.last(), "B. Beta, 1 track, 2 of 4");
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
    hold(Button::Forward); // Rain
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

TEST_F(AutoCacheTest, ASkippedTrackIsNotFetched) {
    playSized();
    listen(5s);
    hold(Button::Forward);
    listen(5s);
    hold(Button::Forward);
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
    hold(Button::Forward);
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
    hold(Button::Forward);
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
    hold(Button::Forward); // Storm Front is not the open one any more
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
    hold(Button::Forward);
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
    hold(Button::Forward);
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

TEST_F(AutoCacheLimitTest, TheFolderOfAnAbandonedTransferGoesWithItsPartWhenOverTheLimit) {
    startSmall(5);
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    ASSERT_TRUE(downloader.finish(downloader.lastRequest().id, Result::Unreachable, true, "", "halfhalf"));
    app->tick(); // failed: the .part is left, nobody resumes it
    ASSERT_TRUE(fs::exists(weatherFolderFile("t1.flac.part")));
    hold(Button::Forward); // the play moves on
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
    app = std::make_unique<App>(speech, player, sounds, volume, server, downloader, indexStore, scanner, settings, state, clock, root, music, cache, &counting);
    app->start();
    ASSERT_TRUE(app->playRemote("Weather", sized(), 0, albumSource("al1"), 0, "Band"));
    listen(11s);
    int before = counting.reads;
    finishLast();
    int first = counting.reads - before;
    EXPECT_GE(first, 4) << "its own list (read to load and to save) and a scan of the two others";
    hold(Button::Forward);
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
    hold(Button::Forward);
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
    hold(Button::Forward);
    listen(11s);
    ASSERT_TRUE(downloader.finish(downloader.lastRequest().id, Result::Unreachable, true, "", "halfhalf"));
    app->tick();
    hold(Button::Forward); // nothing waits for t2's part any more
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
    app = std::make_unique<App>(speech, player, sounds, volume, server, downloader, indexStore, scanner, settings, state, clock, root, music, cache, &counting);
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
    app = std::make_unique<App>(speech, player, sounds, volume, server, downloader, indexStore, scanner, settings, state, clock, root, music, cache, &counting);
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
    app = std::make_unique<App>(speech, player, sounds, volume, server, downloader, indexStore, scanner, reloaded, state, clock, root, music, cache, &diskFiles());
    app->start();
    open("Settings");
    open("Cache size");
    EXPECT_EQ(level().index, 2u);
    open("off");
    app->shutdown();
    app.reset();
    Settings off{&configFile};
    off.load();
    app = std::make_unique<App>(speech, player, sounds, volume, server, downloader, indexStore, scanner, off, state, clock, root, music, cache, &diskFiles());
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
        state.setString("catalogue.server", key());
        state.setString("catalogue.version", version);
    }

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
    EXPECT_EQ(state.getString("catalogue.version"), "v2");
    EXPECT_EQ(state.getString("catalogue.server"), key());
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
    EXPECT_EQ(state.getString("catalogue.version"), "");
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
    EXPECT_EQ(state.getString("catalogue.version"), "v1") << "the next start tries again";
}

TEST_F(CatalogueTest, ASyncThatChangedNothingWritesNothing) {
    start();
    answerVersion("v1");
    answerAlbums({album("a1", "One")});
    app->shutdown();
    app.reset();
    const int saves = indexStore.saves;
    start(); // the version is stored in state, which the test keeps in memory
    state.setString("catalogue.version", "other"); // a changed version, the same albums
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
    EXPECT_EQ(state.getString("catalogue.version"), "");
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
    EXPECT_EQ(state.getString("catalogue.version"), "v1") << "or every start would fetch all the albums again";
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
    seedVersion("v1");
    state.setString("catalogue.server", key("former"));
    start();
    answerVersion("v1");
    EXPECT_EQ(countOf(Kind::Albums), 1u);
}

TEST_F(CatalogueTest, ANewServerOnTheSameIndexIsSyncedFromScratch) {
    seedServerAlbum("al1", "Of the old server", TrackCache::serverKey("https://old.example", "ice"));
    seedVersion("v1"); // the stored version belongs to the old key below
    state.setString("catalogue.server", TrackCache::serverKey("https://old.example", "ice"));
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
        while (level().items[level().index].label != name) press(Button::Down);
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
    for (int i = 0; i < 3; ++i) press(Button::Down);
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
    press(Button::Up); // the last track
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
    press(Button::Down);
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
    press(Button::Down);
    EXPECT_NE(speech.last().find(", 3 albums, 2 of 2"), std::string::npos) << speech.last();
    press(Button::Ok);
    EXPECT_EQ(labels(), (std::vector<std::string>{"Blues Night", "Road Trip", "Weather"})) << "the local album and two server albums";
    press(Button::Down); // Road Trip: on the device, it opens at once
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
    press(Button::Down);
    press(Button::Down);
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
    EXPECT_EQ(speech.last(), "Library updated, 3 albums.") << "Lullaby, Road Trip and Weather, not the book";
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
    EXPECT_EQ(state.values().count("catalogue.folder"), 0u) << "nothing new in the state";
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
    EXPECT_EQ(state.getString("catalogue.version"), "") << "not remembered: the next start asks again, in case the name was a typo";
    EXPECT_EQ(state.values().count("catalogue.folder"), 0u);
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
    EXPECT_EQ(state.getString("catalogue.version"), "");
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
    state.setString("catalogue.folder", "Books");
    start();
    answerVersion("v1");
    EXPECT_EQ(countOf(Kind::MusicFolders), 0u);
    EXPECT_EQ(countOf(Kind::Albums), 0u);
}

TEST_F(ServerBookTest, TheFolderIsRememberedInTheStateWithTheVersion) {
    start();
    syncWithFolder({dune()}, {dune()});
    EXPECT_EQ(state.getString("catalogue.folder"), "Books");
    EXPECT_EQ(state.getString("catalogue.version"), "v1");
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
    hold(Button::Forward);
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
    hold(Button::Forward);
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
    hold(Button::Forward);
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
    hold(Button::Forward);
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
    hold(Button::Forward);
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
        hold(Button::Forward);
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
    press(Button::Down);
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
// Road Trip, Zeta. Its letters: # (7 Seconds), A (2), B, D (2), L, R, Z.
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
        hold(Button::Up);
        for (size_t i = 0; i < level().items.size() && level().items[level().index].label != label; ++i) press(Button::Down);
        ASSERT_EQ(level().items[level().index].label, label);
    }
};

TEST_F(MenuJumpTest, RightGoesToTheFirstItemOfTheNextLetterAndSpeaksIt) {
    openAlbums();
    goTo("Abba Gold");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "B. Beta, 1 track, 4 of 9");
    EXPECT_EQ(sounds.last(), Earcon::Move);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "D. Dune, 1 track, 5 of 9");
    press(Button::Forward);
    EXPECT_EQ(speech.last().rfind("L. Lullaby", 0), 0u) << "Dusk is the same letter as Dune: " << speech.last();
}

TEST_F(MenuJumpTest, RightFromTheMiddleOfALetterGoesToTheNextOne) {
    openAlbums();
    goTo("Aria");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "B. Beta, 1 track, 4 of 9");
    goTo("Dusk");
    press(Button::Forward);
    EXPECT_EQ(speech.last().rfind("L. Lullaby", 0), 0u) << speech.last();
}

TEST_F(MenuJumpTest, LeftGoesToTheFirstItemOfThePreviousLetterAlsoFromTheMiddleOfOne) {
    openAlbums();
    goTo("Dusk"); // the middle of D
    press(Button::Back);
    EXPECT_EQ(speech.last(), "B. Beta, 1 track, 4 of 9") << "not Dune, the start of its own letter";
    EXPECT_EQ(sounds.last(), Earcon::Move);
    press(Button::Back);
    EXPECT_EQ(speech.last(), "A. Abba Gold, 1 track, 2 of 9") << "the first of A, not Aria";
    goTo("Dune");
    press(Button::Back);
    EXPECT_EQ(speech.last(), "B. Beta, 1 track, 4 of 9");
}

TEST_F(MenuJumpTest, DigitsAreAGroupBeforeTheLetters) {
    openAlbums();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "A. Abba Gold, 1 track, 2 of 9") << "from 7 Seconds";
    press(Button::Back);
    EXPECT_EQ(speech.last(), "Number. 7 Seconds, 1 track, 1 of 9");
}

TEST_F(MenuJumpTest, TheEndsPlayTheEdgeSoundWhenWrappingIsOff) {
    settings.setInt("menu.wrap", 0);
    openAlbums();
    press(Button::Back);
    EXPECT_EQ(level().index, 0u);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "7 Seconds, 1 track, 1 of 9");
    goTo("Zeta");
    press(Button::Forward);
    EXPECT_EQ(level().index, 8u);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Zeta, 1 track, 9 of 9");
    goTo("Road Trip"); // the second to last letter: not an edge
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Z. Zeta, 1 track, 9 of 9");
    EXPECT_EQ(sounds.last(), Earcon::Move);
    goTo("Abba Gold");
    press(Button::Back);
    EXPECT_EQ(speech.last(), "Number. 7 Seconds, 1 track, 1 of 9") << "the first letter is not an edge";
    EXPECT_EQ(sounds.last(), Earcon::Move);
}

TEST_F(MenuJumpTest, WrapAroundWrapsTheJump) {
    openAlbums(); // menu.wrap is on by default
    goTo("Zeta");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Number. 7 Seconds, 1 track, 1 of 9");
    EXPECT_EQ(sounds.last(), Earcon::Wrap);
    press(Button::Back);
    EXPECT_EQ(speech.last(), "Z. Zeta, 1 track, 9 of 9") << "Left from the first letter goes to the first item of the last letter";
    EXPECT_EQ(sounds.last(), Earcon::Wrap);
}

TEST_F(MenuJumpTest, ALastLetterWithSeveralItemsWrapsToItsFirst) {
    localAlbum("Zulu", "Band", "Zulu");
    openAlbums();
    goTo("Abba Gold");
    press(Button::Back); // 7 Seconds
    press(Button::Back); // wraps
    EXPECT_EQ(speech.last(), "Z. Zeta, 1 track, 9 of 10") << "the first of Z, not Zulu";
}

TEST_F(MenuJumpTest, HoldingOrPressingDeeperJumpsOnceAndDoesNotRepeat) {
    openAlbums();
    goTo("Abba Gold");
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "B. Beta, 1 track, 4 of 9");
    size_t said = speech.said.size();
    app->keyDown(Button::Forward);
    app->keyDown(Button::ForwardDeep);
    clock.advance(5s);
    app->tick();
    app->keyUp(Button::ForwardDeep);
    app->keyUp(Button::Forward);
    EXPECT_EQ(speech.said.size(), said + 1);
    EXPECT_EQ(speech.last(), "D. Dune, 1 track, 5 of 9");
}

TEST_F(MenuJumpTest, ExpertVerbosityLeavesOutThePositionButKeepsTheLetter) {
    settings.setInt("verbosity", 2);
    openAlbums();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "A. Abba Gold, 1 track");
}

TEST_F(MenuJumpTest, TheLibraryJumpsByLetter) {
    touch(root / "Dune" / "01.mp3");
    touch(root / "Dust" / "01.mp3");
    start();
    open("Library");
    ASSERT_EQ(labels(), (std::vector<std::string>{"Alpha", "Beta", "Dune", "Dust"}));
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "B. Beta, 2 of 4");
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "D. Dune, 3 of 4");
}

TEST_F(MenuJumpTest, ArtistsJumpByLetterIgnoringArticlesWithOtherScriptsAndUnknownArtistAfterTheLatinOnes) {
    localAlbum("Abbey", "The Beatles", "Abbey");
    localAlbum("Ночь", "Агата Кристи", "Ночь");
    localAlbum("Другой", "Би-2", "Другой");
    localAlbum("Alone", "The Alpha", "Alone");
    start();
    open("Music");
    open("Artists");
    EXPECT_EQ(labels(), (std::vector<std::string>{"The Alpha", "Band", "The Beatles", "Агата Кристи", "Би-2", "Unknown artist"}));
    press(Button::Forward);
    EXPECT_EQ(speech.last().rfind("B. Band", 0), 0u) << "The Alpha is under A: " << speech.last();
    press(Button::Forward);
    EXPECT_EQ(speech.last().rfind("\xD0\xB0. \xD0\x90", 0), 0u) << "the Cyrillic group after B, The Beatles skipped: " << speech.last();
    press(Button::Forward);
    EXPECT_EQ(speech.last().rfind("\xD0\xB1. \xD0\x91\xD0\xB8-2", 0), 0u) << speech.last();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Unknown artist, 2 albums, 6 of 6") << "spoken by name, with no letter";
    press(Button::Back);
    EXPECT_EQ(speech.last().rfind("\xD0\xB1. ", 0), 0u) << speech.last();
    press(Button::Forward);
    press(Button::Forward);
    EXPECT_EQ(sounds.last(), Earcon::Wrap);
    EXPECT_EQ(speech.last().rfind("A. The Alpha", 0), 0u) << speech.last();
}

TEST_F(MenuJumpTest, AnArtistNamedUnknownArtistHasItsLetterAndOnlyTheRealOneHasNone) {
    localAlbum("Odd", "Unknown artist", "Odd");
    start();
    open("Music");
    open("Artists");
    ASSERT_EQ(labels(), (std::vector<std::string>{"Band", "Unknown artist", "Unknown artist"}));
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "U. Unknown artist, 1 album, 2 of 3") << "tagged with that name: sorted and spoken like any artist";
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Unknown artist, 2 albums, 3 of 3") << "the albums with no artist: no letter";
}

TEST_F(MenuJumpTest, AnArtistsAlbumsAndTheGenresJumpByLetter) {
    for (const char* name : {"Dune", "Dusk", "Beta", "Zeta"}) {
        TrackTags t = tags.tags.at(music / name / "1.mp3");
        t.genre = std::string(name) == "Zeta" ? "Jazz" : "Rock";
        tags.tags[music / name / "1.mp3"] = t;
    }
    start();
    open("Music");
    open("Artists");
    open("Band");
    press(Button::Forward);
    EXPECT_EQ(speech.last().rfind("A. Abba Gold", 0), 0u) << "the albums of an artist are sorted by title: " << speech.last();
    press(Button::Home);
    press(Button::Home);
    open("Genres");
    ASSERT_EQ(labels(), (std::vector<std::string>{"Jazz", "Rock"}));
    press(Button::Forward);
    EXPECT_EQ(speech.last().rfind("R. Rock", 0), 0u) << "the genres are sorted by name: " << speech.last();
    press(Button::Ok);
    ASSERT_EQ(labels(), (std::vector<std::string>{"Beta", "Dune", "Dusk"}));
    press(Button::Forward);
    EXPECT_EQ(speech.last().rfind("D. Dune", 0), 0u) << speech.last();
    press(Button::Forward);
    EXPECT_EQ(sounds.last(), Earcon::Wrap) << "Dusk is the same letter as Dune";
    EXPECT_EQ(speech.last().rfind("B. Beta", 0), 0u) << speech.last();
}

TEST_F(MenuJumpTest, ARecentlyAddedListIsNotSortedByNameSoItJumpsByTen) {
    start();
    open("Music");
    open("Recently added");
    ASSERT_GT(level().items.size(), 1u);
    press(Button::Forward);
    EXPECT_EQ(level().index, level().items.size() - 1) << "9 albums: to the last, with no letter";
    EXPECT_EQ(speech.last().find(". "), std::string::npos) << speech.last();
}

TEST_F(MenuJumpTest, AServerListSortedByNameJumpsByLetterAndALongOneByTen) {
    start();
    open("Server");
    open("Albums A to Z");
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteAlbum>{album("1", "Apple"), album("2", "Avocado"), album("3", "Banana")}));
    app->tick();
    ASSERT_EQ(level().index, 0u);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "B. Banana, Band, 3 of 3");
    press(Button::Back);
    EXPECT_EQ(speech.last(), "A. Apple, Band, 1 of 3");
}

TEST_F(MenuJumpTest, ARemoteArtistsListJumpsByLetterIgnoringArticles) {
    start();
    open("Server");
    open("Artists");
    RemoteArtist a{"a", "Apple", 1}, b{"b", "The Avocado", 1}, c{"c", "Banana", 1};
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteArtist>{a, b, c}));
    app->tick();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "B. Banana, 1 album, 3 of 3") << "The Avocado is under A";
}

TEST_F(MenuJumpTest, TheStatusRowsOfAServerListThatFailedAreOutsideTheLetters) {
    start();
    open("Server");
    open("Albums A to Z");
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::vector<RemoteAlbum>{album("1", "Apple"), album("2", "Banana")}, /*more=*/true));
    app->tick();
    ASSERT_TRUE(server.fail(server.lastRequest().id, MusicServer::Error::Unreachable)); // the next page
    app->tick();
    ASSERT_EQ(labels(), (std::vector<std::string>{"Apple", "Banana", "Server not reachable", "Try again"}));
    press(Button::Forward);
    EXPECT_EQ(speech.last().rfind("B. Banana", 0), 0u) << speech.last();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Server not reachable, 3 of 4") << "no letter on a status row";
    press(Button::Forward);
    EXPECT_EQ(sounds.last(), Earcon::Wrap) << "the status rows are one group, the last";
    EXPECT_EQ(speech.last().rfind("A. Apple", 0), 0u) << speech.last();
    press(Button::Back);
    EXPECT_EQ(speech.last(), "Server not reachable, 3 of 4") << "wraps to the first of the last group";
    press(Button::Back);
    EXPECT_EQ(speech.last().rfind("B. Banana", 0), 0u) << speech.last();
}

TEST_F(MenuJumpTest, AServerListThatIsNotSortedByNameJumpsByTenWithNoLetter) {
    start();
    open("Server");
    open("Recently added");
    std::vector<RemoteAlbum> albums;
    for (int i = 1; i <= 25; ++i) albums.push_back(album(std::to_string(i), "Album " + std::to_string(i)));
    ASSERT_TRUE(server.reply(server.lastRequest().id, std::move(albums)));
    app->tick();
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Album 11, Band, 11 of 25");
    press(Button::Forward);
    press(Button::Forward);
    EXPECT_EQ(speech.last(), "Album 25, Band, 25 of 25") << "stops at the last item";
    press(Button::Back);
    EXPECT_EQ(speech.last(), "Album 15, Band, 15 of 25");
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
    // Down to the Album unit.
    void albumUnit() {
        press(Button::Down);
        press(Button::Down);
        ASSERT_EQ(speech.last(), "Album");
    }
};

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
    press(Button::Down);
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

TEST_F(MusicQueueTest, TheAlbumUnitGoesToTheFirstEntryOfTheNextAlbumInTheQueue) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm", "Alpha"), localEntry(alphaFile(2), "Calm", "Alpha"),
                                         remoteEntry("t1", "Rain", "Weather"), remoteEntry("t2", "Snow", "Weather"),
                                         localEntry((roadTrip() / "01 - Sunrise.mp3").string(), "Sunrise", "Road Trip")}));
    albumUnit();
    press(Button::Forward);
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t1") << "the first of Weather";
    EXPECT_EQ(speech.last(), "Weather");
    EXPECT_EQ(sounds.last(), Earcon::Move);
    trackEnds(); // Snow, in the middle of Weather
    finishOpen();
    ASSERT_EQ(player.file, "fake://stream/t2");
    press(Button::Forward);
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    EXPECT_EQ(speech.last(), "Road Trip");
    press(Button::Forward);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Last album.");
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    press(Button::Back);
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/t1") << "the first of the album before";
    EXPECT_EQ(speech.last(), "Weather");
    trackEnds(); // Snow again
    finishOpen();
    press(Button::Back);
    EXPECT_EQ(player.file, alphaFile(1)) << "from the middle of Weather: the first of Alpha";
    EXPECT_EQ(speech.last(), "Alpha");
    trackEnds(); // Calm, in the middle of the first album
    ASSERT_EQ(player.file, alphaFile(2));
    press(Button::Back);
    EXPECT_EQ(sounds.last(), Earcon::Edge) << "no album before the first, even from its middle";
    EXPECT_EQ(speech.last(), "First album.");
    EXPECT_EQ(player.file, alphaFile(2));
}

TEST_F(MusicQueueTest, TheAlbumUnitOfAPlaylistMovesBetweenTheAlbumsOfItsTracks) {
    start_();
    std::vector<RemoteTrack> playlist = {{"p1", "One", "Band", "Red", 100, 1},
                                         {"p2", "Two", "Band", "Red", 100, 2},
                                         {"p3", "Three", "Band", "Blue", 100, 3}};
    ASSERT_TRUE(app->playRemote("Mix", playlist, 0));
    finishOpen();
    albumUnit();
    press(Button::Forward);
    finishOpen();
    EXPECT_EQ(player.file, "fake://stream/p3");
    EXPECT_EQ(speech.last(), "Blue");
}

TEST_F(MusicQueueTest, AServerAlbumIsOneAlbumWhateverItsTracksSayTheirAlbumIs) {
    start_();
    std::vector<RemoteTrack> tracks = {{"d1", "One", "Band", "Weather", 100, 1}, {"d2", "Two", "Band", "Weather (Disc 2)", 100, 2}};
    ASSERT_TRUE(app->playRemote("Weather", tracks, 0, RemoteSource{RemoteSource::Kind::Album, "al1"}));
    finishOpen();
    albumUnit();
    press(Button::Forward);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Last album.");
    EXPECT_EQ(player.file, "fake://stream/d1");
}

TEST_F(MusicQueueTest, LocalFilesThatCannotBeOpenedLeaveTheServerTrackThatWasPlayingForPlay) {
    start_();
    player.unopenable = {alphaFile(2), (roadTrip() / "01 - Sunrise.mp3").string()};
    ASSERT_TRUE(app->playQueue("Mixed", {remoteEntry("t2", "Rain"), localEntry(alphaFile(2), "Calm"),
                                         localEntry((roadTrip() / "01 - Sunrise.mp3").string(), "Sunrise")}));
    finishOpen();
    ASSERT_EQ(app->nowPlaying().chapter, "Rain");
    player.pos = 42;
    hold(Button::Forward); // Calm: cannot be opened; neither can Sunrise
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
    press(Button::Down);
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
    EXPECT_EQ(speech.last(), "Rain, source unavailable.");
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_EQ(std::count(player.opened.begin(), player.opened.end(), "fake://stream/t2"), 0) << "not even tried";
    EXPECT_TRUE(server.requests.empty() || pings() == 0) << "another server is not a failure of this one";
}

TEST_F(UnavailableTest, ATrackOfAnotherUserOfTheServerIsSkipped) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"),
                                         ofServer("https://music.example", "bob", "t2", "Rain"), localEntry(alphaFile(2), "Calm")}));
    trackEnds();
    EXPECT_EQ(speech.last(), "Rain, source unavailable.");
    EXPECT_EQ(player.file, alphaFile(2));
}

TEST_F(UnavailableTest, ServerTracksAreSkippedWhenNoServerIsSetUp) {
    start_();
    server.isConfigured = false;
    ASSERT_TRUE(app->playQueue("Mixed", mixed()));
    trackEnds();
    EXPECT_EQ(speech.last(), "Rain, source unavailable.");
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
    EXPECT_EQ(said[said.size() - 2], "Rain, source unavailable.");
    EXPECT_EQ(said.back(), "End of queue. Press play to start again.");
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Storm");
}

TEST_F(UnavailableTest, ASkipIsSaidOnceEvenWhenTheNextFileCannotBeOpenedEither) {
    start_();
    player.unopenable = {alphaFile(2)};
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm"), ofServer("https://other.example", "ann", "t2", "Rain"),
                                         localEntry(alphaFile(2), "Calm"), localEntry(lullaby(), "Lullaby")}));
    hold(Button::Forward);
    EXPECT_EQ(countOf(speech.said, "Rain, source unavailable."), 1);
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
    hold(Button::Forward); // Rain is skipped: Calm plays
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
    hold(Button::Back);
    EXPECT_TRUE(speech.heard("Rain, source unavailable."));
    EXPECT_EQ(player.file, lullaby()) << "the track before the one that was skipped, not Calm again";
    EXPECT_EQ(speech.last(), "Lullaby");
    EXPECT_TRUE(player.playing());
}

TEST_F(UnavailableTest, PreviousWithNothingPlayableBeforeItSaysFirstTrack) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {ofServer("https://other.example", "ann", "t2", "Rain"), localEntry(alphaFile(2), "Calm")}, 1));
    hold(Button::Back);
    EXPECT_EQ(speech.last(), "First track.");
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_FALSE(speech.heard("Rain, source unavailable.")) << "nothing is skipped when nothing plays";
}

TEST_F(UnavailableTest, TheAlbumUnitSaysTheAlbumThatPlaysWhenTheNextOneIsSkipped) {
    start_();
    ASSERT_TRUE(app->playQueue("Mixed", {localEntry(alphaFile(1), "Storm", "One"),
                                         ofServer("https://other.example", "ann", "t2", "Rain"),
                                         localEntry(alphaFile(2), "Calm", "Three")}));
    albumUnit();
    press(Button::Forward);
    EXPECT_TRUE(speech.heard("Rain, source unavailable."));
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_EQ(speech.last(), "Three");
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
    hold(Button::Forward); // Rain: tried, because the user asked for it
    finishOpen();
    ASSERT_EQ(speech.last(), "Cannot play Rain. Server not reachable.");
    ASSERT_EQ(app->nowPlaying().chapter, "Storm");
    player.unopenable.clear();
    hold(Button::Forward); // Rain again: the server is marked down now, so Calm plays
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
    press(Button::Down);
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
    while (level().items[level().index].label != "Lullaby") press(Button::Down); // a loose track
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
    while (level().items[level().index].label != "Road Trip") press(Button::Down);
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
    while (level().items[level().index].label != "Lullaby") press(Button::Down);
    press(Button::Ok); // Lullaby plays
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Road Trip") press(Button::Down);
    options("Play next");
    EXPECT_EQ(speech.last(), "Added Road Trip, 2 tracks. 3 in queue.");
    EXPECT_EQ(level().title, "Albums A to Z");
    options("Add to queue");
    EXPECT_EQ(speech.last(), "Added Road Trip, 2 tracks. 5 in queue.");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Lullaby", "Sunrise", "Highway", "Sunrise", "Highway"}));
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
    while (level().items[level().index].label != "Rain") press(Button::Down);
    options("Play next");
    EXPECT_EQ(speech.last(), "Added Rain. 3 in queue.");
    EXPECT_EQ(level().title, "Weather");
    while (level().items[level().index].label != "Sunshine") press(Button::Down);
    options("Add to queue");
    EXPECT_EQ(speech.last(), "Added Sunshine. 4 in queue.");
    EXPECT_EQ(queue(), (std::vector<std::string>{"Sunrise", "Rain", "Highway", "Sunshine"}));
    hold(Button::Home);
    openWeather();
    while (level().items[level().index].label != "Sunshine") press(Button::Down);
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
    while (level().items[level().index].label != "Road Trip") press(Button::Down);
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
    while (level().items[level().index].label != "Rain") press(Button::Down);
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
    while (level().items[level().index].label != "Road Trip") press(Button::Down);
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
        while (level().items[level().index].label != "Road Trip") press(Button::Down);
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
    press(Button::Up);
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
    EXPECT_EQ(speech.last(), "Road Trip. Sunrise, 1 of 2. Press OK to play, Options for more.");
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Road Trip") press(Button::Down);
    press(Button::Up);
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Road Trip, 2 tracks, 2 of 2. Press OK to open, Options for more.");
    hold(Button::Home);
    open("Music");
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Artists, 2 of 8. Press OK to open.") << "no options, no mention";
}

TEST_F(OptionsTest, ARemoteTrackAndAlbumHaveTheHintToo) {
    settings.setInt("verbosity", 0);
    start();
    open("Server");
    open("Recently added");
    answer(std::vector<RemoteAlbum>{{"al1", "Weather", "Band", 3}});
    EXPECT_EQ(speech.last(), "Weather, Band, 1 of 1. Press OK to open, Options for more.");
    press(Button::Ok);
    answer(weather());
    while (level().items[level().index].label != "Rain") press(Button::Down);
    EXPECT_EQ(speech.last(), "Rain, 4 of 5. Press OK to play, Options for more.");
}

TEST_F(OptionsTest, AServerOnlyAlbumInMusicHasOptionsToo) {
    start();
    sync({album("al1", "Weather")});
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Weather") press(Button::Down);
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
    options("Add to queue");
    answer(std::vector<RemoteTrack>{{"al9-t1", "Song 1", "Band", "Weather", 100, 1}});
    hold(Button::Home);
    open("Server");
    open("Downloaded");
    open("Weather");
    open("Delete download");
    press(Button::Down);
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Cannot delete Weather while it is loaded in the player. Play something else first.");
    EXPECT_TRUE(fs::exists(downloadedFile("al9", 1)));
}

TEST_F(OptionsTest, PlayNowOnAnAlbumOfMusicMarksItAsPlayed) {
    openRoadTrip();
    hold(Button::Home);
    open("Music");
    open("Albums A to Z");
    while (level().items[level().index].label != "Road Trip") press(Button::Down);
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
        while (level().items[level().index].label != label) press(Button::Down);
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
    press(Button::Down);
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
    hold(Button::Forward); // Sunrise, the third in the shuffled order
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
    hold(Button::Forward);
    ASSERT_EQ(player.file, sunrise());
    trackEnds();
    EXPECT_EQ(player.file, highway());
    trackEnds();
    EXPECT_EQ(player.file, sunrise()) << "Three repeats, One does not come back";
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
    hold(Button::Forward);
    ASSERT_EQ(player.file, lullaby());
    trackEnds();
    EXPECT_EQ(player.file, lullaby()) << "the track moved to repeats";
    hold(Button::Back);
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
    hold(Button::Forward);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_TRUE(speech.heard("2 tracks, source unavailable."));
    EXPECT_EQ(speech.last(), "Storm");
}

TEST_F(ShuffleRepeatTest, RepeatAllPreviousOnTheFirstTrackGoesToTheLastOverMissingOnes) {
    play(four(), 0);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "all"));
    fs::remove(lullaby());
    fs::remove(sunrise());
    hold(Button::Back);
    EXPECT_TRUE(speech.heard("2 tracks, source unavailable."));
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_EQ(speech.last(), "Calm");
}

TEST_F(ShuffleRepeatTest, RepeatAllPreviousGoesBackOverMissingOnesPastTheFirstToTheLast) {
    play(four(), 2);
    ASSERT_NO_FATAL_FAILURE(choose("Repeat", "all"));
    fs::remove(alphaFile(1));
    fs::remove(alphaFile(2));
    hold(Button::Back);
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
    hold(Button::Forward);
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
        for (size_t i = 0; i < level().items.size() && level().items[level().index].label != label; ++i) press(Button::Down);
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
    press(Button::Down);
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
    hold(Button::Forward);
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
    hold(Button::Forward); // another track
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
    press(Button::Down);
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
