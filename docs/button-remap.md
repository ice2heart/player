# Button remap: plan

> Status: mapping and implementation parameters confirmed on 2026-10-29;
> the contract is recorded before code. Implementation steps remain below. Contract: [navigation.md](navigation.md).
> Implementation references: [architecture.md](architecture.md).

Simplify navigation after UX testing: Back/Forward become the primary list keys;
held OK exposes Options; Up/Down are unused.

## Requested mapping

| Button | Menu | Player |
|---|---|---|
| Volume + / − | Change content volume; hold repeats | Same |
| Play | Toggle current playback; hold says where am I | Same |
| Forward / Back short | Next / previous item | Next / previous track or chapter |
| Forward / Back deep | Forward / back 10 items immediately; continued hold reaches last / first item | Faster repeated seeking immediately; easing to light stage returns to normal seeking |
| Forward / Back held | Last / first item, once | Seek forward / backward until released |
| OK short | Select current item | Open Player menu |
| OK held | Options for selected item | Options for current playback |
| Esc short | Previous screen; at Home return to current player | Previous screen |
| Esc held | Home | Home |
| Up / Down / Talk | No action | No action |
| Power / Hold switch | Existing behavior | Existing behavior |

## Confirmed decisions

- Fast forward/rewind uses repeated seeks, not a new playback-speed mode.
- Deep-then-hold in a menu jumps 10 immediately, then reaches the end when held.
- Deep in a player starts faster seeking immediately; easing back to the light
  stage returns to normal seeking. Releasing ends seeking without a track skip.
- Short OK in the player retains Player menu; held OK opens Options. The separate
  Options input remains an alias.
- Short Esc at Home returns to a loaded player, even paused, without resuming it.
  Held Esc at Home stays Home. Up/Down/Talk are silent during normal use.
- Ordinary menu steps retain the existing Menu wrap setting. Ten-item and end
  jumps clamp at the boundary regardless of Menu wrap.
- Back keeps the current convention: after more than 3 seconds in a track/chapter,
  restart it; otherwise move to the previous track/chapter.
- A remote-list end jump stops at the loaded boundary and never fetches remaining
  pages automatically. Keep Load more accessible. When more pages remain, append
  "More items are available. Load more to continue." to the boundary announcement.
  Do not say this at the true end of a fully loaded list. Loading/error status
  rows retain their own announcements rather than promising Load more prematurely.

## Confirmed seeking parameters

- Light hold starts at the configured long-press threshold (600 ms by default).
  Menu deep-then-hold uses that threshold measured from the initial light press.
- Seek 5 seconds every 500 ms normally (10 seconds per second held), or 30 seconds
  every 500 ms at the deep stage (60 seconds per second held), in either direction.
  Apply the first step when seeking starts; changing stages changes the rate
  without adding an extra step or resetting the repeat deadline.
- Preserve playing/paused state. Clamp within the current file; short presses
  remain the way to cross tracks/files. For unknown duration, clamp backward at
  zero and allow forward seeking without an invented upper bound.
- Use one movement earcon when seeking starts and one edge earcon on reaching a
  known boundary; announce the final position on release. Do not queue speech on
  each repeat. Confirm responsiveness and usefulness in local/remote device trials.
- These are approved initial implementation parameters, not measured UX results.
  Transcoded streams may need a slower cadence if reopening cannot keep up.

## Findings and reuse

- Volume repeat already works: first step immediately, 400 ms delay, then one
  step every 150 ms. Play and held Play already work globally.
- `app/core/app.cpp`: reuse menu movement from `handleMenu`, Options through
  `openOptions`, player return through `enterPlayer`, and chapter/track movement
  through `stepChapter`, retaining its current restart convention for Back.
- `jump` currently switches to alphabetic groups in sorted lists. Deep presses
  must instead move by 10 everywhere. Remote lists can contain Load more/status
  rows: an end jump reaches the end of the loaded list, not unseen server data.
- `app/core/buttons.{h,cpp}` currently consumes a hold or deep press as a single
  event, suppressing later hold behavior. Seeking needs stage transitions and
  release handling, including pressing deeper after a light hold has started.
- `MediaPlayer` supports absolute seeks, with no playback-speed API. Transcoded
  streams reopen on seek; repeated seeking needs real-server responsiveness checks.
- Player step units, their saved `unit.music` / `unit.book` keys, and Skip back /
  Skip forward settings lose their button purpose. Retire obsolete controls;
  tolerate older saved keys rather than rewriting user state just for migration.

## Phase 1: simpler buttons

Goal: use the requested mapping in both front ends, with reliable stop-on-release seeking.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|---|---|
| ✅ 1.1 | **Contract first.** Record the confirmed mapping and parameters, then update navigation.md before code. Specify timing, boundaries, feedback and inactive buttons. | Walk through menus, player, Home and Options with the agreed mapping. |
| ✅ 1.2 | **Gesture lifecycle.** Extend GestureDetector and App input handling for light/deep holds and release. Preserve volume, Play, Power and Hold behavior. | No short action after a hold/deep action; deep after light hold; easing off deep; release order/bounce; cancellation on Hold, sleep, screen change and shutdown. |
| ✅ 1.3 | **Menu and Options mapping.** Move single-item navigation to Back/Forward, deep to 10 items, hold to ends; held OK opens existing Options; implement Esc return from Home; disable Up/Down/Talk in normal use. Remote end jumps stop at loaded items and announce when more pages remain. | Sorted and unsorted lists, empty/single-item lists, boundaries/wrap, remote loading/error rows, partial-list end speaks the continuation line without fetching, fully loaded end omits it, held OK never activates the item, Options nesting, paused/current/no-player Home return. |
| ✅ 1.4 | **Player mapping.** Short Back/Forward uses chapter/track movement with the existing Back restart convention; holds seek at the agreed rates. Preserve prior play/pause state and stop seeking on release or cancellation. Retire step-unit controls and obsolete skip settings. | Back before/at/after the 3-second threshold; chapters within/across files, queue tracks, paused seeking, both rates/directions, boundaries, unknown duration, pending/failed opens, no trailing track skip, old saved unit keys harmless. |
| 1.5 | **Front ends, help and references.** Update simulator layout/help, spoken key descriptions and beginner hints, input adapters as needed, and scripts to express a deep hold. Update architecture.md, SPEC.md, README.md, and affected future plans in input-methods.md / hal.md. Migrate affected test navigation helpers. | Both front ends produce equivalent gestures; scripts express light/deep holds; help matches actions; run relevant gesture/frontend tests and full core suite. |

**Device check (manual):** navigate a long sorted list using one/ten/end jumps;
open Options with held OK; browse while audio plays; return from Home; seek at
both rates in local audio and a streamed/transcoded book; release, sleep and lock
while seeking; verify volume repeat and held Play remain usable without sight.
Jump to the loaded end of a paged remote list, hear the continuation line, activate
Load more, and verify the line disappears once the actual end is loaded.

## Open questions

None blocking implementation. Seeking responsiveness and usefulness, especially
for transcoded streams, remain device-trial checks rather than measured results.
