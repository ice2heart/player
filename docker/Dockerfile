# Build and test oap in a clean Debian. Targets:
#   native  - x86-64/host build of everything (sim, headless, tests), runs ctest
#   pi      - cross build of oap-headless and the tests for the Pi Zero 2 W (arm64),
#             runs ctest under qemu
#   *-out   - just the binaries, for `docker build --output`
# See README.md ("Docker") for the commands.

# Raspberry Pi OS (64-bit) is trixie too: the Pi binaries need its glibc and FFmpeg.
ARG DEBIAN=debian:trixie-slim

# ---- native toolchain and libraries
FROM ${DEBIAN} AS native-deps
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkgconf ca-certificates git \
        libavformat-dev libavcodec-dev libswresample-dev libavutil-dev \
        libespeak-ng-dev espeak-ng libsdl2-dev libpulse-dev libasound2-dev \
        libyaml-cpp-dev ffmpeg \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src

FROM native-deps AS native
COPY . .
RUN cmake -S . -B /build -G Ninja \
    && cmake --build /build \
    && ctest --test-dir /build --output-on-failure

FROM scratch AS native-out
COPY --from=native /build/oap-sim /build/oap-headless /build/oap-tests /

# ---- Pi Zero 2 W: arm64 cross toolchain, :arm64 libraries and qemu to run the tests
FROM ${DEBIAN} AS pi-deps
RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkgconf ca-certificates git \
        crossbuild-essential-arm64 qemu-user \
        libavformat-dev:arm64 libavcodec-dev:arm64 libswresample-dev:arm64 libavutil-dev:arm64 \
        libespeak-ng-dev:arm64 libpulse-dev:arm64 libasound2-dev:arm64 libyaml-cpp-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src

FROM pi-deps AS pi
COPY . .
RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/pi-zero-2w.cmake \
        -DOAP_BUILD_SIM=OFF \
    && cmake --build /build \
    && ctest --test-dir /build --output-on-failure \
    && qemu-aarch64 -L /usr/aarch64-linux-gnu /build/oap-headless --help
# (--help loads every linked library, so a wrong soname or symbol version fails here.)
# Runtime packages for Raspberry Pi OS: whoever ships each linked library, plus the voices.
# A library that no package ships fails the build instead of silently missing from the list.
RUN set -e; \
    libs=$(aarch64-linux-gnu-objdump -p /build/oap-headless | awk '/NEEDED/ {print $2}'); \
    [ -n "$libs" ] || { echo "no NEEDED libraries found in oap-headless" >&2; exit 1; }; \
    : > /build/pkgs; \
    for lib in $libs; do \
        pkg=$(dpkg -S "/usr/lib/aarch64-linux-gnu/$lib" "/lib/aarch64-linux-gnu/$lib" 2>/dev/null | head -1 | cut -d: -f1); \
        [ -n "$pkg" ] || { echo "no installed package ships $lib" >&2; exit 1; }; \
        echo "$pkg" >> /build/pkgs; \
    done; \
    sort -u -o /build/pkgs /build/pkgs; \
    { echo "# Runtime packages for oap-headless on Raspberry Pi OS (64-bit, trixie):"; \
         echo "sudo apt install $(tr '\n' ' ' < /build/pkgs)espeak-ng-data"; } > /build/DEPENDENCIES.txt \
    && cat /build/DEPENDENCIES.txt

FROM scratch AS pi-out
COPY --from=pi /build/oap-headless /build/oap-tests /build/DEPENDENCIES.txt /
