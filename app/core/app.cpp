#include "app.h"

#include "text.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <memory>
#include <utility>

namespace oap {

namespace {
constexpr const char* kVersion = "0.1";
constexpr size_t kSpeechLogSize = 100;

// Setting keys
constexpr const char* kSpeechRate = "speech.rate";
constexpr const char* kVolume = "volume";
constexpr const char* kVerbosity = "verbosity"; // 0 beginner, 1 normal, 2 expert
constexpr const char* kSkipBack = "skip.back";
constexpr const char* kSkipForward = "skip.forward";
constexpr const char* kRewindOnResume = "rewind_on_resume";
constexpr const char* kLongPress = "buttons.long_press_ms";
constexpr const char* kMenuWrap = "menu.wrap";
constexpr const char* kAutoSleep = "power.auto_sleep_min";
constexpr const char* kLastBook = "last_book";
constexpr const char* kServerName = "server.name"; // the Streaming entry on Home

// Lists from the server (docs/streaming.md). Albums come in pages of the most
// a Subsonic server sends at once; Random albums is one page, since each page
// is a new random draw and would repeat albums.
constexpr int kAlbumPageSize = 500;
constexpr int kRandomAlbums = 50;
constexpr const char* kNoServerHelp =
    "To stream from your own music server, add a server section with its url, user and password to "
    "config.yaml, then restart the player.";
constexpr const char* kUnsupportedServerHelp =
    "This player streams from Subsonic servers, such as Navidrome. Check the type in the server section of "
    "config.yaml.";

enum Verbosity { Beginner = 0, Normal = 1, Expert = 2 };

std::vector<std::pair<int, std::string>> range(int from, int to, int step, const std::string& unit) {
    std::vector<std::pair<int, std::string>> out;
    for (int v = from; v <= to; v += step) out.push_back({v, std::to_string(v) + unit});
    return out;
}

std::vector<std::pair<int, std::string>> seconds(std::initializer_list<int> values) {
    std::vector<std::pair<int, std::string>> out;
    for (int v : values) out.push_back({v, v == 0 ? "off" : spokenDuration(v)});
    return out;
}

// "01 - The Storm" -> "The Storm": track numbers are noise when spoken.
std::string chapterTitle(const std::filesystem::path& file) {
    std::string name = file.stem().string();
    size_t i = 0;
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) ++i;
    if (i == 0) return name;
    while (i < name.size() && (name[i] == ' ' || name[i] == '-' || name[i] == '.' || name[i] == '_')) ++i;
    return i < name.size() ? name.substr(i) : name;
}

// The spoken name of part `i`. A remote part is never named after its file:
// that is a stream URL with the server login in it.
std::string partTitle(const Book& book, size_t i) {
    if (i < book.titles.size() && !book.titles[i].empty()) return book.titles[i];
    if (book.remote) return "Track " + std::to_string(i + 1);
    return chapterTitle(book.files[i]);
}

struct ButtonHelp {
    Button button;
    const char* name;
    const char* text;
};

const ButtonHelp kButtonHelp[] = {
    {Button::PlayPause, "Play pause", "Starts or pauses playback. Hold to hear where you are."},
    {Button::Back, "Left", "In menus, lowers a setting. In player, skip back. Hold, or press deeper, for previous chapter or track."},
    {Button::Forward, "Right", "In menus, raises a setting. In player, skip forward. Hold, or press deeper, for next chapter or track."},
    {Button::Up, "Up", "In menus, previous item. Hold for first item. In player, volume up."},
    {Button::Down, "Down", "In menus, next item. Hold for last item. In player, volume down."},
    {Button::Ok, "OK", "Opens the selected item. In player, opens the player menu. Hold to go back, like Home."},
    {Button::Home, "Home", "Goes back to the previous screen. Hold for the home menu."},
    {Button::Talk, "Talk", "Hold and speak a command. Coming later."},
    {Button::Power, "Power", "Press to sleep. Hold for the power menu."},
};

std::string keyHelp(Button button) {
    for (const ButtonHelp& h : kButtonHelp)
        if (h.button == button) return std::string(h.name) + ". " + h.text;
    return "";
}

struct SleepTime {
    const char* label;
    int minutes;
};

const SleepTime kSleepTimes[] = {{"Off", 0},          {"1 minute", 1},     {"15 minutes", 15},
                                 {"30 minutes", 30}, {"45 minutes", 45}, {"1 hour", 60}};

Screen menuScreen(std::string title, std::function<std::vector<MenuItem>()> build) {
    std::vector<MenuItem> items = build();
    return {Screen::Kind::Menu, std::move(title), std::move(items), std::move(build)};
}

Screen playerScreen() { return {Screen::Kind::Player, "Now playing", {}, {}}; }

// "1 track", "3 tracks"
std::string countValue(size_t n, const std::string& unit) { return std::to_string(n) + " " + unit + (n == 1 ? "" : "s"); }

// What a failed request is called in a list, and what the user can do about it.
std::string errorText(MusicServer::Error error) {
    switch (error) {
    case MusicServer::Error::NotConfigured: return "No server set up";
    case MusicServer::Error::Unreachable: return "Server not reachable";
    case MusicServer::Error::BadLogin: return "Wrong user name or password";
    case MusicServer::Error::Server: return "Server error";
    case MusicServer::Error::None: break;
    }
    return "";
}

std::string errorHelp(MusicServer::Error error) {
    switch (error) {
    case MusicServer::Error::NotConfigured: return kNoServerHelp;
    case MusicServer::Error::Unreachable:
        return "Check that Wi-Fi is on and the server is running, then choose Try again.";
    case MusicServer::Error::BadLogin: return "Check the user and password in the server section of config.yaml.";
    case MusicServer::Error::Server: return "The server could not answer. Choose Try again, or try later.";
    case MusicServer::Error::None: break;
    }
    return "";
}

// "navidrome", "0.64.2" -> "Connected to Navidrome 0.64.2."
std::string connectedText(const MusicServer::ServerInfo& info) {
    std::string name = info.type.empty() ? "the server" : info.type;
    if (!info.type.empty()) name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
    return "Connected to " + name + (info.version.empty() ? "" : " " + info.version) + ".";
}

MenuItem action(std::string label, std::function<void()> fn) {
    MenuItem item;
    item.label = std::move(label);
    item.action = std::move(fn);
    return item;
}

MenuItem submenu(std::string label, std::function<std::vector<MenuItem>()> fn) {
    MenuItem item;
    item.label = std::move(label);
    item.submenu = std::move(fn);
    return item;
}
} // namespace

App::App(Speech& speech, MediaPlayer& player, SoundEffects& sounds, VolumeControl& volume, MusicServer& server,
         Settings& config, Settings& state, Clock& clock, std::filesystem::path libraryRoot,
         std::filesystem::path musicRoot)
    : tts_(speech), player_(player), earcons_(sounds), volume_(volume), server_(server), config_(config), state_(state),
      savedState_(state.values()), clock_(clock), libraryRoot_(std::move(libraryRoot)), musicRoot_(std::move(musicRoot)), lastActivity_(clock.now()) {}

// ---------------------------------------------------------------- lifecycle

void App::start() {
    tts_.setRate(config_.getInt(kSpeechRate, 220));
    volume_.setVolume(config_.getInt(kVolume, 70) / 100.0f);
    gestures_.setLongPress(std::chrono::milliseconds(config_.getInt(kLongPress, 600)));
    lastActivity_ = clock_.now();

    earcons_.play(Earcon::Boot);
    say("Ready.");

    std::string last = state_.getString(kLastBook);
    for (const Book& b : scanLibrary(libraryRoot_)) {
        if (!last.empty() && b.path.string() == last && openBook(b, false)) {
            closeScreens(0);
            pushScreen(menuScreen("Home", [this] { return homeMenu(); }));
            pushScreen(playerScreen());
            say(b.title + ", paused. Press play to continue.");
            return;
        }
    }
    goHome();
}

void App::shutdown() {
    saveOnExit();
    player_.close();
}

void App::keyDown(Button button) {
    held_[static_cast<size_t>(button)] = true;
    if (hold_ && button != Button::Power) {
        tts_.stop();
        earcons_.play(Earcon::Error);
        say("Locked.");
        return;
    }
    if (auto ev = gestures_.press(button, clock_.now())) onGesture(*ev);
}

void App::keyUp(Button button) {
    held_[static_cast<size_t>(button)] = false;
    auto ev = gestures_.release(button, clock_.now());
    if (std::exchange(locked_[static_cast<size_t>(button)], false)) return; // held down when Hold was switched on
    if (ev) onGesture(*ev);
}

void App::onGesture(ButtonEvent ev) {
    if (!locked_[static_cast<size_t>(ev.button)]) onButton(ev);
}

void App::setHold(bool on) {
    if (hold_ == on) return;
    hold_ = on;
    // A key already down when Hold is switched on does nothing more: no long
    // press while it is held, no short press when it is let go. Power still works.
    if (on)
        for (size_t i = 0; i < kButtonCount; ++i)
            if (held_[i] && static_cast<Button>(i) != Button::Power) locked_[i] = true;
    tts_.stop();
    earcons_.play(on ? Earcon::Close : Earcon::Open);
    say(on ? "Buttons locked." : "Buttons unlocked.");
}

App::Mode App::mode() const {
    if (sleeping_) return Mode::Sleeping;
    return !screens_.empty() && screens_.back().kind == Screen::Kind::Player ? Mode::Player : Mode::Menu;
}

void App::tick() {
    auto now = clock_.now();
    for (const ButtonEvent& ev : gestures_.poll(now)) onGesture(ev);
    server_.poll(); // answers from the server fill their lists

    // The end of a file is always the end of a chapter, even when the next
    // chapter has the same number (a one-file book or a loose track starts again).
    bool fileFinished = book_ && player_.finished();
    if (fileFinished) onFileFinished();

    if (sleepDeadline_ && now >= *sleepDeadline_) {
        sleepDeadline_.reset();
        sleep("Sleep timer ended.");
    }
    if (sleepAtChapterEnd_ >= 0 && book_ && (fileFinished || currentChapter() != sleepAtChapterEnd_)) {
        sleepAtChapterEnd_ = -1;
        sleep("End of " + unit() + ". Sleep timer ended.");
    }

    int autoSleep = config_.getInt(kAutoSleep, 5);
    if (!sleeping_ && autoSleep > 0 && !player_.playing() && !tts_.speaking() &&
        now - lastActivity_ > std::chrono::minutes(autoSleep))
        sleep("Going to sleep.");
}

// ---------------------------------------------------------------- buttons

void App::onButton(ButtonEvent ev) {
    usage_[static_cast<size_t>(ev.button)][static_cast<size_t>(ev.gesture)]++;
    lastActivity_ = clock_.now();

    // A value changed with Left / Right is written once the user moves on.
    if (ev.button != Button::Back && ev.button != Button::Forward) saveChangedSettings();

    if (sleeping_) {
        wake();
        return;
    }
    // Left / Right on an item without a value do nothing at all, not even stop speech.
    bool idle = !keyDescriber_ && mode() == Mode::Menu && !screens_.empty() &&
                (ev.button == Button::Back || ev.button == Button::Forward) && !canAdjust();
    if (idle) return;
    tts_.stop(); // any other key interrupts speech

    if (keyDescriber_) {
        describeKey(ev);
        return;
    }

    switch (ev.button) {
    case Button::Power:
        if (ev.gesture == Gesture::Short) sleep("Going to sleep.");
        else openPowerMenu();
        return;
    case Button::Talk:
        earcons_.play(Earcon::Error);
        say("Voice input is not available yet.");
        return;
    case Button::PlayPause:
        if (ev.gesture == Gesture::Short) togglePlay();
        else whereAmI();
        return;
    default:
        break;
    }

    if (mode() == Mode::Player) handlePlayer(ev);
    else handleMenu(ev);
}

// OK goes in, Home goes out, Left / Right change a value (docs/navigation.md).
void App::handleMenu(ButtonEvent ev) {
    if (screens_.empty()) {
        goHome();
        return;
    }
    Screen& screen = screens_.back();
    bool longPress = ev.gesture == Gesture::Long;

    switch (ev.button) {
    case Button::Up:
    case Button::Down: {
        if (screen.items.empty()) {
            earcons_.play(Earcon::Edge);
            say(screen.title + ", empty.");
            return;
        }
        long count = long(screen.items.size());
        if (longPress) {
            screen.index = ev.button == Button::Up ? 0 : size_t(count - 1);
            earcons_.play(Earcon::Move);
        } else {
            long next = long(screen.index) + (ev.button == Button::Up ? -1 : 1);
            if (next < 0 || next >= count) {
                if (!config_.getInt(kMenuWrap, 1)) {
                    earcons_.play(Earcon::Edge);
                    say(itemSpeech(screen));
                    return;
                }
                next = (next + count) % count;
                earcons_.play(Earcon::Wrap);
            } else {
                earcons_.play(Earcon::Move);
            }
            screen.index = size_t(next);
        }
        say(itemSpeech(screen));
        return;
    }
    case Button::Back:
    case Button::Forward: // short, long and deep all step the value once
        adjust(ev.button == Button::Back ? -1 : 1);
        return;
    case Button::Ok:
        if (longPress) back();
        else activate();
        return;
    case Button::Home:
        if (longPress) goHome();
        else back();
        return;
    default:
        return;
    }
}

void App::handlePlayer(ButtonEvent ev) {
    switch (ev.button) {
    case Button::Back:
    case Button::Forward: {
        int dir = ev.button == Button::Back ? -1 : 1;
        if (ev.gesture == Gesture::Short) skip(dir);
        else stepChapter(dir); // long press, or the deep stage of a two-stage key
        return;
    }
    case Button::Up:
        changeVolume(+5);
        return;
    case Button::Down:
        changeVolume(-5);
        return;
    case Button::Ok:
        if (ev.gesture == Gesture::Long) back();
        else openMenu("Player menu", [this] { return playerMenu(); });
        return;
    case Button::Home:
        if (ev.gesture == Gesture::Long) goHome();
        else back();
        return;
    default:
        return;
    }
}

void App::describeKey(ButtonEvent ev) {
    if (ev.button == Button::Home && ev.gesture == Gesture::Long) {
        keyDescriber_ = false;
        earcons_.play(Earcon::Close);
        say("Key describer off.");
        return;
    }
    say(keyHelp(ev.button) + (ev.button == Button::Home ? " Hold Home now to leave key describer." : ""));
}

// ---------------------------------------------------------------- speech

void App::say(const std::string& text) {
    speechLog_.push_back(text);
    if (speechLog_.size() > kSpeechLogSize) speechLog_.pop_front();
    tts_.say(text);
}

std::string App::itemSpeech(const Screen& screen) const {
    if (screen.items.empty()) return screen.title + ", empty.";
    const MenuItem& item = screen.items[screen.index];
    int verbosity = config_.getInt(kVerbosity, Normal);
    std::string text = item.label;
    if (item.value) text += ", " + item.value();
    if (verbosity <= Normal)
        text += ", " + std::to_string(screen.index + 1) + " of " + std::to_string(screen.items.size());
    if (verbosity == Beginner) {
        if (item.adjust && item.submenu) text += ". Use left and right to change, or press OK for the list.";
        else if (item.adjust) text += ". Use left and right to change.";
        else if (item.submenu) text += ". Press OK to open.";
        else if (item.action) text += ". Press OK to select.";
    }
    return text;
}

std::string App::nowPlayingSpeech() const {
    std::string text = "Now playing. " + book_->title;
    std::string track = chapters()[size_t(currentChapter())].title;
    if (music_ && track != book_->title) text += ", " + track; // a loose track is its own "album"
    return text + (player_.playing() ? "." : ", paused.");
}

void App::whereAmI() {
    if (mode() == Mode::Player) {
        say(playerStatus());
        return;
    }
    std::string path;
    for (const Screen& s : screens_) path += (path.empty() ? "" : ", ") + s.title;
    say(path + ". " + (screens_.empty() ? "" : itemSpeech(screens_.back())) +
        (book_ ? std::string(player_.playing() ? ". Playing " : ". Paused, ") + book_->title : ""));
}

// ---------------------------------------------------------------- screens

void App::openMenu(const std::string& title, std::function<std::vector<MenuItem>()> build, size_t index) {
    Screen screen = menuScreen(title, std::move(build));
    screen.index = std::min(index, screen.items.empty() ? 0 : screen.items.size() - 1);
    pushScreen(std::move(screen));
    earcons_.play(Earcon::Open);
    say(title + ". " + itemSpeech(screens_.back()));
}

void App::pushScreen(Screen screen) {
    if (ping_) server_.cancel(std::exchange(ping_, 0)); // the user moved on from Server status
    screen.id = ++lastScreenId_;
    screens_.push_back(std::move(screen));
}

// Every way off the stack comes here, so a screen's request never outlives it.
void App::closeScreens(size_t keep) {
    while (screens_.size() > keep) {
        std::uint64_t id = screens_.back().id;
        if (auto list = remoteLists_.find(id); list != remoteLists_.end()) {
            if (list->second.request) server_.cancel(list->second.request);
            remoteLists_.erase(list);
        }
        if (ping_ && pingScreen_ == id) server_.cancel(std::exchange(ping_, 0));
        screens_.pop_back();
    }
}

Screen* App::findScreen(std::uint64_t id) {
    for (Screen& s : screens_)
        if (s.id == id) return &s;
    return nullptr;
}

void App::activate() {
    Screen& screen = screens_.back();
    if (screen.items.empty()) {
        earcons_.play(Earcon::Edge);
        return;
    }
    MenuItem item = screen.items[screen.index]; // copy: the action may change the screen stack
    if (item.submenu) {
        openMenu(item.label, item.submenu, item.selected ? item.selected() : 0);
    } else if (item.action) {
        item.action();
    } else {
        earcons_.play(Earcon::Edge);
    }
}

// Left / Right only ever change a value. On an item without one they do nothing
// at all, so they can never be mistaken for "back" or "open".
bool App::canAdjust() const {
    const Screen& screen = screens_.back();
    return !screen.items.empty() && screen.items[screen.index].adjust;
}

void App::adjust(int direction) {
    if (!canAdjust()) return;
    MenuItem item = screens_.back().items[screens_.back().index];
    earcons_.play(item.adjust(direction) ? Earcon::Move : Earcon::Edge);
    if (item.value) say(item.value());
}

void App::back() {
    if (screens_.size() <= 1) {
        earcons_.play(Earcon::Edge);
        if (!screens_.empty()) say(screens_.back().title + ". " + itemSpeech(screens_.back()));
        return;
    }
    popScreen();
    earcons_.play(Earcon::Close);
    const Screen& screen = screens_.back();
    say(screen.kind == Screen::Kind::Player ? nowPlayingSpeech() : itemSpeech(screen));
}

void App::popScreen() {
    if (screens_.size() <= 1) return; // Home stays
    closeScreens(screens_.size() - 1);
    Screen& screen = screens_.back();
    if (screen.kind == Screen::Kind::Player || !screen.build) return; // a list from the server keeps what loaded
    // Rebuild, so a list shows what changed meanwhile (e.g. "Now playing" on Home), and stay on the same item.
    std::string label = screen.items.empty() ? "" : screen.items[screen.index].label;
    screen.items = screen.build();
    // Same place if the label is still there (labels may repeat), else its first match, else the nearest index.
    size_t index = std::min(screen.index, screen.items.empty() ? 0 : screen.items.size() - 1);
    if (screen.items.empty() || screen.items[index].label != label) {
        for (size_t i = 0; i < screen.items.size(); ++i) {
            if (screen.items[i].label == label) {
                index = i;
                break;
            }
        }
    }
    screen.index = index;
}

void App::goHome() {
    closeScreens(0);
    openMenu("Home", [this] { return homeMenu(); });
}

// There is only one player screen: return to it if it is on the stack, else push it.
void App::enterPlayer() {
    if (!book_) {
        goHome();
        return;
    }
    auto player = std::find_if(screens_.begin(), screens_.end(),
                               [](const Screen& s) { return s.kind == Screen::Kind::Player; });
    if (player != screens_.end()) {
        closeScreens(size_t(player - screens_.begin()) + 1);
        earcons_.play(Earcon::Close);
    } else {
        pushScreen(playerScreen());
        earcons_.play(Earcon::Open);
    }
    say(nowPlayingSpeech());
}

// Like the player, there is only one Power menu: holding Power again returns to it, on Sleep.
void App::openPowerMenu() {
    auto power = std::find_if(screens_.begin(), screens_.end(),
                              [](const Screen& s) { return s.kind == Screen::Kind::Menu && s.title == "Power"; });
    if (power == screens_.end()) {
        openMenu("Power", [this] { return powerMenu(); });
        return;
    }
    closeScreens(size_t(power - screens_.begin()) + 1);
    Screen& menu = screens_.back();
    menu.index = 0;
    earcons_.play(Earcon::Close);
    say(menu.title + ". " + itemSpeech(menu));
}

void App::saveChangedSettings() {
    if (settingsChanged_) saveConfig();
}

void App::saveConfig() {
    settingsChanged_ = false;
    volumeChanged_ = false;
    config_.save();
}

// Written once here instead of on every pause, book change and 30 s of playback,
// to spare the flash. A crash or power loss loses the place since the last exit.
void App::saveOnExit() {
    rememberPosition();
    if (settingsChanged_ || volumeChanged_) saveConfig();
    saveState();
}

// Power off writes, and then the front end's shutdown() would write the same again.
void App::saveState() {
    if (state_.values() == savedState_) return;
    state_.save();
    savedState_ = state_.values();
}

MenuItem App::choice(const std::string& label, const std::string& key,
                     std::vector<std::pair<int, std::string>> options, int fallback,
                     std::function<void(int)> onChange) {
    // Left / Right step through the options and apply each at once, so the user
    // hears the new speech rate or volume; the file is written when they move on.
    // OK opens the whole list, starting on the current value; choosing one saves it and goes back.
    auto current = [this, key, options, fallback]() -> std::optional<size_t> {
        int v = config_.getInt(key, fallback);
        for (size_t i = 0; i < options.size(); ++i)
            if (options[i].first == v) return i;
        return std::nullopt;
    };
    MenuItem item;
    item.label = label;
    item.value = [this, key, options, fallback, current] {
        auto i = current();
        return i ? options[*i].second : std::to_string(config_.getInt(key, fallback));
    };
    item.adjust = [this, key, options, fallback, onChange, current](int dir) {
        long next = -1;
        if (auto i = current()) {
            next = long(*i) + dir;
        } else { // a stored value that is not an option: step to the nearest option on that side
            int v = config_.getInt(key, fallback);
            for (size_t i = 0; i < options.size(); ++i) {
                int o = options[i].first;
                bool onSide = dir > 0 ? o > v : o < v;
                if (onSide && (next < 0 || (dir > 0 ? o < options[size_t(next)].first : o > options[size_t(next)].first)))
                    next = long(i);
            }
        }
        if (next < 0 || next >= long(options.size())) return false;
        int v = options[size_t(next)].first;
        config_.setInt(key, v);
        settingsChanged_ = true;
        if (onChange) onChange(v);
        return true;
    };
    item.selected = [current] { return current().value_or(0); };
    item.submenu = [this, key, options, onChange, current] {
        std::vector<MenuItem> items;
        auto cur = current();
        for (size_t i = 0; i < options.size(); ++i) {
            int v = options[i].first;
            MenuItem option = action(options[i].second, [this, key, v, onChange] {
                config_.setInt(key, v);
                saveConfig();
                if (onChange) onChange(v);
                back();
            });
            if (cur == i) option.value = [] { return std::string("current"); };
            items.push_back(option);
        }
        return items;
    };
    return item;
}

std::vector<MenuItem> App::homeMenu() {
    std::vector<MenuItem> items;
    if (book_) {
        MenuItem now = action("Now playing", [this] { enterPlayer(); });
        now.value = [this] { return book_ ? book_->title : std::string("nothing"); };
        items.push_back(now);
    }
    items.push_back(submenu("Library", [this] { return libraryMenu(); }));
    items.push_back(submenu("Music", [this] { return musicMenu(); }));
    std::string server = config_.getString(kServerName);
    items.push_back(submenu(server.empty() ? "Server" : server, [this] { return streamingMenu(); }));
    items.push_back(submenu("Podcasts", [this] {
        return std::vector<MenuItem>{action("No podcasts yet", [this] { say("Podcast support is coming later."); })};
    }));
    items.push_back(submenu("Settings", [this] { return settingsMenu(); }));
    items.push_back(submenu("System", [this] { return systemMenu(); }));
    items.push_back(submenu("Help", [this] { return helpMenu(); }));
    return items;
}

std::vector<MenuItem> App::libraryMenu() {
    std::vector<MenuItem> items;
    for (const Book& b : scanLibrary(libraryRoot_)) {
        MenuItem item = action(b.title, [this, b] {
            if (openBook(b, true)) enterPlayer();
        });
        if (b.files.size() > 1) {
            size_t n = b.files.size();
            item.value = [n] { return std::to_string(n) + " parts"; };
        }
        items.push_back(item);
    }
    if (items.empty())
        items.push_back(action("Library is empty", [this] {
            say("Copy audio books into " + libraryRoot_.string() + ". One folder per book.");
        }));
    return items;
}

std::vector<MenuItem> App::musicMenu() {
    std::vector<Book> albums = scanLibrary(musicRoot_);
    Book all;
    all.title = "All music";
    all.path = musicRoot_;
    for (const Book& a : albums) all.files.insert(all.files.end(), a.files.begin(), a.files.end());
    if (all.files.empty())
        return {action("No music", [this] { say("Copy music into " + musicRoot_.string() + ". One folder per album."); })};

    auto tracks = [](size_t n) { return [n] { return countValue(n, "track"); }; };
    std::vector<MenuItem> items;
    MenuItem playAll = action("Play all", [this, all] {
        if (playMusic(all, 0)) enterPlayer();
    });
    playAll.value = tracks(all.files.size());
    items.push_back(playAll);
    for (const Book& a : albums) {
        if (a.files.size() == 1 && a.files.front() == a.path) { // a loose track, not a folder
            items.push_back(action(a.title, [this, a] {
                if (playMusic(a, 0)) enterPlayer();
            }));
        } else {
            MenuItem album = submenu(a.title, [this, a] { return albumMenu(a); });
            album.value = tracks(a.files.size());
            items.push_back(album);
        }
    }
    return items;
}

std::vector<MenuItem> App::albumMenu(const Book& album) {
    std::vector<MenuItem> items;
    for (size_t i = 0; i < album.files.size(); ++i) {
        items.push_back(action(partTitle(album, i), [this, album, i] {
            if (playMusic(album, i)) enterPlayer(); // the rest of the album follows
        }));
    }
    return items;
}

// ---------------------------------------------------------------- streaming

std::vector<MenuItem> App::streamingMenu() {
    if (!server_.configured()) {
        if (serverConfigured(config_)) // set up, but not as a server this player can use
            return {action("Server type not supported", [this] { say(kUnsupportedServerHelp); })};
        return {action("No server set up", [this] { say(kNoServerHelp); })};
    }
    std::vector<MenuItem> items;
    items.push_back(action("Recently added", [this] {
        openAlbumList("Recently added", "newest", kAlbumPageSize, false);
    }));
    items.push_back(action("Albums A to Z", [this] {
        openAlbumList("Albums A to Z", "alphabeticalByName", kAlbumPageSize, true);
    }));
    items.push_back(action("Artists", [this] {
        auto toItems = [this](const std::vector<RemoteArtist>& artists) {
            std::vector<MenuItem> out;
            for (const RemoteArtist& a : artists) {
                MenuItem item = action(a.name, [this, a] {
                    openRemoteList(a.name,
                                   fetchPages<RemoteAlbum>(
                                       [this, id = a.id](int, MusicServer::Done<RemoteAlbum> done) {
                                           return server_.artistAlbums(id, std::move(done));
                                       },
                                       [this](const std::vector<RemoteAlbum>& albums) { return albumItems(albums, false); }),
                                   0, "No albums", "The server has no albums by " + a.name + ".");
                });
                if (a.albumCount > 0) item.value = [n = size_t(a.albumCount)] { return countValue(n, "album"); };
                out.push_back(item);
            }
            return out;
        };
        openRemoteList("Artists",
                       fetchPages<RemoteArtist>(
                           [this](int, MusicServer::Done<RemoteArtist> done) { return server_.artists(std::move(done)); },
                           toItems),
                       0, "No artists", "The server has no music yet. Add music to the server, then come back.");
    }));
    items.push_back(action("Playlists", [this] {
        auto toItems = [this](const std::vector<RemotePlaylist>& playlists) {
            std::vector<MenuItem> out;
            for (const RemotePlaylist& p : playlists) {
                MenuItem item = action(p.name, [this, p] {
                    openRemoteList(p.name,
                                   fetchPages<RemoteTrack>(
                                       [this, id = p.id](int, MusicServer::Done<RemoteTrack> done) {
                                           return server_.playlistTracks(id, std::move(done));
                                       },
                                       [this, name = p.name](const std::vector<RemoteTrack>& tracks) {
                                           return trackItems(name, tracks, "Play playlist");
                                       }),
                                   0, "No tracks", "This playlist is empty. Add tracks to it on the server.");
                });
                if (p.songCount > 0) item.value = [n = size_t(p.songCount)] { return countValue(n, "track"); };
                out.push_back(item);
            }
            return out;
        };
        openRemoteList("Playlists",
                       fetchPages<RemotePlaylist>(
                           [this](int, MusicServer::Done<RemotePlaylist> done) { return server_.playlists(std::move(done)); },
                           toItems),
                       0, "No playlists", "Make a playlist on the server, then come back.");
    }));
    items.push_back(action("Random albums", [this] {
        openAlbumList("Random albums", "random", kRandomAlbums, false);
    }));
    items.push_back(action("Server status", [this] { checkServer(); }));
    return items;
}

template <class T>
App::FetchPage App::fetchPages(std::function<MusicServer::RequestId(int offset, MusicServer::Done<T>)> request,
                               std::function<std::vector<MenuItem>(const std::vector<T>&)> toItems) {
    return [request, toItems](int offset, PageDone done) {
        return request(offset, [toItems, done](MusicServer::Error error, std::vector<T> list, bool more) {
            done(error, error == MusicServer::Error::None ? toItems(list) : std::vector<MenuItem>{}, more);
        });
    };
}

// Opens at once with "Loading"; onPage() fills it in. With a page size, the
// following pages are fetched in the background while the server has more.
void App::openRemoteList(const std::string& title, FetchPage fetch, int pageSize, std::string emptyLabel,
                         std::string emptyHelp) {
    pushScreen({Screen::Kind::Menu, title, {}, {}}); // no build: popScreen() keeps what has loaded
    std::uint64_t id = screens_.back().id;
    RemoteList& list = remoteLists_[id];
    list.fetch = std::move(fetch);
    list.pageSize = pageSize;
    list.emptyLabel = std::move(emptyLabel);
    list.emptyHelp = std::move(emptyHelp);
    requestPage(id, 0, true);
    showStatus(id);
    earcons_.play(Earcon::Open);
    say(title + ". " + itemSpeech(screens_.back()));
}

// The callback holds only the screen's id: by the time it runs, the stack may
// have changed. Leaving the screen cancels the request (closeScreens()).
void App::requestPage(std::uint64_t screenId, int offset, bool waiting) {
    auto it = remoteLists_.find(screenId);
    if (it == remoteLists_.end()) return;
    RemoteList& list = it->second;
    list.error = MusicServer::Error::None;
    list.offset = offset;
    list.waiting = waiting;
    list.request = list.fetch(offset, [this, screenId](MusicServer::Error error, std::vector<MenuItem> items, bool more) {
        onPage(screenId, error, std::move(items), more);
    });
}

void App::onPage(std::uint64_t screenId, MusicServer::Error error, std::vector<MenuItem> items, bool more) {
    auto it = remoteLists_.find(screenId);
    Screen* screen = findScreen(screenId);
    if (it == remoteLists_.end() || !screen) return; // its screen is gone (and the request was cancelled)
    RemoteList& list = it->second;
    list.request = 0;
    size_t first = list.loaded;
    // The user is on "Loading" (not on an item that has loaded): move them to what came.
    bool onLoading = list.waiting && screen->index >= first;
    list.waiting = false;
    if (error != MusicServer::Error::None) {
        list.error = error;
    } else {
        size_t count = items.size();
        screen->items.insert(screen->items.begin() + std::ptrdiff_t(list.loaded), std::make_move_iterator(items.begin()),
                             std::make_move_iterator(items.end()));
        list.loaded += count;
        // The next page starts after what came, whatever size the server sends.
        if (list.pageSize > 0 && more && count > 0) requestPage(screenId, list.offset + int(count), false);
    }
    showStatus(screenId);
    if (onLoading) screen->index = std::min(first, screen->items.size() - 1);
    // Filled silently when the user has moved on to another screen, or sleeps.
    if (!onLoading || &screens_.back() != screen || sleeping_) return;
    tts_.stop();
    if (error != MusicServer::Error::None) earcons_.play(Earcon::Error);
    say(itemSpeech(*screen));
}

// After what has loaded comes one of: the error and "Try again", "Loading", or
// why the list is empty. So the list is never empty.
void App::showStatus(std::uint64_t screenId) {
    auto it = remoteLists_.find(screenId);
    Screen* screen = findScreen(screenId);
    if (it == remoteLists_.end() || !screen) return;
    const RemoteList& list = it->second;
    std::vector<MenuItem>& items = screen->items;
    items.erase(items.begin() + std::ptrdiff_t(list.loaded), items.end()); // the old status
    if (list.error != MusicServer::Error::None) {
        MusicServer::Error error = list.error;
        items.push_back(action(errorText(error), [this, error] { say(errorHelp(error)); }));
        items.push_back(action("Try again", [this, screenId] {
            auto again = remoteLists_.find(screenId);
            Screen* screen = findScreen(screenId);
            if (again == remoteLists_.end() || !screen) return;
            requestPage(screenId, again->second.offset, true);
            showStatus(screenId);
            screen->index = screen->items.size() - 1; // on "Loading"
            say(itemSpeech(*screen));
        }));
    } else if (list.waiting) {
        items.push_back(action("Loading", [this] { say("Still loading."); }));
    } else if (list.loaded == 0) {
        items.push_back(action(list.emptyLabel, [this, help = list.emptyHelp] { say(help); }));
    }
    screen->index = std::min(screen->index, items.size() - 1);
}

void App::openAlbumList(const std::string& title, const std::string& listType, int pageSize, bool paged) {
    openRemoteList(title,
                   fetchPages<RemoteAlbum>(
                       [this, listType, pageSize](int offset, MusicServer::Done<RemoteAlbum> done) {
                           return server_.albums(listType, offset, pageSize, std::move(done));
                       },
                       [this](const std::vector<RemoteAlbum>& albums) { return albumItems(albums, true); }),
                   paged ? pageSize : 0, "No albums", "The server has no music yet. Add music to the server, then come back.");
}

void App::openRemoteAlbum(const RemoteAlbum& album) {
    openRemoteList(album.name,
                   fetchPages<RemoteTrack>(
                       [this, id = album.id](int, MusicServer::Done<RemoteTrack> done) {
                           return server_.albumTracks(id, std::move(done));
                       },
                       [this, name = album.name](const std::vector<RemoteTrack>& tracks) {
                           return trackItems(name, tracks, "Play album");
                       }),
                   0, "No tracks", "This album has no tracks on the server.");
}

// `withArtist`: lists of many artists say whose album it is; one artist's list says how long.
std::vector<MenuItem> App::albumItems(const std::vector<RemoteAlbum>& albums, bool withArtist) {
    std::vector<MenuItem> items;
    for (const RemoteAlbum& a : albums) {
        MenuItem item = action(a.name, [this, a] { openRemoteAlbum(a); });
        if (withArtist && !a.artist.empty()) item.value = [artist = a.artist] { return artist; };
        else if (!withArtist && a.songCount > 0) item.value = [n = size_t(a.songCount)] { return countValue(n, "track"); };
        items.push_back(item);
    }
    return items;
}

// "Play album" first, then the tracks: OK on one plays from it to the end, like Music.
std::vector<MenuItem> App::trackItems(const std::string& title, const std::vector<RemoteTrack>& tracks,
                                      const std::string& playAll) {
    if (tracks.empty()) return {};
    auto list = std::make_shared<const std::vector<RemoteTrack>>(tracks); // one copy for all the items
    std::vector<MenuItem> items;
    MenuItem all = action(playAll, [this, title, list] { playRemote(title, *list, 0); });
    all.value = [n = tracks.size()] { return countValue(n, "track"); };
    items.push_back(all);
    for (size_t i = 0; i < tracks.size(); ++i) {
        std::string label = tracks[i].title.empty() ? "Track " + std::to_string(i + 1) : tracks[i].title;
        items.push_back(action(label, [this, title, list, i] { playRemote(title, *list, i); }));
    }
    return items;
}

// Says "Checking" now and the answer when it comes, if the user is still on
// this screen. Leaving the screen cancels it.
void App::checkServer() {
    if (ping_) server_.cancel(std::exchange(ping_, 0));
    std::uint64_t screenId = screens_.back().id;
    pingScreen_ = screenId;
    say("Checking the server.");
    ping_ = server_.ping([this, screenId](MusicServer::Error error, MusicServer::ServerInfo info) {
        ping_ = 0;
        if (sleeping_ || screens_.empty() || screens_.back().id != screenId) return; // the user has moved on
        tts_.stop();
        if (error != MusicServer::Error::None) {
            earcons_.play(Earcon::Error);
            say(errorText(error) + ".");
            return;
        }
        say(connectedText(info));
    });
}

// The entry of sleepTimerMenu() that is set now.
size_t App::sleepTimerIndex() const {
    if (sleepAtChapterEnd_ >= 0) return std::size(kSleepTimes);
    for (size_t i = 0; i < std::size(kSleepTimes); ++i)
        if (kSleepTimes[i].minutes == (sleepDeadline_ ? sleepMinutes_ : 0)) return i;
    return 0;
}

std::vector<MenuItem> App::sleepTimerMenu() {
    auto set = [this](std::string label, int minutes, bool endOfChapter) {
        return action(std::move(label), [this, minutes, endOfChapter] {
            setSleepTimer(minutes, endOfChapter);
            back();
        });
    };
    std::vector<MenuItem> items;
    for (const SleepTime& t : kSleepTimes) items.push_back(set(t.label, t.minutes, false));
    // With nothing loaded there is no chapter to wait for.
    if (book_) items.push_back(set("End of " + unit(), 0, true));
    if (size_t current = sleepTimerIndex(); current < items.size())
        items[current].value = [] { return std::string("current"); };
    return items;
}

std::vector<MenuItem> App::settingsMenu() {
    std::vector<MenuItem> items;
    items.push_back(choice("Speech rate", kSpeechRate, range(100, 460, 30, " words per minute"), 220,
                           [this](int v) { tts_.setRate(v); }));
    items.push_back(choice("Volume", kVolume, range(0, 100, 5, " percent"), 70,
                           [this](int v) { volume_.setVolume(v / 100.0f); }));
    items.push_back(choice("Verbosity", kVerbosity, {{Beginner, "beginner"}, {Normal, "normal"}, {Expert, "expert"}}, Normal));
    items.push_back(choice("Skip back", kSkipBack, seconds({5, 10, 15, 30, 60}), 10));
    items.push_back(choice("Skip forward", kSkipForward, seconds({5, 10, 15, 30, 60, 120}), 30));
    items.push_back(choice("Rewind on resume", kRewindOnResume, seconds({0, 2, 5, 10}), 2));
    items.push_back(choice("Long press time", kLongPress, range(300, 1500, 100, " milliseconds"), 600,
                           [this](int v) { gestures_.setLongPress(std::chrono::milliseconds(v)); }));
    items.push_back(choice("Menu wrap around", kMenuWrap, {{0, "off"}, {1, "on"}}, 1));
    items.push_back(choice("Auto sleep", kAutoSleep,
                           {{1, "1 minute"}, {2, "2 minutes"}, {5, "5 minutes"}, {10, "10 minutes"}, {30, "30 minutes"}, {0, "never"}}, 5));
    MenuItem timer = submenu("Sleep timer", [this] { return sleepTimerMenu(); });
    timer.selected = [this] { return sleepTimerIndex(); };
    timer.value = [this] {
        if (sleepDeadline_) return spokenDuration(std::chrono::duration<double>(*sleepDeadline_ - clock_.now()).count()) + " left";
        return sleepAtChapterEnd_ >= 0 ? "end of " + unit() : std::string("off");
    };
    items.push_back(timer);
    return items;
}

std::vector<MenuItem> App::systemMenu() {
    std::vector<MenuItem> items;
    items.push_back(action("Status", [this] { say(statusReport()); }));
    items.push_back(action("Time and date", [this] {
        char buf[64];
        std::time_t t = clock_.wallTime();
        std::strftime(buf, sizeof buf, "It is %H:%M, %A %d %B.", std::localtime(&t));
        say(buf);
    }));
    items.push_back(submenu("Wi-Fi", [this] {
        return std::vector<MenuItem>{
            action("Status", [this] { say("Connected to Simulated network. Signal good."); }),
            action("Scan for networks", [this] { say("Found 2 networks: Home, Cafe. Joining needs voice input, coming later."); }),
            action("Turn off Wi-Fi", [this] { say("Wi-Fi control is simulated."); }),
        };
    }));
    items.push_back(action("Sync", [this] { say("Sync server not configured."); }));
    items.push_back(action("Software update", [this] { say(std::string("Software is up to date. Version ") + kVersion + "."); }));
    items.push_back(action("About", [this] { say(std::string("Open Audio Player, version ") + kVersion + ". Simulator."); }));
    items.push_back(submenu("Factory reset", [this] {
        return std::vector<MenuItem>{
            action("Cancel", [this] { back(); }),
            action("Erase all settings", [this] { factoryReset(); }),
        };
    }));
    items.push_back(submenu("Power", [this] { return powerMenu(); }));
    return items;
}

std::vector<MenuItem> App::powerMenu() {
    return {
        action("Sleep", [this] {
            popScreen(); // wake up where you were, not in the Power menu
            sleep("Going to sleep.");
        }),
        action("Power off", [this] { powerOff(); }),
        action("Restart", [this] { restart(); }),
        action("Cancel", [this] { back(); }),
    };
}

std::vector<MenuItem> App::helpMenu() {
    std::vector<MenuItem> items;
    items.push_back(action("Getting started", [this] {
        say("This player has no screen. Up and Down move through a menu. OK opens the item, Home goes back, "
            "and holding Home goes to the home menu. Left and Right change a setting. Play pause starts and "
            "stops playback, and holding it tells you where you are. Power puts the player to sleep.");
    }));
    items.push_back(submenu("Buttons", [this] {
        std::vector<MenuItem> buttons;
        for (const ButtonHelp& h : kButtonHelp) {
            Button b = h.button;
            buttons.push_back(action(h.name, [this, b] { say(keyHelp(b)); }));
        }
        return buttons;
    }));
    items.push_back(action("Key describer", [this] {
        keyDescriber_ = true;
        earcons_.play(Earcon::Open);
        say("Key describer on. Press any key to hear what it does. Hold Home to leave.");
    }));
    return items;
}

std::vector<MenuItem> App::playerMenu() {
    std::vector<MenuItem> items;
    MenuItem parts = submenu(music_ ? "Tracks" : "Chapters", [this] { return chaptersMenu(); });
    parts.selected = [this] { return size_t(currentChapter()); };
    items.push_back(parts);
    MenuItem timer = submenu("Sleep timer", [this] { return sleepTimerMenu(); });
    timer.selected = [this] { return sleepTimerIndex(); };
    items.push_back(timer);
    items.push_back(action(music_ ? "Album info" : "Book info", [this] {
        if (!book_) return;
        auto ch = chapters();
        say(book_->title + ". " + std::to_string(ch.size()) + " " + unit() + "s. " + playerStatus());
    }));
    items.push_back(action(music_ ? "Go to first track" : "Go to start of book", [this] {
        bool wasPlaying = player_.playing();
        if (loadFile(0, 0, wasPlaying)) enterPlayer(); // else the error was said: stay in the menu
    }));
    items.push_back(action("Speed", [this] { say("Speed control is coming later."); }));
    items.push_back(action("Add bookmark", [this] { say("Bookmarks are coming later."); }));
    return items;
}

std::vector<MenuItem> App::chaptersMenu() {
    std::vector<MenuItem> items;
    auto list = chapters();
    for (size_t i = 0; i < list.size(); ++i) {
        items.push_back(action(list[i].title, [this, i] {
            if (gotoChapter(int(i))) enterPlayer(); // else the error was said: stay in the list
        }));
    }
    return items;
}

// ---------------------------------------------------------------- playback

bool App::openBook(const Book& book, bool autoplay) {
    if (book_) rememberPosition();
    size_t file = 0;
    double pos = 0;
    std::string saved = state_.getString("pos:" + book.path.string());
    if (auto colon = saved.find(':'); colon != std::string::npos) {
        try {
            file = std::stoul(saved.substr(0, colon));
            pos = std::stod(saved.substr(colon + 1));
        } catch (...) {
            file = 0;
            pos = 0;
        }
        if (file >= book.files.size()) file = 0, pos = 0;
    }
    if (!switchTo(book, false, file, pos, autoplay)) return false;
    state_.setString(kLastBook, book.path.string());
    sleepAtChapterEnd_ = -1;
    return true;
}

// Music plays from the chosen track and is never resumed later, so it neither
// saves a position nor replaces the book that start() resumes.
bool App::playMusic(const Book& album, size_t track) {
    if (book_) rememberPosition();
    if (!switchTo(album, true, track, 0, true)) return false;
    sleepAtChapterEnd_ = -1;
    pausedAt_.reset();
    return true;
}

bool App::playRemote(const std::string& title, const std::vector<RemoteTrack>& tracks, size_t index) {
    if (index >= tracks.size()) {
        earcons_.play(Earcon::Error);
        say("Nothing to play.");
        return false;
    }
    Book list;
    list.title = title;
    list.remote = true;
    for (const RemoteTrack& t : tracks) {
        list.files.emplace_back(server_.streamUrl(t.id));
        list.titles.push_back(t.title);
    }
    if (!playMusic(list, index)) return false;
    enterPlayer();
    return true;
}

// Loads `book` from `file`. If none of its files from there on opens, what was
// loaded before stays loaded, at the same place, so the user is where they were.
bool App::switchTo(const Book& book, bool music, size_t file, double position, bool autoplay) {
    std::optional<Book> previous = std::exchange(book_, book);
    bool wasMusic = std::exchange(music_, music);
    size_t wasFile = fileIndex_;
    bool wasOpen = player_.isOpen(), wasPlaying = player_.playing();
    double wasAt = player_.position();
    if (openFile(file, position, autoplay)) return true;

    book_ = std::move(previous);
    music_ = wasMusic;
    fileIndex_ = wasFile;
    if (book_ && wasOpen && book_->remote) {
        reopenAt_ = wasAt; // its server is likely gone too: open it on Play, not now
    } else if (book_ && wasOpen) {
        if (!player_.open(book_->files[wasFile].string(), wasAt)) book_.reset(), music_ = false;
        else if (wasPlaying && !book.remote) player_.play(); // the server is gone: stay paused
    }
    return false;
}

// Moves to `file` of the loaded book. If a remote track does not open, the
// track that was loaded stays the current one, paused at the same place.
bool App::loadFile(size_t file, double position, bool autoplay, bool announce) {
    size_t wasFile = fileIndex_;
    bool wasOpen = player_.isOpen();
    double wasAt = player_.position();
    if (openFile(file, position, autoplay, announce)) return true;
    if (book_ && book_->remote) {
        fileIndex_ = wasFile;
        if (wasOpen) reopenAt_ = wasAt; // opened on Play: not a second wait on the server now
    }
    return false;
}

// Opens `file`, or the next one that opens: a file that cannot be read is
// reported and skipped, so one bad file does not stop a book or an album. A
// remote track is tried once: when the server cannot be reached, every
// following track would block the device too.
bool App::openFile(size_t file, double position, bool autoplay, bool announce) {
    if (!book_) return false;
    for (; file < book_->files.size(); ++file, position = 0) {
        fileIndex_ = file;
        // The open waits for the server on this thread (up to 10 s): say why nothing answers.
        if (book_->remote && announce) say("Loading.");
        if (player_.open(book_->files[file].string(), position)) {
            reopenAt_.reset();
            if (autoplay) player_.play();
            return true;
        }
        earcons_.play(Earcon::Error);
        if (book_->remote) { // never say the URL: it holds the login
            say("Cannot play " + partTitle(*book_, file) + ". Server not reachable.");
            return false;
        }
        say("Cannot open " + book_->files[file].filename().string());
    }
    return false;
}

void App::togglePlay() {
    if (player_.playing()) pause(true);
    else play();
}

void App::play() {
    if (book_ && !player_.isOpen() && reopenAt_ && !openFile(fileIndex_, *reopenAt_, false))
        return; // said "Server not reachable"
    if (!book_ || !player_.isOpen()) {
        earcons_.play(Earcon::Error);
        say("Nothing to play. Choose a book in the library.");
        return;
    }
    int rewind = config_.getInt(kRewindOnResume, 2);
    if (rewind > 0 && pausedAt_ && clock_.now() - *pausedAt_ > std::chrono::seconds(10))
        player_.seek(player_.position() - rewind);
    pausedAt_.reset();
    player_.play();
    earcons_.play(Earcon::Play);
}

void App::pause(bool announce) {
    if (!player_.playing()) return;
    player_.pause();
    pausedAt_ = clock_.now();
    rememberPosition();
    if (announce) say("Paused.");
}

void App::skip(int direction) {
    if (!player_.isOpen()) {
        earcons_.play(Earcon::Edge);
        return;
    }
    int amount = direction < 0 ? config_.getInt(kSkipBack, 10) : config_.getInt(kSkipForward, 30);
    double target = player_.position() + direction * amount;
    if (target < 0 || (player_.duration() > 0 && target >= player_.duration())) {
        earcons_.play(Earcon::Edge);
        target = std::clamp(target, 0.0, std::max(0.0, player_.duration() - 1));
    } else {
        earcons_.play(Earcon::Move);
    }
    player_.seek(target);
    if (!player_.playing())
        say((direction < 0 ? "Back " : "Forward ") + spokenDuration(amount) + ".");
}

void App::changeVolume(int delta) {
    int v = std::clamp(config_.getInt(kVolume, 70) + delta, 0, 100);
    config_.setInt(kVolume, v);
    volumeChanged_ = true; // a config.yaml write per press would wear the flash
    volume_.setVolume(v / 100.0f);
    earcons_.play(Earcon::Volume);
    if (v == 100) say("Maximum volume.");
    else if (v == 0) say("Muted.");
    else if (!player_.playing()) say("Volume " + std::to_string(v) + " percent.");
}

std::vector<App::ChapterRef> App::chapters() const {
    std::vector<ChapterRef> out;
    if (!book_) return out;
    if (book_->files.size() > 1) {
        for (size_t i = 0; i < book_->files.size(); ++i)
            out.push_back({partTitle(*book_, i), i, 0});
    } else if (!player_.chapters().empty()) {
        for (const Chapter& c : player_.chapters()) out.push_back({c.title, 0, c.start});
    } else {
        out.push_back({book_->remote ? partTitle(*book_, 0) : book_->title, 0, 0});
    }
    return out;
}

int App::currentChapter() const {
    if (!book_) return 0;
    if (book_->files.size() > 1) return int(fileIndex_);
    double pos = player_.position() + 0.25;
    int cur = 0;
    const auto& list = player_.chapters();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].start <= pos) cur = int(i);
    return cur;
}

bool App::gotoChapter(int index) {
    auto list = chapters();
    if (index < 0 || index >= int(list.size())) return false;
    const ChapterRef& ch = list[size_t(index)];
    if (ch.file != fileIndex_ || !player_.isOpen()) { // closed: a remote track that failed or ended the list
        if (!loadFile(ch.file, ch.start, player_.playing())) return false;
    } else {
        player_.seek(ch.start);
    }
    if (sleepAtChapterEnd_ >= 0) sleepAtChapterEnd_ = index;
    return true;
}

void App::stepChapter(int direction) {
    auto list = chapters();
    int cur = currentChapter();
    int target = cur + direction;
    if (direction < 0) {
        // Like a CD player: first press goes to the start of the current chapter.
        double intoChapter = player_.position() - list[size_t(cur)].start;
        if (intoChapter > 3) target = cur;
    }
    if (target < 0 || target >= int(list.size())) {
        earcons_.play(Earcon::Edge);
        say(target < 0 ? "First " + unit() + "." : "Last " + unit() + ".");
        return;
    }
    if (!gotoChapter(target)) return; // the error was said
    earcons_.play(Earcon::Move);
    say(list[size_t(target)].title);
}

void App::onFileFinished() {
    if (fileIndex_ + 1 < book_->files.size()) {
        // Quietly: "Loading." between every two tracks of an album would be too much.
        if (loadFile(fileIndex_ + 1, 0, true, false)) return;
        if (book_->remote) return; // "Server not reachable" was said: stop here
    }
    player_.pause();
    if (book_->remote) { // next play starts the list again, opening it only then
        player_.close();
        fileIndex_ = 0;
        reopenAt_ = 0.0;
    } else {
        loadFile(0, 0, false); // next play starts the book again
    }
    rememberPosition();
    earcons_.play(Earcon::Close);
    say(music_ ? "End of music. Press play to start again." : "End of book. Press play to start again.");
}

void App::rememberPosition() {
    if (!book_ || music_ || !player_.isOpen()) return;
    char pos[32];
    std::snprintf(pos, sizeof pos, "%.1f", player_.position());
    state_.setString("pos:" + book_->path.string(), std::to_string(fileIndex_) + ":" + pos);
}

std::string App::playerStatus() const {
    if (!book_) return "Nothing playing.";
    auto list = chapters();
    int cur = currentChapter();
    double pos = player_.position();
    std::string text = book_->title + ". ";
    if (list.size() > 1)
        text += std::string(music_ ? "Track " : "Chapter ") + std::to_string(cur + 1) + " of " + std::to_string(list.size()) + ", " +
                list[size_t(cur)].title + ". ";
    double chapterEnd = player_.duration();
    if (book_->files.size() == 1 && size_t(cur) + 1 < list.size()) chapterEnd = list[size_t(cur) + 1].start;
    text += spokenDuration(chapterEnd - pos) + " left in " + unit() + ". ";
    if (!music_ && book_->files.size() == 1 && list.size() > 1)
        text += spokenDuration(player_.duration() - pos) + " left in book. ";
    text += player_.playing() ? "Playing." : "Paused.";
    if (sleepDeadline_)
        text += " Sleep timer, " + spokenDuration(std::chrono::duration<double>(*sleepDeadline_ - clock_.now()).count()) + " left.";
    return text;
}

App::NowPlaying App::nowPlaying() const {
    NowPlaying np;
    if (!book_) return np;
    auto list = chapters();
    int cur = currentChapter();
    np.loaded = true;
    np.playing = player_.playing();
    np.title = book_->title;
    np.chapter = list.empty() ? "" : list[size_t(cur)].title;
    np.chapterIndex = cur;
    np.chapterCount = int(list.size());
    np.position = player_.position();
    np.duration = player_.duration();
    if (sleepDeadline_) np.sleepTimerLeft = std::chrono::duration<double>(*sleepDeadline_ - clock_.now()).count();
    return np;
}

// ---------------------------------------------------------------- power

void App::setSleepTimer(int minutes, bool endOfChapter) {
    sleepDeadline_.reset();
    sleepAtChapterEnd_ = -1;
    if (endOfChapter) {
        sleepAtChapterEnd_ = currentChapter();
        say("Sleep timer set to end of " + unit() + ".");
    } else if (minutes > 0) {
        sleepMinutes_ = minutes;
        sleepDeadline_ = clock_.now() + std::chrono::minutes(minutes);
        say("Sleep timer set to " + spokenDuration(minutes * 60) + ".");
    } else {
        say("Sleep timer off.");
    }
}

void App::sleep(const std::string& announcement) {
    pause(false);
    saveOnExit(); // sleep on the device may end in the power being cut
    sleepDeadline_.reset();
    sleepAtChapterEnd_ = -1;
    sleeping_ = true;
    earcons_.play(Earcon::Sleep);
    say(announcement);
}

void App::wake() {
    tts_.stop();
    sleeping_ = false;
    earcons_.play(Earcon::Wake);
    if (mode() == Mode::Player && book_) {
        auto list = chapters();
        say(book_->title + ", paused. " + list[size_t(currentChapter())].title + ".");
    } else if (!screens_.empty()) {
        say(screens_.back().title + ". " + itemSpeech(screens_.back()));
    } else {
        goHome();
    }
}

void App::powerOff() {
    pause(false);
    saveOnExit();
    earcons_.play(Earcon::Sleep);
    say("Powering off. Goodbye.");
    quit_ = true;
}

void App::restart(bool saveFirst) {
    if (saveFirst) saveOnExit();
    player_.close();
    book_.reset();
    music_ = false;
    closeScreens(0);
    sleepDeadline_.reset();
    sleepAtChapterEnd_ = -1;
    keyDescriber_ = false;
    say("Restarting.");
    start();
}

// Clears what the user set and the state, but keeps the server: a blind user
// must not lose a server someone else set up for them.
void App::factoryReset() {
    config_.clearExcept(kServerPrefix);
    state_.clear();
    saveConfig();
    saveState();
    say("Settings erased.");
    restart(false); // saving first would put the position straight back
}

std::string App::statusReport() const {
    std::string text = "Battery 80 percent, simulated. ";
    std::error_code ec;
    auto space = std::filesystem::space(libraryRoot_, ec);
    if (!ec) text += std::to_string(space.available / (1024 * 1024 * 1024)) + " gigabytes free. ";
    text += "Wi-Fi connected. ";
    char buf[32];
    std::time_t t = clock_.wallTime();
    std::strftime(buf, sizeof buf, "Time %H:%M.", std::localtime(&t));
    return text + buf;
}

} // namespace oap
