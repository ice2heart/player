# Music queue, new buttons and separate players: plan

> Status: **a plan, nothing built.** The rules it changes are in [navigation.md](navigation.md),
> which is updated first (step Q0), before any code. The code as it is now is described in
> [architecture.md](architecture.md); streaming is in [streaming.md](streaming.md).
> Last updated: 2026-10-27.

Today the device has no queue. `App` holds one `book_`: the book, the local album or
"Play all" list, or the album or playlist from the server. Choosing a track replaces
it. Music and books share one player, told apart by `music_`. Volume sits on Up /
Down in the player, and an item has no actions beyond OK.

This plan adds:

- **Separate players** for music and for books (podcasts later use the book player).
- **A queue** for the music player, with tracks from any source (local files and the
  server, mixed), shuffle and repeat, a list of what is queued, and "Save as playlist".
- **New buttons:** Volume up, Volume down, and Options, which opens the actions for the
  selected item or the playing track ("Play next", "Add to queue", ...).

Each step leaves a device that works.

---

## Buttons

| Button | What it is for | Changes |
|--------|----------------|---------|
| Up / Down | move in menus; in a player, choose what Left / Right move by | no volume any more |
| Left / Right (two-stage) | in a player: skip, and a deep press or hold goes to the previous / next track or chapter, as now | in menus: no value change any more (see [Menus](#menus-left-and-right)) |
| OK | go in; in a player, the Player menu | – |
| Home | go out; hold for the Home menu | – |
| Play/Pause | play / pause; hold for "where am I?" | – |
| **Volume up / down** | content volume, on every screen | **new** |
| **Options** | actions for the selected item, or in a player for the playing track | **new** |
| Talk | push to talk (STT, later) | – |
| Power | sleep / wake; hold for the Power menu | – |
| Hold switch | locks every key except Power | – |

Text (playlist names, search, Wi-Fi passwords) comes from STT or a keyboard later.
Until then names are made by the device.

### Volume

- **Volume up / down change the content volume** on every screen, menus included.
  Speech keeps its own level. At 0 and 100 % the edge sound plays.
- **Holding a volume key repeats the step** (about every 150 ms) until it is let go.
  `GestureDetector` gets a repeat event for these two keys; the other keys keep
  short / long / deep.
- As now, the volume is kept in memory and written to `config.yaml` only on exit.

### Up / Down in a player: the step unit

With volume on its own keys, Up / Down in a player choose **what Left / Right move by**,
like the navigation levels of a Victor Reader Stream. As in a menu, Down moves to the
next unit and Up to the previous one, and each speaks the unit ("Track"). The edge sound
plays at the first and last unit.

| Player | Units (Up / Down) | Light press of Left / Right |
|--------|-------------------|-----------------------------|
| Music | Skip · Track · Album | the skip of Settings (back / forward), the previous / next track, or the first track of the previous / next album in the queue |
| Book | Skip · Chapter · 10 minutes | the skip of Settings, the previous / next chapter, or 10 minutes |

- The first unit is **Skip**, so Left / Right do exactly what they do now until the
  user presses Down.
- **A deep press or hold of Left / Right always goes to the previous / next track or
  chapter**, whatever the unit, as now.
- The unit is kept per player and saved in `state.tsv` (`unit.music`, `unit.book`),
  written on sleep and exit like the rest of the state, so it is the same after a wake
  or a restart.

### Menus: Left and Right

Settings no longer change in place with Left / Right. A setting is changed in its list
(OK opens it on the current value, as for "Setting with list" now). Left / Right in
menus then **jump in long lists**:

- In a list sorted by name (Albums A to Z, Artists, Library, Music), Left / Right go to
  the first item of the previous / next letter and speak it: "D. Dune, 41 of 500".
- In other lists they go 10 items back or forward.
- At the first or last item the edge sound plays (with Menu wrap around: it wraps, as
  Up / Down do).
- This does not repeat what OK and Home do, so each action keeps one key.
- **To be confirmed on a real device** with blind users before it is final (open question 1).

### Options

- **Options on a menu item** opens "Options, Weather": a short list of what can be done
  with that item. **Options in a player** gives the actions for the playing track.
- **OK and Options in a player stay different.** OK opens the Player menu, which is
  about the player (Queue, Shuffle, Repeat, Sleep timer, info). Options is about the
  playing track: Go to album, and Star / Unstar for a remote track. Star is built in
  the Player menu in [streaming.md phase 2a](streaming.md#phase-2a-stars); Q7 moves it
  to Options, so it has one key. Go to artist comes later.
- An item with no actions plays the edge sound and says "No options" (rule 9: every
  press of a key is heard).
- An action that finishes a task (Play next, Add to queue) goes back by itself and
  says what it did. An action that only reports something stays.
- `MenuItem` gets an `options` builder, next to `submenu` and `action`. Lists build it
  for their items; the player builds it for the current track.
- At Beginner verbosity the hint names it: "Press OK to play, Options for more."

---

## Two players

- **The music player** plays the queue. **The book player** plays one book, with its
  position kept per book and `last_book`, as now. Podcasts will use the book player.
- They share one `MediaPlayer`. Starting one pauses the other and keeps its place, so
  going back to it resumes where it stopped.
- Each is its own program: `Screen::Kind::MusicPlayer` and `Screen::Kind::BookPlayer`,
  each with its own Player menu. The "one player screen" rule becomes "one screen per
  player".
- **Home:** "Now playing, <album or book>" for the player used last, and "Continue,
  <the other>" below it when the other player has something loaded.
- The book player has no queue, shuffle or repeat.

---

## The queue

### What it holds

- **Entries**, each one of:
  - a local track: path, title, album, artist;
  - a remote track: the server key (url and user, never the password or a stream URL),
    the track id, title, album, artist and length.
- **The original order** and **the play order** (the same unless shuffled). Turning
  shuffle off goes back to the original order.
- **The current entry** and its position.
- **Shuffle**: off · tracks · albums. "Albums" keeps each album in order and shuffles
  which album comes next.
- **Repeat**: off · all · track. Repeat track plays the current track again each
  time it ends; Left / Right and a deep press still move to other tracks, and the track
  moved to is then the one that repeats.
- **Shuffle and repeat belong to the queue.** A new queue starts with both off. They
  are not settings in `config.yaml`.
- **Edited**: whether the user changed the queue by hand (added, moved, removed).

The queue is a plain class in `app/core/queue.{h,cpp}` with no `App` and no I/O. The
random generator is passed in, so tests choose the shuffle.

### Filling it

| Action | Where | Does |
|--------|-------|------|
| OK on a track | any list of tracks | **Replaces** the queue with that list and plays from the track, as now. The common case stays one press. |
| Play album / Play playlist / Play all | first item of the list | Replaces the queue with the whole list. |
| Options → Play now | a track, album, playlist, artist | Replaces the queue with it and plays. |
| Options → Play next | the same | Puts it after the current entry. |
| Options → Add to queue | the same | Puts it at the end. |

- A remote album, playlist or artist is loaded first: "Loading.", then "Added
  Weather, 12 tracks. 30 in queue." Home while it loads drops the late answer.
- With nothing loaded, Play next and Add to queue make a queue that is paused at its
  first entry: "Added Weather, 12 tracks. Press play."
- **Previous queue:** when a queue that was edited by hand is replaced, it is kept as
  "Previous queue" at the end of the Queue list, and OK on it brings it back. This is
  the undo, so replacing a queue needs no confirmation.

### Playing it

- Entries play in the play order. Read-ahead pre-opens the next remote entry that is
  available (with Repeat track, the current one again), and scrobbling works per remote track as now. Local tracks are never
  scrobbled.
- At the end of the queue: "End of queue", paused at the first entry, unless Repeat
  is on. Repeat all goes on from the first entry; Repeat track plays the last track
  again, as it does any track.
- **An entry that cannot play is skipped:** the device says **"Storm, source
  unavailable."** and goes on with the next entry. A run of entries skipped together is
  said once: "3 tracks, source unavailable." An entry is unavailable when:
  - its local file is missing;
  - its server is not the one set up now (another url or user), there is no server, or
    the build has no network support;
  - its server is **marked down** (below).
- **A server that fails is marked down.** When a remote track does not open because
  the server cannot be reached, or a stream breaks off and `Player`'s retries give up,
  the server is marked down. Its entries are then skipped without being opened, since
  each try can take up to 15 s, and local entries go on playing.
- **The server is checked while it is marked down.** The device pings it in the
  background while the music player has remote entries ahead: after 10 s, then 30 s,
  then every 60 s, and at once when the user presses Play. Once a ping answers, the
  mark is cleared and the next remote entry is opened normally. **Server status** in
  the Streaming menu says the server is not reachable while it is marked down, and a
  ping from there that answers clears the mark too.
- **When nothing ahead can play** (the server is down and only its tracks are left),
  playback stops at the first skipped entry and the device says "Server not reachable.
  Press play to try again." Play checks the server and plays if it answers.

### Player menu (music)

```
Player menu (music player)
├── Queue → <entries>      opens on the current entry, spoken "current"
├── Shuffle, off           list: off · tracks · albums
├── Repeat, off            list: off · all · track
├── Album info
├── Go to first track      the first entry in the play order
└── Sleep timer → …
```

Queue takes the place of today's "Tracks" item, which lists the album: Q3 keeps
"Tracks" in the music Player menu, and Q9 replaces it with Queue.

Shuffle and Repeat are "Setting with list" items backed by the queue instead of
`config.yaml`. A change works at once: shuffle keeps the current entry current and
shuffles what follows.

### The Queue list

```
Queue
├── <entries>              OK plays it; Options: Play next · Move · Remove
├── Save as playlist
├── Previous queue         only when there is one
└── Clear queue →          Cancel · Clear queue
```

- **Opens on the current entry**, which is spoken as "current": "Storm, current, 5 of 30".
- **Move** opens a list of positions on the entry's own, like a setting: OK on another
  position moves it there and says "Moved to 3 of 30".
- **Remove** of the current entry stops it and makes the next entry current, paused.
- **Clear queue** sits behind a list that opens on Cancel (navigation rule).

### Save as playlist

- **Only remote tracks of the server set up now are saved, as a playlist on that
  server** (`createPlaylist`). A remote track that plays from a local cached copy is
  still a remote track and is saved.
- Local tracks and tracks of another server are left out: "Saved Queue, 27 October
  14:05. 3 tracks not saved."
- The name is made from `clock_.wallTime()`: "Queue, 27 October 14:05". Renaming comes
  with STT or a keyboard.
- With no entries from the server set up now, or no server, the item says why: "Only tracks from the server
  can be saved."
- An error is spoken ("Server not reachable."), and the queue stays as it was.

### Keeping it between restarts

- The queue is saved in its own file, `queue.tsv`, next to `state.tsv`: every entry with
  its titles (so a restart needs no network to list it), the current entry and its
  position, the play order, shuffle and repeat. No stream URL, token or password.
- **Written on sleep and on exit** (Power off, Restart, `shutdown()`), only when it has
  changed, like `state.tsv`, to spare the flash. A crash loses changes since then.
- It replaces `last_stream.*`. The old keys are read once and become a queue of that
  album or playlist, then removed.
- After a restart the music player has its queue at once. Nothing plays until the user
  chooses it, as now.
- A queue that is very long is cut to 5000 entries around the current one when it is saved.

---

## Steps

Every step is one commit and gets tests, each checked with a sabotage (CLAUDE.md).

### Part 1: buttons and two players

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| Q0 | **Navigation contract first:** navigation.md gets the new buttons and key table, the Options rule, the step unit, Left / Right in menus jumping by letter or by 10, the two players and the queue rules. SPEC §3.3 and §5.4 button tables. | – |
| Q1 | **Volume and Options buttons.** `Button::VolumeUp`, `VolumeDown`, `Options`; a repeat event in `GestureDetector` for the volume keys; volume on every screen, edge sound at the limits; Options says "No options" where there are none. Key describer, Help → Buttons, "Getting started". Front ends: simulator keys, terminal keys, evdev `KEY_VOLUMEUP` / `KEY_VOLUMEDOWN` / `KEY_MENU`, gamepad layout. | Volume up in a menu and in the player changes the volume and writes nothing until exit. Holding it repeats; letting go stops. Edge sound at 100 %. Options with nothing to act on says "No options". Key describer names the new keys. Terminal, gamepad and evdev maps give the new buttons. |
| Q2 | **Up / Down in a player choose the step unit; menus lose in-place change.** Units per player as in the table above; Left / Right move by the unit; deep and hold still change track or chapter. The unit is saved per player in `state.tsv`. Settings change only through their list. Left / Right in menus jump to the previous / next letter in lists sorted by name, else 10 items. Existing tests that press Up / Down for volume or Right to change a setting are rewritten for the new keys, each checked with a sabotage. | Down from Skip says "Track", and Right then starts the next track; Up from Skip plays the edge sound. Album unit goes to the first track of the next album. Book: 10 minutes moves 600 s. Deep press changes track whatever the unit. Edge sound past the last unit and before the first. Sleep, wake and a restart keep each player's unit. A setting is no longer changed by Right. Right in Albums A to Z goes to the first album of the next letter and speaks the letter; in an unsorted list it moves 10; edge sound (or wrap) at the end. |
| Q3 | **Separate music and book players.** Book state (book, file, position, `last_book`) and music state apart; `Screen::Kind::MusicPlayer` / `BookPlayer`; each its own Player menu; Home "Now playing" and "Continue". | A book, then music, then the book again: the book resumes where it was and music where it was. Home lists both. Each Player menu has its own items. The existing book and music tests (as rewritten in Q2) pass. |

**Device check (manual), with blind users:** the volume and Options keys can be found
by touch. Left / Right jumping by letter in Albums A to Z feels natural and is not
mistaken for changing a value. The step unit is clear from what Up / Down say. Decide
open question 1 here, before Part 2.

### Part 2: the queue

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| Q4 | **`Queue` class** (`app/core/queue.{h,cpp}`): entries, original and play order, current, shuffle off / tracks / albums, repeat off / all / track, edited flag; replace, play next, append, remove, move, next / previous, set shuffle. | `tests/test_queue.cpp` on the class. Shuffle albums keeps each album's order. Shuffle off gives back the original order with the same current entry. Repeat all wraps; repeat track stays at the end of a track, but next / previous move; off ends. Remove and move of the current entry. Replace resets shuffle and repeat. |
| Q5 | **The music player plays the queue**, local and remote mixed. Read-ahead and scrobbling follow the play order. "End of queue". | A mixed queue plays local, remote, local in order. Read-ahead pre-opens the next remote entry. A local track is not scrobbled. End of queue pauses at the first entry. |
| Q6 | **Unavailable sources and server checks.** "Storm, source unavailable." (or "3 tracks, source unavailable." for a run) and skip; a server marked down after a failure; background pings (10 s, 30 s, 60 s, and on Play); Server status reflects the mark; "Server not reachable. Press play to try again." when nothing ahead can play. | A missing local file is skipped with "Storm, source unavailable.". A track from another server is skipped. After one failed open, the next remote entries are skipped without an open and a local entry plays. A ping that answers clears the mark and the next remote entry opens. Pings stop when no remote entries are ahead. Only remote entries left: stops and says so; Play pings and plays. A run of skips is said once, with the count. |
| Q7 | **Options: Play now, Play next, Add to queue** on local tracks, albums and Play all, and on remote tracks, albums, playlists and artists (loaded first). Options in the music player for the current track: Go to album, and Star / Unstar moved there from the Player menu if phase 2a is built. Previous queue when an edited queue is replaced. | Each action on a local and a remote item puts the right entries in the right place and says the counts. Home while a remote album loads: nothing is added and nothing is said. Play next with nothing loaded: paused, "Press play". Replacing an edited queue keeps "Previous queue", and OK on it restores it. |
| Q8 | **Shuffle and Repeat in the music Player menu**, backed by the queue. | Choosing Shuffle tracks works at once and keeps the current entry. A new queue starts with both off. The book Player menu has neither. |
| Q9 | **The Queue list**, in place of "Tracks" in the music Player menu: opens on the current entry ("current"); OK plays an entry; Options on an entry: Play next, Move, Remove; Clear queue behind Cancel. | Opens on the current entry and says "current". Move to 3 says "Moved to 3 of 30" and the order changes. Remove of the current entry makes the next one current, paused. Clear queue opens on Cancel. |
| Q10 | **Keep the queue**: `queue.tsv` written on sleep and exit, when changed; read on start; `last_stream.*` read once and removed; cut to 5000 entries. | Sleep, then restart: the same mixed, shuffled queue, current entry, position and repeat. Exit writes it; a second exit with no change writes nothing. Old `last_stream.*` keys become a queue and are removed. Nothing in `queue.tsv` contains a stream URL, a token (`t=`, `s=`) or the password. |
| Q11 | **Save as playlist**: `MusicServer::createPlaylist(name, trackIds, done)`, `Subsonic` `createPlaylist`, `FakeMusicServer`. Remote entries only, cached copies count as remote; name from the wall time. | Net tests: the URL has `name` and one `songId` per track, and no `p`. A mixed queue saves only the ids of the server set up now and says how many tracks were left out (local, and from another server). No remote entries: says why and sends nothing. An error is spoken. |
| Q12 | Docs: architecture.md (players, queue, `queue.tsv`, new buttons, server checks), navigation.md where the build differs from Q0, streaming.md (`last_stream` replaced), SPEC. | – |

### Later

- **Offline playlists** ("Keep on device" in the Options of a server playlist): download
  its tracks to the cache so the queue plays them from local files, and follow changes on
  the server. This is the same machinery as the cache in
  [streaming.md phase 3](streaming.md#phase-3-local-cache-and-downloads) and "Keep offline"
  in [phase 5](streaming.md#phase-5-sync-and-playlists), so it is built there. A queue
  entry already prefers a cached copy once that exists, and stays a remote track.
- **Play queue sync** with the server (`savePlayQueue` / `getPlayQueue`), streaming.md
  phase 5.
- Options for books (Add bookmark, Mark finished) and in the book player.

---

## Decisions

- **A new Options button** opens the actions for an item. Hold OK keeps going out.
- **Volume gets two buttons of its own** and changes the content volume on every screen.
- **Left / Right in a player stay as they are**: skip, and deep or hold for the previous /
  next track or chapter.
- **Up / Down in a player choose the step unit** for Left / Right. The unit is saved
  per player.
- **Settings change only in their list.** Left / Right in menus jump by letter in lists
  sorted by name, else by 10 items (to be confirmed on a device, open question 1).
- **OK in a player opens the Player menu, and Options gives the playing track's actions.**
- **Music and books (and later podcasts) have separate players.** Only music has a queue.
- **A queue mixes any sources.** An entry that cannot play is skipped with "Storm,
  source unavailable." (a run: "3 tracks, source unavailable.").
- **A server that fails is marked down** and checked in the background until it answers.
- **Shuffle and repeat reset with each new queue.**
- **The queue is saved on sleep and on exit**, in `queue.tsv`.
- **Save as playlist saves remote tracks only** (cached copies count as remote), as a
  server playlist.

## Open questions

1. **Left / Right in menus jump by letter or by 10** (chosen): check it on a real device
   with blind users. The ones not chosen, in case it does not work out: Left goes out
   and Right goes in, like a tree (this duplicates Home and OK), or Left / Right do
   nothing in menus.
