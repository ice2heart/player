#include "app.h"

#include "text.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

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

App::App(Speech& speech, MediaPlayer& player, SoundEffects& sounds, VolumeControl& volume, Settings& settings,
         Clock& clock, std::filesystem::path libraryRoot)
    : tts_(speech), player_(player), earcons_(sounds), volume_(volume), settings_(settings), clock_(clock),
      libraryRoot_(std::move(libraryRoot)), lastActivity_(clock.now()), lastSave_(clock.now()) {}

// ---------------------------------------------------------------- lifecycle

void App::start() {
    tts_.setRate(settings_.getInt(kSpeechRate, 220));
    volume_.setVolume(settings_.getInt(kVolume, 70) / 100.0f);
    gestures_.setLongPress(std::chrono::milliseconds(settings_.getInt(kLongPress, 600)));
    lastActivity_ = clock_.now();

    earcons_.play(Earcon::Boot);
    say("Ready.");

    std::string last = settings_.getString(kLastBook);
    for (const Book& b : scanLibrary(libraryRoot_)) {
        if (!last.empty() && b.path.string() == last) {
            openBook(b, false);
            mode_ = Mode::Player;
            say(b.title + ", paused. Press play to continue.");
            return;
        }
    }
    goHome();
}

void App::shutdown() {
    savePosition();
    player_.close();
}

void App::keyDown(Button button) {
    if (hold_ && button != Button::Power) {
        tts_.stop();
        earcons_.play(Earcon::Error);
        say("Locked.");
        return;
    }
    gestures_.press(button, clock_.now());
}

void App::keyUp(Button button) {
    if (auto ev = gestures_.release(button, clock_.now())) onButton(*ev);
}

void App::setHold(bool on) {
    if (hold_ == on) return;
    hold_ = on;
    tts_.stop();
    earcons_.play(on ? Earcon::Close : Earcon::Open);
    say(on ? "Buttons locked." : "Buttons unlocked.");
}

void App::tick() {
    auto now = clock_.now();
    for (const ButtonEvent& ev : gestures_.poll(now)) onButton(ev);

    if (book_ && player_.finished()) onFileFinished();

    if (sleepDeadline_ && now >= *sleepDeadline_) {
        sleepDeadline_.reset();
        sleep("Sleep timer ended.");
    }
    if (sleepAtChapterEnd_ >= 0 && book_ && currentChapter() != sleepAtChapterEnd_) {
        sleepAtChapterEnd_ = -1;
        sleep("End of chapter. Sleep timer ended.");
    }

    if (player_.playing() && now - lastSave_ > std::chrono::seconds(30)) savePosition();

    int autoSleep = settings_.getInt(kAutoSleep, 5);
    if (mode_ != Mode::Sleeping && autoSleep > 0 && !player_.playing() && !tts_.speaking() &&
        now - lastActivity_ > std::chrono::minutes(autoSleep))
        sleep("Going to sleep.");
}

// ---------------------------------------------------------------- buttons

void App::onButton(ButtonEvent ev) {
    usage_[static_cast<size_t>(ev.button)][static_cast<size_t>(ev.gesture)]++;
    lastActivity_ = clock_.now();

    if (mode_ == Mode::Sleeping) {
        wake();
        return;
    }
    tts_.stop(); // any key interrupts speech

    if (keyDescriber_) {
        describeKey(ev);
        return;
    }

    switch (ev.button) {
    case Button::Power:
        if (ev.gesture == Gesture::Short) sleep("Going to sleep.");
        else openMenu("Power", powerMenu());
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

    if (mode_ == Mode::Menu) handleMenu(ev);
    else handlePlayer(ev);
}

void App::handleMenu(ButtonEvent ev) {
    if (menu_.empty()) {
        goHome();
        return;
    }
    MenuLevel& level = menu_.back();
    bool longPress = ev.gesture == Gesture::Long;

    switch (ev.button) {
    case Button::Up:
    case Button::Down: {
        if (level.items.empty()) {
            earcons_.play(Earcon::Edge);
            say(level.title + ", empty.");
            return;
        }
        long count = long(level.items.size());
        if (longPress) {
            level.index = ev.button == Button::Up ? 0 : size_t(count - 1);
            earcons_.play(Earcon::Move);
        } else {
            long next = long(level.index) + (ev.button == Button::Up ? -1 : 1);
            if (next < 0 || next >= count) {
                if (!settings_.getInt(kMenuWrap, 1)) {
                    earcons_.play(Earcon::Edge);
                    say(itemSpeech(level));
                    return;
                }
                next = (next + count) % count;
                earcons_.play(Earcon::Wrap);
            } else {
                earcons_.play(Earcon::Move);
            }
            level.index = size_t(next);
        }
        say(itemSpeech(level));
        return;
    }
    case Button::Back:
    case Button::Forward: {
        int dir = ev.button == Button::Back ? -1 : 1;
        const MenuItem* item = level.items.empty() ? nullptr : &level.items[level.index];
        if (item && item->adjust) {
            auto adjust = item->adjust;
            auto value = item->value;
            adjust(dir);
            earcons_.play(Earcon::Move);
            if (value) say(value());
        } else if (dir < 0) {
            back(); // Left = out, like a file browser
        } else {
            activate(); // Right = in
        }
        return;
    }
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
    bool longPress = ev.gesture == Gesture::Long;
    switch (ev.button) {
    case Button::Back:
    case Button::Forward: {
        int dir = ev.button == Button::Back ? -1 : 1;
        if (longPress) stepChapter(dir);
        else skip(dir);
        return;
    }
    case Button::Up:
        changeVolume(+5);
        return;
    case Button::Down:
        changeVolume(-5);
        return;
    case Button::Ok:
        openMenu("Player menu", playerMenu());
        return;
    case Button::Home:
        goHome();
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
    switch (ev.button) {
    case Button::PlayPause: say("Play pause. Starts or pauses playback. Hold to hear where you are."); break;
    case Button::Back: say("Left. In player, skip back. Hold for previous chapter. In menus, go back, or decrease a value."); break;
    case Button::Forward: say("Right. In player, skip forward. Hold for next chapter. In menus, open, or increase a value."); break;
    case Button::Up: say("Up. In menus, previous item. Hold for first item. In player, volume up."); break;
    case Button::Down: say("Down. In menus, next item. Hold for last item. In player, volume down."); break;
    case Button::Ok: say("OK. Opens the selected item. In player, opens the player menu. Hold to go back."); break;
    case Button::Home: say("Home. Goes back one level. Hold for the home menu. Hold Home now to leave key describer."); break;
    case Button::Talk: say("Talk. Hold and speak a command. Coming later."); break;
    case Button::Power: say("Power. Press to sleep. Hold for the power menu."); break;
    case Button::Count: break;
    }
}

// ---------------------------------------------------------------- speech

void App::say(const std::string& text) {
    speechLog_.push_back(text);
    if (speechLog_.size() > kSpeechLogSize) speechLog_.pop_front();
    tts_.say(text);
}

std::string App::itemSpeech(const MenuLevel& level) const {
    if (level.items.empty()) return level.title + ", empty.";
    const MenuItem& item = level.items[level.index];
    int verbosity = settings_.getInt(kVerbosity, Normal);
    std::string text = item.label;
    if (item.value) text += ", " + item.value();
    if (verbosity <= Normal)
        text += ", " + std::to_string(level.index + 1) + " of " + std::to_string(level.items.size());
    if (verbosity == Beginner) {
        if (item.submenu) text += ". Press OK to open.";
        else if (item.adjust) text += ". Use left and right to change.";
        else if (item.action) text += ". Press OK to select.";
    }
    return text;
}

void App::whereAmI() {
    if (mode_ == Mode::Player) {
        say(playerStatus());
        return;
    }
    std::string path;
    for (const MenuLevel& l : menu_) path += (path.empty() ? "" : ", ") + l.title;
    say(path + ". " + (menu_.empty() ? "" : itemSpeech(menu_.back())) +
        (book_ ? std::string(player_.playing() ? ". Playing " : ". Paused, ") + book_->title : ""));
}

// ---------------------------------------------------------------- menus

void App::openMenu(const std::string& title, std::vector<MenuItem> items, size_t index) {
    mode_ = Mode::Menu;
    menu_.push_back({title, std::move(items), 0});
    MenuLevel& level = menu_.back();
    level.index = std::min(index, level.items.empty() ? 0 : level.items.size() - 1);
    earcons_.play(Earcon::Open);
    say(title + ". " + itemSpeech(level));
}

void App::activate() {
    MenuLevel& level = menu_.back();
    if (level.items.empty()) {
        earcons_.play(Earcon::Edge);
        return;
    }
    MenuItem item = level.items[level.index]; // copy: the action may change the menu stack
    if (item.submenu) {
        openMenu(item.label, item.submenu());
    } else if (item.action) {
        item.action();
    } else if (item.adjust) {
        say(item.label + ", " + item.value() + ". Use left and right to change.");
    } else {
        earcons_.play(Earcon::Edge);
    }
}

void App::back() {
    if (menu_.size() > 1) {
        menu_.pop_back();
        earcons_.play(Earcon::Close);
        say(itemSpeech(menu_.back()));
    } else if (book_) {
        enterPlayer();
    } else if (menu_.empty() || menu_.front().title != "Home") {
        goHome();
    } else {
        earcons_.play(Earcon::Edge);
        say("Home. " + itemSpeech(menu_.back()));
    }
}

void App::goHome() {
    menu_.clear();
    openMenu("Home", homeMenu());
}

void App::enterPlayer() {
    if (!book_) {
        goHome();
        return;
    }
    menu_.clear();
    mode_ = Mode::Player;
    earcons_.play(Earcon::Close);
    say("Now playing. " + book_->title + (player_.playing() ? "." : ", paused."));
}

MenuItem App::choice(const std::string& label, const std::string& key,
                     std::vector<std::pair<int, std::string>> options, int fallback,
                     std::function<void(int)> onChange) {
    MenuItem item;
    item.label = label;
    item.value = [this, key, options, fallback] {
        int v = settings_.getInt(key, fallback);
        for (const auto& [val, name] : options)
            if (val == v) return name;
        return std::to_string(v);
    };
    item.adjust = [this, key, options, fallback, onChange](int dir) {
        int v = settings_.getInt(key, fallback);
        long idx = 0;
        for (size_t i = 0; i < options.size(); ++i)
            if (options[i].first == v) idx = long(i);
        long next = idx + dir;
        if (next < 0 || next >= long(options.size())) {
            earcons_.play(Earcon::Edge);
            return;
        }
        settings_.setInt(key, options[size_t(next)].first);
        settings_.save();
        if (onChange) onChange(options[size_t(next)].first);
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
    items.push_back(submenu("Podcasts", [this] {
        return std::vector<MenuItem>{action("No podcasts yet", [this] { say("Podcast support is coming later."); })};
    }));
    MenuItem timer = submenu("Sleep timer", [this] { return sleepTimerMenu(); });
    timer.value = [this] {
        if (sleepDeadline_) return spokenDuration(std::chrono::duration<double>(*sleepDeadline_ - clock_.now()).count()) + " left";
        return std::string(sleepAtChapterEnd_ >= 0 ? "end of chapter" : "off");
    };
    items.push_back(timer);
    items.push_back(submenu("Settings", [this] { return settingsMenu(); }));
    items.push_back(submenu("System", [this] { return systemMenu(); }));
    return items;
}

std::vector<MenuItem> App::libraryMenu() {
    std::vector<MenuItem> items;
    for (const Book& b : scanLibrary(libraryRoot_)) {
        MenuItem item = action(b.title, [this, b] {
            openBook(b, true);
            enterPlayer();
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

std::vector<MenuItem> App::sleepTimerMenu() {
    auto set = [this](std::string label, int minutes, bool endOfChapter) {
        return action(std::move(label), [this, minutes, endOfChapter] {
            setSleepTimer(minutes, endOfChapter);
            back();
        });
    };
    return {set("Off", 0, false),          set("1 minute", 1, false),   set("15 minutes", 15, false),
            set("30 minutes", 30, false),  set("45 minutes", 45, false), set("1 hour", 60, false),
            set("End of chapter", 0, true)};
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
    items.push_back(action("Key describer", [this] {
        keyDescriber_ = true;
        earcons_.play(Earcon::Open);
        say("Key describer on. Press any key to hear what it does. Hold Home to leave.");
    }));
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
            action("Erase all settings", [this] {
                settings_.clear();
                settings_.save();
                say("Settings erased.");
                restart();
            }),
        };
    }));
    items.push_back(submenu("Power", [this] { return powerMenu(); }));
    return items;
}

std::vector<MenuItem> App::powerMenu() {
    return {
        action("Sleep", [this] { sleep("Going to sleep."); }),
        action("Power off", [this] { powerOff(); }),
        action("Restart", [this] { restart(); }),
        action("Cancel", [this] { back(); }),
    };
}

std::vector<MenuItem> App::playerMenu() {
    std::vector<MenuItem> items;
    // An action rather than a submenu so the list opens on the current chapter.
    items.push_back(action("Chapters", [this] { openMenu("Chapters", chaptersMenu(), size_t(currentChapter())); }));
    items.push_back(submenu("Sleep timer", [this] { return sleepTimerMenu(); }));
    items.push_back(action("Book info", [this] {
        if (!book_) return;
        auto ch = chapters();
        say(book_->title + ". " + std::to_string(ch.size()) + " chapters. " + playerStatus());
    }));
    items.push_back(action("Go to start of book", [this] {
        bool wasPlaying = player_.playing();
        loadFile(0, 0, wasPlaying);
        enterPlayer();
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
            gotoChapter(int(i));
            enterPlayer();
        }));
    }
    return items;
}

// ---------------------------------------------------------------- playback

void App::openBook(const Book& book, bool autoplay) {
    if (book_) savePosition();
    book_ = book;
    size_t file = 0;
    double pos = 0;
    std::string saved = settings_.getString("pos:" + book.path.string());
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
    settings_.setString(kLastBook, book.path.string());
    settings_.save();
    sleepAtChapterEnd_ = -1;
    loadFile(file, pos, autoplay);
}

bool App::loadFile(size_t file, double position, bool autoplay) {
    if (!book_ || file >= book_->files.size()) return false;
    fileIndex_ = file;
    if (!player_.open(book_->files[file].string(), position)) {
        earcons_.play(Earcon::Error);
        say("Cannot open " + book_->files[file].filename().string());
        return false;
    }
    if (autoplay) player_.play();
    return true;
}

void App::togglePlay() {
    if (player_.playing()) pause(true);
    else play();
}

void App::play() {
    if (!book_ || !player_.isOpen()) {
        earcons_.play(Earcon::Error);
        say("Nothing to play. Choose a book in the library.");
        return;
    }
    int rewind = settings_.getInt(kRewindOnResume, 2);
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
    savePosition();
    if (announce) say("Paused.");
}

void App::skip(int direction) {
    if (!player_.isOpen()) {
        earcons_.play(Earcon::Edge);
        return;
    }
    int amount = direction < 0 ? settings_.getInt(kSkipBack, 10) : settings_.getInt(kSkipForward, 30);
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
    int v = std::clamp(settings_.getInt(kVolume, 70) + delta, 0, 100);
    settings_.setInt(kVolume, v);
    settings_.save();
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
            out.push_back({chapterTitle(book_->files[i]), i, 0});
    } else if (!player_.chapters().empty()) {
        for (const Chapter& c : player_.chapters()) out.push_back({c.title, 0, c.start});
    } else {
        out.push_back({book_->title, 0, 0});
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

void App::gotoChapter(int index) {
    auto list = chapters();
    if (index < 0 || index >= int(list.size())) return;
    const ChapterRef& ch = list[size_t(index)];
    if (ch.file != fileIndex_) loadFile(ch.file, ch.start, player_.playing());
    else player_.seek(ch.start);
    if (sleepAtChapterEnd_ >= 0) sleepAtChapterEnd_ = index;
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
        say(target < 0 ? "First chapter." : "Last chapter.");
        return;
    }
    gotoChapter(target);
    earcons_.play(Earcon::Move);
    say(list[size_t(target)].title);
}

void App::onFileFinished() {
    if (fileIndex_ + 1 < book_->files.size()) {
        loadFile(fileIndex_ + 1, 0, true);
        return;
    }
    player_.pause();
    loadFile(0, 0, false); // next play starts the book again
    savePosition();
    earcons_.play(Earcon::Close);
    say("End of book. Press play to start again.");
}

void App::savePosition() {
    lastSave_ = clock_.now();
    if (!book_ || !player_.isOpen()) return;
    char pos[32];
    std::snprintf(pos, sizeof pos, "%.1f", player_.position());
    settings_.setString("pos:" + book_->path.string(), std::to_string(fileIndex_) + ":" + pos);
    settings_.save();
}

std::string App::playerStatus() const {
    if (!book_) return "Nothing playing.";
    auto list = chapters();
    int cur = currentChapter();
    double pos = player_.position();
    std::string text = book_->title + ". ";
    if (list.size() > 1)
        text += "Chapter " + std::to_string(cur + 1) + " of " + std::to_string(list.size()) + ", " +
                list[size_t(cur)].title + ". ";
    double chapterEnd = player_.duration();
    if (book_->files.size() == 1 && size_t(cur) + 1 < list.size()) chapterEnd = list[size_t(cur) + 1].start;
    text += spokenDuration(chapterEnd - pos) + " left in chapter. ";
    if (book_->files.size() == 1 && list.size() > 1)
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
        say("Sleep timer set to end of chapter.");
    } else if (minutes > 0) {
        sleepDeadline_ = clock_.now() + std::chrono::minutes(minutes);
        say("Sleep timer set to " + spokenDuration(minutes * 60) + ".");
    } else {
        say("Sleep timer off.");
    }
}

void App::sleep(const std::string& announcement) {
    pause(false);
    savePosition();
    sleepDeadline_.reset();
    sleepAtChapterEnd_ = -1;
    modeBeforeSleep_ = mode_;
    mode_ = Mode::Sleeping;
    earcons_.play(Earcon::Sleep);
    say(announcement);
}

void App::wake() {
    tts_.stop();
    mode_ = modeBeforeSleep_;
    earcons_.play(Earcon::Wake);
    if (mode_ == Mode::Player && book_) {
        auto list = chapters();
        say(book_->title + ", paused. " + list[size_t(currentChapter())].title + ".");
    } else if (!menu_.empty()) {
        say(menu_.back().title + ". " + itemSpeech(menu_.back()));
    } else {
        goHome();
    }
}

void App::powerOff() {
    pause(false);
    savePosition();
    earcons_.play(Earcon::Sleep);
    say("Powering off. Goodbye.");
    quit_ = true;
}

void App::restart() {
    savePosition();
    player_.close();
    book_.reset();
    menu_.clear();
    sleepDeadline_.reset();
    sleepAtChapterEnd_ = -1;
    keyDescriber_ = false;
    say("Restarting.");
    start();
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
