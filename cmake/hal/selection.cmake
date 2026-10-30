# No HAL part backend exists yet. Legacy input/audio paths remain independent.
# New backends are added here with their factories when implemented.
foreach(part INPUT AUDIO POWER WIFI BLUETOOTH HAPTICS STORAGE)
    set(OAP_HAL_${part} "none" CACHE STRING "Built HAL ${part} backends")
    set_property(CACHE OAP_HAL_${part} PROPERTY STRINGS none)
    if(NOT OAP_HAL_${part} STREQUAL "none")
        message(FATAL_ERROR "Unknown or unavailable HAL ${part} backend '${OAP_HAL_${part}}'; available: none")
    endif()
endforeach()
configure_file(${CMAKE_CURRENT_LIST_DIR}/registry.cpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/hal_registry.cpp @ONLY)
add_library(oaphal STATIC ${CMAKE_CURRENT_BINARY_DIR}/generated/hal_registry.cpp)
target_link_libraries(oaphal PUBLIC oapcore)
target_compile_options(oaphal PRIVATE -Wall -Wextra)
