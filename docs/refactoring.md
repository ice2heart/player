# Shared runtime and App decomposition: plan

> Status: proposed; no implementation yet. Based on code inspected 2026-10-29.
> Follow [architecture.md](architecture.md), [navigation.md](navigation.md) and REPO.md.
> Coordinate scheduling changes with [async.md](async.md); reliability changes with
> [fixes.md](fixes.md) phase 7. This plan changes ownership and structure, not save frequency.

## Today and scope

`app/sim/main.cpp` and `app/headless/main.cpp` already instantiate the same `App`.
They duplicate common argument handling, path derivation, service construction,
script startup and shutdown sequencing. Headless has a ten-second goodbye deadline;
the simulator waits indefinitely for speech. SDL rendering and Linux input polling
are intentionally different. `app/core/app.cpp` is about 5,500 lines and also owns
playback, queue transitions, catalogue sync, progress sync, cache and downloads.

Unify service wiring first, then extract cohesive core responsibilities. Keep one
process, one UI-thread App, existing service interfaces, and both front ends working
after every step. Moving the same large class into several source files alone does
not achieve the ownership goal.

## Shared startup and lifecycle

- Add dependency-free `FrontendOptions` and `ResolvedPaths` helpers in `app/frontend/`.
  Parse common options once; adapters handle `--font`, `--input`, `--evdev-device` and
  their help text. Preserve the separate simulator/headless config defaults and
  headless XDG behavior. Reject unknown options and missing values in both programs
  with exit status 2; this deliberately fixes the simulator's positional fallback.
- Add `app/runtime/application_runtime.{h,cpp}` in a separate `oapruntime` target,
  built only with a front end. It owns Mixer, AudioOutput, Tts, Earcons, Player,
  Clock, SettingsFiles, Servers, library store, tag reader, scanner, TextFiles and App.
  Keep this dependency-heavy construction out of core and dependency-free oapfrontend.
  Preserve core-only builds and `OAP_BUILD_NET=OFF`.
- Runtime exposes `start()`, `tick()`, `app()`, `shouldExit()` and idempotent `shutdown()`.
  Front ends deliver input, call tick and present output; runtime does not own their
  event loops. Constructor/start failure must unwind already-started services safely.
- Normal quit allows goodbye speech at most ten seconds, using the injected clock.
  Signals/window close/script exit initiate shutdown without waiting for goodbye.
  Shutdown order: App save/cancel, Servers finish with its existing overall three-second
  budget while App is alive, audio stop, then destroy consumers before dependencies.
  Verify actual callback/destructor lifetimes before selecting member declaration order.
  No extra save from a second shutdown; no callback may outlive App.
- Reuse `HeldButtons`, `PressSequencer`, gamepad mapping and terminal decoding rather
  than replacing them. Share input dispatch only where semantics are identical; SDL
  events, evdev hotplug, raw-terminal lifetime and ImGui stay in their adapters.

## Core ownership after extraction

| Owner | Responsibility | Boundary |
|---|---|---|
| App | screen stack, gestures, user intent, spoken presentation | Calls domain operations; projects results into menus. |
| PlaybackSession | loaded/parked book and music sessions, pending-open rollback, queue advancement, seek and resume | Owns playback state exactly once; exposes snapshots and typed results, no menu closures. |
| ProgressSync | progress pull offers, pending pushes, acknowledgements and retry generations | Receives book/session identity and observed progress; offers become App Options actions. |
| DownloadCoordinator | explicit downloads, automatic-cache work and manifest updates | Owns cancellation and transfer queues; reports progress/results, never speaks directly. |
| LibraryCoordinator | scanner/catalogue refresh and index updates | Publishes stable item ids/revisions; no Screen pointers. |

`Queue`, `TrackCache` and `LibraryIndex` stay reusable domain types. Extract one owner
at a time and delete App's duplicate mutable fields in the same step. Inject existing
services and Clock. Use operation generations and explicit cancel/stop; ordinary
callbacks continue to arrive through poll on the UI thread. Do not create a generic
event bus, new threads, or a second persistence subsystem during extraction.

## Phase 1: unify the front ends

Goal: simulator and headless use the same startup and shutdown behavior.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 1.1 | **Common options and paths.** Add frontend helpers, migrate both parsers, preserve platform defaults and document consistent errors. | Same common flags resolve identically; relative paths/default siblings; missing values and unknown flags return 2; frontend-only flags remain isolated. |
| 1.2 | **Shared runtime ownership.** Introduce oapruntime, move composition from both mains, keep platform input/UI local. | Start failure releases started services; both executables start with null audio and scripted input; core-only and no-network builds still succeed. |
| 1.3 | **One shutdown policy.** Move goodbye deadline, App shutdown, server draining and output stop into runtime; expose a small lifecycle policy testable with fake services/clock. | Hung speech exits after ten seconds; save precedes drain; App lives through acknowledgements; shutdown twice saves once; early initialization failure is safe. |
| 1.4 | **Input dispatch and parity checks.** Remove only identical dispatch code and keep HeldButtons ownership explicit. | Overlapping inputs release only after last source releases; script produces same spoken actions in both adapters; removed device releases its held keys. |
| 1.5 | **Docs as built.** Update architecture.md, REPO.md layering for oapruntime, and SPEC workflow. | – |

**Device check (manual):** run identical play/pause/sleep scripts in simulator and
headless with a temporary library; compare speech and saved state. Quit while offline
and while speech is stuck; verify bounded exit. Check SDL and evdev input separately.

## Phase 2: extract playback and progress

Goal: one owner manages playback transitions and one manages server progress, while
App retains presentation and public input behavior.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 2.1 | **PlaybackSession.** Move loaded/parked state, pending-open transitions and queue advancement to `app/core/playback_session.{h,cpp}`; replace App fields with session queries/commands. | Existing App tests preserve spoken behavior; failed/abandoned opens restore prior state; book/music switch preserves both places; repeat/shuffle and unavailable entries remain correct. |
| 2.2 | **ProgressSync.** Move pull/push state to `app/core/progress_sync.{h,cpp}` with typed offers and acknowledgements; App owns spoken notice and acceptance. | Late pull cannot seek a replacement book; accepting offer survives older push acknowledgement; offline retry/completion semantics and current persistence write counts remain unchanged. |
| 2.3 | **Docs as built.** Update ownership/thread diagrams and document service lifetimes in architecture.md. | – |

**Device check (manual):** switch between local book and mixed music queue, cancel a
remote open, accept a server-place offer, sleep and restart; compare with baseline.

## Phase 3: extract library and transfer coordination

Goal: App delegates background domain work without changing its user-facing behavior.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 3.1 | **DownloadCoordinator.** Move explicit/automatic jobs, transfer cancellation and manifest/cache updates to `app/core/download_coordinator.{h,cpp}`. | Explicit download preempts auto work; resume retains parts; pinning/eviction and sleep behavior unchanged; deleted lists cannot receive stale completions. |
| 3.2 | **LibraryCoordinator.** Move scanner/catalogue request ownership and index mutation to `app/core/library_coordinator.{h,cpp}`. Menu builders consume stable snapshots/ids. | Refresh errors retain usable library; cancelled scans/syncs do not replace newer results; merging and menu focus remain correct. |
| 3.3 | **Docs as built.** Update architecture.md and remove obsolete ownership descriptions. | – |

**Device check (manual):** browse while scanning/downloading, cancel or disconnect a
server, then restart offline and play downloaded content.

## Validation and coordination

Use existing behavioral tests as the baseline. Add tests for ownership/lifecycle gaps,
not private field layouts. Every new or changed test follows REPO.md's sabotage loop.
Run relevant core, net and media suites and frontend build combinations at each affected
phase; do not claim device parity from fakes alone.

The libuv plan may change the concrete services owned by ApplicationRuntime. Integrate
its runtime there after its own measurements; no libuv dependency in core. Phase 7 fixes
can land before or after extraction, but their tests must stay attached to the moved
behavior. Do not combine the extraction with checkpoint-policy or sync-policy changes.
