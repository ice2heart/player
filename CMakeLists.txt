cmake_minimum_required(VERSION 3.20)
project(oap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(OAP_BUILD_SIM "Build the desktop simulator (needs FFmpeg, eSpeak NG, SDL2)" ${APPLE}) # macOS only so far
option(OAP_BUILD_TESTS "Build the core unit tests" ON)

include(FetchContent)

# ---- core: UI logic, mixer and earcons. Plain C++, no third-party dependencies.
add_library(oapcore STATIC
    app/core/app.cpp
    app/core/audio_stream.cpp
    app/core/buttons.cpp
    app/core/earcons.cpp
    app/core/library.cpp
    app/core/mixer.cpp
    app/core/settings.cpp
    app/core/text.cpp
)
target_include_directories(oapcore PUBLIC app)
target_compile_options(oapcore PRIVATE -Wall -Wextra)

if(OAP_BUILD_SIM)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswresample libavutil)
    pkg_check_modules(ESPEAK REQUIRED IMPORTED_TARGET espeak-ng)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

    # ---- media: FFmpeg decoding and eSpeak NG speech, implementing core's interfaces
    add_library(oapmedia STATIC
        app/media/player.cpp
        app/media/tts.cpp
    )
    target_link_libraries(oapmedia PUBLIC oapcore PkgConfig::FFMPEG PkgConfig::ESPEAK)
    target_compile_options(oapmedia PRIVATE -Wall -Wextra)

    # ---- platform audio output
    if(APPLE)
        add_library(oapaudio STATIC app/platform/audio_coreaudio.cpp)
        target_link_libraries(oapaudio PUBLIC oapcore "-framework AudioToolbox" "-framework CoreAudio")
    else()
        message(FATAL_ERROR "Only the macOS simulator exists so far; the PulseAudio backend is planned (see SPEC.md).")
    endif()

    # ---- simulator: SDL2 window + Dear ImGui visualisation
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
    target_link_libraries(oap-sim PRIVATE oapmedia oapaudio imgui)
    target_compile_options(oap-sim PRIVATE -Wall -Wextra)
endif()

# ---- tests: core only, with fakes for speech, playback, sounds and time
if(OAP_BUILD_TESTS)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(oap-tests
        tests/test_app.cpp
        tests/test_audio_stream.cpp
        tests/test_gestures.cpp
        tests/test_library.cpp
        tests/test_mixer.cpp
        tests/test_settings.cpp
        tests/test_text.cpp
    )
    target_link_libraries(oap-tests PRIVATE oapcore GTest::gtest_main)
    target_compile_options(oap-tests PRIVATE -Wall -Wextra)
    include(GoogleTest)
    gtest_discover_tests(oap-tests PROPERTIES TIMEOUT 10) # a sabotaged writer can block forever
endif()
