# Background tasks with libuv: plan

> Status: proposed; no implementation yet. Based on [architecture.md](architecture.md),
> especially sections 3, 8 and 11, and code inspected on 2026-10-29.
> Expands the background-task work in [fixes.md](fixes.md), steps 1.5 and 6.2.
> Menus and keys remain as specified in [navigation.md](navigation.md).

## Goal and scope

Share network scheduling across servers, keep buttons and speech responsive during
downloads and scans, and bound background memory and concurrency on the Pi Zero 2 W.
Use one libuv loop on a background thread within the existing process. A separate
daemon and IPC are outside this plan.

Libuv is a scheduling foundation, not a promise of lower CPU usage. The current HTTP
workers already sleep efficiently. Measure the improvement before declaring success.

## Today

| Work | Implementation | Consequence |
|------|----------------|-------------|
| API GET/PATCH | `app/net/http_client.cpp`: one thread and curl multi handle per client; interruptible `curl_multi_poll`, up to 60 s idle wait | Already concurrent within a server; thread count grows with servers. |
| Downloads | `app/net/http_downloader.cpp`: one thread per downloader; one active file; synchronous write and flush in curl's data callback | Disk stalls are isolated today; moving this callback unchanged would stall every service on a shared loop. |
| Parsing | Subsonic/Audiobookshelf HTTP handlers parse on their HTTP worker | A large response must not monopolize the shared loop. |
| Scan | `app/frontend/threaded_scanner.cpp`: one serial worker for traversal, stat and tags | Blocking work; cancellation is cooperative between files. |
| Completion | Service inboxes, drained by `poll()` in `App::tick()` | All ordinary App callbacks stay on the UI thread. |
| Exit | `Servers::finish()` starts temporary threads to drain transports concurrently | Preserve one overall 3 s network budget and the special exit acknowledgements. |
| Persistence | Library index save runs in the UI tick; settings and queue writes have existing save points | Separate latency source; changing durability and persistence interfaces is outside this migration. |

With N configured supported servers there are normally 2N network service threads,
plus scanner, media and audio threads. Curl's resolver may create additional threads.
Replacing `std::thread` with `uv_thread_t` alone would not improve this design.

## Target design

```text
UI / App -- existing service methods --> command queues
   ^                                      |
   | service poll()                       v
typed completion inboxes <-- one BackgroundRuntime / uv_loop thread
                                         |              |
                                CurlReactor          bounded work
                               sockets/timers       parse / file / scan

Player, preload, TTS and audio keep their existing execution model.
```

- Add `app/async/background_runtime.{h,cpp}` in a separate `oapasync` target.
  Keep libuv headers out of core and the dependency-free `oapfrontend` target.
  Front ends own the runtime and inject it into services; it outlives all users.
  Create it only when there are consumers. Initially network-only builds need it;
  the later scanner adapter also uses it when network support is disabled.
- Add `app/net/curl_reactor.{h,cpp}`. Use curl's socket-action API with `uv_poll_t`
  socket watchers and `uv_timer_t` deadlines. Start with one multi handle per
  client/downloader on the shared loop to retain connection/authentication isolation.
  Connection-pool consolidation is a later measured optimization.
- Only the loop thread mutates curl handles and libuv handles. Producers append
  commands under a mutex and call `uv_async_send()`. Drain the queue, not one command
  per notification: notifications can coalesce. Use bounded batches and reschedule
  remaining commands so a burst cannot starve sockets or timers.
- Identify operations by service instance, request id and generation. IDs are not
  globally unique; replacing an id must not let an old parse or write completion
  finish the new request. Preserve each service's existing ordering requirements.
- Keep the public `MusicServer`, `Downloader` and `LibraryScanner` contracts.
  Cancellation immediately suppresses user callbacks, including completions already
  queued. Internal cleanup callbacks still run. A callback may cancel another request
  or enqueue work during `poll()`.
- Move HTTP response handlers/parsing to bounded work before sharing transport threads.
  Preserve per-client serialization where required and count parsing in drain completion.
  Never run blocking file operations, FFmpeg probes or user callbacks on the loop.

### Work limits and downloads

Use libuv's shared work pool for finite tasks, with application admission limits.
Initial limits: one scan chunk, one parse task and two disk tasks in flight globally;
at most one disk task per destination. These are tuning defaults, not new product
settings. The libuv pool is process-global (default four workers), so do not increase
`UV_THREADPOOL_SIZE` as a substitute for bounded queues. Measure total threads and RSS,
including curl resolver workers; a single-server setup may gain no thread reduction.

Retain one active download per downloader and initially cap active downloads at two
globally, with round-robin admission across servers. API requests use separate admission
from downloads. Set explicit API request/body/parse queue budgets from the phase 1
baseline before phase 2 integration; overload must complete with an existing error,
never silently drop an accepted request. Cancellation and shutdown commands must always
be admitted. Keep the existing 16 MB per-response cap and timeout/error mappings.

Download callbacks copy bytes into bounded buffers and submit ordered disk work.
Start with 256 KiB of application buffering per active download. Pause curl before
accepting a chunk that does not fit; resume on the loop when capacity returns. Handle
reentrant delivery on resume and do not copy a paused chunk twice. Budget curl's own
buffers too; use HTTP/1.1 without automatic content decompression for file downloads
initially to avoid multiplexed paused-stream buffering. API transport remains unchanged.

Keep `.part` resume, restart on 200/416, body validation, expected-size checks, progress
coalescing and error mapping. Progress counts completed writes. `Ok` requires all writes,
close and final rename to succeed. Preserve existing flush visibility; do not claim
that a stream flush is an fsync durability guarantee.

Cancellation currently fences file operations before returning. Preserve this contract:
stop accepting chunks, suppress callbacks, and fence any already-running write before
allowing destination reuse or deletion. Queued disk jobs recheck cancellation before
touching files. Never wait for that fence on the event loop. Measure UI cancellation
latency on slow storage; an asynchronous cancellation API would be a separate design
change, not an incidental regression in this migration.

### Shutdown and lifetime

1. `App::shutdown()` queues final progress/scrobbles as today.
2. Close network admission and drain all servers concurrently to one absolute deadline
   of 3 s. Keep only the existing permitted exit acknowledgements; no browsing callbacks.
3. Abort remaining transfers, invalidate generations, settle download file fences and
   cancel pending work. Running tasks stop cooperatively and retain owned state until done.
4. Destroy service adapters after their work is settled. Close socket/timer/async handles,
   run close callbacks, then close the loop and join its thread.

Libuv handle storage must stay at stable addresses until close callbacks finish.
`uv_cancel()` cannot interrupt a running worker; a hung mount or tag probe can still
delay teardown, as it can today. The network drain budget is not a guarantee that the
whole process exits within 3 s. Check curl's asynchronous DNS support in every shipped
build; a blocking resolver on the shared loop defeats transport responsiveness.

## Libavformat integration: assessment and optional experiment

Recommendation: retain FFmpeg's native playback I/O during phases 1–4. Investigate
shared HTTP separately when implementing cache-from-stream (fixes.md 1.6/6.3).
Do not route ordinary local playback or tag reads through async filesystem callbacks
just to give every component the same interface.

Current `Player::openInput()` lets libavformat open local files and HTTP streams.
`avio_open2()` is used explicitly on some reopen paths to distinguish a server answer
without audio from a connection failure. This is still FFmpeg protocol I/O, not our
own byte transport. `seekInput()` uses `av_seek_frame()` for originals and reopens a
time-offset URL for transcodes. `FfmpegTagReader` also opens files directly.

Libavformat supports application-provided read/seek callbacks through
[`avio_alloc_context()`](https://www.ffmpeg.org/doxygen/trunk/avio_8h.html).
That is an integration point, not an asynchronous demuxer interface. A proposed bridge
would look like:

```text
CurlReactor -> bounded encoded-byte buffer -> custom AVIO -> demux/decode thread
                         |
                  optional cache sink
```

The read callback waits on the decoder/open thread for bytes, EOF, error or cancellation;
the libuv loop never waits for the decoder. Empty-buffer reads must not report EOF or
return zero. Do not assume EAGAIN makes every demuxer safely resumable. Cancellation
must explicitly wake custom callback waits; FFmpeg's interrupt callback alone cannot
interrupt an arbitrary condition-variable wait implemented by us.

| Option | Potential benefit | Cost / recommendation |
|--------|-------------------|-----------------------|
| Shared HTTP transport via custom AVIO | Shared limits, auth/TLS policy, cancellation, telemetry; opportunity to cache the bytes already received | Worth a bounded experiment for cache-from-stream. Does not remove the decoder thread. |
| Tee around FFmpeg's own AVIO reads | Explore caching without replacing HTTP protocol handling | Compare first: still needs offset-aware writes, seek handling and completeness validation. |
| Local reads via `uv_fs_read` | Central file scheduling or future remote/virtual storage | Defer. Async filesystem operations normally use libuv's work pool; a waiting decoder plus pool work and extra buffering may cost more than native file reads. |
| Shared cache-writing sink | Reuse bounded writes, cancellation and atomic completion | Reuse phase 3's sink mechanics, but extend its sequential-file assumptions explicitly before accepting seeked media. |

FFmpeg already implements HTTP seek/reconnect behaviours in its
[protocol layer](https://www.ffmpeg.org/ffmpeg-protocols.html). Replacing it requires:

- Original-file byte seeks, size queries (`AVSEEK_SIZE`), Range validation, servers
  ignoring Range, and formats that read the file tail during probing (such as some M4B).
  A seek invalidates old requests/buffers by generation. Preserve format hints normally
  derived from the URL, without logging credential-bearing URLs.
- Separate byte offsets from playback time. A `timeOffset` transcode is a new encoded
  response, not a byte-contiguous continuation. Never append it to the previous stream
  and label the result a complete cached track.
- Coverage tracking for seeks/retries: reaching EOF does not prove the whole file was
  fetched. Publish only a validated complete representation; distinguish quality and
  source identity, and avoid combining ranges if the remote representation changed.
- Playback priority over bulk downloads, bounded preload memory, no nested waits on
  the shared worker pool, and no App callbacks on transport threads. If an optional
  cache sink falls behind, abandon caching rather than stall audible playback.
- Explicit ownership for custom AVIO buffers/context on open failure, cancelled preload,
  handover and close. Native `avio_open2` cleanup cannot simply be reused unchanged.
- A media I/O adapter outside core with an optional transport dependency: net-off
  builds must retain local playback. A generic core filesystem abstraction is unnecessary.

**Experiment scope (not part of the required phases):** compare native FFmpeg I/O,
an AVIO tee, and curl-backed AVIO on the exact FFmpeg versions shipped by macOS/Linux/Pi
builds. Start with original HTTP MP3 and seekable M4B, keeping native I/O selectable for
comparison. Test tail metadata, forward/backward seeks, ignored Range, changed content,
midstream disconnect, cancel while blocked, preload handover and shutdown. Then test
chunked transcodes and time-offset reopen separately. New tests require sabotage checks.
Measure downloaded bytes, open/seek latency, underruns, CPU and peak memory while a
scan and download run. Adopt shared playback transport only if it eliminates redundant
fetches or demonstrates another measured benefit without breaking these behaviours.

Local-file opening currently happens on the UI thread; if measurement finds stalls,
moving open/probe onto the existing player job mechanism is a smaller independent fix
than replacing local I/O. Scanner tag reads already happen off the UI thread.

## Phase 1: Measurement and runtime foundation

Goal: establish reproducible costs and a tested runtime without changing service behaviour.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 1.1 | **Baseline.** Add a repeatable local-server workload and record results in `docs/async-measurements.md`: zero, one and four servers; idle, browsing, two downloads plus scan, stalled server and shutdown. Record build/device versions, CPU, RSS, threads, wakeups, API p95/p99, tick delays, cancel latency and audio underruns. Select explicit API queue/body budgets and relative regression thresholds before phase 2. Compare libuv overhead with retaining curl multi and adding a small worker runner. | Manual benchmark; no timing assertions in unit tests. Validate workload completion and reproducibility. |
| 1.2 | **Dependency boundary.** Add `oapasync`, system libuv discovery and actionable missing-dependency errors; update `docker/Dockerfile` native and Pi dependencies. No implicit network fetch. Preserve a core-only/no-network build without libuv. | Configure/build macOS, native Linux, Pi Zero 2 W and Pi 4 cross targets; no-libuv core-only build succeeds. |
| 1.3 | **Runtime lifecycle.** Implement owned loop/thread, command wakeup, bounded dispatch, finite work admission and graceful close. Keep current service workers active. | Burst submissions all finish; callback thread affinity; cancellation before/after work begins; shutdown rejects new submissions and leaves no live handles; exception/failure paths do not terminate the loop. |
| 1.4 | **Docs for phase 1 as built.** Record measurements, selected budgets, runtime ownership and dependency boundary in this plan, architecture and REPO guidance where applicable. | – |

**Device check (manual):** run the baseline on Pi Zero 2 W while playing and pressing
Talk/Play; record speech interruption, playback glitches and exit time. Desktop results
must be labelled separately if no device is available.

## Phase 2: Shared API networking

Goal: every server uses one network loop while browsing, cancellation and exit saves behave as before.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 2.1 | **Curl reactor.** Implement socket watchers, timers, completion draining and error cleanup in `CurlReactor`, initially exercised without replacing production clients. Handle timer removal/zero timeout, watcher removal and fd reuse. | Local sockets: simultaneous transfers, stalled peer isolation, timer expiry, removal/reuse and close with active requests. |
| 2.2 | **HTTP adapter.** Inject runtime into `HttpClient`; preserve `Http` and fake injection. Replace worker polling with reactor submission and bounded off-loop response handling. Apply phase 1 budgets. | Existing `test_http_client`, Subsonic and Audiobookshelf tests; identical ids on separate clients; stale completion after id replacement; large parse does not delay another server; overload completes with error. |
| 2.3 | **Ownership and exit.** Wire both front ends and `makeServers()` to one runtime; replace temporary finish threads with coordinated drain. Preserve downloader-before-source destruction and all exit acknowledgement semantics. | `test_make_music_server` with net on/off; final progress/scrobble acknowledgements; one stalled server does not block another; deadline abort; no callback after destruction. |
| 2.4 | **Docs for phase 2 as built.** Update architecture thread table, build instructions and measurements. | – |

**Device check (manual):** browse two servers with one disconnected; play, change books,
exit and restart. Confirm acknowledged progress is retained and failed saves remain retryable.

## Phase 3: Downloads without blocking the loop

Goal: API responses and buttons stay responsive during downloads to slow storage.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 3.1 | **Ordered file sink.** Add `app/net/download_sink.{h,cpp}` with bounded buffers, serialized file work, cancel fence and finalization. Keep production downloader unchanged until the sink is ready. | Byte-exact output with short writes; disk failure; bounded backlog with a gated writer; cancelled jobs never rename or touch a reused destination; success waits for final write/close/rename. |
| 3.2 | **Download adapter.** Move `HttpDownloader` onto `CurlReactor` and the sink; implement pause/resume, two-download global admission and fair server scheduling. Remove dedicated downloader threads. | Existing download tests; resume 206, restart 200/416, invalid bodies; pause/replay byte equality; cancellation while paused/writing; slow disk does not delay an API response; latest-only progress. |
| 3.3 | **Docs for phase 3 as built.** Update download and cancellation architecture; repeat mixed-load measurements including curl buffer overhead. | – |

**Device check (manual):** download two albums while streaming and navigating. Cancel one,
retry it, unplug/reconnect the network, then play the completed downloads offline.

## Phase 4: Bounded scan work and final evaluation

Goal: scans share finite work scheduling without starving network parsing or download writes.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 4.1 | **Separate scan algorithm.** Extract incremental traversal/tag/grouping state from `ThreadedScanner`; keep the existing adapter working. Bound each traversal batch to 32 entries and each tag task to one file; retain cancellation checks in grouping. | Existing scanner tests, including unchanged-tag reuse, grouping, unreadable files, throws and cancellation between files; chunk boundaries preserve the complete result. |
| 4.2 | **Async scanner adapter.** Add `app/async/async_scanner.{h,cpp}` implementing `LibraryScanner`; use one active scan chunk globally. Wire both front ends, including net-off builds. Keep TagReader alive until work settles. Remove obsolete threaded adapter only after parity. | Scanner contract suite on new adapter; scan saturation permits parse/disk progress; cancel suppresses queued completion; destruction with outstanding work; net-off frontend builds. |
| 4.3 | **Evaluate and document.** Repeat all phase 1 workloads. Compare against the recorded thresholds; investigate regressions before calling the migration complete. Update architecture, SPEC dependency details and REPO build guidance; record real-device checks still pending. | Existing core/net/media suites in applicable builds; native and Pi build matrix. No ASan/TSan unless explicitly requested. |

**Device check (manual):** scan a large library while downloading and playing. Confirm
speech interruption and navigation remain responsive, and compare CPU, memory, wakeups,
latency and underruns to baseline. Do not infer battery improvement from thread count alone.

## Decisions

- Plan around libuv inside the existing process, with a measured acceptance gate.
- Preserve dependency-free core, UI callback delivery and existing service fakes.
- Keep player/preload, speech and audio execution dedicated. Continuous decoding or
  synthesis must not occupy the shared finite-task pool.
- Keep the current UI tick frequency initially. Reducing the headless 10 ms wakeups
  needs an App next-deadline API covering gestures, sleeps and retries; defer it.
- Keep persistence semantics and stream-cache tee work separate. Future language-pack
  downloads can reuse the downloader; finite transliteration jobs can use bounded work.
- Every added or changed automated test needs the repository's pass/sabotage/fail/restore
  check. Benchmarks are evidence, not fragile wall-clock unit tests.

## Open questions

No product decision blocks phase 1. That phase must record queue budgets and performance
thresholds before later phases begin. Whether scanner pooling is beneficial on the Pi
is an empirical question; report a regression and revise the plan rather than assuming
that replacing a dedicated scanner thread saves resources.

## API references

- [curl multi with libuv example](https://curl.se/libcurl/c/multi-uv.html): socket/timer integration.
- [libuv async handles](https://docs.libuv.org/en/stable/async.html): cross-thread wakeup and coalescing.
- [libuv work pool](https://docs.libuv.org/en/v1.x/threadpool.html): global pool and finite work.
- [libuv cancellation](https://docs.libuv.org/en/v1.x/request.html): pending versus running requests.
- [libuv handle lifetime](https://docs.libuv.org/en/stable/handle.html): stable addresses and deferred close.
- [curl pause/resume](https://curl.se/libcurl/c/curl_easy_pause.html): replay, reentrancy and internal buffering.
- [libuv filesystem operations](https://docs.libuv.org/en/stable/fs.html): async file work and worker-pool defaults.
