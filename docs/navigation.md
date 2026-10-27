# Navigation guidelines

Users can't see the menu, so they learn it from what each key does. A key that
does one thing in one menu and something else in the next menu breaks that.
These rules are the contract. Follow them when you add a menu or an item, and
change this file before you change a rule.

## Screens

Everything the user is "in" is a **screen** on one stack (`App::screens()`):

- a **menu**: a spoken list of items, or
- a **program**: a screen with its own keys. Today the only program is the
  **player**. A podcast player or a button tutorial would be programs too.

Home is always at the bottom of the stack. OK pushes a screen, and Home pops
one, back to wherever you came from. If you open a book from Library, Home
takes you back to Library. There is only one player screen: opening it again
returns to the one already on the stack. The Power menu works the same way, and
Sleep in it closes the menu first, so the device wakes where you were.

## The rules

1. **Up / Down move, and only move.** In a menu they go to the previous or next
   item, and holding them jumps to the first or last item. In the player they
   change the volume.
2. **OK goes in.** It opens the selected item's submenu or program, or runs its action.
3. **Home goes out** to the previous screen. Holding OK does the same.
   Leaving a screen changes nothing. Home on the Home menu plays the edge
   sound and says "Home".
4. **Holding Home goes to the Home menu**, from anywhere.
5. **Left / Right change a value in place, and do nothing else.** On an item
   without a value they do nothing at all, with no sound. They never mean
   "back" or "open". At the end of a value's range you hear the edge sound.
6. **A changed value works at once, and is saved when you leave the item.**
   You hear the new speech rate straight away. Steps are not written one by
   one: the file is written when another key is pressed or the device sleeps
   or powers off. Streaming quality is the one exception: a stream that plays
   would have to open again, so the new quality starts with the next track.
7. **In a program, Left / Right are that program's own keys.** In the player
   they skip back and forward, and holding them or pressing them deeper
   moves to the previous or next chapter or track.
8. **Play/Pause, Power and Talk work the same on every screen.** Holding
   Play/Pause ("where am I?") speaks the screen path and the selected item,
   and the progress of a download that runs ("Downloading, 3 of 9 tracks.").
9. **Every press of a key that has a job is heard.** The press interrupts
   speech and plays an earcon, speaks, or both. Rule 5 is the one exception.

## Item types

What an item does depends on which parts of `MenuItem` are set:

| Type | `value` | `adjust` | OK opens | Example |
|------|:-:|:-:|---|---|
| **Submenu** | (info) | – | `submenu` | Library, Road Trip (`2 tracks`) |
| **Setting** | ✓ | ✓ | – | (none yet; use for on/off values) |
| **Setting with list** | ✓ | ✓ | `submenu` of all values, on the current one | Speech rate, Volume |
| **Status with list** | ✓ | – | `submenu` of choices, on the current one | Sleep timer (`15 minutes left`) |
| **Action** | (info) | – | runs `action` | Status, Time and date |
| **Program** | (info) | – | `action` pushes the program's screen | a book, a track, Now playing |
| **List from the server** | (info) | – | `action` opens a list that loads from the server | Recently added, an album on the server |

`choice()` in `app.cpp` builds a *setting with list* from a settings key and
its options. In the list the current value is spoken as "current". Choosing a
value with OK saves it and goes back. Use `choice()` for every user setting.

## Two-stage keys

Left and Right may be **two-stage** keys: a light press, and a deeper press
through a second contact. The deep stage is a separate input (`BackDeep`,
`ForwardDeep`). The gesture detector turns it into a `Deep` gesture on Left /
Right as soon as it closes, and the light press that came with it is then
ignored. On one-stage hardware, holding the key does the same as a deep press.

| | Light press | Hold | Deep press |
|---|---|---|---|
| Player | skip back / forward | previous / next chapter | previous / next chapter, without waiting |
| Menu | step the value | step the value | step the value |

## Key table

| Key | Menu | Player |
|-----|------|--------|
| Up / Down | previous / next item | volume up / down |
| Up / Down held | first / last item | volume up / down |
| Left / Right | change the value (nothing if none) | skip back / forward |
| Left / Right held or deep | change the value (nothing if none) | previous / next chapter / track |
| OK | **in**: open / run | Player menu |
| OK held | **out** | **out** |
| Home | **out** | **out** |
| Home held | Home menu | Home menu |
| Play/Pause | play / pause | play / pause |
| Play/Pause held | where am I? | where am I? |
| Power / Power held | sleep / Power menu | sleep / Power menu |

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
- **An action that finishes a task goes back by itself** (choosing a value or
  a sleep timer). An action that only reports something stays where it is.
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
  A local book played after the stream is resumed instead, as before.
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
  its tracks, so that list follows "Lists from the server": "Loading", the error with "Try again", "Play album" first, Album info last.
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
- **Album info** is the last item of an album's list, after the tracks. OK says
  where the album plays from ("On the device", "Downloaded" or "On the server"),
  then "Also on the server" for an entry that is on the device or downloaded and
  also on the server, the artist, and the number of tracks. It only reports, so it stays where it
  is. Nothing else speaks where an album plays from. When the queue plan
  ([queue.md](queue.md) Q7) is built first, Album info moves to Options.
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
  book that played last (it is the last played unless a stream played after it); a downloaded book plays from
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
- [ ] The Help texts (`kButtonHelp`, "Getting started") still match.
- [ ] A test in `tests/test_app.cpp` drives it with the real keys.
