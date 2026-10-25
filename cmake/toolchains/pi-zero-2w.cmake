# Cross-compile for the Raspberry Pi Zero 2 W (Cortex-A53) running 64-bit
# Raspberry Pi OS. Used by docker/Dockerfile (PI_MODEL=pi-zero-2w, the default).
set(OAP_PI_CPU cortex-a53)
include(${CMAKE_CURRENT_LIST_DIR}/pi-arm64.cmake)
