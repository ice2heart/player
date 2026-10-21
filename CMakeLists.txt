cmake_minimum_required(VERSION 3.20)
project(oap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswresample libavutil)
pkg_check_modules(ESPEAK REQUIRED IMPORTED_TARGET espeak-ng)

# ---- core: platform independent, shared by simulator and device
add_library(oapcore STATIC
    app/core/app.cpp
    app/core/audio_stream.cpp
    app/core/buttons.cpp
    app/core/earcons.cpp
    app/core/library.cpp
    app/core/mixer.cpp
    app/core/player.cpp
    app/core/settings.cpp
    app/core/text.cpp
    app/core/tts.cpp
)
target_include_directories(oapcore PUBLIC app)
target_link_libraries(oapcore PUBLIC PkgConfig::FFMPEG PkgConfig::ESPEAK)
target_compile_options(oapcore PRIVATE -Wall -Wextra)

# ---- platform audio output
if(APPLE)
    add_library(oapaudio STATIC app/platform/audio_coreaudio.cpp)
    target_link_libraries(oapaudio PUBLIC oapcore "-framework AudioToolbox" "-framework CoreAudio")
else()
    message(FATAL_ERROR "Only the macOS simulator exists so far; the PulseAudio backend is planned (see SPEC.md).")
endif()

# ---- simulator: SDL2 window + Dear ImGui visualisation
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

include(FetchContent)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC PkgConfig::SDL2)

add_executable(oap-sim app/sim/main.cpp)
target_link_libraries(oap-sim PRIVATE oapaudio imgui)
target_compile_options(oap-sim PRIVATE -Wall -Wextra)
