#include "core/app.h"
#include "fakes.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <algorithm>
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
        app = std::make_unique<App>(speech, player, sounds, volume, server, settings, state, clock, root, music);
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
    FakeMusicServer server;
    FakeClock clock;
    FakeStore configFile; // config.yaml
    FakeStore stateFile;  // state.tsv
    Settings settings{&configFile};
    Settings state{&stateFile};
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
    start();
    open("Settings"); // Speech rate
    press(Button::Forward);
    press(Button::Forward);
    EXPECT_EQ(configFile.saves, 0) << "not written while still on the item";
    press(Button::Down);
    EXPECT_EQ(configFile.saves, 1);
    EXPECT_EQ(configFile.onDisk("speech.rate"), "280");
    EXPECT_EQ(stateFile.saves, 0) << "a setting is not state";
}

TEST_F(AppTest, AChangedSettingIsWrittenWhenTheDeviceSleeps) {
    start();
    open("Settings");
    press(Button::Forward);
    idle(6min); // auto sleep, no key pressed
    ASSERT_EQ(app->mode(), App::Mode::Sleeping);
    EXPECT_EQ(configFile.onDisk("speech.rate"), "250");
}

TEST_F(AppTest, LeftAndRightDoNothingOnItemsWithoutAValue) {
    start();
    open("Settings");
    hold(Button::Down); // Sleep timer: a value, but nothing to step
    for (Button b : {Button::Back, Button::Forward}) {
        size_t said = speech.said.size(), played = sounds.played.size();
        int stops = speech.stops;
        press(b);
        EXPECT_EQ(level().title, "Settings");
        EXPECT_EQ(level().index, 9u);
        EXPECT_EQ(speech.said.size(), said) << "silent";
        EXPECT_EQ(sounds.played.size(), played) << "silent";
        EXPECT_EQ(speech.stops, stops) << "speech goes on";
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
    EXPECT_EQ(labels(), (std::vector<std::string>{"Library", "Music", "Server", "Podcasts", "Settings", "System", "Help"}));
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
    player.unopenable = {(music / "Lullaby.mp3").string()};
    open("Lullaby");
    EXPECT_EQ(speech.last(), "Cannot open Lullaby.mp3");
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    EXPECT_EQ(level().title, "Music");
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
    open("Road Trip");
    open("Sunrise");
    player.atEnd = true;
    app->tick();
    EXPECT_TRUE(speech.heard("Cannot open 02 - Highway.mp3"));
    EXPECT_EQ(player.file, (roadTrip() / "01 - Sunrise.mp3").string());
    EXPECT_TRUE(player.isOpen());
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "End of music. Press play to start again.");
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

TEST_F(AppTest, PlayerVolumeIsWrittenOnlyOnExit) {
    playAlpha();
    press(Button::Up);
    press(Button::Up);
    press(Button::PlayPause);
    press(Button::Down);
    EXPECT_EQ(configFile.saves, 0) << "no config.yaml write per press";
    app->shutdown();
    EXPECT_EQ(configFile.saves, 1);
    EXPECT_EQ(configFile.onDisk("volume"), "75");
}

TEST_F(AppTest, VolumeStopsAtMaximum) {
    settings.setInt("volume", 100);
    playAlpha();
    press(Button::Up);
    EXPECT_EQ(settings.getInt("volume", 0), 100);
    EXPECT_EQ(speech.last(), "Maximum volume.");
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
    EXPECT_EQ(player.file, (roadTrip() / "02 - Highway.mp3").string());
    EXPECT_TRUE(player.playing());
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
    EXPECT_EQ(speech.last(), "Sleep timer, off, 10 of 10");
}

TEST_F(AppTest, MusicDoesNotTakeOverTheBookToResume) {
    playAlpha();
    player.pos = 20;
    hold(Button::Home);
    open("Music");
    open("Road Trip");
    open("Sunrise");
    EXPECT_EQ(state.getString("pos:" + alpha().string()), "0:20.0") << "the book's place is kept";
    press(Button::PlayPause);
    EXPECT_EQ(state.getString("pos:" + roadTrip().string()), "");
    EXPECT_EQ(state.getString("last_book"), alpha().string());
}

TEST_F(AppTest, RestartAfterMusicResumesTheBook) {
    playAlpha();
    player.pos = 20;
    hold(Button::Home);
    open("Music");
    open("Road Trip");
    open("Sunrise");
    hold(Button::Power);
    open("Restart");
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_EQ(player.file, alphaFile(1));
    EXPECT_DOUBLE_EQ(player.pos, 20);
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(speech.last(), "Alpha, paused. Press play to continue.");
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

TEST_F(AppTest, RemoteTracksAloneWriteNoStateOnExit) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.pos = 50;
    hold(Button::Power);
    open("Power off");
    app->shutdown();
    EXPECT_EQ(stateFile.saves, 0) << "nothing to remember";
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
    EXPECT_EQ(speech.last().rfind("Alpha. Chapter 1 of 2, Storm.", 0), 0u) << speech.last();
    expectNoUrlSpoken(speech);
}

TEST_F(AppTest, ARemoteTrackThatFailsWhenTheOneBeforeEndsStopsThere) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.unopenable = {"fake://stream/t2"};
    player.pos = 200;
    player.atEnd = true;
    app->tick(); // Storm Front ends, Rain cannot be opened
    EXPECT_EQ(speech.last(), "Cannot play Rain. Server not reachable.");
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t1", "fake://stream/t2"}))
        << "no other track tried, and no second wait on the server";
    EXPECT_FALSE(player.playing());
    // Storm Front was heard to the end: the track that failed is the current one,
    // so Play does not open the finished one again (a transcode cannot seek to its end).
    EXPECT_EQ(app->nowPlaying().chapter, "Rain");
    player.unopenable.clear(); // the server is back
    press(Button::PlayPause);
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
    EXPECT_FALSE(speech.heard("End of music. Press play to start again."));
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
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 80);
    EXPECT_TRUE(player.playing());
}

// The player is closed until Play: it knows neither the place nor the length.
TEST_F(AppTest, WhereAmIAfterAFailedRemoteTrackSaysNoTimeLeft) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.pos = 42;
    player.unopenable = {"fake://stream/t2"};
    hold(Button::Forward); // fails: Storm Front stays current, closed
    hold(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Weather. Track 1 of 3, Storm Front. Paused.");
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
    EXPECT_EQ(speech.last(), "Weather. Track 2 of 3, Rain. 8 minutes 20 seconds left in track. Playing.");
    player.pos = 1;
    hold(Button::Back);
    EXPECT_EQ(speech.last(), "Storm Front");
    EXPECT_EQ(player.file, "fake://stream/t1");
    press(Button::Ok); // Player menu
    open("Tracks");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Storm Front", "Rain", "Sunshine"}));
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
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_DOUBLE_EQ(player.pos, 42);
    EXPECT_TRUE(player.playing());
}

TEST_F(AppTest, TheEndOfARemoteListOpensTheFirstTrackOnlyOnPlay) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 2));
    player.atEnd = true;
    app->tick();
    EXPECT_EQ(speech.last(), "End of music. Press play to start again.");
    EXPECT_EQ(player.opened, (std::vector<std::string>{"fake://stream/t3"})) << "no network until Play";
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Storm Front");
    player.unopenable = {"fake://stream/t1"};
    press(Button::PlayPause);
    EXPECT_EQ(speech.last(), "Cannot play Storm Front. Server not reachable.");
    player.unopenable.clear();
    press(Button::PlayPause);
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
    open("Tracks");
    open("Storm Front");
    EXPECT_EQ(player.file, "fake://stream/t1");
    EXPECT_TRUE(player.isOpen());
    EXPECT_EQ(app->mode(), App::Mode::Player);
}

TEST_F(AppTest, ChoosingAnUnreachableTrackFromTheTracksListStaysInTheList) {
    start();
    ASSERT_TRUE(app->playRemote("Weather", weather(), 0));
    player.unopenable = {"fake://stream/t3"};
    press(Button::Ok); // Player menu
    open("Tracks");
    open("Sunshine");
    EXPECT_EQ(speech.last(), "Cannot play Sunshine. Server not reachable.");
    EXPECT_EQ(level().title, "Tracks");
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
    EXPECT_EQ(speech.last(), "Cannot play Rain. Server not reachable.");
    EXPECT_FALSE(player.playing());
    EXPECT_EQ(app->nowPlaying().chapter, "Rain") << "Storm Front was heard to the end";
    player.unopenable.clear();
    press(Button::PlayPause);
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
    EXPECT_EQ(speech.last(), "Weather. Track 2 of 3, Rain. Loading.");
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
    EXPECT_EQ(labels(), (std::vector<std::string>{"Recently added", "Albums A to Z", "Artists", "Playlists",
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

TEST_F(StreamingTest, AnAnswerForArtistsDoesNotFillPlaylistsOpenedAfterIt) {
    openList("Artists");
    auto artists = server.lastRequest().id;
    press(Button::Home);
    open("Playlists");
    auto playlists = server.lastRequest().id;
    EXPECT_FALSE(server.reply(artists, std::vector<RemoteArtist>{{"a1", "Band", 2}}));
    app->tick();
    EXPECT_EQ(level().title, "Playlists");
    EXPECT_EQ(labels(), (std::vector<std::string>{"Loading"}));
    ASSERT_TRUE(server.reply(playlists, std::vector<RemotePlaylist>{{"p1", "Road", 3}}));
    app->tick();
    EXPECT_EQ(labels(), (std::vector<std::string>{"Road"}));
    EXPECT_EQ(speech.last(), "Road, 3 tracks, 1 of 1");
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
    openList("Playlists");
    answer(std::vector<RemotePlaylist>{});
    EXPECT_EQ(labels(), (std::vector<std::string>{"No playlists"}));
    EXPECT_EQ(speech.last(), "No playlists, 1 of 1");
    press(Button::Ok);
    EXPECT_EQ(speech.last(), "Make a playlist on the server, then come back.");
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
    openList("Playlists");
    answer(std::vector<RemotePlaylist>{{"p1", "Road", 3}});
    open("Road");
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
    EXPECT_EQ(speech.last(), "Sleep timer, off, 10 of 10");
    press(Button::Ok);
    open("15 minutes");
    EXPECT_EQ(level().title, "Settings") << "choosing a time goes back";
    EXPECT_EQ(speech.last(), "Sleep timer, 15 minutes left, 10 of 10");
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
