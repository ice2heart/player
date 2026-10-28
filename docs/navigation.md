# Navigation guidelines

> Status: the parts of this contract that [queue.md](queue.md) phases 1 and 2 cover are **built**: the
> volume and Options buttons, the step unit, Left / Right in menus, settings changed only in
> their list, the two players, the queue and the Queue list, Shuffle and Repeat, the actions
> behind Options, Save as playlist and the saved queue (see [The queue](#the-queue)). Star /
> Unstar in the player's Options is **not built** ([streaming.md](streaming.md) 2a.1). Where the
> build differs from the text, a note says so; [architecture.md](architecture.md) describes the
> code. Last updated: 2026-10-28.

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

Home is always at the bottom of the stack. OK pushes a screen, and Home pops
one, back to wherever you came from. If you open a book from Library, Home
takes you back to Library. There is only one screen per player: opening a player again
returns to the one already on the stack. The Power menu works the same way, and
Sleep in it closes the menu first, so the device wakes where you were.

## The rules

1. **Up / Down move, and only move.** In a menu they go to the previous or next
   item, and holding them jumps to the first or last item. In a player they
   choose the step unit that Left / Right move by (see
   [The step unit](#the-step-unit-up--down-in-a-player)). They never change the volume.
2. **OK goes in.** It opens the selected item's submenu or program, or runs its action.
3. **Home goes out** to the previous screen. Holding OK does the same.
   Leaving a screen changes nothing. Home on the Home menu plays the edge
   sound and says "Home".
4. **Holding Home goes to the Home menu**, from anywhere.
5. **Left / Right jump in a menu, and nothing else.** They go to the previous
   or next letter in a list sorted by name, else 10 items back or forward (see
   [Left and Right in menus](#left-and-right-in-menus)). They never change a
   value, and never mean "back" or "open". At the first or last item you hear
   the edge sound.
6. **A value is changed in its list, and works at once.** A setting is changed
   by opening it with OK: its list opens on the current value, and choosing a
   value with OK saves it and goes back. You hear the new speech rate straight
   away. The file is written at once; the volume, changed with its own keys, is
   kept in memory and written on exit and sleep. Streaming quality is the one exception: a stream that plays
   would have to open again, so the new quality starts with the next track.
7. **In a program, Left / Right are that program's own keys.** In a player
   they skip back and forward by the step unit, and holding them or pressing
   them deeper moves to the previous or next chapter or track.
8. **Play/Pause, Volume up / down, Power and Talk work the same on every
   screen.** Holding Play/Pause ("where am I?") speaks the screen path and the
   selected item, and the progress of a download that runs ("Downloading, 3 of 9
   tracks."). The volume keys change the content volume in menus and players
   alike (see [Volume](#volume)).
9. **Every press of a key is heard.** The press interrupts speech and plays an
   earcon, speaks, or both. A key with no job on the screen plays the edge
   sound. There is no silent key. A held key that repeats (volume) plays the edge
   sound once at the limit, not on each repeat.
10. **Options is about the selected item.** It opens the actions for the selected
    item in a menu and for the playing track in a player, and is never used for
    anything else (see [Options](#options)). An item with none says "No options".

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

Left and Right may be **two-stage** keys: a light press, and a deeper press
through a second contact. The deep stage is a separate input (`BackDeep`,
`ForwardDeep`). The gesture detector turns it into a `Deep` gesture on Left /
Right as soon as it closes, and the light press that came with it is then
ignored. On one-stage hardware, holding the key does the same as a deep press.

| | Light press | Hold | Deep press |
|---|---|---|---|
| Player | skip back / forward, or the unit's step | previous / next chapter or track | previous / next chapter or track, without waiting |
| Menu | jump (letter or 10) | the same jump, once | the same jump, once |

A hold or a deep press in a menu does what a light press does, once; it does not
repeat. A hold or deep press in a player moves to the previous or next track or chapter
whatever the step unit.

## Buttons and keys

| Button | What it is for |
|--------|----------------|
| Up / Down | move in menus; in a player, choose what Left / Right move by |
| Left / Right (two-stage) | in menus: jump by letter or by 10; in a player: skip, and a deep press or hold goes to the previous / next track or chapter |
| OK | go in; in a player, the Player menu |
| Home | go out; hold for the Home menu |
| Play/Pause | play / pause; hold for "where am I?" |
| **Volume up / down** | content volume, on every screen |
| **Options** | actions for the selected item, or in a player for the playing track |
| Talk | push to talk (STT, later) |
| Power | sleep / wake; hold for the Power menu |
| Hold switch | locks every key except Power |

Text (playlist names, search, Wi-Fi passwords) comes from STT or a keyboard later.
Until then names are made by the device.

## Key table

| Key | Menu | Player |
|-----|------|--------|
| Up / Down | previous / next item | previous / next step unit |
| Up / Down held | first / last item | once, like a light press (no repeat) |
| Left / Right | previous / next letter, or 10 items back / forward | move by the unit (skip, track or chapter, album or 10 minutes) |
| Left / Right held or deep | the same, once | previous / next chapter / track |
| OK | **in**: open / run | Player menu |
| OK held | **out** | **out** |
| Home | **out** | **out** |
| Home held | Home menu | Home menu |
| Play/Pause | play / pause | play / pause |
| Play/Pause held | where am I? | where am I? |
| Volume up / down | content volume | content volume |
| Volume up / down held | repeats the step until let go | repeats the step until let go |
| Options | actions of the selected item ("No options" if none) | actions of the playing track |
| Power / Power held | sleep / Power menu | sleep / Power menu |

Options pressed inside an Options list says "No options": it does not open a second level.

### Volume

- **Volume up / down change the content volume** on every screen, menus included.
  Speech keeps its own level. At 0 and 100 % the edge sound plays once per press, not on each repeat.
- **Holding a volume key repeats the step** (about every 150 ms, after a short 400 ms
  delay so a slow tap steps once) until it is let go.
  `GestureDetector` gets a repeat event for these two keys; the other keys keep
  short / long / deep.
- The volume is kept in memory and written to `config.yaml` only on exit and sleep.

### The step unit (Up / Down in a player)

With volume on its own keys, Up / Down in a player choose **what Left / Right move
by**, like the navigation levels of a Victor Reader Stream. As in a menu, Down moves
to the next unit and Up to the previous one, and each speaks the unit ("Track"). The
edge sound plays at the first and last unit; holding Up or Down does not jump.

| Player | Units (Up / Down) | Light press of Left / Right |
|--------|-------------------|-----------------------------|
| Music | Skip · Track · Album | the skip of Settings (back / forward), the previous / next track, or the first track of the previous / next album in the queue |
| Book | Skip · Chapter · 10 minutes | the skip of Settings, the previous / next chapter, or 10 minutes |

- The first unit is **Skip**, and it is the unit after a first start.
- **A deep press or hold of Left / Right always goes to the previous / next track or
  chapter**, whatever the unit.
- The unit is kept per player and saved in `state.tsv` (`unit.music`, `unit.book`),
  written on sleep and exit like the rest of the state, so it is the same after a wake
  or a restart.
- The Album unit moves to the first track of the previous / next album of the queue: a run of
  entries with the same album name in the play order. It speaks the album ("Dune"). A queue
  with no other album in that direction plays the edge sound and says "First album." or
  "Last album."

### Left and Right in menus

Settings no longer change in place with Left / Right (rule 6). Left / Right in menus
**jump in long lists**:

- In a list sorted by name (Albums A to Z, Artists, the albums of an artist and Genres
  on the device, Library; on the server Albums A to Z and Artists), Left / Right go to
  the first item of the previous / next letter and speak it: "D. Dune, 41 of 500". A
  letter is the first letter of the sort name (so "The" is skipped as in Artists);
  names in other scripts and "Unknown artist" form their own groups after the Latin
  ones. Left goes to the first item of the previous letter, also from the middle of a
  letter. The spoken letter is the first letter of the sort name; an item in a
  group of its own ("Unknown artist") is spoken by name without a letter.
- Names that start with a digit (or no letter) are one group before A, spoken "Number".
  Only artist names skip "The"; album titles are sorted and grouped as written. In
  Library the server's audiobooks follow the local books, so the letters start again there.
- In other lists they go 10 items back or forward, and stop at the first or last item.
- At the first or last item (Right on the last, Left on the first) the edge sound plays;
  with Menu wrap around (the default) it wraps instead, as Up / Down do: to the first item, or from the start to the first item of
  the last letter in a list by letter, and to the last item in one that jumps by 10.
- A list says by itself whether it is sorted by name (a flag on the list); a new list
  that is not marked jumps by 10.
- This does not repeat what OK and Home do, so each action keeps one key.
- **To be confirmed on a real device** with blind users before it is final (queue.md
  open question 1).

### Options

**As built (step 2.4):** Options on a menu item opens "Options, <item>" with Play now, Play next
and Add to queue, on local tracks, local albums and Play all, and on remote tracks, albums,
playlists and artists. Every other item, and Options in the book player, plays the edge sound and
says "No options". An album's Options also has Album info (step 2.5). A remote
album, playlist or artist is loaded first ("Loading."); Play now says "Playing Weather." and the
other two "Added Weather, 12 tracks. 30 in queue."; with no music queue loaded they say "Press play."
instead (and while a book plays they leave it playing and keep the queue for Continue).

- **Options on a menu item** opens "Options, Weather": a short list of what can be done
  with that item. **Options in a player** gives the actions for the playing track.
- **OK and Options in a player stay different.** OK opens the Player menu, which is
  about the player (Queue, Shuffle, Repeat, Sleep timer). Options is about the
  playing track: Go to album, Album info, and Star / Unstar for a remote track. Album
  info moves to Options from the Player menu and from the end of an album's list (an
  album in a list has it in its Options). Star / Unstar is not built yet: streaming.md
  [phase 2a](streaming.md#phase-2a-stars) builds it in the player's Options, so it has one key
  and never sits in the Player menu. Go to artist comes later.
- An item with no actions plays the edge sound and says "No options" (rule 9: every
  press of a key is heard). So does Options in a player with nothing loaded.
- **Play now** is an action that finishes a task: it starts playback, goes back by
  itself and says what plays ("Playing Weather."), like Play next and Add to queue.
- An action that finishes a task (Play next, Add to queue) goes back by itself and
  says what it did. An action that only reports something stays.
- `MenuItem` gets an `options` builder, next to `submenu` and `action`. Lists build it
  for their items; a player builds it for the current track.
- At Beginner verbosity the hint names it: "Press OK to play, Options for more."
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
- The book player has no queue, shuffle or repeat. Its step units are Skip · Chapter ·
  10 minutes, the music player's are Skip · Track · Album.

## The queue

Only the music player has a queue ([queue.md](queue.md) has the full design). The rules
that touch navigation:

- **OK on a track** in any list of tracks replaces the queue with that list and plays from
  the track. Play album / Play playlist / Play all replace it with the whole list.
  **Options → Play now / Play next / Add to queue** (on a track, album, playlist or
  artist) replace the queue, put the item after the current entry, or at the end.
- A remote item is loaded first ("Loading.", then "Added Weather, 12 tracks. 30 in
  queue."). Home while it loads drops the late answer. With nothing loaded, Play next and
  Add to queue make a queue that is paused at its first entry: "Added Weather, 12 tracks.
  Press play."
- **Shuffle** (off · tracks · albums) and **Repeat** (off · all · track) belong to the
  queue and start off with each new queue. They are "Setting with list" items in the
  music Player menu, not settings in `config.yaml`. Repeat track repeats the current
  track when it ends; Left / Right and a deep press still move to other tracks. With Repeat
  all, Next on the last track goes to the first and Previous on the first to the last.
- At the end of the queue: "End of queue", paused at the first entry, unless Repeat is on.
- **An entry that cannot play is skipped**: "Storm, source unavailable." (a run: "3
  tracks, source unavailable."). A server that fails is marked down and checked in the
  background; with nothing ahead that can play the device says "Server not reachable.
  Press play to try again."
- The music Player menu:

```
Player menu (music player)
├── Queue → <entries>      opens on the current entry, spoken "current"
├── Shuffle, off           list: off · tracks · albums
├── Repeat, off            list: off · all · track
├── Go to first track      the first entry in the play order
└── Sleep timer → …
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

**As built (phase 2):** the music player's Player menu is Queue · Shuffle · Repeat · Go to first track ·
Sleep timer, Options in the music player is Go to album · Album info (Star comes with streaming.md
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

The Streaming entry on Home is named after `server.name` in `config.yaml`
("Server" when it is not set). Its lists come over the network, so they follow
these rules too ([streaming.md](streaming.md) has the plan).

- **A list that loads from the network opens at once with one item, "Loading".**
  OK on it says "Still loading." Keys work meanwhile: Home leaves, and the late
  answer is dropped, so it is never spoken.
- **The answer fills the list where it is.** If the user is still on "Loading"
  and the list is on top, they are moved to the first item that came and it is
  spoken. If they have opened something else, gone to sleep, or moved to an item
  that had already loaded, the list is filled silently.
- **An error is a list too**: "Server not reachable" (or "Wrong user name or
  password", "Server error"), then "Try again". OK on the error says what to do.
  Try again goes back to "Loading" and asks again. Items that had loaded stay.
- **Without a server the list says why and what to do.** With no `server:`
  section in `config.yaml` the Streaming menu has one item, "No server set up",
  and OK says how to add one. A server this player cannot use (another type, or a
  build without network support) gives "Server not supported". An empty answer says so too ("No albums",
  "No tracks", "No playlists").
- **Long lists are paged by the server, not by the user.** Albums A to Z loads
  500 albums and fetches the next 500 in the background while the server has
  more. Recently added is one page of 500 and Random albums one page of 50.
  Holding Up or Down jumps to the first or last item of what has loaded.
- **A list from the server is not rebuilt when you come back to it.** It keeps
  what has loaded and the selected item, so going back does not wait on the
  network again. This is the one exception to "a menu is rebuilt".
- **Server status reports and stays.** It says "Checking the server.", then
  "Connected to Navidrome 0.64.2." (the server's name and full version) or the
  error. Moving to another screen drops the answer.
- **Download album / Download playlist** is the item right after "Play album" /
  "Play playlist". It is offered only where it can work: a cache folder, a server
  and a downloader that is set up. OK starts the download and says "Downloading, 0
  of 9 tracks." (tracks already whole count as done and are not fetched again).
  OK on it while it runs says the same with the count so far, and so does "where
  am I?", in a menu and in the player. One list downloads at a time: starting
  another says "Another download is running." Not enough room on the disk for the
  tracks that are missing starts nothing and says "Not enough space." A track
  that fails is said once ("Cannot download Rain. Server not reachable."), the
  download stops, and OK on Download again goes on from there. Leaving the menu,
  Home and playing something do not stop it. When it ends the device says
  "Download finished, Weather." unless something is playing, which gets only an
  earcon (and a sleeping device says nothing).
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
- **Offline.** When a list from the server fails with an error ("Server not
  reachable", "Server error" and so on), the list is "the error", "Try again" and, if
  anything is downloaded (not just cached by playing), "Downloaded" (the same list as in the Streaming menu, which
  needs no server). Without downloads there is no such item. Try again replaces the
  error list with "Loading" as before. A cached album played offline never says
  "Loading." and does not wait on the server.
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
  will). Leaving that list (Home), choosing another track or going to sleep
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
- **Server books are in Library**, after the local books, with the value "on the
  server" (spoken after the title). They are never in Music. They open as books:
  chapters are the tracks, and the place is kept and resumed like a local book. A book that is
  both on the device and on the server is listed twice, once for each; they are not merged.
  While their tracks load, the "Loading" rules apply. After a restart "Now playing" on Home resumes the
  book when the book player was used last (otherwise it is "Continue"); a downloaded book plays from
  its files at once, otherwise with the server down "Server not reachable." is said and Home stays.
- **Streaming stays a separate Home entry.** It keeps its own lists as above.

## Checklist for a new menu, item or program

- [ ] It is one of the item types above. A setting uses `choice()`.
- [ ] It adds no special cases for keys to `App::handleMenu`.
- [ ] A list that comes from the server opens with `App::openRemoteList()`, so
      it gets "Loading", the error with "Try again", and the empty text.
- [ ] A new program is a `Screen::Kind` with its own key handler. Home and
      holding OK leave it, and holding Home still goes to the Home menu.
- [ ] Anything destructive is behind a list whose first item is harmless.
- [ ] Left / Right in it jump (by letter if the list is sorted by name), and a
      setting changes only in its list.
- [ ] Options on its items does something or says "No options".
- [ ] The Help texts (`kButtonHelp`, "Getting started") still match.
- [ ] A test in `tests/test_app.cpp` drives it with the real keys.
