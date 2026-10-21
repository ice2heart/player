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
class AppTest : public testing::Test {
protected:
    void SetUp() override {
        root = testTempDir();
        fs::remove_all(root);
        touch(alpha() / "01 - Storm.mp3");
        touch(alpha() / "02 - Calm.mp3");
        touch(beta());
    }
    void TearDown() override { fs::remove_all(root); }

    fs::path alpha() const { return root / "Alpha"; }
    fs::path beta() const { return root / "Beta.m4b"; }
    std::string alphaFile(int n) const {
        return (alpha() / (n == 1 ? "01 - Storm.mp3" : "02 - Calm.mp3")).string();
    }

    void start() {
        app = std::make_unique<App>(speech, player, sounds, volume, settings, clock, root);
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

    const MenuLevel& level() const { return app->menuStack().back(); }

    FakeSpeech speech;
    FakePlayer player;
    FakeSounds sounds;
    FakeVolume volume;
    FakeClock clock;
    Settings settings{fs::path()};
    fs::path root;
    std::unique_ptr<App> app;
};

// ---------------------------------------------------------------- start up

TEST_F(AppTest, StartsAtHomeWhenNoBookWasPlaying) {
    start();
    EXPECT_EQ(app->mode(), App::Mode::Menu);
    ASSERT_EQ(app->menuStack().size(), 1u);
    EXPECT_EQ(level().title, "Home");
    EXPECT_TRUE(speech.heard("Ready."));
    EXPECT_EQ(speech.last(), "Home. Library, 1 of 5");
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
    EXPECT_EQ(speech.last(), "Podcasts, 2 of 5");
    EXPECT_EQ(sounds.last(), Earcon::Move);
    press(Button::Up);
    press(Button::Up);
    EXPECT_EQ(speech.last(), "System, 5 of 5");
    EXPECT_EQ(sounds.last(), Earcon::Wrap);
}

TEST_F(AppTest, NoWrapWhenWrapAroundIsOff) {
    settings.setInt("menu.wrap", 0);
    start();
    press(Button::Up);
    EXPECT_EQ(level().index, 0u);
    EXPECT_EQ(sounds.last(), Earcon::Edge);
    EXPECT_EQ(speech.last(), "Library, 1 of 5");
}

TEST_F(AppTest, LongPressDownJumpsToLastItemOnce) {
    start();
    hold(Button::Down);
    EXPECT_EQ(level().index, 4u);
    EXPECT_EQ(speech.last(), "System, 5 of 5");
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
    EXPECT_EQ(speech.last(), "Podcasts");
}

TEST_F(AppTest, BeginnerVerbosityAddsAHint) {
    settings.setInt("verbosity", 0);
    start();
    press(Button::Down);
    EXPECT_EQ(speech.last(), "Podcasts, 2 of 5. Press OK to open.");
}

TEST_F(AppTest, OkOpensSubmenuAndLongOkGoesBack) {
    start();
    press(Button::Ok);
    EXPECT_EQ(level().title, "Library");
    EXPECT_EQ(speech.last(), "Library. Alpha, 2 parts, 1 of 2");
    hold(Button::Ok);
    EXPECT_EQ(app->menuStack().size(), 1u);
    EXPECT_EQ(sounds.last(), Earcon::Close);
}

TEST_F(AppTest, ChangingASettingSavesAndAppliesIt) {
    start();
    hold(Button::Down);   // System
    press(Button::Up);    // Settings
    press(Button::Ok);    // Speech rate
    press(Button::Forward);
    EXPECT_EQ(settings.getInt("speech.rate", 0), 250);
    EXPECT_EQ(speech.rate, 250);
    EXPECT_EQ(speech.last(), "250 words per minute");
    press(Button::Down);  // Volume
    press(Button::Back);
    EXPECT_EQ(settings.getInt("volume", 0), 65);
    EXPECT_FLOAT_EQ(volume.volume, 0.65f);
}

TEST_F(AppTest, SettingStopsAtTheEndOfItsRange) {
    settings.setInt("speech.rate", 460);
    start();
    hold(Button::Down);
    press(Button::Up);
    press(Button::Ok);
    press(Button::Forward);
    EXPECT_EQ(settings.getInt("speech.rate", 0), 460);
    ASSERT_GE(sounds.played.size(), 2u);
    EXPECT_EQ(sounds.played[sounds.played.size() - 2], Earcon::Edge);
}

TEST_F(AppTest, KeyDescriberDescribesInsteadOfActing) {
    start();
    hold(Button::Down);
    press(Button::Up);
    press(Button::Ok);   // Settings
    hold(Button::Down);  // Key describer
    press(Button::Ok);
    ASSERT_TRUE(app->keyDescriber());
    size_t index = level().index;
    press(Button::Up);
    EXPECT_EQ(level().index, index);
    EXPECT_EQ(speech.last().rfind("Up.", 0), 0u) << speech.last();
    hold(Button::Home);
    EXPECT_FALSE(app->keyDescriber());
    EXPECT_EQ(speech.last(), "Key describer off.");
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

TEST_F(AppTest, HomeAtTopOfMenuReturnsToPlayer) {
    playAlpha();
    press(Button::Ok);
    EXPECT_EQ(level().title, "Player menu");
    press(Button::Home);
    EXPECT_EQ(app->mode(), App::Mode::Player);
    EXPECT_TRUE(app->menuStack().empty());
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
