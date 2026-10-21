// Desktop simulator: keyboard (or on-screen buttons) stand in for the device
// buttons, and the window visualises the menu stack, playback and speech.

#include "core/app.h"
#include "core/text.h"
#include "platform/audio_output.h"

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>
#include <sstream>

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

// --script "down ok wait:3000 right:long quit" presses buttons automatically,
// for repeatable UI walkthroughs. Names: play left right up down ok home talk power.
struct ScriptStep {
    std::optional<Button> button;
    bool longPress = false;
    int waitMs = 0;
    bool quit = false;
};

std::vector<ScriptStep> parseScript(const std::string& text) {
    static const std::pair<const char*, Button> kNames[] = {
        {"play", Button::PlayPause}, {"left", Button::Back}, {"right", Button::Forward},
        {"up", Button::Up}, {"down", Button::Down}, {"ok", Button::Ok},
        {"home", Button::Home}, {"talk", Button::Talk}, {"power", Button::Power}};
    std::vector<ScriptStep> steps;
    std::istringstream in(text);
    std::string word;
    while (in >> word) {
        ScriptStep step;
        std::string name = word, arg;
        if (auto colon = word.find(':'); colon != std::string::npos) {
            name = word.substr(0, colon);
            arg = word.substr(colon + 1);
        }
        if (name == "wait") step.waitMs = std::atoi(arg.c_str());
        else if (name == "quit") step.quit = true;
        else {
            for (const auto& [n, b] : kNames)
                if (name == n) step.button = b;
            if (!step.button) {
                std::fprintf(stderr, "script: unknown step '%s'\n", word.c_str());
                continue;
            }
            step.longPress = arg == "long";
        }
        steps.push_back(step);
    }
    return steps;
}

class ScriptRunner {
public:
    explicit ScriptRunner(std::vector<ScriptStep> steps) : steps_(std::move(steps)) {}

    // Returns false once the script asks to quit.
    bool tick(App& app, std::set<Button>& held) {
        auto now = std::chrono::steady_clock::now();
        if (now < next_ || index_ >= steps_.size()) return true;
        const ScriptStep& step = steps_[index_];
        if (step.quit) return false;
        if (step.waitMs) {
            next_ = now + std::chrono::milliseconds(step.waitMs);
            ++index_;
        } else if (!pressed_) {
            std::printf("script: %s%s\n", buttonName(*step.button), step.longPress ? " (long)" : "");
            held.insert(*step.button);
            app.keyDown(*step.button);
            pressed_ = true;
            next_ = now + std::chrono::milliseconds(step.longPress ? 1600 : 80);
        } else {
            held.erase(*step.button);
            app.keyUp(*step.button);
            pressed_ = false;
            ++index_;
            next_ = now + std::chrono::milliseconds(700);
        }
        return true;
    }

private:
    std::vector<ScriptStep> steps_;
    size_t index_ = 0;
    bool pressed_ = false;
    std::chrono::steady_clock::time_point next_{};
};

void printKeys() {
    std::printf(
        "Keys:  Space play/pause   Left/Right back/forward   Up/Down\n"
        "       Enter OK   Esc/Backspace Home   T talk   P power   H hold switch\n"
        "       Hold a key for a long press. Cmd+Q quits.\n\n");
}

// ---------------------------------------------------------------- drawing

void drawMenu(const App& app) {
    const ImVec4 kSelected(0.25f, 0.55f, 1.0f, 1.0f);
    if (app.mode() == App::Mode::Sleeping) {
        ImGui::TextDisabled("(sleeping - any key wakes the device)");
        return;
    }
    if (app.mode() == App::Mode::Player) {
        ImGui::TextColored(kSelected, "PLAYER SCREEN");
        ImGui::Separator();
        ImGui::TextWrapped(
            "Space: play / pause      hold Space: where am I\n"
            "Left / Right: skip       hold Left / Right: previous / next chapter\n"
            "Up / Down: volume\n"
            "Enter: player menu       Esc: home menu");
        return;
    }
    const auto& stack = app.menuStack();
    for (size_t i = 0; i < stack.size(); ++i) {
        const MenuLevel& level = stack[i];
        ImGui::BeginChild(ImGui::GetID((void*)(intptr_t)(i + 1)), ImVec2(250, 0), ImGuiChildFlags_Borders);
        ImGui::TextColored(kSelected, "%s", level.title.c_str());
        ImGui::Separator();
        for (size_t j = 0; j < level.items.size(); ++j) {
            const MenuItem& item = level.items[j];
            std::string text = item.label;
            if (item.value) text += ": " + item.value();
            if (item.submenu) text += "  >";
            if (item.adjust) text += "  <>";
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

void deviceButton(App& app, const std::set<Button>& held, Button b, ImVec2 size) {
    bool isHeld = held.count(b) > 0;
    if (isHeld) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    std::string label = std::string(buttonName(b)) + "\n" + keyLabel(b);
    ImGui::Button(label.c_str(), size);
    if (ImGui::IsItemActivated()) app.keyDown(b);
    if (ImGui::IsItemDeactivated()) app.keyUp(b);
    if (isHeld) ImGui::PopStyleColor();
}

void drawDevice(App& app, const std::set<Button>& held) {
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
    if (ImGui::BeginTable("usage", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Button");
        ImGui::TableSetupColumn("Short");
        ImGui::TableSetupColumn("Long");
        ImGui::TableHeadersRow();
        const auto& usage = app.buttonUsage();
        for (size_t i = 0; i < kButtonCount; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(buttonName(Button(i)));
            for (int g = 0; g < 2; ++g) {
                ImGui::TableNextColumn();
                if (usage[i][size_t(g)]) ImGui::Text("%d", usage[i][size_t(g)]);
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

void drawUi(App& app, const std::set<Button>& held) {
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

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path library = "library";
    const char* home = std::getenv("HOME");
    std::filesystem::path config = std::filesystem::path(home ? home : ".") / ".config/oap-sim/settings.tsv";
    std::string script;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--script" && i + 1 < argc) script = argv[++i];
        else if (arg == "--config" && i + 1 < argc) config = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            std::printf("usage: oap-sim [--config FILE] [--script STEPS] [LIBRARY_DIR]\n");
            return 0;
        } else library = arg;
    }

    library = std::filesystem::absolute(library).lexically_normal();

    Mixer mixer;
    auto output = AudioOutput::create();
    if (!output->start(mixer)) return 1;
    Tts tts(mixer);
    if (!tts.start("en")) return 1;
    Earcons earcons(mixer);
    Settings settings(config);
    settings.load();
    App app(mixer, tts, earcons, settings, library);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
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
    // The ImGui renderer backend scales to pixels itself; we only rasterise the font at that density.
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
    std::printf("Library: %s\nSettings: %s\n\n", std::filesystem::absolute(library).c_str(), config.c_str());
    app.start();

    std::set<Button> held;
    ScriptRunner scriptRunner(parseScript(script));
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
                bool down = e.type == SDL_KEYDOWN;
                if (e.key.keysym.sym == SDLK_h && down) {
                    app.setHold(!app.hold());
                } else if (auto b = mapKey(e.key.keysym.sym)) {
                    if (down) {
                        held.insert(*b);
                        app.keyDown(*b);
                    } else {
                        held.erase(*b);
                        app.keyUp(*b);
                    }
                }
            }
        }
        if (!scriptRunner.tick(app, held)) running = false;
        app.tick();
        if (app.quitRequested() && !tts.speaking()) running = false;

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        drawUi(app, held);
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
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
