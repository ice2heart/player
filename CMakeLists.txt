cmake_minimum_required(VERSION 3.20)
project(oap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(OAP_LINUX ON)
else()
    set(OAP_LINUX OFF)
endif()

option(OAP_BUILD_SIM "Build the desktop simulator oap-sim (needs FFmpeg, eSpeak NG, SDL2)" ON)
option(OAP_BUILD_HEADLESS "Build oap-headless, the front end for the device (Linux; needs FFmpeg, eSpeak NG)" ${OAP_LINUX})
option(OAP_BUILD_TESTS "Build the core unit tests" ON)
if(APPLE)
    set(OAP_AUDIO_DEFAULT "coreaudio")
else()
    set(OAP_AUDIO_DEFAULT "pulse;alsa")
endif()
set(OAP_AUDIO_BACKENDS "${OAP_AUDIO_DEFAULT}" CACHE STRING
    "Audio outputs to build in, tried in this order by --audio auto: coreaudio pulse alsa (null is always built)")

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

# ---- frontend: code shared by the front ends (scripted presses, terminal keys, gamepad map). No dependencies.
add_library(oapfrontend STATIC
    app/frontend/gamepad.cpp
    app/frontend/presses.cpp
    app/frontend/terminal_keys.cpp
)
target_link_libraries(oapfrontend PUBLIC oapcore)
target_compile_options(oapfrontend PRIVATE -Wall -Wextra)

if(OAP_BUILD_SIM OR OAP_BUILD_HEADLESS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswresample libavutil)
    pkg_check_modules(ESPEAK REQUIRED IMPORTED_TARGET espeak-ng)

    # ---- media: FFmpeg decoding and eSpeak NG speech, implementing core's interfaces
    add_library(oapmedia STATIC
        app/media/player.cpp
        app/media/tts.cpp
    )
    target_link_libraries(oapmedia PUBLIC oapcore PkgConfig::FFMPEG PkgConfig::ESPEAK)
    target_compile_options(oapmedia PRIVATE -Wall -Wextra)

    # ---- platform audio output: the chosen backends plus null
    add_library(oapaudio STATIC
        app/platform/audio_output.cpp
        app/platform/audio_null.cpp
    )
    target_link_libraries(oapaudio PUBLIC oapcore)
    target_compile_options(oapaudio PRIVATE -Wall -Wextra)
    find_package(Threads REQUIRED)
    target_link_libraries(oapaudio PUBLIC Threads::Threads)
    foreach(backend IN LISTS OAP_AUDIO_BACKENDS)
        if(backend STREQUAL "coreaudio")
            target_sources(oapaudio PRIVATE app/platform/audio_coreaudio.cpp)
            target_link_libraries(oapaudio PUBLIC "-framework AudioToolbox" "-framework CoreAudio")
        elseif(backend STREQUAL "pulse")
            pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple)
            target_sources(oapaudio PRIVATE app/platform/audio_pulse.cpp)
            target_link_libraries(oapaudio PUBLIC PkgConfig::PULSE)
        elseif(backend STREQUAL "alsa")
            pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
            target_sources(oapaudio PRIVATE app/platform/audio_alsa.cpp)
            target_link_libraries(oapaudio PUBLIC PkgConfig::ALSA)
        else()
            message(FATAL_ERROR "Unknown audio backend '${backend}' in OAP_AUDIO_BACKENDS")
        endif()
        string(TOUPPER "${backend}" BACKEND)
        target_compile_definitions(oapaudio PRIVATE OAP_AUDIO_${BACKEND})
    endforeach()
endif()

if(OAP_BUILD_SIM)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

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
    target_link_libraries(oap-sim PRIVATE oapfrontend oapmedia oapaudio imgui)
    target_compile_options(oap-sim PRIVATE -Wall -Wextra)
endif()

if(OAP_BUILD_HEADLESS)
    if(NOT OAP_LINUX)
        message(FATAL_ERROR "oap-headless reads Linux evdev input; build it on Linux or set -DOAP_BUILD_HEADLESS=OFF")
    endif()

    # ---- headless: the device front end. evdev buttons, terminal keys, no window.
    add_executable(oap-headless
        app/headless/main.cpp
        app/headless/evdev_input.cpp
    )
    target_link_libraries(oap-headless PRIVATE oapfrontend oapmedia oapaudio)
    target_compile_options(oap-headless PRIVATE -Wall -Wextra)
endif()

# ---- tests: core and frontend only, with fakes for speech, playback, sounds and time
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
    tests/test_gamepad.cpp
        tests/test_gestures.cpp
        tests/test_library.cpp
        tests/test_mixer.cpp
        tests/test_settings.cpp
        tests/test_terminal_keys.cpp
        tests/test_text.cpp
    )
    target_link_libraries(oap-tests PRIVATE oapcore oapfrontend GTest::gtest_main)
    target_compile_options(oap-tests PRIVATE -Wall -Wextra)
    include(GoogleTest)
    gtest_discover_tests(oap-tests PROPERTIES TIMEOUT 10) # a sabotaged writer can block forever
endif()
