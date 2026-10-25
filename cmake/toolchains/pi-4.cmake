# Cross-compile for the Raspberry Pi 4 (Cortex-A72) running 64-bit
# Raspberry Pi OS. Used by docker/Dockerfile (PI_MODEL=pi-4).
set(OAP_PI_CPU cortex-a72)
include(${CMAKE_CURRENT_LIST_DIR}/pi-arm64.cmake)
