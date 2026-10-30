# Navigation guidelines

> Status: this contract includes the approved [button-remap.md](button-remap.md)
> mapping and parameters, recorded before code in step 1.1. The gesture lifecycle, menu/Options/player mapping,
> front-end input and help are built (steps 1.2–1.5). [architecture.md](architecture.md) describes
> the build; real listening checks remain pending.
> The two players, queue, Options actions, server menus, progress sync, download
> quality and cache quality are built. Star / Unstar and Streaming → Starred are built
> ([streaming.md](streaming.md) 2a.1). Real-server and device listening checks
> remain pending. Last updated: 2026-10-30.

Users can't see the menu, so they learn it from what each key does. A key that
does one thing in one menu and something else in the next menu breaks that.
These rules are the contract. Follow them when you add a menu or an item, and
change this file before you change a rule.

## Screens

Everything the user is "in" is a **screen** on one stack (`App::screens()`):

- a **menu**: a spoken list of items, or
- a **program**: a screen with its own keys. The programs are the **music
  player** and the **book player** (see [Two players](#two-players)). A
  podcast player or a button tutorial would be programs too.

Home is always at the bottom of the stack. Short OK pushes a screen, and short
Esc (the `Home` input in code) pops one, back to wherever you came from. If you
open a book from Library, Esc takes you back to Library. Short Esc at Home
returns to the loaded player without changing whether it is playing or paused;
with no loaded player it plays the edge sound and says "Home". Held Esc stays at Home.
There is only one screen per player: opening a player again returns to the one
already on the stack. The Power menu works the same way, and Sleep in it closes
the menu first, so the device wakes where you were.

## The rules

1. **Back / Forward move through menus.** A short press moves one item, a deep
   press jumps ten immediately, and a hold reaches the first / last item once.
   Single steps follow Menu wrap; ten-item and end jumps always clamp.
2. **Short OK goes in.** It opens the selected item's submenu or program, or runs
   its action. In a player it opens the Player menu.
3. **Short Esc goes out** to the previous screen. Leaving a screen changes nothing.
   At Home it returns to the loaded player, including a paused one, without resuming it.
   With nothing loaded it plays the edge sound and says "Home".
4. **Held Esc goes to Home**, from anywhere, and stays Home if already there.
5. **Held OK opens Options** for the selected item or current playback. The
   separate Options input is an alias. Releasing held OK never activates the item.
6. **A value is changed in its list, and works at once.** OK opens a setting's
   list on the current value; choosing a value saves it and goes back. Speech rate
   changes immediately. Volume is kept in memory and written on exit and sleep.
   Streaming quality applies to the next track, Download quality to the next
   download, and Replace cached copies when the next track opens. They do not
   restart a track or transfer already running.
7. **In a player, short Back / Forward move through chapters or tracks.** Back
   restarts the current chapter/track after more than three seconds; at or before
   three seconds it goes to the previous one. Holding seeks until released; deep
   seeks faster (see [Seeking](#seeking)).
8. **Play/Pause, Volume and Power work the same on every screen.** Held Play
   speaks the menu path and selected item, or the player's name, album, artist
   and status (see [Where am I in a player](#where-am-i-in-a-player)), followed
   by any download progress. Volume changes content volume (see [Volume](#volume)).
9. **Active keys give audible feedback.** An action interrupts speech and plays
   an earcon, speaks, or both. Up, Down and Talk are silent during normal use:
   they do not interrupt speech or change anything. Help's key describer can
   still describe them. Repeating volume and seeking play an edge sound once
   on reaching a limit, rather than on each repeat.
10. **Options is about the selected item.** It opens actions for the selected
    menu item or current playback. An item with none says "No options" with the
    edge sound. Inside an Options list, held OK or Options says "No options";
    it never nests another Options list.

## Item types

What an item does depends on which parts of `MenuItem` are set:

| Type | `value` | OK opens | Example |
|------|:-:|---|---|
| **Submenu** | (info) | `submenu` | Library, Road Trip (`2 tracks`) |
| **Setting with list** | ✓ | `submenu` of all values, on the current one | Speech rate, Volume (an on/off value is a list of two) |
| **Status with list** | ✓ | `submenu` of choices, on the current one | Sleep timer (`15 minutes left`) |
| **Action** | (info) | runs `action` | Status, Time and date |
| **Program** | (info) | `action` pushes the program's screen | a book, a track, Now playing |
| **List from the server** | (info) | `action` opens a list that loads from the server | Recently added, an album on the server |

No type changes its value with Left / Right: `MenuItem` has no `adjust` part. Any item may also have an `options` builder (see
[Options](#options)).

`choice()` in `app.cpp` builds a *setting with list* from a settings key and
its options. In the list the current value is spoken as "current". Choosing a
value with OK saves it and goes back. Use `choice()` for every user setting.
Volume stays in Settings as a list; the volume keys change the same value.

## Two-stage keys

Back and Forward (Left and Right) may have a light contact and a separate deep
contact (`BackDeep`, `ForwardDeep`). A short action happens only on release,
if no hold or deep action has happened. A hold starts at the configured Long
press time (600 ms by default), measured from the initial light press.

| | Short light press | Light hold | Deep press |
|---|---|---|---|
| Menu | previous / next item | first / last item, once | ten items back / forward immediately, then first / last on continued hold |
| Player | previous / next chapter or track | repeated normal seeking | repeated faster seeking immediately |

Deep can begin before or after a light hold. In a menu, deep-then-hold still
uses the threshold measured from the initial light press. In a player, easing
from deep to light changes to normal seeking without a track skip. Fully
releasing stops seeking; release order and contact bounce must not produce a
short action after a hold or deep action. On one-stage hardware, holding gives
normal seeking or an end jump; the deep stage is optional.

Hold lock, sleep, a screen change and shutdown cancel the active gesture and
seeking. Keys already held remain suppressed until released, so cancellation
cannot cause a late short action or restart seeking on a new screen.

## Buttons and keys

| Button | What it is for |
|--------|----------------|
| Back / Forward (two-stage) | menu movement; chapter/track movement and seeking in a player |
| OK | short selects or opens the Player menu; held opens Options |
| Esc (`Home` input) | previous screen; at Home return to the loaded player; held goes Home |
| Play/Pause | play / pause; held says "where am I?" |
| Volume up / down | content volume on every screen; held repeats |
| Options | alias for held OK |
| Up / Down / Talk | silent, no action during normal use |
| Power | sleep / wake; held opens the Power menu |
| Hold switch | locks every key except Power |

Text entry is not built yet; until then names are made by the device. The planned
button editor and keyboard adapter are described below.

## Key table

| Key | Menu | Player |
|-----|------|--------|
| Back / Forward short | previous / next item | previous / next chapter or track (Back restart rule applies) |
| Back / Forward deep | ten items immediately; continued hold reaches first / last | faster seeking immediately; ease to light for normal seeking |
| Back / Forward held | first / last item, once | seek until released |
| OK short | open / run | Player menu |
| OK held / Options | selected item's Options ("No options" if none) | current playback's Options |
| Esc short | previous screen; Home returns to loaded player without resuming | previous screen |
| Esc held | Home menu | Home menu |
| Play/Pause short / held | play / pause; where am I? | same |
| Volume up / down | content volume; held repeats | same |
| Up / Down / Talk | silent, no action | silent, no action |
| Power short / held | sleep / Power menu | same |

In Help → Key describer, keys describe their actions rather than performing them;
held Esc leaves the mode. Normal Hold locking and wake behavior remain unchanged.

## Planned input and hardware navigation

This section is a proposed contract, not a description of the current build. The
button editor is planned in [input-methods.md](input-methods.md) phase 1; the HAL
steps below are in [hal.md](hal.md). Ordinary menus and players keep the keys above.

### Text entry

The planned TextEntry program keeps a draft in memory. Its list is Characters,
Cursor, Delete previous character, Read text, Clear text, Done and Cancel. Back /
Forward navigate as in any list; OK activates. Short Esc in a character group or
group list goes back one level; at the editor it cancels and returns. Held Esc
discards the draft and goes Home. Leaving never commits. Clear text asks
“Clear text?” with Cancel selected first. Done validates: an error is spoken and
the draft remains in the editor; only successful Done changes the caller's value.
There is no editor or confirmation timeout.

Characters offers lowercase, uppercase, digits, all printable ASCII punctuation,
and space. OK opens a group; OK on a character inserts at the cursor and stays in
the group. Cursor lists insertion positions including Start and End, described by
adjacent character and ordinal; selecting one returns to the editor. Delete removes
the previous Unicode code point; at Start it plays Edge and says “Start of text.”
Initial button insertion is ASCII and imported UTF-8 must remain valid. A request
provides a field label, initial value, secret flag, byte limit and validation; the
editor ceiling is 4096 UTF-8 bytes and callers may set a smaller limit. Oversize
input says “Text too long.”

Play/Pause and Volume retain their global jobs. Held Play describes the editor,
field and cursor; held OK / Options says “No options”. Up and Down remain inactive.
For a secret, insertion says “Character added”, readback says “Hidden, N characters.”,
and cursor descriptions and other feedback use positions and counts rather than
characters or values. Secrets must not appear in speech,
logs, debug widgets or snapshots. The keyboard and future voice input use this same
draft, validation, confirmation and secret handling; they never commit implicitly.
How a user selects a secret character from a spoken button list without revealing
it remains open; resolve that interaction before implementing secret button entry.

HAL phase 2 routes button events with their timestamps and maps keyboard controls
to the existing button actions, ignoring mapped-key autorepeat. Both Shift keys
retain the staged Back / Forward lifecycle. In TextEntry, keyboard text and
Backspace, Delete, Left / Right, Home / End, Enter and Escape edit, confirm or
cancel the same draft. Disconnect does not commit. Keyboard connection and
disconnection are announced at low priority. Linux keyboard layouts follow in HAL
phase 4; Android native keyboard input is planned for phase 9. Talk remains
inactive until its separate capture and recognition contract is implemented.

HAL phase 2 also trials completed touch gestures: swipe left / right for short Back /
Forward, two-finger swipe left / right for menu jumps, double tap for OK, two-finger
double tap for Play/Pause, touch and hold for Options, two-finger swipe down for
Home, and three-finger tap for held Play. Swipe up / down is unassigned; unknown
gestures play Edge. Player touch seeking remains unavailable pending a bounded seek
or a start/change/release lifecycle. TalkBack conflicts and the production gesture
contract require the phase 8 trial.

### Hardware-dependent menus and output loss

System → Status speaks the battery percentage and whole GiB free from the selected
HAL snapshots, omitting either phrase when that part is absent. It also says “Wi-Fi
connected.” as a placeholder and reports the time. The simulator presets supply 80
percent and 10 GiB free; interactive snapshot controls are planned for phase 3.
Battery events announce 20, 10 and 5 percent once each, rearmed when charging
starts; they also announce charging start and full. At 3 percent, the app saves
before requesting power off if supported, or says “Battery critical.” The 5 percent
and unsupported critical warnings interrupt current speech. The Wi-Fi placeholder
remains until phase 5.
Phase 3 adds Audio output when selection is supported, with Automatic, Speaker,
Headphones and available Bluetooth outputs; each output retains its own volume.
Losing headphones or Bluetooth while playing pauses and saves position, says
“Headphones unplugged, paused.” or “Bluetooth disconnected, paused.”, and does
not resume automatically on the speaker.

Phase 5 plans capability-dependent Wi-Fi status, toggle, networks and confirmed
forget actions; phase 6 plans Bluetooth discovery, pairing and device actions if
Bluetooth is in scope. Unsupported parts and actions are omitted, including on
Android. Live scan lists refresh without unnecessarily changing the selection.
Exact Wi-Fi prompts, errors and forget confirmation remain open before step 5.3;
Bluetooth scope, prompts and pairing policy remain open before phase 6. Maximum
volume, pause-on-unplug settings and USB file transfer need their own decisions and
are not scheduled. These open choices must be resolved before their affected code.

### Volume

- **Volume up / down change the content volume** on every screen, menus included.
  Speech keeps its own level. At 0 and 100 % the edge sound plays once per press, not on each repeat.
- **Holding a volume key repeats the step** (about every 150 ms, after a short 400 ms
  delay so a slow tap steps once) until it is let go.
  These immediate steps and repeats are independent of the navigation hold and
  deep-stage lifecycle.
- The volume is kept in memory and written to `config.yaml` only on exit and sleep.

### Seeking

- A light hold seeks 5 seconds every 500 ms; a deep press seeks 30 seconds
  every 500 ms, in either direction. Apply the first step immediately when
  seeking starts. Changing stages changes the step size without an extra step
  or resetting the next repeat deadline.
- Preserve playing/paused state. Clamp within the current file; short presses
  cross chapters/tracks/files. A seek to the file end stays in that file after
  release; seeking backward, short navigation, or explicit Play resumes normal
  end handling. With unknown duration, clamp backward at zero
  and permit forward seeking without an invented upper bound.
- Play one movement earcon when seeking starts and one edge earcon on reaching
  a known boundary. Speak the final position on release, with no speech queued
  on each repeat. Cancellation stops seeking and leaves feedback to the new
  context (for example "Locked"), without a trailing track skip.
- Retire the step-unit controls and Skip back / Skip forward settings. Older
  saved `unit.music` / `unit.book` keys are harmless and do not determine actions;
  do not rewrite saved state solely for migration.
- These rates are approved implementation defaults, not measured UX results.
  Local and streamed/transcoded playback need device listening trials.

### Back and Forward in menus

- Short Back / Forward moves one item, in sorted and unsorted lists alike.
  Ordinary steps obey Menu wrap, with the existing wrap or edge earcon.
- Deep Back / Forward jumps ten items immediately. Hold reaches the first /
  last item once, including after a deep jump. Ten-item and end jumps clamp
  regardless of Menu wrap. Sorting does not change the gesture to a letter jump.
- Speak the destination item and its usual position information; play the edge
  sound if movement is blocked at the boundary. Empty/status lists retain their
  own explanatory announcement; a single-item list remains on that item.
- A remote end jump stops at the loaded boundary and never fetches unseen pages.
  Keep Load more accessible. When more pages remain, append "More items are
  available. Load more to continue." to the boundary announcement. At the true
  end of a fully loaded list omit that line. Loading and error rows keep their
  own announcements instead of promising Load more prematurely.

### Options

**Existing actions (queue.md step 2.4):** Options on a menu item opens "Options, <item>" with Play now, Play next
and Add to queue, on local tracks, local albums and Play all, and on remote tracks, albums,
playlists and artists. Every other item plays the edge sound and says "No options".
The book player also says "No options" unless it has a later server place to offer
(audiobookshelf.md 3.4). An album's Options also has Album info (step 2.5). A remote
album, playlist or artist is loaded first ("Loading."); Play now says "Playing Weather." and the
other two "Added Weather, 12 tracks. 30 in queue."; with no music queue loaded they say "Press play."
instead (and while a book plays they leave it playing and keep the queue for Continue).

- **Options on a menu item** opens "Options, Weather": a short list of what can be done
  with that item. **Options in a player** gives the actions for the playing track.
- **Short OK and held OK in a player stay different.** Short OK opens the Player menu, which is
  about the player (Queue, Shuffle, Repeat, Sleep timer). Held OK / Options is about the
  playing track: Go to album, Album info, and Star / Unstar for a server-linked track (including local playback). Album
  info moves to Options from the Player menu and from the end of an album's list (an
  album in a list has it in its Options). Star / Unstar loads the current state from the server in the player's Options,
  after Go to album and Album info. While it loads those two actions remain usable.
  OK says "Saving."; success returns to the player and says "Starred." or "Unstarred."
  An error is spoken and the Options stay for retry. Leaving, opening another screen,
  changing tracks, sleeping or exiting cancels the callback.
  Stars are offered for remote tracks and locally played tracks whose library entry has
  an exact server track reference. Local playback stays local. Choose the first configured
  star-capable server in configuration order with a valid track reference; album, playlist,
  empty, malformed and removed-server references do not qualify. Unlinked local tracks and
  book providers have no star action. Stars never sit in the Player menu.
  Streaming → Starred loads the server's starred tracks with the usual Loading, error,
  Try again and empty-list feedback. OK plays from the selected track as a normal queue. Go to artist comes later.
- **Download album / Download playlist** is in the Options of an
  album or playlist of the server, in Music and in Streaming, after Add to queue. It is
  offered only where it can work (a cache folder, a server and a downloader that are set
  up, as for the list item below), and not for an album or playlist that is whole on the
  device at the requested Download quality. OK loads the tracks first, as Play now and Add to queue do ("Loading."; leaving
  cancels it; an error is spoken with the error sound and the Options stay), then starts the
  download, says "Downloading, 0 of 9 tracks." and goes back by itself. While a download
  runs, OK on the same album or playlist says the progress without loading, and on another
  says "Another download is running." without loading. Everything else is as for the list
  item ([Download album](#lists-from-the-server)).
- An item with no actions plays the edge sound and says "No options" (rule 9: every
  active key gives feedback). So does Options in a player with nothing loaded.
- **Play now** is an action that finishes a task: it starts playback, goes back by
  itself and says what plays ("Playing Weather."), like Play next and Add to queue.
- An action that finishes a task (Play next, Add to queue) goes back by itself and
  says what it did. An action that only reports something stays.
- `MenuItem` gets an `options` builder, next to `submenu` and `action`. Lists build it
  for their items; a player builds it for the current track.
- At Beginner verbosity the hint names it: "Press OK to play, hold OK for options."
- The actions are Play now, Play next and Add to queue (see [The queue](#the-queue)),
  plus the ones above.

## Two players

- **The music player** plays the queue. **The book player** plays one book, with its
  position kept per book and `last_book`. Podcasts will use the book player.
- They share one `MediaPlayer`. Starting one pauses the other and keeps its place (in
  memory), so going back to it resumes where it stopped.
- **After a restart** a book resumes from `last_book` and the music player from the saved queue
  (`queue.tsv`, local and server tracks); `last_player` in `state.tsv` says which was last.
  The parked player is kept across a restart the same way: the book by `last_book`, the music
  queue in `queue.tsv`. An older version's `last_stream.*` is still read when there is no saved
  queue (see [architecture.md](architecture.md#9-persistence)).
- Each is its own program: `Screen::Kind::MusicPlayer` and `Screen::Kind::BookPlayer`,
  each with its own Player menu.
- **Home:** "Now playing, <album or book>" for the player used last, and "Continue,
  <the other>" below it when the other player has something: loaded, parked, or saved for
  a restart. Each is left out when it has nothing.
- The book player has no queue, shuffle or repeat. Both players use short
  Back / Forward for chapter/track movement and holds for seeking.

### A later server place in a book

A book on a server that syncs progress starts playing its local place immediately and asks
for the server's place silently. This also applies to a whole download while offline: a
failed progress request says nothing and does not delay playback.

When the server's saved place is newer and more than 30 seconds away, a book with no local
place moves there and says the chapter title. With a local place it keeps playing and says
once: "The server has a later place, chapter 5. It is in Options." Options in the book
player then opens "Options, <book>" with a first action, "Go to the server's place, chapter
5, 12 minutes" (the chapter number and time are the server's). OK moves to that exact
place, returns to the player, and says the chapter; a paused book stays paused. The offer
is gone after use. Choosing a chapter explicitly keeps that chapter, including the first;
a later server place is offered in Options instead of moving automatically.
Older places, differences of 30 seconds or less, missing progress and
errors say nothing. Leaving the player, changing books, sleep or shutdown drops the pending
answer and offer. A later opening asks again.

### Where am I in a player

- Holding Play/Pause in a player speaks the player first, then the album and artist of the
  current track, then what it says now: "Music player. Dune, by Weather. Track 5 of 30,
  Storm. 3 minutes left in track. Playing." The book player says "Book player. <title>, by
  <author>. Chapter ..."
- In the music player the album and artist come from the current entry of the queue, not
  from the queue's title: in a queue of several albums the title names only the first. The
  queue's title is not spoken.
- The artist is left out when it is empty ("Dune. Track 5 ..."); a track with no album gives
  its artist alone ("By Weather."), and neither gives nothing. The progress of a download
  follows. A book from the server gives its author; a book on the device has none ("Book
  player. <title>.").
- In a menu it stays as it is: the path, the selected item, and what plays.
- The player's name comes first because in a menu the answer starts with the path, and the
  player is the end of a path: the same start in both places tells at once whether the
  answer is about a menu or a player. Still to be confirmed on a device (fixes.md open question 1).

## The queue

Only the music player has a queue ([queue.md](queue.md) has the full design). The rules
that touch navigation:

- **OK on a track** in any list of tracks replaces the queue with that list and plays from
  the track. Play album / Play playlist / Play all replace it with the whole list.
  **Options → Play now / Play next / Add to queue** (on a track, album, playlist or
  artist) replace the queue, put the item after the current entry, or at the end.
- A remote item is loaded first ("Loading.", then "Added Weather, 12 tracks. 30 in
  queue."). Esc while it loads drops the late answer. With nothing loaded, Play next and
  Add to queue make a queue that is paused at its first entry: "Added Weather, 12 tracks.
  Press play."
- **Shuffle** (off · tracks · albums) and **Repeat** (off · all · album · track) belong to the
  queue and start off with each new queue. They are "Setting with list" items in the
  music Player menu, not settings in `config.yaml`. Repeat track repeats the current
  track when it ends; short Back / Forward still move to other tracks. With Repeat
  all, Next on the last track goes to the first and Previous on the first to the last.
- **Repeat album** repeats the album of the current track. The
  album of an entry is the entries of the queue with the same `albumId`, when both have
  one, else the same album name; an entry with no album name and no `albumId` is an album
  of its own, so Repeat album repeats it like Repeat track. When an entry of the album
  ends, the next entry of the same album in play order plays, skipping the entries of other
  albums; after the album's last entry, its first. With Shuffle off or Shuffle albums the
  album is one run and nothing is skipped; with Shuffle tracks its entries are apart in the
  play order and play in that shuffled order, the others skipped. An entry of the album that
  cannot play is skipped within the album, never for a track of another one. Short Back / Forward
  and Next / Previous still move through the play order, other albums included, and the album
  of the track moved to is then the one that repeats. The spoken value is "album"; the saved
  queue keeps it, and an older build reads it as off.
- **Add to queue follows Shuffle; Play next does not.** With
  Shuffle tracks the entries added with Add to queue go to random places among the entries
  that have not played yet, after the current one. With Shuffle albums they are cut into
  runs of one album, and each run goes, whole and in its order, to a random place between
  two runs of what has not played yet. Play next puts its entries right after the current
  entry, in their own order, with any Shuffle. Turning Shuffle off gives the original order,
  with Add to queue's entries at the end and Play next's after the current entry. With
  Shuffle off nothing changes. What is spoken does not change ("Added Weather, 12 tracks. 30
  in queue."). Entries put there by Play next stay next: Add to queue mixes its entries into what
  comes after them.
- At the end of the queue: "End of queue", paused at the first entry, unless Repeat is on.
- **An entry that cannot play is skipped**: "Storm, source unavailable." (a run: "3
  tracks, source unavailable."); one whose server is no longer set up says "Cannot play
  Storm. Server not set up." A server that fails is marked down and checked in the
  background; with nothing ahead that can play the device says "Server not reachable.
  Press play to try again."
- The music Player menu:

```
Player menu (music player)
├── Queue → <entries>      opens on the current entry, spoken "current"
├── Shuffle, off           list: off · tracks · albums
├── Repeat, off            list: off · all · album · track
├── Go to first track      the first entry in the play order
├── Sleep timer → …
└── Add to playlist → <eligible playlists>
```

  The book Player menu has no Queue, Shuffle or Repeat.
- The **Queue list** opens on the current entry ("Storm, current, 5 of 30"). OK plays an
  entry; Options on an entry: Play next · Move · Remove. After the entries: Save as
  playlist, Previous queue (only when there is one: the edited queue that the last
  replacement kept, and OK on it brings it back), and Clear queue, which is behind a list
  that opens on Cancel (a dangerous action is never first). Move is a list of positions that
  opens on the entry's own, like a setting. Remove of the current entry stops it and
  makes the next entry current, paused. Clear queue leaves nothing loaded in the music player:
  the device goes to Home, which no longer offers it ("Queue cleared."); an edited queue is
  kept as Previous queue, which shows in the Queue list of the next music queue.
- **Save as playlist** saves the remote tracks of the server set up now as a server
  playlist, named from the wall time ("Queue, 27 October 14:05"), and says how many tracks
  were left out. With none to save it says "Only tracks from the server can be saved."
- The queue is saved on sleep and exit and is back after a restart; nothing plays until
  the user chooses it.

**As built:** the music player's Player menu is Queue · Shuffle · Repeat · Go to first track ·
Sleep timer · Add to playlist (streaming 5.4), Options in the music player is Go to album · Album info · Star / Unstar for supported server-linked tracks, including local playback (streaming.md
2a.1), and the Queue list works as above. Where the build differs from the text above:

- Play now says "Playing Weather." once the track opens. "End of queue. Press play to start
  again." is the whole message.
- Move says "Moved to 3 of 30." and "Not moved." for the entry's own place. Play next on the current
  entry plays the error sound, says "Storm is the current entry." and leaves the Options open; on another
  entry it says "Storm will play next." Removing the last entry says "Removed
  Storm. The queue is empty." and unloads like Clear queue. While a track you chose is still opening,
  Play next, Move and Remove say "Loading. Try again when it has started."
- Save as playlist says "Saving." first, then "Saved Queue, 27 October 14:05." and how many tracks were not
  saved. At most 150 tracks go in one playlist (the request is a GET); the rest are counted as not saved.
- A track you choose from a list the server just gave (OK in a menu) is tried even while the server is
  marked down; the entries that follow it are skipped until a ping answers, except the ones that are
  downloaded: they play from their files.
- The queue file keeps up to 5000 entries around the current one. The Previous queue is not kept
  across a restart. A restored queue does not cache tracks automatically until another list plays.

## Lists

- **A list that shows a current state opens on that state**: a setting on its
  current value, Sleep timer on the timer that is set, Chapters on the playing
  chapter. Use `MenuItem::selected`.
- **Offer only what can work now.** "End of chapter" is not in the Sleep timer
  list when nothing is loaded.
- **A menu is rebuilt when you come back to it**, so it shows what changed
  meanwhile ("Now playing" appears on Home). The same item stays selected.
  Lists from the server are the exception (see below).
- **A dangerous action is never the first item of its list.** It sits in a
  list that opens on something harmless: `Cancel` in Factory reset, `Sleep`
  in Power. Pressing OK twice by accident then does no harm.
- **An action that finishes a task goes back by itself** (choosing a value, a
  sleep timer, Play next or Add to queue). An action that only reports something stays where it is.
- **An empty list says why and what to do** ("Library is empty", "No music",
  "No server set up").

## Lists from the server

The Streaming entry on Home is named after the server's `name` in `config.yaml`
("Server" when it is not set). Its lists come over the network, so they follow
these rules too ([streaming.md](streaming.md) has the plan).

**Several servers** ([audiobookshelf.md](audiobookshelf.md), phase 1). Home has one entry per server in `servers:`, in config order, between Music and
Podcasts. Its name is the server's `name`; without one it is "Server" when only one server is
set up, else "Server 1", "Server 2", ... by its place in the list. Each entry opens the menu
of its own server, and its requests, Server status and "Loading" state belong to that
server alone. All the rules below hold for every server.

- **A list that loads from the network opens at once with one item, "Loading".**
  OK on it says "Still loading." Keys work meanwhile: Esc leaves, and the late
  answer is dropped, so it is never spoken.
- **The answer fills the list where it is.** If the user is still on "Loading"
  and the list is on top, they are moved to the first item that came and it is
  spoken. If they have opened something else, gone to sleep, or moved to an item
  that had already loaded, the list is filled silently.
- **An error is a list too**: "Server not reachable" (or "Wrong user name or
  password", "Server error"), then "Try again". OK on the error says what to do.
  Try again goes back to "Loading" and asks again. Items that had loaded stay.
- **Without a server the list says why and what to do.** With no server in
  `config.yaml` (`servers:`, or the old `server:`) the Streaming menu has one item, "No server set up",
  and OK says how to add one. A server this player cannot use (another type, or a
  build without network support) gives "Server not supported". An empty answer says so too ("No albums",
  "No tracks", "No playlists").
- **Long lists expose the loaded boundary.** Albums A to Z uses pages of
  500 albums. Recently added is one page of 500 and Random albums one page of 50.
  Held Back / Forward reaches the first / last item of what has loaded. End
  jumps never fetch more pages; keep Load more accessible when more pages remain,
  with the continuation announcement specified above. Loading/error rows retain
  their own announcements. Choose Load more with short OK to request the next
  page; returning to loaded items while it runs leaves them usable.
- **A list from the server is not rebuilt when you come back to it.** It keeps
  what has loaded and the selected item, so going back does not wait on the
  network again. This is the one exception to "a menu is rebuilt".
- **Server status reports and stays.** It says "Checking the server.", then
  "Connected to Navidrome 0.64.2." (the server's name and full version) or the
  error. Moving to another screen drops the answer.
- **Download album / Download playlist** is the item right after "Play album" /
  "Play playlist". It is offered only where it can work: a cache folder, a server
  and a downloader that is set up. OK starts the download and says "Downloading, 0
  of 9 tracks." (complete copies at least as good as Download quality count as done
  and are not fetched again).
  OK on it while it runs says the same with the count so far, and so does "where
  am I?", in a menu and in the player. One list downloads at a time: starting
  another says "Another download is running." Not enough room on the disk for the
  tracks that are missing starts nothing and says "Not enough space." A track
  that fails is said once ("Cannot download Rain. Server not reachable."), the
  download stops, and OK on Download again goes on from there. Leaving the menu,
  Esc and playing something do not stop it. When it ends the device says
  "Download finished, Weather." unless something is playing, which gets only an
  earcon (and a sleeping device says nothing).
- **Download from Options** starts the same download from
  the Options of an album or playlist ([Options](#options)).
- **Download quality** (Settings) opens a list of
  original file, 320, 192 and 128 kbps, in that order, with original file the default.
  OK selects and saves `downloads.bitrate` (0 for original) and goes back, as for
  Streaming quality. It is independent of Streaming quality and applies to the next
  Download album / playlist. A complete copy at least as good is kept and pinned:
  original is best, otherwise higher kbps is better. A lower-quality copy is fetched
  again and replaced only once the new file is complete; the old copy plays until then.
  Originals use the server's download endpoint; other qualities request an MP3 stream
  capped at that bitrate. Original-only Books providers keep the original regardless
  of this setting. A partial original can resume; a partial transcode starts
  over on retry. Space for a transcode is checked using duration times bitrate when
  duration is known, otherwise the original's size, otherwise the existing unknown-size
  rule. Insufficient space still says "Not enough space." and starts nothing.
- **Downloaded** (in the Streaming menu after Random albums, only where a cache
  folder is set up) lists the albums and playlists that you downloaded (at least
  one track kept by Download album / playlist, not just cached by playing), by name
  (an album says its artist, a playlist says "playlist"). It is read from the
  disk, not the server, so it opens at once, and it is rebuilt when you come back
  to it, like a local menu. An album or playlist opens with "Play album" / "Play
  playlist" first, then "Delete download", then the tracks that are whole; they
  play from their files like any list, and the place is kept as for the server
  list. "Delete download" is a dangerous action, so it opens a list of "Cancel"
  (selected) and "Yes, delete". Yes removes the files, says "Deleted Weather." and
  goes back to Downloaded, which no longer has it. The list that is loaded in the
  player is not deleted: the device says "Cannot delete Weather while it is loaded
  in the player. Play something else first." and stays on the list. Deleting the
  list that is downloading cancels the download. With nothing downloaded the list
  has one item, "No downloads"; OK says "Nothing is downloaded. Choose Download
  album in an album."
- **Cache size** (Settings) is a `choice()`: off, 1, 4 or 16 gigabytes for the tracks
  cached while streaming. A streamed track is cached silently after 10 s of
  listening: it says nothing and plays no earcon, and a download you asked for goes
  first. A lower size takes effect on the next exit or cached track. Off caches nothing
  new but keeps what is cached. Downloads you asked for never count toward the size
  and are never removed by it.
- **Cache quality:** the automatic cache fetches
  the quality that was streamed, with a second fetch at that quality after 10 s of
  listening. It does not fetch the original for a lower-quality stream. Each cached
  file records its bitrate in `manifest.tsv` (0 for original; older rows mean original).
- **Replace cached copies** (Settings) opens off / on,
  default off, and saves `cache.upgrade` on OK before going back. Off plays a complete
  cached copy whatever its quality. On, when that server is not marked down, a copy
  below Streaming quality is streamed at the selected quality instead; the automatic
  cache queues a replacement after 10 s of listening, subject to Cache size and free
  space. A pinned copy stays pinned after replacement; the old file remains until
  the new file and its manifest are saved.
  If opening the stream fails, the cached copy plays immediately at the same place
  and the server is marked down. An asynchronous failure or open timeout follows
  the same fallback, preserving Play/Pause intent; cancelling the open does not
  start it. A server already marked down uses the copy at once.
- **Offline.** When a list from the server fails with an error ("Server not
  reachable", "Server error" and so on), the list is "the error", "Try again" and, if
  anything is downloaded (not just cached by playing), "Downloaded" (the same list as in the Streaming menu, which
  needs no server). Music → Playlists is not a list from the server (see below), but a
  playlist in it that is not whole on the disk loads its tracks and follows these rules. Without downloads there is no such item. Try again replaces the
  error list with "Loading" as before. A cached album played offline never says
  "Loading." and does not wait on the server when Replace cached copies is off or
  the server is already marked down. Otherwise Replace cached copies can attempt
  the better stream first, with the fallback described above.
- **The last album or playlist resumes after a restart.** When a stream played
  last, the device starts on Home (nothing plays), with "Now playing, Weather"
  first. OK on it, or Play, says "Loading.", loads the list from the server and
  opens the track where it was, then the player. If the album was downloaded whole
  (every track pinned and on the disk, the saved track among them) it plays from
  the files at once: no "Loading.", no question to the server. If it is only partly
  on the disk, the server is asked first so no track is lost; with the server down
  the tracks that are on the disk play (from the saved track, or the first if that
  one is not there) and no error is spoken. A new screen, sleep or Power off
  while it loads drops it; an error with nothing of the list on the disk is spoken
  ("Server not reachable.") and Home stays. OK on "Now playing" again while the track opens goes to the player; if
  the track then does not open, or sleep drops it, Home comes back instead of an
  empty player.
  A local book played after the stream is resumed instead (the book player was used last), and the
  stream is "Continue" on Home.
- **The player does not change.** A streamed track has the same keys, Player
  menu and "where am I?" as a local music track. Because opening a stream can
  take a few seconds, the device says "Loading." before it opens a track the
  user chose, and the keys keep working meanwhile: a track chosen in a list
  keeps the user on that list and the player opens once the track plays.
  Play/Pause while it opens says "Paused." (it will not start) or "Loading." (it
  will). Leaving that list (Esc), choosing another track or going to sleep
  drops the open, so a late result never plays or speaks. A skip in the player
  keeps opening when the user leaves the player. After 15 s without an answer
  it gives up. A track that does not open says "Cannot play Storm. Server not
  reachable." and no other track is tried. What played before stays current and
  paused. If that was a streamed track too, it is opened again only when the
  user presses Play.
- **A stream that runs dry says so, and the track goes on.** After 0.5 s without
  audio the device plays the buffering earcon, and after 3 s it says "Waiting for
  network." once. The player keeps retrying at the same place, so a short dropout
  does not skip the track. If it cannot go on for 30 s, the device says "Server not
  reachable. Press play to try again." once what came has been heard, and Play
  opens the track again where it stopped. The next track's stream opens in the
  background 10 s before the end, so tracks change without a wait.

## Playlist editing and planned sync changes (streaming phase 5)

Playlist editing is built in step 5.4; Keep offline and queue sync below are planned.
Music → Playlists combines local, downloaded and server playlists. Create playlist selects "On the
device" or a configured playlist-capable server and uses a generated timestamp name;
renaming waits for [input-methods.md](input-methods.md). Each playlist opens with Play
playlist, tracks and Delete playlist. Keep offline / Stop keeping offline for server
playlists is planned for step 5.5. Options on a track offers Remove from playlist; for a server playlist
it first loads the current ordered tracks for the user to select the occurrence to remove.
A fetch error uses the usual server error feedback. Delete confirms with
Cancel first. The music Player menu offers Add to playlist and opens that list on the
last eligible playlist used. Server playlists accept only tracks known to that server;
local-only and other-server tracks are skipped. Local playlists accept local tracks.

Success says "Added to playlist.", "Removed from playlist." or "Playlist deleted.";
cancelling says "Cancelled." Server writes say "Saving." and use existing server error
feedback on failure. An unavailable action says "Not available." and a failed local
write says "Could not save playlist." Offline star and server-playlist edits are not
queued in this phase. Keep offline owns a pin for each track; deleting the playlist or
stopping offline keeping releases only its own pins. Book progress follows the existing
Audiobookshelf interaction; it is independent of playlist editing. Queue sync compares
server and local timestamps, keeps the newer version and does not interrupt playback;
[streaming.md](streaming.md#phase-5-design) specifies the remaining rules.

## Music and Library

Music and Library list one library: the files on the device, the downloads and the albums of the
server. Streaming is its own Home entry (see below). [streaming.md](streaming.md) phase 4 has the
plan and what was built; the tree in [architecture.md](architecture.md#current-menu-tree) is the
same menu with the code behind it.

```
Music
├── Play all
├── Artists → <artist> → <albums>
├── Albums A to Z → <albums>
├── Playlists → <playlist>
├── Genres → <genre> → <albums>
├── Recently added → <albums>
├── Recently played → <albums>
└── Refresh library
```

- **One list for both sources.** An album that is on the device and on the server
  is one entry. It plays from the local files first, then from a download, then
  from the server. Browsing uses the saved index, so it opens at once, works
  offline and never waits on the network. Start-up shows the saved index at once and rescans the files in the background.
- **While the first scan runs** (a first start, no saved index) an empty Library or Music
  has the item "Scanning" in place of "Library is empty" / "No music"; OK says "Looking for
  books. Try again in a moment." or "Looking for music. Try again in a moment.", and so does Play all when it has no
  files yet. Music keeps "Refresh library" under it. Lists are built when opened, so what the
  scan finds shows the next time you open them.
- **Music lists are local menus.** They are not "lists from the server": no
  "Loading" and no "Try again". A scan or sync that finishes meanwhile replaces
  the index silently, and the menu shows it the next time you come back to it
  (the selected item stays). Opening a server album that is not downloaded loads
  its tracks, so that list follows "Lists from the server": "Loading", the error with "Try again", "Play album" first.
  Offline that album gives the error item and a device album still plays.
- **Artists** lists each artist once, local and server together, sorted by name without a
  leading "The" (the server's `ignoredArticles` is not read yet); names in Cyrillic and other
  scripts come after the Latin ones, and albums with no artist are under "Unknown artist", last.
  An artist's list holds the albums of both sources. **Albums A to Z** lists every album,
  server-only ones too.
- **Playlists**, after Albums A to Z, offers Create playlist and lists local M3U files and the
  server's playlists by name, with the track count as the value ("Road Trip, 12 tracks"),
  sorted by name (Back / Forward still move one, ten or to an end). It is a local menu: it is read from the
  index, so it opens at once and works offline. The sync stores the playlists in the index
  (id, name, song count and the server's `changed` time, from `getPlaylists`) at each sync and
  Refresh library, and a playlist made with Save as playlist is added at once. A playlist
  deleted on the server leaves the index at the next sync that gets `getPlaylists` (a sync
  that fails keeps the old list). **Downloaded playlists** are listed too, from the cache
  manifests, so one is there when the index has never synced, and it stays in the list after
  it is deleted on the server until its download is deleted; a playlist in both is one entry.
  **Opening a playlist** that is whole on the disk plays from the files with no "Loading.";
  otherwise its tracks load under the rules of "Lists from the server" ("Loading", the error,
  "Try again", "Downloaded" when offline). It opens with "Play playlist", then "Download
  playlist" where it can work, then the tracks and Delete playlist. An edited server playlist's
  old download remains available in Downloaded, while Music fetches the current track list;
  see [Playlist editing](#playlist-editing-and-planned-sync-changes-streaming-phase-5).
  **Streaming no longer has Playlists**; Streaming → Downloaded still lists downloaded
  playlists. Playlists is also available when there is no music yet, so a playlist can be created.
  Options on a whole, current download use its files; an edited snapshot needs the server.
- **Play all** plays what is on the device (local and downloaded), never the
  whole server. With nothing on the device it says "No music".
- **Genres** lists each genre once, local tags and the server's album genre together; names
  that differ only in case or spacing are one genre ("Rock", "rock"). A genre's list holds
  the albums of both sources; an album on both under two genres is in both. Albums with no genre are not listed, and with none at all
  the list is one item, "No genres".
- **Recently added** is newest first. **Recently played** is the albums you
  played, the last one first. Each says why it is empty ("No new albums", "Nothing
  played yet") and lists at most 50 albums. An album is added when it first entered the
  library: the first-seen time of a device or downloaded album, `created` for a server
  album, and for an album on both the earliest of them. An album plays when it starts from
  Music (an album, a track or Play album; not Play all, Streaming or Downloaded); the time
  is that of the last play and, for an album on both, the latest of its items. A time
  before 2020-01-01 is a clock that was never set and counts as none. Play times are saved
  on exit and sleep only.
- **Album info** is in the Options of an album (Music lists), after Play now, Play next and Add to queue:
  OK says where the album plays from ("On the device", "Downloaded" or "On the server"),
  then "Also on the server" for an entry that is on the device or downloaded and
  also on the server, the artist, and the number of tracks. It only reports, so the Options
  screen stays. A loose track has none. Nothing else speaks where an album plays from. The
  music player's Options has its own Album info (title, tracks, status), and **Go to album**,
  which opens the album of the playing track on that track (a loose track says "No album for this track.").
- **Previous queue:** when a queue edited by hand is replaced, it is kept, and it comes
  after Save as playlist in the Queue list of the music Player menu. OK restores it (entries, order, current
  entry with its position, shuffle and repeat) and plays; the queue it replaces is kept in turn if it was edited.
- **Refresh library** is an action that reports and stays. It says "Updating the
  library." at once, then the result ("Library updated, 12 albums.") or the error
  ("Server not reachable.", "Wrong user name or password.", "Server error."). Keys
  work meanwhile. Moving to another screen or sleeping drops the answer, so a late
  result is never spoken. A second press while it runs says "Still updating.". With
  no server set up it only rescans the files. A refresh that changed nothing writes
  nothing.
  **With several servers** it syncs every server, one after the other, and the count is of
  the merged library. A server that fails adds its name and the error, in lower case:
  "Library updated, 12 albums. Music: server not reachable." When every server fails it says
  only the errors. The books of the servers are counted after the albums ("Library updated,
  12 albums, 2 books."); with none of either it says "Library updated.".
- **Server books are in Library**, after the local books, with the value "on the
  server" (spoken after the title). They are never in Music. They open as books:
  a Subsonic server book has its tracks as chapters, an Audiobookshelf book the chapters the server lists
  (a chapter can span files), and the place is kept and resumed like a local book. A book that is
  both on the device and on the server is listed twice, once for each; they are not merged.
  While their tracks load, the "Loading" rules apply. After a restart "Now playing" on Home resumes the
  book when the book player was used last (otherwise it is "Continue"); a downloaded book plays from
  its files at once, otherwise with the server down "Server not reachable." is said and Home stays.
- **Streaming stays a separate Home entry.** It keeps its own lists as above, except
  Playlists, which is in Music.

### Books server menu

An Audiobookshelf server ([audiobookshelf.md](audiobookshelf.md)) is a Books server. Its Home
entry has this menu instead of the Streaming one; a Subsonic server keeps the Streaming menu.
The menu, the lists, the book page, downloading and progress sync are implemented.
Real-server progress and device listening checks remain pending.

```
<server name>
├── Continue listening → <books>      from this server’s selected book library
├── Recently added     → <books>      newest first, one page of 500
├── Books A to Z       → <books>      pages of 500
├── Authors            → <author> → <books>
├── Downloaded         the downloaded books (only where a cache folder is set up)
└── Server status      "Connected to Audiobookshelf 2.37.2." or the error

<book> → Play book · Download book · Delete download · <chapters>
```

- A book in these lists says its title and author ("Neuromancer, William Gibson").
- **Continue listening** is first. It follows the network-list rules (including "Nothing in progress"
  when empty). OK on a book opens the player and follows its server place, even when there is a
  saved local place or the server place is older or nearby. Playback starts at the local place
  while progress is fetched; a failed progress request stays silent and leaves playback there.
- **Play book** (value: the number of chapters) opens the book player at the book's place (or the start), like a book in
  Library. OK on a chapter plays the book from that chapter. The chapters are the server's,
  and a chapter can span files.
- **Download book** behaves like Download album ("Downloading, 0 of 24 files."), counting
  files, not chapters; a file that fails is named by its title, else "file 3". It is not
  offered once the book is whole on the device, and **Delete download** (behind Cancel, as for
  an album) is offered once some of it is.
- **Downloaded** lists the books, with the author as the value. A book that is whole opens
  the same page from its files and the chapters the download kept, with no question to the
  server, so it plays offline with the same place as in Library; a book cut short opens as on
  the server ("Loading", "Server not reachable" and Try again), where Download book goes on.
  A downloaded book is never in Music.
- Library lists the books of every Books server after the local books, with the value "on the
  server". Books of two servers are not merged.
- All the rules of [Lists from the server](#lists-from-the-server) hold: Loading, errors, Try
  again, and a list that is not rebuilt when you come back to it.
- With no usable server the lists keep "No server set up" and "Server not supported". The help
  for an unsupported type names both Subsonic and Audiobookshelf.

## Checklist for a new menu, item or program

- [ ] It is one of the item types above. A setting uses `choice()`.
- [ ] It adds no special cases for keys to `App::handleMenu`.
- [ ] A list that comes from the server opens with `App::openRemoteList()`, so
      it gets "Loading", the error with "Try again", and the empty text.
- [ ] A new program is a `Screen::Kind` with its own key handler. Short Esc
      leaves it, held Esc goes Home, and held OK opens Options.
- [ ] Anything destructive is behind a list whose first item is harmless.
- [ ] Back / Forward move one item, deep moves ten, and hold reaches an end;
      jumps clamp, ordinary steps follow Menu wrap, and settings change in their lists.
- [ ] Options on its items does something or says "No options".
- [ ] The Help texts (`kButtonHelp`, "Getting started") still match.
- [ ] A test in `tests/test_app.cpp` drives it with the real keys.
