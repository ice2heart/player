cmake_minimum_required(VERSION 3.25) # FetchContent_Declare(... SYSTEM)
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

# ---- config: config.yaml (yaml-cpp) and state.tsv files behind core's SettingsStore.
# yaml-cpp comes from the system if found, else it is fetched and built like GoogleTest.
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
    URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
    FIND_PACKAGE_ARGS 0.7 CONFIG)
block()
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10) # yaml-cpp 0.8.0 asks for CMake 3.4, which CMake 4 refuses
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(yaml-cpp)
endblock()
if(TARGET yaml-cpp::yaml-cpp)
    set(OAP_YAML_CPP yaml-cpp::yaml-cpp)
else()
    set(OAP_YAML_CPP yaml-cpp) # 0.7 names its target without a namespace
endif()

add_library(oapconfig STATIC
    app/config/atomic_file.cpp
    app/config/settings_files.cpp
    app/config/stores.cpp
)
target_link_libraries(oapconfig PUBLIC oapcore PRIVATE ${OAP_YAML_CPP})
target_compile_options(oapconfig PRIVATE -Wall -Wextra)

# ---- frontend: code shared by the front ends (scripted presses, terminal keys, gamepad map, held buttons). No dependencies.
add_library(oapfrontend STATIC
    app/frontend/gamepad.cpp
    app/frontend/held_buttons.cpp
    app/frontend/presses.cpp
    app/frontend/terminal_keys.cpp
)
target_link_libraries(oapfrontend PUBLIC oapcore)
target_compile_options(oapfrontend PRIVATE -Wall -Wextra)

# ---- platform audio output: null always, device backends (below) only with a front end.
# No third-party dependencies of its own, so the tests can link it.
find_package(Threads REQUIRED)
add_library(oapaudio STATIC
    app/platform/audio_output.cpp
    app/platform/audio_null.cpp
)
target_link_libraries(oapaudio PUBLIC oapcore Threads::Threads)
target_compile_options(oapaudio PRIVATE -Wall -Wextra)

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

    # ---- device audio backends, in the order --audio auto tries them
    foreach(backend IN LISTS OAP_AUDIO_BACKENDS)
        if(backend STREQUAL "null")
            continue() # always built
        elseif(backend STREQUAL "coreaudio")
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
    target_link_libraries(oap-sim PRIVATE oapfrontend oapconfig oapmedia oapaudio imgui)
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
    target_link_libraries(oap-headless PRIVATE oapfrontend oapconfig oapmedia oapaudio)
    target_compile_options(oap-headless PRIVATE -Wall -Wextra)
endif()

# ---- tests: core, config, frontend and the null audio output, with fakes for speech, playback, sounds and time
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
        tests/test_audio_output.cpp
        tests/test_audio_stream.cpp
        tests/test_config.cpp
        tests/test_fake_music_server.cpp
        tests/test_gamepad.cpp
        tests/test_gestures.cpp
        tests/test_held_buttons.cpp
        tests/test_library.cpp
        tests/test_mixer.cpp
        tests/test_presses.cpp
        tests/test_settings.cpp
        tests/test_terminal_keys.cpp
        tests/test_text.cpp
    )
    target_link_libraries(oap-tests PRIVATE oapcore oapconfig oapfrontend oapaudio GTest::gtest_main)
    target_compile_options(oap-tests PRIVATE -Wall -Wextra)
    include(GoogleTest)
    # A sabotaged writer can block forever, hence the timeout. Under qemu
    # (cross builds) everything runs several times slower, so allow more.
    if(CMAKE_CROSSCOMPILING)
        set(OAP_TEST_TIMEOUTS DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 60)
        set(OAP_MEDIA_TEST_TIMEOUTS DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 120)
    else()
        set(OAP_TEST_TIMEOUTS DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 10)
        set(OAP_MEDIA_TEST_TIMEOUTS DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 20)
    endif()
    gtest_discover_tests(oap-tests ${OAP_TEST_TIMEOUTS})

    # ---- media tests: the FFmpeg Player on files written by the tests
    if(TARGET oapmedia)
        add_executable(oap-media-tests tests/test_player.cpp)
        target_link_libraries(oap-media-tests PRIVATE oapmedia GTest::gtest_main)
        target_compile_options(oap-media-tests PRIVATE -Wall -Wextra)
        gtest_discover_tests(oap-media-tests ${OAP_MEDIA_TEST_TIMEOUTS})
    endif()
endif()
