#pragma once

#include "buttons.h"
#include "library.h"
#include "menu.h"
#include "services.h"
#include "settings.h"

#include <array>
#include <deque>
#include <filesystem>
#include <map>
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
    // `server` gives the stream URLs of remote tracks; a front end without one
    // passes a server that is not configured.
    App(Speech& speech, MediaPlayer& player, SoundEffects& sounds, VolumeControl& volume, MusicServer& server,
        Settings& config, Settings& state, Clock& clock, std::filesystem::path libraryRoot,
        std::filesystem::path musicRoot);

    void start();
    void keyDown(Button button);
    void keyUp(Button button);
    void setHold(bool on);
    void tick();
    bool quitRequested() const { return quit_; }
    void shutdown(); // writes the state and any changed setting

    // Plays `tracks` from the server like a local album, from `index` on. It
    // says "Loading." and opens the player once the stream has opened, if the
    // user is still on the screen they chose it from; leaving that screen first
    // drops the open. `title` is what the list is called (album or playlist
    // name). Saves no position and leaves `last_book` alone. If the track cannot
    // be opened it says "Server not reachable", tries no other track and keeps
    // what was loaded before (paused). Returns false if that happened at once.
    bool playRemote(const std::string& title, const std::vector<RemoteTrack>& tracks, size_t index);

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
    void pushScreen(Screen screen);     // gives it an id
    void closeScreens(size_t keep);     // all but the first `keep`, cancelling their requests
    Screen* findScreen(std::uint64_t id); // nullptr once it has left the stack
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
    std::vector<MenuItem> streamingMenu();
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

    // Lists loaded from the server (docs/streaming.md). A list opens at once with
    // "Loading" and is filled when the answer comes. The answer finds its screen
    // by id; leaving the screen cancels the request, so a late answer never runs.
    using PageDone = std::function<void(MusicServer::Error, std::vector<MenuItem>, bool more)>;
    using FetchPage = std::function<MusicServer::RequestId(int offset, PageDone done)>;
    struct RemoteList {
        FetchPage fetch;
        int pageSize = 0;                  // > 0: fetch the next page while the server says there is more
        std::string emptyLabel, emptyHelp; // what an empty list says, and what to do
        size_t loaded = 0;                 // the first items of the screen; a status item may follow
        MusicServer::RequestId request = 0; // the page being loaded, 0 when none
        int offset = 0;                     // of that page, or of the page that failed
        bool waiting = false;               // the user waits for it: a "Loading" item is shown
        MusicServer::Error error = MusicServer::Error::None; // the last page failed
    };
    template <class T>
    static FetchPage fetchPages(std::function<MusicServer::RequestId(int offset, MusicServer::Done<T>)> request,
                                std::function<std::vector<MenuItem>(const std::vector<T>&)> toItems);
    void openRemoteList(const std::string& title, FetchPage fetch, int pageSize, std::string emptyLabel,
                        std::string emptyHelp);
    void requestPage(std::uint64_t screenId, int offset, bool waiting);
    void onPage(std::uint64_t screenId, MusicServer::Error error, std::vector<MenuItem> items, bool more);
    void showStatus(std::uint64_t screenId); // Loading, the error, or why it is empty, after the loaded items
    void openAlbumList(const std::string& title, const std::string& listType, int pageSize, bool paged);
    void openRemoteAlbum(const RemoteAlbum& album);
    std::vector<MenuItem> albumItems(const std::vector<RemoteAlbum>& albums, bool withArtist);
    std::vector<MenuItem> trackItems(const std::string& title, const std::vector<RemoteTrack>& tracks,
                                     const std::string& playAll);
    void checkServer(); // Server status

    // Playback. A local file opens at once. A remote track opens in the
    // background (MediaPlayer::opening()): the open is kept in pendingOpen_ and
    // tick() finishes it. `opened` runs once the file is open (and playing, with
    // `autoplay`); `undo` runs if it fails or is dropped, and goes back to what
    // was loaded before.
    enum class OpenResult { Opened, Pending, Failed };
    // Who asked for an open: what a remote track says, and what drops it.
    enum class Origin {
        Menu,    // chosen in a menu: says "Loading.", and leaving that menu drops it
        Key,     // Play, or a skip in the player: says "Loading."
        Next,    // the next track at the end of one: quiet
        Restore, // what played before a choice that failed: quiet, even if it fails too
    };
    using Then = std::function<void()>;
    bool openBook(const Book& book, bool autoplay); // false if it cannot be opened
    bool playMusic(const Book& album, size_t track, Then opened = {}); // false if it failed at once
    OpenResult switchTo(const Book& book, bool music, size_t file, double position, bool autoplay, Then opened = {});
    std::string unit() const { return music_ ? "track" : "chapter"; } // what one part is called
    OpenResult loadFile(size_t file, double position, bool autoplay, Origin origin, Then opened = {},
                        Then undo = {}); // within the loaded book
    OpenResult openFile(size_t file, double position, bool autoplay, Origin origin, Then opened, Then undo);
    void onOpened(bool autoplay, const Then& opened);
    // A remote track did not open: says so (unless `quiet`), then runs `undo`.
    void onCannotPlay(const std::string& title, bool quiet, const Then& undo);
    void finishPendingOpen(); // tick(): the player is done opening
    void giveUpPendingOpen(); // tick(): it takes too long (Player's own cap did not end it)
    void dropPendingOpen();   // closes the player and runs the open's undo, silently
    bool willPlay() const;    // playing, or will once the pending open is done
    void enterPlayerFrom(std::uint64_t screenId); // only if the user is still on that screen
    void togglePlay();
    void play();
    void pause(bool announce);
    void skip(int direction);
    void changeVolume(int delta);
    std::vector<ChapterRef> chapters() const;
    int currentChapter() const;
    bool gotoChapter(int index, Origin origin, Then opened = {}); // false if its file did not open
    void stepChapter(int direction);
    void onFileFinished();
    void onStreamFailed(); // a remote track broke off: stop, and open it there on Play
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
    MusicServer& server_;
    Settings& config_;
    Settings& state_;
    SettingsMap savedState_; // what state.tsv holds
    Clock& clock_;
    std::filesystem::path libraryRoot_;
    std::filesystem::path musicRoot_;
    GestureDetector gestures_;

    std::vector<Screen> screens_;
    std::uint64_t lastScreenId_ = 0;
    std::map<std::uint64_t, RemoteList> remoteLists_; // by Screen::id, while the screen is on the stack
    MusicServer::RequestId ping_ = 0;                 // Server status, while it waits; cancelled when the user moves on
    std::uint64_t pingScreen_ = 0;                    // the screen it was asked from
    bool sleeping_ = false;
    bool settingsChanged_ = false; // changed with Left / Right, not yet written to disk
    bool volumeChanged_ = false;   // changed in the player, written on exit
    std::optional<Book> book_; // the loaded book, or the album / playlist when music_
    bool music_ = false;
    size_t fileIndex_ = 0;
    // Set while a remote track is loaded but closed: after a failed open (opening
    // it again at once would wait on the same dead server) or at the end of the
    // list. play() opens it at this position.
    std::optional<double> reopenAt_;
    // A remote track that is opening. Its book and file are already book_ and
    // fileIndex_. A newer open, sleep and leaving `screen` drop it (undo).
    struct PendingOpen {
        std::uint64_t screen = 0; // the menu it was chosen in (Origin::Menu); 0: none
        Origin origin = Origin::Menu;
        std::string title; // of the track, for "Cannot play"
        bool autoplay = false; // Play / Pause while it opens changes this
        double position = 0;   // where it opens
        TimePoint since;       // when it was asked for
        Then opened, undo;
    };
    std::optional<PendingOpen> pendingOpen_;

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
