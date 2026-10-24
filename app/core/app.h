#pragma once

#include "buttons.h"
#include "library.h"
#include "menu.h"
#include "services.h"
#include "settings.h"

#include <array>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>

namespace oap {

// The device's "brain": turns button gestures into navigation, playback and speech.
// Platform independent and free of audio code: everything it talks to is an
// interface from services.h, so tests can drive it with fakes. The simulator and
// the real device both drive it through keyDown/keyUp/setHold/tick, all from one thread.
class App {
public:
    enum class Mode { Menu, Player, Sleeping };

    struct NowPlaying {
        bool loaded = false;
        bool playing = false;
        std::string title;
        std::string chapter;
        int chapterIndex = 0;
        int chapterCount = 0;
        double position = 0;
        double duration = 0;
        std::optional<double> sleepTimerLeft;
    };

    // `config` holds what the user sets (config.yaml), `state` the positions and
    // the book to resume (state.tsv). State is written only on exit and on sleep.
    App(Speech& speech, MediaPlayer& player, SoundEffects& sounds, VolumeControl& volume, Settings& config,
        Settings& state, Clock& clock, std::filesystem::path libraryRoot, std::filesystem::path musicRoot);

    void start();
    void keyDown(Button button);
    void keyUp(Button button);
    void setHold(bool on);
    void tick();
    bool quitRequested() const { return quit_; }
    void shutdown(); // writes the state and any changed setting

    // Read-only state for the simulator's visualisation.
    Mode mode() const;
    const std::vector<Screen>& screens() const { return screens_; }
    NowPlaying nowPlaying() const;
    const std::deque<std::string>& speechLog() const { return speechLog_; }
    const std::array<std::array<int, kGestureCount>, kButtonCount>& buttonUsage() const { return usage_; }
    void resetButtonUsage() { usage_ = {}; }
    bool hold() const { return hold_; }
    bool keyDescriber() const { return keyDescriber_; }

private:
    struct ChapterRef {
        std::string title;
        size_t file = 0;
        double start = 0;
    };

    void onGesture(ButtonEvent ev); // drops gestures of keys locked by the hold switch
    void onButton(ButtonEvent ev);
    void handleMenu(ButtonEvent ev);
    void handlePlayer(ButtonEvent ev);
    void describeKey(ButtonEvent ev);

    // Speech
    void say(const std::string& text);
    std::string itemSpeech(const Screen& screen) const;
    std::string nowPlayingSpeech() const;
    void whereAmI();

    // Screens and menus
    void openMenu(const std::string& title, std::function<std::vector<MenuItem>()> build, size_t index = 0);
    void activate();
    bool canAdjust() const; // the selected item has a value that Left / Right step
    void adjust(int direction);
    void back();
    void popScreen(); // back() without a sound or speech
    void goHome();
    void enterPlayer();
    void openPowerMenu();
    void saveChangedSettings(); // a value stepped with Left / Right, once the user moves on
    void saveConfig();
    void saveOnExit(); // exit and sleep: the only times state.tsv is written
    void saveState();  // only if it changed since it was loaded or last written
    std::vector<MenuItem> homeMenu();
    std::vector<MenuItem> libraryMenu();
    std::vector<MenuItem> musicMenu();
    std::vector<MenuItem> albumMenu(const Book& album);
    std::vector<MenuItem> sleepTimerMenu();
    size_t sleepTimerIndex() const;
    std::vector<MenuItem> settingsMenu();
    std::vector<MenuItem> systemMenu();
    std::vector<MenuItem> powerMenu();
    std::vector<MenuItem> helpMenu();
    std::vector<MenuItem> playerMenu();
    std::vector<MenuItem> chaptersMenu();
    MenuItem choice(const std::string& label, const std::string& key,
                    std::vector<std::pair<int, std::string>> options, int fallback,
                    std::function<void(int)> onChange = {});

    // Playback
    bool openBook(const Book& book, bool autoplay); // false if it cannot be opened
    bool playMusic(const Book& album, size_t track);
    bool switchTo(const Book& book, bool music, size_t file, double position, bool autoplay);
    std::string unit() const { return music_ ? "track" : "chapter"; } // what one part is called
    bool loadFile(size_t file, double position, bool autoplay);
    void togglePlay();
    void play();
    void pause(bool announce);
    void skip(int direction);
    void changeVolume(int delta);
    std::vector<ChapterRef> chapters() const;
    int currentChapter() const;
    void gotoChapter(int index);
    void stepChapter(int direction);
    void onFileFinished();
    void rememberPosition(); // in the state map only, written by saveOnExit()
    std::string playerStatus() const;

    // Power
    void setSleepTimer(int minutes, bool endOfChapter);
    void sleep(const std::string& announcement);
    void wake();
    void powerOff();
    void restart(bool saveFirst = true);
    void factoryReset();
    std::string statusReport() const;

    using TimePoint = Clock::TimePoint;

    Speech& tts_;
    MediaPlayer& player_;
    SoundEffects& earcons_;
    VolumeControl& volume_;
    Settings& config_;
    Settings& state_;
    SettingsMap savedState_; // what state.tsv holds
    Clock& clock_;
    std::filesystem::path libraryRoot_;
    std::filesystem::path musicRoot_;
    GestureDetector gestures_;

    std::vector<Screen> screens_;
    bool sleeping_ = false;
    bool settingsChanged_ = false; // changed with Left / Right, not yet written to disk
    bool volumeChanged_ = false;   // changed in the player, written on exit
    std::optional<Book> book_; // the loaded book, or the album / playlist when music_
    bool music_ = false;
    size_t fileIndex_ = 0;

    bool hold_ = false;
    std::array<bool, kButtonCount> held_{};   // raw keys down now
    std::array<bool, kButtonCount> locked_{}; // down when Hold went on: ignored until released
    bool keyDescriber_ = false;
    bool quit_ = false;
    std::optional<TimePoint> pausedAt_;
    std::optional<TimePoint> sleepDeadline_;
    int sleepMinutes_ = 0; // the time chosen for sleepDeadline_
    int sleepAtChapterEnd_ = -1;
    TimePoint lastActivity_;

    std::deque<std::string> speechLog_;
    std::array<std::array<int, kGestureCount>, kButtonCount> usage_{};
};

} // namespace oap
