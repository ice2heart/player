# Code review findings

Review of every commit from f315d8e (when `docs/hal.md` was added) to a5d9037
(gamepad input), done 2026-10-24 by one reviewer per commit. Only findings that
are **still present at HEAD** are listed. Fixed ones are left out.

- **Confirmed**: reproduced with a probe test or a sabotage, or certain from reading the code.
- **Plausible**: likely from reading the code, but not reproduced.

Fix rules (see [CLAUDE.md](CLAUDE.md)):
- Every fix to core behaviour gets a test in `tests/`.
- Every test gets a sabotage check.
- Tick the box when a fix is done.

Line numbers are as of a5d9037 and may drift.

---

## P1: the device goes silent, crashes or misbehaves

- [x] **R1. A dead audio output silences the device for good, and Power-off hangs.** Confirmed.
  - **Where:** [app/platform/audio_thread.h:48-51](app/platform/audio_thread.h#L48-L51), and `Tts::speaking()` in [app/media/tts.cpp](app/media/tts.cpp).
  - **Problem:** when `write()` fails, `run()` returns and `running_` stays true. Nothing is told and nothing reopens the device. This happens when Pulse/PipeWire restarts or a USB DAC is unplugged (`snd_pcm_recover` does not handle ALSA `-ENODEV`).
  - **Knock-on effects:**
    - The speech buffer never drains, so `tts.speaking()` stays true for ever.
    - Power never quits, because [main.cpp:186](app/headless/main.cpp#L186) waits for `!tts.speaking()`.
    - Auto sleep never fires, and `Player::finished()` never becomes true.
    - The process never exits, so systemd cannot restart it.
  - **Fix:** retry opening the device with backoff in the audio thread, and keep draining or discarding the mixer meanwhile. Otherwise exit with an error so systemd restarts the service.

- [x] **R2. macOS `._` files inside album folders become tracks.** Confirmed.
  - **Where:** [app/core/library.cpp:29-34](app/core/library.cpp#L29-L34).
  - **Problem:** dot files are filtered only at the top level. The recursive scan keeps `._01 - Sunrise.mp3`, which macOS writes when copying to FAT/exFAT. "Cannot open" is then spoken every other track, and Play all keeps hitting broken files.
  - **Fix:** skip every entry whose name starts with `.`, at every depth.
  - **Test:** a library with `._x.mp3` next to `x.mp3` lists one track.

- [x] **R3. A mid-stream format change can read out of bounds.** Plausible.
  - **Where:** [app/media/player.cpp:175](app/media/player.cpp#L175).
  - **Problem:** `swr_` is set up once in `open()`. If a frame's `sample_rate`, `format` or `ch_layout` changes, `swr_convert` reads the planes with the old layout. Chained Ogg Vorbis/Opus, concatenated MP3s and HE-AAC/PS signalling can do this. With fewer planes it is an OOB read; with a new rate the pitch and speed are wrong.
  - **Fix:** compare each frame against the parameters `swr_` was built with, and rebuild it when they differ.

- [x] **R4. An unreadable folder in the library can crash the app.** Plausible.
  - **Where:** [app/core/library.cpp:28-33](app/core/library.cpp#L28-L33).
  - **Problem:** the iterators are built with an `error_code`, but range-for advances them with the throwing `operator++`. A permission error, or an SD card pulled during the scan, throws `filesystem_error` out of `musicMenu()` or `start()`, and nothing catches it.
  - **Fix:** loop with `it.increment(ec)` and skip entries that fail.

- [x] **R5. A script `wait:` with a missing or bad number reads an empty optional.** Confirmed.
  - **Where:** [app/frontend/presses.cpp:30](app/frontend/presses.cpp#L30) and [:56-58](app/frontend/presses.cpp#L56-L58).
  - **Problem:** `wait`, `wait:` or `wait:abc` give `waitMs=0` with no button. `tick()` then takes the press branch and dereferences `*step.button`, which is undefined behaviour. Affects `--script` in both front ends.
  - **Fix:** reject a bad wait in `parseScript` (print it and skip, like an unknown step), and make `tick()` treat `waitMs == 0 && !button` as a no-op.
  - **Test:** `parseScript` / `PressSequencer` currently have no tests at all.

- [ ] **R6. Keys stay held after an unplug or when evdev drops events.** Confirmed by reading.
  - **Where:** [app/headless/evdev_input.cpp:153-175](app/headless/evdev_input.cpp#L153-L175).
  - **Problem:**
    - When a device disappears, held keyboard or gpio keys and `deepHeld` get no `keyUp`. Only the gamepad mapper calls `releaseAll()`.
    - `SYN_DROPPED` is not handled. A DualSense streams stick axes at about 250 Hz, so if the loop stalls on a Pi Zero the buffer can overflow and lose a release.
    - In both cases `GestureDetector` thinks the key is still down, and a long press fires.
  - **Fix:** track the down buttons per device and release them all on unplug. On `SYN_DROPPED`, resync with `EVIOCGKEY`/`EVIOCGABS`, or at least release everything.

## P2: wrong behaviour users will notice

- [x] **R7. Two-stage keys misfire on bounce or when the stages arrive out of order.** Confirmed.
  - **Where:** [app/core/buttons.cpp:33-43](app/core/buttons.cpp#L33-L43).
  - **Problem:** each sequence below is followed by what happens.
    - `ForwardDeep↓ Forward↓ Forward↑ ForwardDeep↑`: the chapter jump **plus** a 30 s skip, because `press(Forward)` resets `consumed`.
    - `Forward↓ ForwardDeep↓ Forward↑ Forward↓`, then hold: a second chapter jump (a Long fires).
    - `F↓ FD↓ FD↑ FD↓` within one light hold: only one step. `longFired = first.down && first.consumed` cannot tell "a long fired" apart from "an earlier deep fired", so a repeated deep press is silent (breaks navigation rule 9).
  - **Fix:**
    - In `press()`, treat a light press as consumed when its deep stage is already down.
    - Track "long fired" separately from "consumed".
  - **Tests:** deep-before-light, a bounce while deep, and repeated deep presses during one hold.

- [x] **R8. Track order is not numeric, and non-ASCII names sort wrongly.** Confirmed.
  - **Where:** [app/core/library.cpp:20-23](app/core/library.cpp#L20-L23) and [:35](app/core/library.cpp#L35).
  - **Problem:**
    - `std::sort` over paths puts "10 - Ten" before "3 - Three".
    - `lower()` uses byte-wise `tolower`, so Cyrillic and accented names are not case-folded: "Б…" sorts before "а…", and "Ängel" after "Zebra".
    - Menu order is the only way to find things without a screen.
  - **Fix:** a natural sort (runs of digits compare as numbers) plus a UTF-8-aware case fold, at least for Latin-1 Supplement and Cyrillic.
  - **Test:** unpadded numbers, mixed-case Cyrillic, accented Latin.

- [x] **R9. The "end of track" sleep timer never fires on a loose track or a single-file book without chapters.** Confirmed.
  - **Where:** [app/core/app.cpp:156](app/core/app.cpp#L156) and `sleepTimerMenu`.
  - **Problem:** `currentChapter()` is always 0. When the file ends, `onFileFinished` reloads file 0, so the chapter number never changes. The timer stays armed, and Settings keeps saying "end of track".
  - **Fix:** also trigger on file finished.

- [x] **R10. The sleep timer tests don't check that the timer waits.** Confirmed.
  - **Where:** `SleepTimerAtEndOfTrack` and `SleepTimerIsInSettings` in [tests/test_app.cpp](tests/test_app.cpp).
  - **Problem:** with the tick condition sabotaged to `sleepAtChapterEnd_ >= 0 && book_` (sleep on the next tick), all six sleep and chapter tests still pass.
  - **Fix:** assert "not sleeping" just before the end of the chapter and at 14 minutes.

- [x] **R11. Any `av_read_frame` error counts as end of file.** Plausible.
  - **Where:** [app/media/player.cpp:143](app/media/player.cpp#L143).
  - **Problem:** a transient SD/USB read error ends the file, and the App moves to the next track mid-book.
  - **Fix:** only `AVERROR_EOF` means end of file. Retry or log other errors.

- [x] **R12. A failed seek is ignored, and the start position is not clamped.** Plausible.
  - **Where:** [app/media/player.cpp:130](app/media/player.cpp#L130) and [:70](app/media/player.cpp#L70).
  - **Problem:** on a failed `av_seek_frame`, `endPts_` and `skipUntil_` are set anyway. Backward, the reported position disagrees with what is playing. Forward, everything up to the target is decoded and thrown away, so the seek is slow. A saved position past the end decodes the whole file silently.
  - **Fix:** check the result, and clamp `startAt` to the duration.

- [x] **R13. The hold switch doesn't block releases or long presses.** Confirmed.
  - **Where:** [app/core/app.cpp:142](app/core/app.cpp#L142) and `tick()`.
  - **Problem:** only `keyDown` checks Hold. Press Right, switch Hold on, release: the skip happens anyway. Holding past the long-press time also fires `stepChapter`.
  - **Fix:** when Hold turns on, drop the gesture state of every held key, except Power.

- [x] **R14. Left/Right on an item without a value still stop speech.** Confirmed.
  - **Where:** [app/core/app.cpp:195](app/core/app.cpp#L195).
  - **Problem:** `tts_.stop()` runs before `handleMenu`. This breaks navigation rule 5, which says these keys do nothing there. `LeftAndRightDoNothingOnItemsWithoutAValue` doesn't check `speech.stops`.
  - **Fix:** stop speech only when the key has a job. Extend the test to check `speech.stops`.

- [x] **R15. The Sleep timer list always opens on "Off".** Confirmed.
  - **Where:** [app/core/app.cpp:619](app/core/app.cpp#L619) and [:699](app/core/app.cpp#L699).
  - **Problem:** `MenuItem::selected` is not set, so the list doesn't open on the current value. Pressing OK twice by accident cancels the timer.
  - **Fix:** set `selected` to the current value.

- [x] **R16. Holding Power again stacks a second Power menu.** Confirmed.
  - **Where:** [app/core/app.cpp:205](app/core/app.cpp#L205).
  - **Problem:** the stack becomes [Home, Power, Power], and Home then goes "back" to another Power menu.
  - **Fix:** return to the Power screen already on the stack, as `enterPlayer` does for the player.

- [x] **R17. Sleep from the Power menu leaves you in the Power menu.** Confirmed.
  - **Where:** [app/core/app.cpp:664](app/core/app.cpp#L664).
  - **Problem:** on wake the device says "Power. Sleep, 1 of 4", and OK sends it straight back to sleep.
  - **Fix:** pop the Power screen before sleeping.

- [x] **R18. "End of chapter" can be set with nothing loaded.** Confirmed by reading.
  - **Where:** `setSleepTimer`.
  - **Problem:** it says "set" but does nothing. Settings keeps reporting it until a book opens, which then clears it silently.

- [x] **R19. An unopenable track leaves a misleading state.** Plausible.
  - **Where:** `playMusic` / `loadFile` / `onFileFinished`.
  - **Problem:** `book_` and `music_` are already switched when `open` fails. The device says "Cannot open" and then "Now playing. X, paused". Mid-album, the next file may be skipped, or playback may just stop.

## P3: headless front end and gamepad

- [ ] **R20. A device given with `--evdev-device` is never reopened.** Confirmed.
  - **Where:** [app/headless/evdev_input.cpp:146](app/headless/evdev_input.cpp#L146).
  - **Problem:** rescanning only runs when `only_` is empty. If the MCU's HID re-enumerates, its buttons stay dead until restart.

- [ ] **R21. SIGHUP, background starts and the default input.** Plausible.
  - **Where:** [app/headless/main.cpp:111](app/headless/main.cpp#L111) and [:129-131](app/headless/main.cpp#L129-L131).
  - **Problem:**
    - `signal(SIGHUP, …)` replaces the SIG_IGN that `nohup` set.
    - Starting with `&` on a tty stops the process on SIGTTOU/SIGTTIN.
    - Starting from SSH defaults to `terminal` only, so the physical evdev buttons are off.
  - **Fix:**
    - Keep SIGHUP ignored if it already was.
    - Use the terminal only in the foreground process group (`tcgetpgrp`).
    - Consider defaulting to `evdev,terminal`.

- [ ] **R22. An escape sequence split across reads becomes Home.** Plausible.
  - **Status (macOS pass):** done in `app/frontend/` (`TerminalKeyDecoder::decode(bytes, now)` + `poll(now)`, `kEscTimeout` 200 ms) and tested. **Left for Linux:** wire it in [app/headless/main.cpp](app/headless/main.cpp): call `decoder.decode(bytes, steady_clock::now())`; every loop, when `useTerminal`, feed `decoder.poll(steady_clock::now())` into the same push/toggle code (share it in a lambda); on stdin EOF call `poll` with a far-future time so a trailing lone Esc is not lost. Then delete the untimed `decode(bytes)` overload.
  - **Where:** [app/frontend/terminal_keys.cpp:52-55](app/frontend/terminal_keys.cpp#L52-L55), [:70-77](app/frontend/terminal_keys.cpp#L70-L77).
  - **Problem:**
    - Over slow SSH, `ESC` arrives alone and `[A` follows in the next read: a spurious Home, and the arrow is lost.
    - `ESC [` at the end of a read is dropped.
    - `ESC ESC [ A` (Alt+arrow in rxvt/tmux) is lost.
  - **Fix:** keep a partial sequence across reads, and treat a lone ESC as the key only after a short timeout.

- [ ] **R23. The poll loop spins on a stdin error.** Plausible.
  - **Where:** [app/headless/main.cpp:170](app/headless/main.cpp#L170).
  - **Problem:** only `POLLIN|POLLHUP` are checked, so `POLLERR`/`POLLNVAL` make the loop busy-wait at 100 % CPU.
  - **Fix:** treat them like end of input.

- [ ] **R24. The terminal `h` jumps ahead of queued presses.** Confirmed, minor.
  - **Where:** [app/headless/main.cpp:177](app/headless/main.cpp#L177).
  - **Problem:** "Space h" in one read answers "Locked." to the Space.

- [ ] **R25. Shutdown gaps.** Plausible.
  - **Problem:**
    - SIGPIPE is not ignored. Stdout piped to a reader that died kills the process without `app.shutdown()`, so position and settings are not saved.
    - Ctrl+\ or a crash leaves the terminal without echo.

- [x] **R26. The PipeWire/PulseAudio advice for the DualSense may drive the haptic motors.** Plausible.
  - **Where:** [README.md](README.md), "DualSense audio".
  - **Problem:** the controller is a 4.0 sink. Pulse `enable-remixing` and PipeWire's upmix can copy stereo into channels 3-4, which are the motors.
  - **Fix:** recommend `--audio alsa:plughw:CARD=Controller` only, unless this is checked on hardware.

- [ ] **R27. The same button from two sources ends a press early.** Confirmed, minor.
  - **Status (macOS pass):** `HeldButtons` in [app/frontend/held_buttons.h](app/frontend/held_buttons.h) (tested), used by the sim for keyboard, pads, on-screen buttons and `--script`; `PressSequencer::tick` takes a `HeldButtons*`. **Left for Linux:** give `EvdevInput` a `HeldButtons` shared with `main`; in `applyChanges` and the keyboard path (both `*b` and `*deep`) call `keyDown` only when `held.press()` returns true and `keyUp` only when `held.release()` does; pass the same object to `terminalPresses.tick` and `scriptPresses.tick`.
  - **Where:** [app/headless/evdev_input.cpp](app/headless/evdev_input.cpp) (one mapper per device), [app/sim/main.cpp:97-107](app/sim/main.cpp#L97-L107) (shared `held`).
  - **Problem:** a keyboard plus a pad, or Steam's virtual uinput pad that duplicates every press: the first `keyUp` ends the press while the other source still holds it.
  - **Fix:** count holders per `Button` across sources, and call `keyUp` only when the count reaches 0.

- [ ] **R28. Xbox pads with `dpad_to_buttons` get no D-pad.** Plausible.
  - **Where:** [app/headless/evdev_input.cpp:55-75](app/headless/evdev_input.cpp#L55-L75).
  - **Problem:** xpad (wireless 360 receivers) reports the D-pad as `BTN_TRIGGER_HAPPY1..4`.
  - **Fix:** map those codes, or stop saying "Xbox" in the docs.

- [ ] **R29. Gamepad test gaps.** Confirmed.
  - **Status (macOS pass):** hat Y axis, both hat axes at once and `defaultGamepadLayout()` are tested; doc wording and CMake indentation fixed. **Left for Linux:** move the evdev code → `PadControl` table out of `app/headless/` into `app/frontend/` and test it; `mapPad` stays untested.
  - **Problem:**
    - Only the X axis of `setAxis` is tested.
    - `ABS_HAT0Y` → Up/Down and `mapPad` are untested in both front ends.
    - Moving the evdev code → `PadControl` table into `app/frontend/` would make it testable.
  - **Also:** a doc wording fix in [docs/architecture.md](docs/architecture.md) §10: "the same map as on the device" should say "the same map as `oap-headless`". The indentation of `tests/test_gamepad.cpp` in [CMakeLists.txt](CMakeLists.txt) is wrong.

## P4: build, Docker and CI

- [x] **R30. The qemu test run never starts `oap-headless`.** Confirmed.
  - **Where:** [docker/Dockerfile](docker/Dockerfile), `pi` stage.
  - **Problem:** `oap-tests` doesn't link FFmpeg, eSpeak, pulse or asound, so a wrong soname or symbol version passes CI. This is how the bookworm/trixie mismatch went unnoticed.
  - **Fix:** add `qemu-aarch64-static -L /usr/aarch64-linux-gnu /build/oap-headless --help`.

- [x] **R31. The README says CMake 3.20+, but the build needs at least 3.25.** Confirmed.
  - **Where:** [CMakeLists.txt:1](CMakeLists.txt#L1) and the README package table.
  - **Problem:** `FetchContent_Declare(... SYSTEM)` needs 3.25.
  - **Fix:** raise `cmake_minimum_required` and the README to 3.25.

- [x] **R32. `DEPENDENCIES.txt` can leave out a package without failing.** Plausible.
  - **Where:** the `objdump`/`dpkg -S` loop in [docker/Dockerfile](docker/Dockerfile).
  - **Problem:** the pipeline always exits 0.
  - **Fix:** fail the build when a library has no package.

- [x] **R33. Tests under qemu may time out on slow runners.** Plausible.
  - **Problem:** gtest discovery (5 s) and the per-test `TIMEOUT 10` both run under emulation.
  - **Fix:** use `DISCOVERY_TIMEOUT` and a longer timeout when cross-compiling.

- [x] **R34. Release workflow problems.** Confirmed.
  - **Where:** [.github/workflows/release.yml](.github/workflows/release.yml).
  - **Problem:**
    - `github.ref_name` with a `/` (manual run on `feature/x`) breaks the tar file name.
    - `gh release create` fails on a re-run, and there is no `concurrency` guard.
    - `checkout`/`upload-artifact`/`download-artifact@v4` run on the deprecated Node 20.
  - **Fix:**
    - Sanitise the name.
    - Use `gh release upload --clobber` when the release exists.
    - Add `concurrency`.
    - Move to v5 actions.

- [ ] **R35. Smaller build items.**
  - **Status (macOS pass):** qemu-user, README and CMake `null` are done. **Left:** the bookworm decision only.
  - **Bookworm:** there is no longer a way to build for bookworm Raspberry Pi OS. Decide whether that is intended.
  - **qemu:** `qemu-user-static` is transitional on trixie, so install `qemu-user` instead.
  - **README:** the outside-Docker steps are missing `ca-certificates` and `git`. It also says the release ships `oap-headless`, but it actually ships a tar.gz with DEPENDENCIES.txt.
  - **CMake:** `null` in `OAP_AUDIO_BACKENDS` is rejected, although the help text lists it.

## Test gaps without a bug (yet)

- [ ] **R36. Weak tests.**
  - **Status (macOS pass):** all done except the evdev key, Shift, deep and hold logic, which still lives in the Linux-only class.
  - **Terminal keys:** 4 of 5 sabotages survived [tests/test_terminal_keys.cpp](tests/test_terminal_keys.cpp). Missing cases: Shift+Alt (modifier 4), a truncated sequence, and `l`+`d` together.
  - **`PlayAllPlaysEveryTrackInOrder`** never checks that the third track plays.
  - **Missing tests:**
    - resuming after a restart that follows music
    - library hidden files, ordering and non-ASCII names
    - `AudioOutput::open` selection (null backend)
    - the `ThreadedAudioOutput` failure path
    - the evdev key, Shift, deep and hold logic (it would need to move out of the Linux-only class)
  - **Misnamed test:** `FirstStageAloneIsAnOrdinaryPress` in [tests/test_gestures.cpp](tests/test_gestures.cpp) also presses the deep stage.

## Status after the macOS pass (2026-10-24)

Fixed and tested on macOS: R1–R5, R7–R19, R26, R30–R34 (ticked). Docker
and the workflow changes (R30, R32–R35, ffmpeg in the native image) were not run
locally; the first CI run checks them.

**Still to fix on Linux** (needs `app/headless/` to compile): R6, R20, R21, R23,
R24, R25, R28, plus the headless halves of R22, R27, R29 and R36 (see their
status notes). Also not compiled on macOS, so build them first:
- [app/headless/main.cpp](app/headless/main.cpp): `#include <chrono>` and a 10 s quit deadline after Power off (R1 fallback).
- CMake: the pulse/alsa sources are now added to `oapaudio` in a block after it is defined.
- Run both Docker targets: `docker build -f docker/Dockerfile --target native .` and `--target pi`. The new `oap-media-tests` should run in native and skip its MP3 cases on pi.

### Check on the target (Linux / Pi Zero 2 W with real hardware)

Audio output (R1):
- [ ] USB DAC on ALSA: unplug during playback. Expect "output failed, trying to reopen it", the position still advancing, and `alsa: cannot open` at most every 4 s. Plug it back in and audio resumes. Check that `snd_pcm_writei` returns `-ENODEV` and doesn't block, and that `snd_pcm_drop` on a dead pcm is safe.
- [ ] PipeWire/Pulse: `systemctl --user restart pipewire pipewire-pulse` during playback and during speech. Audio comes back by itself.
- [ ] Power off while the output is dead: the goodbye drains in about real time, the process exits within 10 s, and systemd restarts it.
- [ ] Auto sleep and the automatic next chapter still work while the output is dead.
- [ ] Journal noise while the device stays gone: one error line per attempt, at most every 4 s.

Media (R3, R11, R12):
- [ ] Concatenated MP3s with different sample rates or channel counts play at the right speed.
- [ ] Chained Ogg with a format change plays the first part, then ends cleanly. FFmpeg refuses the change, so it doesn't play on.
- [ ] A truncated or corrupt file ends normally, after at most about 0.5 s of retries.
- [ ] A saved position past the end of the file goes straight to the end, or to the next chapter.
- [ ] A seek on a slow SD card that fails logs "cannot seek", and playback goes on with an honest position.
- [ ] R19: after a failed open, the previous file reopens and resumes where it was, without an audible glitch.

Library on the SD card (R2, R4, R8):
- [ ] Copy a library from a Mac to FAT/exFAT. The `._*` files, `.Trashes` and `.Spotlight-V100` are ignored.
- [ ] Cyrillic and accented names come back as UTF-8 with the card's mount options (`iocharset`/`utf8`) and sort in alphabet order.
- [ ] `LibraryTest.UnreadableFoldersAreSkipped` passes under libstdc++ (native Docker or the Pi), not only macOS libc++.
- [ ] Known gap: Latin Extended-A (Polish Ł, Czech č) is not folded, so those names sort after z.

Buttons (R7, R13):
- [ ] How the real two-stage switches order and bounce their contacts: can deep close before light, and does light chatter while deep is held?
- [ ] Whether the deep contact bounces. There is no debounce, and every deep press now steps. A gpio-keys `debounce-interval` or an MCU debounce may be needed.
- [ ] A long press followed by pushing deeper feels right: one step, then each further push steps again.
- [ ] Hold switch flipped while a key is down: the release and the long press are both swallowed (R13).

Speech and menus:
- [ ] Sleep from the Power menu says only "Going to sleep.", and on waking it speaks the screen you were on (R17).

Terminal and input (R22, R27, after the headless wiring):
- [ ] Over SSH on a slow link: arrows and Shift/Alt arrows never become a stray Home, and Esc alone gives Home after about 200 ms.
- [ ] Inside tmux and rxvt, Alt+arrow is a long press, and Esc pressed twice quickly gives Home twice.
- [ ] Holding the same button on a keyboard and a pad, or on two pads, keeps it pressed until both are released.
- [ ] Unplugging a pad mid-press releases its buttons (R6).
- [ ] Hat Up/Down on a DualSense and other pads, including diagonals.

CI:
- [ ] The `pi` stage builds on trixie with `qemu-user`, and `qemu-aarch64 … oap-headless --help` passes.
- [ ] The DEPENDENCIES.txt step finds a package for every library.
- [ ] Release workflow: actions v5, a re-run takes the `--clobber` path, and the concurrency group works.

## HAL plan (docs/hal.md): fix the design before building it

- [ ] **H1. Headphone unplug detection may have no source.** `snd-usb-audio` (the CM108-class codec in SPEC §3.4) creates no `SW_HEADPHONE_INSERT`. It needs a GPIO or MCU jack-sense line, or a codec chosen for jack detection.
- [ ] **H2. Waking from suspend is not designed.**
  - HID over USB needs USB remote wakeup, which RK3566 dwc controllers support poorly.
  - After resume the MCU may re-enumerate, so `McuLink` must reconnect and Hold must be read again.
  - A failed write to `/sys/power/state` (EBUSY) is not handled.
- [ ] **H3. The key that wakes the device would also act.** gpio-keys reports the wake key after `onResume()`, so Play-to-wake also toggles playback. Swallow the first key after resume.
- [ ] **H4. Cached getters and queued events can disagree.** `wifi->state()` can say Connected while a `WifiChanged{Connecting}` is still queued. Simpler and testable: App keeps the snapshots and updates them from events in `drain()`.
- [ ] **H5. Sim backends can't double as test fakes.** Their timers run on the HAL thread's real-time `poll()`, while `FakeClock` only moves when a test advances it.
- [ ] **H6. The Android plan promises what a normal app can't do.**
  - It can't turn the screen off (needs device admin).
  - KeyEvents don't arrive with the screen off; only MediaSession media keys do.
  - Android has no `KEYCODE_F13`–`F24`.
  - Reading the SSID needs location permission.
- [ ] **H7. Keyboard autorepeat and grabbing.** Mapped keyboard keys would repeat and break long presses, while Backspace in a text field does want repeat. `KeyInput` needs a `repeat` flag, and keyboards should be grabbed with `EVIOCGRAB` so a passphrase doesn't reach the console.
- [ ] **H8. Wi-Fi is keyed by SSID.** SSIDs aren't unique. iwd uses object paths and needs a registered `net.connman.iwd.Agent` for the passphrase.
- [ ] **H9. A daemon that starts late is lost.** If iwd or bluetoothd starts after the app, it stays `nullptr`. Watch `NameOwnerChanged` instead.
- [ ] **H10. The BlueZ pairing flow is wrong.**
  - `NoInputNoOutput` never shows a PIN.
  - With `DisplayOnly`, a keyboard asks the user to type the passkey, so "Confirm on the other device" is the wrong prompt.
  - `BtPairing` mixes "show this passkey" with the final result.
- [ ] **H11. USB mass storage unmounts the library.** While exported, the player can't read books or save position. The partition must be FAT or exFAT.
- [ ] **H12. Smaller plan items.**
  - `OAP_HAL_INPUT` defaults to `sdl`, which clashes with the core-only build.
  - Whether Hold blocks keyboard and touch input is not specified.
  - The generic HID driver rarely produces `EV_SW`, so the Hold switch over HID may need uinput.
