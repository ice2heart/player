// Desktop simulator: keyboard, a game controller or the on-screen buttons stand
// in for the device buttons, and the window visualises the menu stack, playback and speech.

#include "config/disk_text_files.h"
#include "config/settings_files.h"
#include "core/app.h"
#include "core/earcons.h"
#include "core/mixer.h"
#include "core/text.h"
#include "frontend/gamepad.h"
#include "frontend/held_buttons.h"
#include "frontend/make_music_server.h"
#include "frontend/presses.h"
#include "media/player.h"
#include "media/tts.h"
#include "platform/audio_output.h"

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>

using namespace oap;

namespace {

struct KeyBinding {
    SDL_Keycode key;
    Button button;
    const char* label;
};

const KeyBinding kKeys[] = {
    {SDLK_SPACE, Button::PlayPause, "Space"},
    {SDLK_LEFT, Button::Back, "Left"},
    {SDLK_RIGHT, Button::Forward, "Right"},
    {SDLK_UP, Button::Up, "Up"},
    {SDLK_DOWN, Button::Down, "Down"},
    {SDLK_RETURN, Button::Ok, "Enter"},
    {SDLK_KP_ENTER, Button::Ok, "Enter"},
    {SDLK_ESCAPE, Button::Home, "Esc"},
    {SDLK_BACKSPACE, Button::Home, "Esc"},
    {SDLK_t, Button::Talk, "T"},
    {SDLK_p, Button::Power, "P"},
};

std::optional<Button> mapKey(SDL_Keycode key) {
    for (const auto& k : kKeys)
        if (k.key == key) return k.button;
    return std::nullopt;
}

const char* keyLabel(Button b) {
    if (b == Button::BackDeep) return "Shift+Left";
    if (b == Button::ForwardDeep) return "Shift+Right";
    for (const auto& k : kKeys)
        if (k.button == b) return k.label;
    return "";
}

std::string clock(double seconds) {
    long s = long(seconds);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    return buf;
}

// SDL names controller buttons by position (A = South), like Linux evdev.
std::optional<PadControl> mapPad(Uint8 button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return PadControl::DpadUp;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return PadControl::DpadDown;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return PadControl::DpadLeft;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return PadControl::DpadRight;
    case SDL_CONTROLLER_BUTTON_A: return PadControl::South;
    case SDL_CONTROLLER_BUTTON_B: return PadControl::East;
    case SDL_CONTROLLER_BUTTON_X: return PadControl::West;
    case SDL_CONTROLLER_BUTTON_Y: return PadControl::North;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return PadControl::L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return PadControl::R1;
    case SDL_CONTROLLER_BUTTON_BACK: return PadControl::Select;
    case SDL_CONTROLLER_BUTTON_START: return PadControl::Start;
    case SDL_CONTROLLER_BUTTON_GUIDE: return PadControl::Mode;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return PadControl::ThumbL;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return PadControl::ThumbR;
    default: return std::nullopt;
    }
}

// Open game controllers by joystick instance id, each with its own mapper.
struct Gamepad {
    SDL_GameController* controller = nullptr;
    GamepadMapper mapper;
};

// Every input (keyboard, each pad, the on-screen buttons, --script) goes through
// `held`, so a button held from two of them stays down until both let go.
void press(App& app, HeldButtons& held, Button b) {
    if (held.press(b)) app.keyDown(b);
}

void release(App& app, HeldButtons& held, Button b) {
    if (held.release(b)) app.keyUp(b);
}

void applyChanges(App& app, HeldButtons& held, const std::vector<ButtonChange>& changes) {
    for (const auto& c : changes) {
        if (c.down) press(app, held, c.button);
        else release(app, held, c.button);
    }
}

void printKeys() {
    std::printf(
        "Keys:  Space play/pause   Left/Right back/forward   Up/Down\n"
        "       Enter OK   Esc/Backspace Home   T talk   P power   H hold switch\n"
        "       Shift+Left/Right deep press. Hold a key for a long press.\n"
        "Pad:   D-pad arrows   Square/X play/pause   Cross/A OK   Circle/B Home   L1+Left/Right deep press\n"
        "       Close the window (Cmd+Q on macOS) to quit.\n\n");
}

// ---------------------------------------------------------------- drawing

void drawMenu(const App& app) {
    const ImVec4 kSelected(0.25f, 0.55f, 1.0f, 1.0f);
    if (app.mode() == App::Mode::Sleeping) {
        ImGui::TextDisabled("(sleeping - any key wakes the device)");
        return;
    }
    const auto& stack = app.screens();
    for (size_t i = 0; i < stack.size(); ++i) {
        const Screen& level = stack[i];
        ImGui::BeginChild(ImGui::GetID((void*)(intptr_t)(i + 1)), ImVec2(250, 0), ImGuiChildFlags_Borders);
        ImGui::TextColored(kSelected, "%s", level.title.c_str());
        ImGui::Separator();
        if (level.kind == Screen::Kind::Player) {
            ImGui::TextWrapped(
                "PLAYER\n\n"
                "Space: play / pause\nhold Space: where am I\n"
                "Left / Right: skip\nhold or Shift: previous / next chapter\n"
                "Up / Down: volume\n"
                "Enter: player menu\nEsc: back   hold Esc: home");
        }
        for (size_t j = 0; j < level.items.size(); ++j) {
            const MenuItem& item = level.items[j];
            std::string text = item.label;
            if (item.value) text += ": " + item.value();
            if (item.adjust) text += "  <>";
            if (item.submenu) text += "  >";
            bool selected = j == level.index;
            bool active = i + 1 == stack.size();
            if (selected && !active) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.3f, 0.35f, 1));
            ImGui::PushID(int(j));
            ImGui::Selectable(text.c_str(), selected);
            ImGui::PopID();
            if (selected && !active) ImGui::PopStyleColor();
            if (selected && active) ImGui::SetScrollHereY();
        }
        ImGui::EndChild();
        ImGui::SameLine();
    }
    if (!stack.empty()) ImGui::SetScrollHereX(1.0f);
}

void deviceButton(App& app, HeldButtons& held, Button b, ImVec2 size) {
    bool isHeld = held.held(b);
    if (isHeld) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    std::string label = std::string(buttonName(b)) + "\n" + keyLabel(b);
    ImGui::Button(label.c_str(), size);
    if (ImGui::IsItemActivated()) press(app, held, b);
    if (ImGui::IsItemDeactivated()) release(app, held, b);
    if (isHeld) ImGui::PopStyleColor();
}

void drawDevice(App& app, HeldButtons& held) {
    const ImVec2 kSize(84, 44);
    float x0 = ImGui::GetCursorPosX();
    float gap = ImGui::GetStyle().ItemSpacing.x;
    auto col = [&](int c) { ImGui::SetCursorPosX(x0 + c * (kSize.x + gap)); };

    col(0); deviceButton(app, held, Button::Power, kSize);
    ImGui::SameLine(); col(2); deviceButton(app, held, Button::Talk, kSize);
    col(1); deviceButton(app, held, Button::Up, kSize);
    col(0); deviceButton(app, held, Button::Back, kSize);
    ImGui::SameLine(); col(1); deviceButton(app, held, Button::PlayPause, kSize);
    ImGui::SameLine(); col(2); deviceButton(app, held, Button::Forward, kSize);
    col(1); deviceButton(app, held, Button::Down, kSize);
    col(0); deviceButton(app, held, Button::Home, kSize);
    ImGui::SameLine(); col(2); deviceButton(app, held, Button::Ok, kSize);
    col(0); deviceButton(app, held, Button::BackDeep, kSize);
    ImGui::SameLine(); col(2); deviceButton(app, held, Button::ForwardDeep, kSize);

    bool hold = app.hold();
    if (ImGui::Checkbox("Hold switch (H)", &hold)) app.setHold(hold);
    if (app.keyDescriber()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "KEY DESCRIBER");
    }
}

void drawNowPlaying(const App& app) {
    App::NowPlaying np = app.nowPlaying();
    if (!np.loaded) {
        ImGui::TextDisabled("Nothing loaded");
        return;
    }
    ImGui::Text("%s  %s", np.playing ? "[>]" : "[||]", np.title.c_str());
    ImGui::Text("Chapter %d/%d: %s", np.chapterIndex + 1, np.chapterCount, np.chapter.c_str());
    float fraction = np.duration > 0 ? float(np.position / np.duration) : 0.0f;
    std::string overlay = clock(np.position) + " / " + clock(np.duration);
    ImGui::ProgressBar(fraction, ImVec2(-1, 0), overlay.c_str());
    if (np.sleepTimerLeft) ImGui::Text("Sleep timer: %s", clock(*np.sleepTimerLeft).c_str());
}

void drawUsage(App& app) {
    if (ImGui::BeginTable("usage", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Button");
        ImGui::TableSetupColumn("Short");
        ImGui::TableSetupColumn("Long");
        ImGui::TableSetupColumn("Deep");
        ImGui::TableHeadersRow();
        const auto& usage = app.buttonUsage();
        for (size_t i = 0; i < kButtonCount; ++i) {
            if (Button(i) == Button::BackDeep || Button(i) == Button::ForwardDeep) continue; // counted as Deep
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(buttonName(Button(i)));
            for (size_t g = 0; g < kGestureCount; ++g) {
                ImGui::TableNextColumn();
                if (usage[i][g]) ImGui::Text("%d", usage[i][g]);
                else ImGui::TextDisabled("-");
            }
        }
        ImGui::EndTable();
    }
    if (ImGui::SmallButton("Reset counts")) app.resetButtonUsage();
}

void drawSpeechLog(const App& app) {
    const auto& log = app.speechLog();
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        if (it == log.rbegin()) ImGui::TextWrapped("> %s", it->c_str());
        else ImGui::TextDisabled("  %s", it->c_str());
    }
}

void drawUi(App& app, HeldButtons& held) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("OAP simulator", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    float sideWidth = 380;
    ImGui::BeginChild("menu", ImVec2(io.DisplaySize.x - sideWidth - 24, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    drawMenu(app);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("side", ImVec2(0, 0));
    ImGui::SeparatorText("Device");
    drawDevice(app, held);
    ImGui::SeparatorText("Now playing");
    drawNowPlaying(app);
    ImGui::SeparatorText("Button usage");
    drawUsage(app);
    ImGui::SeparatorText("Speech");
    ImGui::BeginChild("speech", ImVec2(0, 0), ImGuiChildFlags_Borders);
    drawSpeechLog(app);
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::End();
}

void saveSnapshot(SDL_Renderer* renderer, const char* path) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    SDL_RenderSetScale(renderer, 1.0f, 1.0f);
    SDL_RenderSetViewport(renderer, nullptr);
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return;
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) == 0)
        SDL_SaveBMP(surface, path);
    SDL_FreeSurface(surface);
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path library = "library";
    std::filesystem::path music = "music";
    const char* home = std::getenv("HOME");
    std::filesystem::path config = std::filesystem::path(home ? home : ".") / ".config/oap-sim/config.yaml";
    std::filesystem::path state, cache;
    std::string script, audio = "auto";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--script" && i + 1 < argc) script = argv[++i];
        else if (arg == "--config" && i + 1 < argc) config = argv[++i];
        else if (arg == "--state" && i + 1 < argc) state = argv[++i];
        else if (arg == "--music" && i + 1 < argc) music = argv[++i];
        else if (arg == "--cache" && i + 1 < argc) cache = argv[++i];
        else if (arg == "--audio" && i + 1 < argc) audio = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            std::string names;
            for (const auto& name : AudioOutput::backends()) names += (names.empty() ? "" : "|") + name;
            std::printf("usage: oap-sim [--config FILE] [--state FILE] [--music DIR] [--cache DIR] [--audio %s] [--script STEPS] "
                        "[LIBRARY_DIR]\n"
                        "  --config FILE  settings and server (default ~/.config/oap-sim/config.yaml)\n"
                        "  --state FILE   positions and the book to resume (default state.tsv next to the config)\n"
                        "  --cache DIR    downloaded tracks (default cache/ next to the state file)\n",
                        names.c_str());
            return 0;
        } else library = arg;
    }

    library = std::filesystem::absolute(library).lexically_normal();
    music = std::filesystem::absolute(music).lexically_normal();
    if (state.empty()) state = defaultStateFile(config);
    if (cache.empty()) cache = state.parent_path() / "cache";
    cache = std::filesystem::absolute(cache).lexically_normal();

    Mixer mixer;
    auto output = AudioOutput::open(audio, mixer);
    if (!output) return 1;
    Tts tts(mixer);
    if (!tts.start("en")) return 1;
    Earcons earcons(mixer);
    Player player(mixer);
    SystemClock clock;
    SettingsFiles settings(config, state);
    std::unique_ptr<MusicServer> server = makeMusicServer(settings.config());
    std::unique_ptr<Downloader> downloader = makeDownloader(settings.config());
    DiskTextFiles manifests; // the cache's manifests, written atomically
    App app(tts, player, earcons, mixer, *server, *downloader, settings.config(), settings.state(), clock, library, music, cache,
            &manifests);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Open Audio Player - simulator", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, 1280, 760,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!window || !renderer) {
        std::fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        return 1;
    }
    // Vertices are scaled to pixels via SDL_RenderSetScale each frame; rasterise the font at that density.
    int pw = 0, ww = 0, h = 0;
    SDL_GetRendererOutputSize(renderer, &pw, &h);
    SDL_GetWindowSize(window, &ww, &h);
    float scale = ww > 0 ? float(pw) / float(ww) : 1.0f;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImFontConfig fontConfig;
    fontConfig.SizePixels = 15.0f * scale;
    ImGui::GetIO().Fonts->AddFontDefault(&fontConfig);
    ImGui::GetIO().FontGlobalScale = 1.0f / scale;
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    printKeys();
    std::printf("Library: %s\nMusic: %s\nConfig: %s\nState: %s\nCache: %s\n\n", library.c_str(), music.c_str(),
                config.c_str(), state.c_str(), cache.c_str());
    app.start();

    // Debug aid: OAP_SIM_SNAPSHOT=file.bmp saves the last rendered frame on exit.
    const char* snapshot = std::getenv("OAP_SIM_SNAPSHOT");
    HeldButtons held;
    std::set<SDL_Keycode> shifted; // keys pressed with Shift, whose deep stage is down too
    std::map<SDL_JoystickID, Gamepad> gamepads;
    PressSequencer scriptPresses(std::chrono::milliseconds(700));
    scriptPresses.push(parseScript(script));
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_CONTROLLERDEVICEADDED) { // also sent at startup for pads already plugged in
                if (SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which)) {
                    gamepads[SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c))].controller = c;
                    std::printf("gamepad: %s\n", SDL_GameControllerName(c));
                }
            }
            if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
                if (auto it = gamepads.find(e.cdevice.which); it != gamepads.end()) {
                    applyChanges(app, held, it->second.mapper.releaseAll());
                    SDL_GameControllerClose(it->second.controller);
                    gamepads.erase(it);
                }
            }
            if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
                auto it = gamepads.find(e.cbutton.which);
                auto control = mapPad(e.cbutton.button);
                if (it != gamepads.end() && control)
                    applyChanges(app, held, it->second.mapper.set(*control, e.type == SDL_CONTROLLERBUTTONDOWN));
            }
            if (e.type == SDL_CONTROLLERAXISMOTION) { // L2 / R2 are axes; count them as pressed past half way
                auto it = gamepads.find(e.caxis.which);
                bool pressed = e.caxis.value > SDL_JOYSTICK_AXIS_MAX / 2;
                if (it != gamepads.end() && e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
                    applyChanges(app, held, it->second.mapper.set(PadControl::L2, pressed));
                if (it != gamepads.end() && e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
                    applyChanges(app, held, it->second.mapper.set(PadControl::R2, pressed));
            }
            if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
                bool down = e.type == SDL_KEYDOWN;
                if (e.key.keysym.sym == SDLK_h && down) {
                    app.setHold(!app.hold());
                } else if (auto b = mapKey(e.key.keysym.sym)) {
                    auto deep = deepOf(*b);
                    if (down) {
                        press(app, held, *b);
                        if (deep && (e.key.keysym.mod & KMOD_SHIFT)) {
                            shifted.insert(e.key.keysym.sym);
                            press(app, held, *deep);
                        }
                    } else {
                        if (deep && shifted.erase(e.key.keysym.sym)) release(app, held, *deep);
                        release(app, held, *b);
                    }
                }
            }
        }
        if (!scriptPresses.tick(app, &held)) running = false;
        app.tick();
        if (app.quitRequested() && !tts.speaking()) running = false;

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        drawUi(app, held);
        ImGui::Render();
        ImGuiIO& io = ImGui::GetIO();
        SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (!running && snapshot) saveSnapshot(renderer, snapshot);
        SDL_RenderPresent(renderer);
    }

    app.shutdown();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    output->stop();
    return 0;
}
