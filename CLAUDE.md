# CLAUDE.md

Audiobook player for blind users: buttons and speech, no screen. Read
[SPEC.md](SPEC.md) for the product and [docs/architecture.md](docs/architecture.md)
for the code. Menus and keys follow [docs/navigation.md](docs/navigation.md).

## Build and test

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure      # or ./build/oap-tests
./build/oap-net-tests                           # Subsonic client (built when libcurl is found; no internet)
./build/oap-sim library                         # simulator (needs ./tools/make_sample_library.sh library music)
./build/oap-headless library                    # device front end, Linux (--audio null without a sound card)
docker build -f docker/Dockerfile --target native .   # clean Debian build + tests
docker build -f docker/Dockerfile --target pi .       # Pi Zero 2 W cross build + tests under qemu
```

`-DOAP_BUILD_SIM=OFF -DOAP_BUILD_HEADLESS=OFF` builds only the core, the config
library and the tests, without FFmpeg, eSpeak NG, SDL2 or audio libraries. yaml-cpp
comes from the system, or CMake downloads it like GoogleTest.

## Layering rules

- `app/core/` is plain C++20 with no third-party dependencies and no OS calls
  except `std::filesystem`. `App` talks to the outside world only through the
  interfaces in `app/core/services.h` (`Speech`, `MediaPlayer`, `SoundEffects`,
  `VolumeControl`, `Clock`). Do not include `media/` or `platform/` headers from core.
- `app/media/` holds the FFmpeg `Player` and eSpeak NG `Tts` that implement those interfaces.
- `app/net/` holds `HttpClient` (libcurl) and `Subsonic : MusicServer`. A stream
  URL holds the user name and a token that never expires: never speak, log or print it.
- `app/frontend/` is code shared by the front ends with no dependencies (tested);
  `app/sim/` (SDL) and `app/headless/` (evdev, terminal) are the front ends.
- `App` never reads the real time. Use `clock_.now()` / `clock_.wallTime()` so tests can control time.
- New core behaviour gets a test in `tests/`. Use the fakes in `tests/fakes.h`
  and drive `App` through `keyDown` / `keyUp` / `tick` as a user would. Test
  observable behaviour (what is spoken, which file plays, what is saved), not private state.

## Every test must be checked with a sabotage

A test that never failed proves nothing. For **every** test you add or change:

1. Run it and see it pass.
2. **Sabotage** the production code it guards with the smallest plausible bug,
   such as a flipped comparison, an off-by-one, a deleted line or a wrong
   constant. Do not change the test itself.
3. Rebuild, run that test, and confirm it **fails** for the expected reason (read the message).
4. Restore the code and confirm the test passes again. Check with `git diff`
   that no sabotage is left in the source.

If the sabotage does not make the test fail, the test is too weak: fix the test,
not the sabotage. When you report on the work, list each test with the sabotage
that broke it.

Practical notes:
- Run sabotage loops in a separate **Ninja** build dir
  (`cmake -S . -B build-sabotage -G Ninja -DOAP_BUILD_SIM=OFF -DOAP_BUILD_HEADLESS=OFF`). macOS ships
  GNU Make 3.81, which compares timestamps to the whole second, so it can skip
  a rebuild after a quick edit and make a sabotage look "missed".
- Run one test with `build-sabotage/oap-tests --gtest_filter=Suite.Name`.
- A sabotage that hangs a test counts as caught. ctest stops each test after 10 s.

## Git

Commit straight to `main`. Do not create feature branches.
