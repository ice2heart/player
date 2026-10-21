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
returns to the one already on the stack.

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
   or powers off (or earlier, if something else saves, such as the playback position).
7. **In a program, Left / Right are that program's own keys.** In the player
   they skip back and forward, and holding them or pressing them deeper
   moves to the previous or next chapter or track.
8. **Play/Pause, Power and Talk work the same on every screen.** Holding
   Play/Pause ("where am I?") speaks the screen path and the selected item.
9. **Every press of a key that has a job is heard.** The press interrupts
   speech and plays an earcon, speaks, or both. Rule 5 is the one exception.

## Item types

What an item does depends on which parts of `MenuItem` are set:

| Type | `value` | `adjust` | OK opens | Example |
|------|:-:|:-:|---|---|
| **Submenu** | (info) | – | `submenu` | Library, Road Trip (`2 tracks`) |
| **Setting** | ✓ | ✓ | – | (none yet; use for on/off values) |
| **Setting with list** | ✓ | ✓ | `submenu` of all values, on the current one | Speech rate, Volume |
| **Status with list** | ✓ | – | `submenu` of choices | Sleep timer (`15 minutes left`) |
| **Action** | (info) | – | runs `action` | Status, Time and date |
| **Program** | (info) | – | `action` pushes the program's screen | a book, a track, Now playing |

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
  current value, Chapters on the playing chapter. Use `MenuItem::selected`.
- **A menu is rebuilt when you come back to it**, so it shows what changed
  meanwhile ("Now playing" appears on Home). The same item stays selected.
- **A dangerous action is never the first item of its list.** It sits in a
  list that opens on something harmless: `Cancel` in Factory reset, `Sleep`
  in Power. Pressing OK twice by accident then does no harm.
- **An action that finishes a task goes back by itself** (choosing a value or
  a sleep timer). An action that only reports something stays where it is.
- **An empty list says why and what to do** ("Library is empty", "No music").

## Checklist for a new menu, item or program

- [ ] It is one of the item types above. A setting uses `choice()`.
- [ ] It adds no special cases for keys to `App::handleMenu`.
- [ ] A new program is a `Screen::Kind` with its own key handler. Home and
      holding OK leave it, and holding Home still goes to the Home menu.
- [ ] Anything destructive is behind a list whose first item is harmless.
- [ ] The Help texts (`kButtonHelp`, "Getting started") still match.
- [ ] A test in `tests/test_app.cpp` drives it with the real keys.
