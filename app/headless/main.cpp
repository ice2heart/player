// Headless front end: runs the player on the target device (or any Linux box)
// with no window. Buttons come from evdev input devices, from the terminal
// (handy over SSH) or from --script; everything spoken is also printed to stdout.

#include "config/disk_text_files.h"
#include "config/settings_files.h"
#include "core/app.h"
#include "core/earcons.h"
#include "core/mixer.h"
#include "frontend/make_music_server.h"
#include "frontend/presses.h"
#include "frontend/terminal_keys.h"
#include "frontend/threaded_scanner.h"
#include "library/sqlite_library_store.h"
#include "headless/evdev_input.h"
#include "media/player.h"
#include "media/tag_reader.h"
#include "media/tts.h"
#include "platform/audio_output.h"

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>

using namespace oap;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop = true; }

// Puts the terminal into unbuffered, no-echo mode and restores it on exit.
// Ctrl+C still raises SIGINT, which shuts down cleanly.
class RawTerminal {
public:
    RawTerminal() {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_) != 0) return;
        termios raw = saved_;
        raw.c_lflag &= ~tcflag_t(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        active_ = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
    ~RawTerminal() {
        if (active_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
    }

private:
    termios saved_{};
    bool active_ = false;
};

std::filesystem::path defaultConfig() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "oap/config.yaml";
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / ".config/oap/config.yaml";
}

void usage() {
    std::string audio;
    for (const auto& name : AudioOutput::backends()) audio += (audio.empty() ? "" : "|") + name;
    std::printf(
        "usage: oap-headless [options] [LIBRARY_DIR]\n"
        "  --config FILE        settings and server (default ~/.config/oap/config.yaml)\n"
        "  --state FILE         positions and the book to resume (default state.tsv next to the config)\n"
        "  --music DIR          music folder (default ./music)\n"
        "  --cache DIR          downloaded tracks (default cache/ next to the state file)\n"
        "  --library-db FILE    library index (default library.db next to the state file)\n"
        "  --audio NAME         %s (default auto); alsa:DEVICE picks an ALSA PCM\n"
        "  --input LIST         comma-separated: evdev, terminal, none\n"
        "                       (default: " OAP_HEADLESS_TTY_INPUT " if stdin is a terminal, else evdev)\n"
        "  --evdev-device PATH  read only this input device instead of all of /dev/input\n"
        "  --script STEPS       press buttons automatically, e.g. \"ok wait:2000 play quit\"\n",
        audio.c_str());
}

void printTerminalKeys() {
    std::printf(
        "Keys:  Space play/pause   Left/Right back/forward   Up/Down   Enter OK\n"
        "       Esc/Backspace Home   t talk   p power   h hold switch\n"
        "       Alt+key or l then key: long press.  Shift+Left/Right or d then Left/Right: deep press.\n"
        "       Ctrl+C quits.\n\n");
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path library = "library";
    std::filesystem::path music = "music";
    std::filesystem::path config = defaultConfig();
    std::filesystem::path state, cache, libraryDb;
    std::string script, audio = "auto", inputs, evdevDevice;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        bool hasValue = i + 1 < argc;
        if (arg == "--script" && hasValue) script = argv[++i];
        else if (arg == "--config" && hasValue) config = argv[++i];
        else if (arg == "--state" && hasValue) state = argv[++i];
        else if (arg == "--music" && hasValue) music = argv[++i];
        else if (arg == "--cache" && hasValue) cache = argv[++i];
        else if (arg == "--library-db" && hasValue) libraryDb = argv[++i];
        else if (arg == "--audio" && hasValue) audio = argv[++i];
        else if (arg == "--input" && hasValue) inputs = argv[++i];
        else if (arg == "--evdev-device" && hasValue) evdevDevice = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            usage();
            return 2;
        } else library = arg;
    }
    if (inputs.empty()) inputs = isatty(STDIN_FILENO) ? OAP_HEADLESS_TTY_INPUT : "evdev";
    bool useEvdev = false, useTerminal = false;
    {
        std::istringstream in(inputs);
        std::string name;
        while (std::getline(in, name, ',')) {
            if (name == "evdev") useEvdev = true;
            else if (name == "terminal") useTerminal = true;
            else if (name != "none") {
                std::fprintf(stderr, "unknown input '%s'\n", name.c_str());
                return 2;
            }
        }
    }

    library = std::filesystem::absolute(library).lexically_normal();
    music = std::filesystem::absolute(music).lexically_normal();
    if (state.empty()) state = defaultStateFile(config);
    if (cache.empty()) cache = state.parent_path() / "cache";
    cache = std::filesystem::absolute(cache).lexically_normal();
    // The library index file: the SqliteLibraryStore below opens it.
    if (libraryDb.empty()) libraryDb = state.parent_path() / "library.db";
    libraryDb = std::filesystem::absolute(libraryDb).lexically_normal();

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGHUP, onSignal);

    Mixer mixer;
    auto output = AudioOutput::open(audio, mixer);
    if (!output) return 1;
    Tts tts(mixer);
    if (!tts.start("en")) return 1;
    Earcons earcons(mixer);
    Player player(mixer);
    SystemClock clock;
    SettingsFiles settings(config, state);
    Servers servers = makeServers(settings.config());
    // The library index (library.db) and the background scan of the local files that keeps it up to date.
    SqliteLibraryStore libraryStore(libraryDb);
    FfmpegTagReader tagReader;
    ThreadedScanner scanner(tagReader);
    DiskTextFiles manifests; // the cache's manifests, written atomically
    App app(tts, player, earcons, mixer, servers.links(), libraryStore, scanner, settings.config(), settings.state(), clock, library, music, cache,
            &manifests, defaultQueueFile(state), defaultScrobbleFile(state));

    EvdevInput evdev;
    if (useEvdev && evdev.open(evdevDevice) == 0)
        std::fprintf(stderr, "input: no button devices found yet (are you in the 'input' group?)\n");
    std::optional<RawTerminal> rawTerminal;
    if (useTerminal) {
        rawTerminal.emplace();
        printTerminalKeys();
    }

    std::printf("Library: %s\nMusic: %s\nConfig: %s\nState: %s\nCache: %s\nLibrary index: %s\n\n",
                library.c_str(), music.c_str(), config.c_str(), state.c_str(), cache.c_str(), libraryDb.c_str());
    std::fflush(stdout);
    app.start();

    PressSequencer scriptPresses(std::chrono::milliseconds(700));
    scriptPresses.push(parseScript(script));
    PressSequencer terminalPresses(std::chrono::milliseconds(30));
    TerminalKeyDecoder decoder;

    bool running = true;
    // Power off waits for the goodbye to be heard, but not for ever if the audio output hangs.
    std::optional<std::chrono::steady_clock::time_point> quitDeadline;
    while (running && !g_stop) {
        std::vector<pollfd> fds;
        if (useTerminal) fds.push_back({STDIN_FILENO, POLLIN, 0});
        evdev.addPollFds(fds);
        poll(fds.data(), fds.size(), 10); // App::tick() wants about 10 ms

        if (useTerminal && (fds[0].revents & (POLLIN | POLLHUP))) {
            char buf[64];
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) {
                useTerminal = false; // end of piped input
            } else {
                for (const TerminalKey& key : decoder.decode(std::string_view(buf, size_t(n)))) {
                    if (key.toggleHold) app.setHold(!app.hold());
                    else terminalPresses.push(key.press);
                }
            }
        }
        if (useEvdev) evdev.process(app);
        if (!scriptPresses.tick(app)) running = false;
        terminalPresses.tick(app);
        app.tick();
        if (app.quitRequested()) {
            auto now = std::chrono::steady_clock::now();
            if (!quitDeadline) quitDeadline = now + std::chrono::seconds(10);
            if (!tts.speaking() || now >= *quitDeadline) running = false;
        }
        std::fflush(stdout);
    }

    app.shutdown();
    servers.finish();
    output->stop();
    return 0;
}
