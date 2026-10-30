# Exercise each independent cache setting without fetching the main project's dependencies.
file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}/source")
file(WRITE "${TEST_DIR}/source/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.25)\nproject(hal_selection LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 20)\nadd_library(oapcore INTERFACE)\ntarget_include_directories(oapcore INTERFACE \"${SOURCE_DIR}/app\")\ninclude(\"${SOURCE_DIR}/cmake/hal/selection.cmake\")\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${TEST_DIR}/source" -B "${TEST_DIR}/none"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "None selection failed: ${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${TEST_DIR}/none"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "None registry failed to compile: ${error}")
endif()
foreach(part INPUT AUDIO POWER WIFI BLUETOOTH HAPTICS STORAGE)
    foreach(backend sim nonexistent)
        execute_process(COMMAND "${CMAKE_COMMAND}" -S "${TEST_DIR}/source" -B "${TEST_DIR}/${part}-${backend}" "-DOAP_HAL_${part}=${backend}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
        if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "Unknown or unavailable HAL ${part} backend")
            message(FATAL_ERROR "${part} accepted ${backend} or gave an incorrect diagnostic: ${output}${error}")
        endif()
    endforeach()
endforeach()
