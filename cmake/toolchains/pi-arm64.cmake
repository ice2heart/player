# Cross-compile for a 64-bit Raspberry Pi running Raspberry Pi OS, with Debian's
# gcc-aarch64-linux-gnu and multiarch :arm64 libraries. Not used on its own:
# pi-zero-2w.cmake and pi-4.cmake set OAP_PI_CPU and include it.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
if(NOT OAP_PI_CPU)
    message(FATAL_ERROR "Use cmake/toolchains/pi-zero-2w.cmake or pi-4.cmake, not pi-arm64.cmake")
endif()
set(CMAKE_C_FLAGS_INIT "-mcpu=${OAP_PI_CPU}")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=${OAP_PI_CPU}")

# Started over SSH, oap-headless reads the gamepad and buttons as well as the terminal.
set(OAP_HEADLESS_TTY_INPUT "evdev,terminal" CACHE STRING "oap-headless default --input on a terminal")

# Run the arm64 test binary through qemu so gtest_discover_tests and ctest work.
find_program(OAP_QEMU_AARCH64 NAMES qemu-aarch64-static qemu-aarch64)
if(OAP_QEMU_AARCH64)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${OAP_QEMU_AARCH64};-L;/usr/aarch64-linux-gnu")
endif()

# pkg-config must see only the arm64 .pc files.
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu)
