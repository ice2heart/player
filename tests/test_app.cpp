#include "core/app.h"
#include "fakes.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>

using namespace oap;
using namespace oap::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}
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

    void start() {
        app = std::make_unique<App>(speech, player, sounds, volume, settings, clock, root, music);
        app->start();
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

    const Screen& level() const { return app->screens().back(); }

    FakeSpeech speech;
    FakePlayer player;
    FakeSounds sounds;
    FakeVolume volume;
    FakeClock clock;
    Settings settings{fs::path()};
    fs::path base;
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
    EXPECT_EQ(speech.last(), "Home. Library, 1 of 6");
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
    settings.setString("last_book", alpha().string());
    settings.setString("pos:" + alpha().string(), "1:42.5");
    start();
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, alphaFile(2));
    EXPECT_DOUBLE_EQ(player.pos, 42.5);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "Alpha, paused. Press play to continue.");
}

TEST_F(AppTest, InvalidSavedPositionStartsFromTheBeginning) {
    settings.setString("last_book", alpha().string());
    settings.setString("pos:" + alpha().string(), "7:99.0"); // the book has only two files
    start();
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_DOUBLE_EQ(player.pos, 0);
}

// ---------------------------------------------------------------- menus

TEST_F(AppTest, UpAndDownMoveThroughTheMenuAndWrap) {
    start();
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Music, 2 of 6");
    EXPECT_EQ(sounds.last(), Earcon::Move);
    press(Button::Up);
    press(Button::Up);
    EXPECT_EQ(speech.last(), "Help, 6 of 6");
    EXPECT_EQ(sounds.last(), Earcon::Wrap);
}

TEST_F(AppTest, NoWrapWhenWrapAroundIsOff) {
    settings.setInt("menu.wrap", 0);
    start();
    press(Button::Up);
    EXPECT_EQ(level().index, 0u);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Library, 1 of 6");
}

TEST_F(AppTest, LongPressDownJumpsToLastItemOnce) {
    start();
    hold(Button::Down);
    EXPECT_EQ(level().index, 5u);
    EXPECT_EQ(speech.last(), "Help, 6 of 6");
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
    EXPECT_EQ(speech.last(), "Music, 2 of 6. Press OK to open.");
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
    EXPECT_EQ(speech.rate, 250);
    EXPECT_EQ(level().title, "Settings");
    EXPECT_EQ(speech.last(), "Speech rate, 250 words per minute, 1 of 10");
}

TEST_F(AppTest, LeavingAValueListChangesNothing) {
    start();
    open("Settings");
    open("Volume");
    press(Button::Down);
    press(Button::Home);
    EXPECT_EQ(level().title, "Settings");
    EXPECT_EQ(settings.getInt("volume", 70), 70);
    EXPECT_EQ(speech.last(), "Volume, 70 percent, 2 of 10");
}

TEST_F(AppTest, LeftAndRightChangeASettingInPlace) {
    start();
    open("Settings"); // Speech rate
    press(Button::Forward);
    EXPECT_EQ(level().title, "Settings");
    EXPECT_EQ(settings.getInt("speech.rate", 0), 250);
    EXPECT_EQ(speech.rate, 250) << "applied at once, so the user hears the new rate";
    EXPECT_EQ(speech.last(), "250 words per minute");
    EXPECT_EQ(sounds.last(), Earcon::Move);
    press(Button::Down); // Volume
    press(Button::Back);
    EXPECT_EQ(settings.getInt("volume", 0), 65);
    EXPECT_FLOAT_EQ(volume.volume, 0.65f);
}

TEST_F(AppTest, ASettingStopsAtTheEndOfItsRange) {
    settings.setInt("speech.rate", 460);
    start();
    open("Settings");
    press(Button::Forward);
    EXPECT_EQ(settings.getInt("speech.rate", 0), 460);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "460 words per minute");
}

TEST_F(AppTest, AChangedSettingIsWrittenWhenLeavingTheItem) {
    fs::path file = base / "settings.tsv";
    settings = Settings(file);
    start();
    open("Settings"); // Speech rate
    auto saved = [&] {
        Settings onDisk(file);
        onDisk.load();
        return onDisk.getInt("speech.rate", 220);
    };
    press(Button::Forward);
    press(Button::Forward);
    EXPECT_EQ(saved(), 220) << "not written while still on the item";
    press(Button::Down);
    EXPECT_EQ(saved(), 280);
}

TEST_F(AppTest, AChangedSettingIsWrittenWhenTheDeviceSleeps) {
    fs::path file = base / "settings.tsv";
    settings = Settings(file);
    start();
    open("Settings");
    press(Button::Forward);
    idle(6min); // auto sleep, no key pressed
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    Settings onDisk(file);
    onDisk.load();
    EXPECT_EQ(onDisk.getInt("speech.rate", 220), 250);
}

TEST_F(AppTest, LeftAndRightDoNothingOnItemsWithoutAValue) {
    start();
    open("Settings");
    hold(Button::Down); // Sleep timer: a value, but nothing to step
    for (Button b : {Button::Back, Button::Forward}) {
        size_t said = speech.said.size(), played = sounds.played.size();
        press(b);
        EXPECT_EQ(level().title, "Settings");
        EXPECT_EQ(level().index, 9u);
        EXPECT_EQ(speech.said.size(), said) << "silent";
        EXPECT_EQ(sounds.played.size(), played) << "silent";
    }
    press(Button::Home);
    press(Button::Forward); // Home > Settings: a submenu
    EXPECT_EQ(level().title, "Home");
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
    EXPECT_EQ(speech.last(), "Library, 2 of 7") << "and stays on the same item";
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
    open("Twins");
    press(Button::Down);
    press(Button::Ok); // the second "Song"
    ASSERT_EQ(player.file, (music / "Twins" / "02 - Song.mp3").string());
    press(Button::Home);
    EXPECT_EQ(level().index, 1u);
    EXPECT_EQ(speech.last(), "Song, 2 of 2");
}

TEST_F(AppTest, AStoredValueBetweenOptionsStepsToTheNearestOne) {
    settings.setInt("speech.rate", 225); // between 220 and 250
    start();
    open("Settings");
    press(Button::Forward);
    EXPECT_EQ(settings.getInt("speech.rate", 0), 250);
    settings.setInt("speech.rate", 225);
    press(Button::Back);
    EXPECT_EQ(settings.getInt("speech.rate", 0), 220);
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
    EXPECT_EQ(labels(), (std::vector<std::string>{"Library", "Music", "Podcasts", "Settings", "System", "Help"}));
}

// ---------------------------------------------------------------- help

TEST_F(AppTest, HelpGettingStartedExplainsTheBasics) {
    start();
    open("Help");
    open("Getting started");
    EXPECT_EQ(speech.last().rfind("This player has no screen. Up and Down move through a menu.", 0), 0u)
        << speech.last();
}

TEST_F(AppTest, HelpButtonsDescribesEachButton) {
    start();
    open("Help");
    open("Buttons");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play pause", "Left", "Right", "Up", "Down", "OK", "Home", "Talk",
                                                  "Power"}));
    open("Right");
    EXPECT_EQ(speech.last(), "Right. In menus, raises a setting. In player, skip forward. Hold, or press deeper, for next chapter or track.");
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
    EXPECT_EQ(settings.getString("last_book"), alpha().string());
    EXPECT_EQ(speech.last(), "Now playing. Alpha.");
}

TEST_F(AppTest, PlayWithNothingLoadedExplains) {
    start();
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Nothing to play. Choose a book in the library.");
    EXPECT_EQ(sounds.last(), Earcon::Error);
}

TEST_F(AppTest, FileThatCannotBeOpenedIsReported) {
    player.openSucceeds = false;
    playAlpha();
    EXPECT_TRUE(speech.heard("Cannot open 01 - Storm.mp3"));
    EXPECT_FALSE(player.playing());
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

TEST_F(AppTest, UpAndDownChangeVolumeInPlayer) {
    playAlpha();
    press(Button::Up);
    EXPECT_EQ(settings.getInt("volume", 0), 75);
    EXPECT_FLOAT_EQ(volume.volume, 0.75f);
    press(Button::Down);
    press(Button::Down);
    EXPECT_EQ(settings.getInt("volume", 0), 65);
}

TEST_F(AppTest, VolumeStopsAtMaximum) {
    settings.setInt("volume", 100);
    playAlpha();
    press(Button::Up);
    EXPECT_EQ(settings.getInt("volume", 0), 100);
    EXPECT_EQ(speech.last(), "Maximum volume.");
}

TEST_F(AppTest, PauseSavesPosition) {
    playAlpha();
    player.pos = 77.3;
    press(Button::PlayPause);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(settings.getString("pos:" + alpha().string()), "0:77.3");
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

TEST_F(AppTest, PositionIsSavedEvery30SecondsWhilePlaying) {
    playAlpha();
    player.pos = 12;
    std::string key = "pos:" + alpha().string();
    idle(20s);
    EXPECT_EQ(settings.getString(key), "");
    idle(11s);
    EXPECT_EQ(settings.getString(key), "0:12.0");
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

TEST_F(AppTest, PushingDeeperAfterALongPressDoesNotStepAgain) {
    start();
    open("Settings");
    app->keyDown(Button::Forward);
    clock.advance(700ms);
    app->tick(); // long press: one step
    app->keyDown(Button::ForwardDeep);
    app->keyUp(Button::ForwardDeep);
    app->keyUp(Button::Forward);
    EXPECT_EQ(settings.getInt("speech.rate", 0), 250);
}

TEST_F(AppTest, DeepPressInAMenuStepsTheValueOnce) {
    start();
    open("Settings");
    app->keyDown(Button::Forward);
    app->keyDown(Button::ForwardDeep);
    app->keyUp(Button::ForwardDeep);
    app->keyUp(Button::Forward);
    EXPECT_EQ(settings.getInt("speech.rate", 0), 250);
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

TEST_F(AppTest, MusicMenuListsPlayAllLooseTracksAndAlbums) {
    start();
    open("Music");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Play all", "Lullaby", "Road Trip"}));
    EXPECT_EQ(speech.last(), "Music. Play all, 3 tracks, 1 of 3");
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Lullaby, 2 of 3");
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Road Trip, 2 tracks, 3 of 3");
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

TEST_F(AppTest, ChoosingATrackPlaysTheRestOfTheAlbum) {
    start();
    open("Music");
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
    player.atEnd = true;
    app->tick();
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "End of music. Press play to start again.");
}

TEST_F(AppTest, MusicTalksAboutTracksNotChapters) {
    start();
    open("Music");
    open("Road Trip");
    open("Highway");
    player.pos = 100;
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Road Trip. Track 2 of 2, Highway. 8 minutes 20 seconds left in track. Playing.");
    hold(Button::Forward);
    EXPECT_EQ(speech.last(), "Last track.");
    press(Button::Ok);
    EXPECT_EQ(labels()[0], "Tracks");
}

TEST_F(AppTest, SleepTimerAtEndOfTrack) {
    start();
    open("Music");
    open("Road Trip");
    open("Sunrise");
    press(Button::Ok);    // Player menu
    open("Sleep timer");
    open("End of track");
    EXPECT_TRUE(speech.heard("Sleep timer set to end of track."));
    player.atEnd = true;
    app->tick();          // Highway starts
    app->tick();
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_TRUE(speech.heard("End of track. Sleep timer ended."));
}

TEST_F(AppTest, MusicDoesNotTakeOverTheBookToResume) {
    playAlpha();
    player.pos = 20;
    hold(Button::Home);
    open("Music");
    open("Road Trip");
    open("Sunrise");
    EXPECT_EQ(settings.getString("pos:" + alpha().string()), "0:20.0") << "the book's place is kept";
    press(Button::PlayPause);
    EXPECT_EQ(settings.getString("pos:" + roadTrip().string()), "");
    EXPECT_EQ(settings.getString("last_book"), alpha().string());
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
    EXPECT_EQ(speech.last(), "Sleep timer, off, 10 of 10");
    press(Button::Ok);
    open("15 minutes");
    EXPECT_EQ(level().title, "Settings") << "choosing a time goes back";
    EXPECT_EQ(speech.last(), "Sleep timer, 15 minutes left, 10 of 10");
    idle(15min);
    EXPECT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_TRUE(speech.heard("Sleep timer ended."));
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

TEST_F(AppTest, PowerOffFromPowerMenu) {
    playAlpha();
    player.pos = 33;
    hold(Button::Power);
    EXPECT_EQ(level().title, "Power");
    press(Button::Down);
    press(Button::Ok);
    EXPECT_TRUE(app->quitRequested());
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(settings.getString("pos:" + alpha().string()), "0:33.0");
    EXPECT_EQ(speech.last(), "Powering off. Goodbye.");
}
