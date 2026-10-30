# Deferred work and pending sync

This tracks the deferrals and pending states introduced by [streaming phase 5](streaming.md#phase-5-sync-and-playlists). Phase 5's real-server and device listening checks are still pending. These are not an exhaustive list of the project's planned work.

## Deferred features

- **Playlist names:** creation uses a generated timestamp name; user-entered names and renaming wait for [input methods](input-methods.md).
- **Offline star and server-playlist edits:** there is no general outbox for them. A failed server edit reports an error and is not queued. A later design must decide ordering, retries, idempotency and conflicts, including playlist creation followed by edits. The existing scrobble outbox and book-progress retry are separate.
- **Uploading local files:** Subsonic has no upload in this design. Moving local books or music to a server is outside this plan.

## Pending queue sync

The music queue's per-server pending markers are stored in `queue.tsv` on exit or sleep. Sync checks servers in the background; a pending marker does not by itself mean a save has succeeded. A newer valid server queue wins, subject to the playback and conflict rules in [navigation.md](navigation.md#playlist-editing-and-sync-streaming-phase-5).

| Pending condition | What can resolve it |
|---|---|
| Server unreachable, request fails, or the response is invalid | A later successful sync round can compare versions and retry. |
| Current track has no reference on that server | Play an eligible track for that server; a mixed queue is filtered for that server. |
| The device clock has no usable wall time | Set the clock and make a new local queue or place change to give it a usable timestamp. |
| Server returns an empty queue without a usable timestamp | A later server response with a usable version permits a comparison. Until then upload is deferred: the empty response could represent a newer clear. |
| Legacy server cannot identify the current occurrence among duplicate track IDs | Use a server with `indexBasedQueue`, or change the queue so its current track is unambiguous. |
| Track opening or seeking is in progress | Wait for it to finish so the device can send the actual place. |

A newer remote queue arriving during music playback is held until it can be applied without interrupting playback. A subsequent local change discards that held snapshot. Failed book progress saves are a separate pending state: they retry on startup. A Subsonic bookmark conflict suppresses retries of the same older local place until a new local place is recorded; opening the book still pulls the server place. A changed track order or duration can prevent safe progress mapping.

## Implementation limits

- Subsonic has no conditional queue or bookmark write. A second client can update the server between the device's read and write. If a mixed-queue save reaches the server but its follow-up acknowledgement is lost, its filtered subset can replace the full device queue on restart; an identical newer write from another client cannot always be distinguished from an echo.
- Local progress and queue wall timestamps have whole-second precision, so changes within one second cannot always be ordered.
- Shared cached audio uses filesystem hard links. If creating a link fails, a user-initiated operation reports "Cannot write the download."; background reconciliation keeps the last complete snapshot and retries on a later refresh.
