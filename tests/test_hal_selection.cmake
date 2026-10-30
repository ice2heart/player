# Exercise selection without fetching the main project's dependencies.
file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}/source")
file(WRITE "${TEST_DIR}/source/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.25)\nproject(hal_selection LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 20)\nadd_library(oapcore INTERFACE)\ntarget_include_directories(oapcore INTERFACE \"${SOURCE_DIR}/app\")\ninclude(\"${SOURCE_DIR}/cmake/hal/selection.cmake\")\nadd_executable(probe probe.cpp)\ntarget_link_libraries(probe PRIVATE oaphal)\n")
file(WRITE "${TEST_DIR}/source/probe.cpp" [=[
#include "hal/registry.h"
#include "hal/sim/power.h"
#include "hal/sim/storage.h"
#include <chrono>
#include <cstdlib>
struct Clock : oap::Clock {
    TimePoint now() const override { return {}; }
    std::time_t wallTime() const override { return {}; }
};
int main(int argc, char** argv) {
    oap::HalEvents events; Clock clock; oap::HalContext context{events, clock};
    auto parts = oap::makeHal(context);
    if (bool(parts.power) != (argv[1][0] == '1') || bool(parts.storage) != (argv[2][0] == '1')) return 1;
    if (parts.power && parts.power->battery().percent != 80) return 2;
    if (parts.storage && parts.storage->state().freeBytes != 10ULL * 1024 * 1024 * 1024) return 3;
    if (parts.power) {
        static_cast<oap::SimPower*>(parts.power.get())->setBattery({35, true, true, false});
        if (parts.power->battery().percent != 35 || !parts.power->battery().charging) return 5;
    }
    if (parts.storage) {
        static_cast<oap::SimStorage*>(parts.storage.get())->setState({7, 20, true});
        if (parts.storage->state().freeBytes != 7 || !parts.storage->state().usbHostConnected) return 6;
    }
    return argc == 3 ? 0 : 4;
}
]=])
foreach(pair "none;none;0;0" "sim;none;1;0" "none;sim;0;1" "sim;sim;1;1")
    list(GET pair 0 power)
    list(GET pair 1 storage)
    list(GET pair 2 want_power)
    list(GET pair 3 want_storage)
    set(dir "${TEST_DIR}/${power}-${storage}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${TEST_DIR}/source" -B "${dir}" "-DOAP_HAL_POWER=${power}" "-DOAP_HAL_STORAGE=${storage}" RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Selection configure failed: ${error}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${dir}" RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Registry build failed: ${error}")
    endif()
    execute_process(COMMAND "${dir}/probe" "${want_power}" "${want_storage}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Selected parts or snapshots incorrect: ${power} ${storage}: ${result}")
    endif()
    execute_process(COMMAND nm "${dir}/probe" RESULT_VARIABLE nm_result OUTPUT_VARIABLE symbols)
    if(NOT nm_result EQUAL 0)
        message(FATAL_ERROR "Cannot inspect linked factories")
    endif()
    file(READ "${dir}/generated/hal_registry.cpp" registry)
    foreach(part Power Storage)
        string(TOLOWER "${part}" lower)
        if(${lower} STREQUAL "sim")
            if(NOT registry MATCHES "makeSim${part}" OR NOT symbols MATCHES "makeSim${part}")
                message(FATAL_ERROR "Selected ${part} factory missing")
            endif()
        elseif(registry MATCHES "makeSim${part}" OR symbols MATCHES "makeSim${part}")
            message(FATAL_ERROR "Unselected ${part} factory linked")
        endif()
    endforeach()
endforeach()
foreach(part INPUT AUDIO POWER WIFI BLUETOOTH HAPTICS STORAGE)
    set(invalid nonexistent)
    if(NOT part STREQUAL "POWER" AND NOT part STREQUAL "STORAGE")
        list(APPEND invalid sim)
    endif()
    foreach(backend IN LISTS invalid)
        execute_process(COMMAND "${CMAKE_COMMAND}" -S "${TEST_DIR}/source" -B "${TEST_DIR}/${part}-${backend}" "-DOAP_HAL_${part}=${backend}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
        if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "Unknown or unavailable HAL ${part} backend")
            message(FATAL_ERROR "${part} accepted ${backend} or gave an incorrect diagnostic: ${output}${error}")
        endif()
    endforeach()
endforeach()
