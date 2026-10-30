# Input methods: draft plan

> Status: proposed, not implemented. Reviewed 2026-10-30. Current behavior is in
> [architecture.md](architecture.md); keys are governed by [navigation.md](navigation.md).
> Phase 1 is button text entry. Speech engines and provisioning transports are later work.

## Goal

A user can enter and correct text without sight, speech recognition, a phone, or a
computer. Voice and a keyboard feed the same editor; they do not bypass validation
or confirmation. First-boot Wi-Fi/server setup needs this editor but also needs the
network/setup services in [hal.md](hal.md); building an editor alone does not deliver setup.

## Proposed button editor

Use a dedicated `Screen::Kind::TextEntry` program. Ordinary menus retain their keys.
The draft is held in RAM and changes the caller's value only on Done. No keystroke,
cursor move, or recognition partial result writes to flash.

- The screen contains a spoken list: Characters, Cursor, Delete previous character,
  Read text, Clear text, Done, Cancel. Back/Forward select (deep ten, held first/last), OK activates, Esc cancels
  and returns; holding Esc discards the draft and goes Home. Returning never commits.
- Characters opens a list of groups: lowercase letters, uppercase letters, digits,
  punctuation, space. OK on a group opens its characters; OK inserts at the cursor,
  speaks the inserted character, and stays in that list for repeated entry. Esc
  returns one level. The ASCII punctuation set includes every printable ASCII symbol.
- Cursor opens a list of insertion positions, including Start and End, described by
  the adjacent character and its ordinal. Choosing a position returns to the editor.
  This deliberately uses normal list navigation instead of inventing cursor keys.
- Delete previous character at Start gives the edge sound and says "Start of text."
  Otherwise it removes one Unicode code point before the cursor and announces deletion.
  The first version supports ASCII insertion; imported UTF-8 must remain valid. Grapheme
  editing and localized character sets require a later decision before language packs.
- Clear text asks "Clear text?" with Cancel first. Done validates and, on error,
  keeps the draft, speaks the problem and returns to the editor. Cancellation leaves
  the caller's original value intact. No confirmation or editor timeout.
- Play/Pause and Volume retain their global jobs. Hold Play describes the editor,
  field name and cursor, never a secret. Held OK / Options has no editor actions initially and says "No options";
  Up/Down remain inactive.
- For password/API-key fields, echo "Character added" and counts rather than values;
  Read text says "Hidden, N characters." No secret enters the speech log, stdout,
  debug widgets or snapshots. Explicit secret readback is outside phase 1.
- A `TextEntryRequest` supplies field label, initial value, secret flag, maximum UTF-8
  byte length and validation callback. The editor itself has a 4096-byte ceiling;
  callers may impose smaller limits. Oversize input is rejected with "Text too long."
  Use the same validation for buttons, a keyboard and recognized text.

This layout is a draft for blind-user trials. Phase 1.1 records the agreed contract
before code; phase 1.4 evaluates entry time, error correction and fatigue on realistic
SSID, password and URL examples. A shortcut may follow evidence from those trials.

## Voice and keyboard

Talk is currently silent and inactive. Before phase 2 enables it, update the
navigation contract and lifecycle tests. Proposed Talk contract: press and hold starts capture with an earcon; release stops
capture and starts recognition. Holding longer never switches into dictation. In a
text editor the result is proposed text; elsewhere it is a command. A short press
with no usable speech says "No speech heard." Home cancels capture/recognition and
late results are ignored. Content pauses during capture and returns to its prior
play/pause state when capture ends, unless the user changed that state meanwhile.

Recognized text remains a proposal until accepted. Non-secret text is read back;
secret text is described by length only. Retry, spelling and correction are available
through buttons. Recognition never sends credentials or runs destructive commands
without the normal explicit confirmation. STT is offline; the engine/model and capture
adapter remain open decisions. No global raw Talk routing change is implemented until
its tests cover press/release, cancellation, Hold and sleeping.

A physical keyboard adapter later sends text/edit events into the same editor and
keeps device controls separate. UTF-8 paste must obey length and secret handling.
The simulator must not implement its own value editor or validation rules in ImGui.

## Phase 1: button-only text entry

Goal: enter, correct and confirm text using existing buttons in both front ends.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 1.1 | **Navigation contract first.** Review the draft interaction and update navigation.md with TextEntry, cancellation and secret rules. | Manual walkthrough of every operation with no visual display; document any changed choices before implementation. |
| 1.2 | **Editor model.** Add `app/core/text_entry.{h,cpp}` with draft, cursor, limits, insertion, deletion and validation; no I/O. | Editing at both ends, valid UTF-8 after deletion, rejected oversize input, invalid Done retains draft, Cancel preserves original. |
| 1.3 | **Spoken editor.** Add App screen builders and a callback returning accepted text or cancellation. Use it for the queue's Save as playlist name; buttons work in both front ends. | Enter and edit a name through keyDown/keyUp/tick; save uses accepted name; cancellation sends no server request; secret harness never exposes its contents; editing causes no settings/state writes. |
| 1.4 | **User trial and docs as built.** Record button-only name, SSID and URL editing trials; update architecture.md, navigation.md and SPEC with actual capability and limits. | Manual trials with blind users; real setup remains pending its service integration. |

**Device check (manual):** with the screen hidden, enter a name containing uppercase,
space and punctuation, fix a middle character, cancel once, then save. In an isolated
secret-field trial, inspect logs as well as speech for disclosure. No real credentials
in test fixtures or trial reports.

## Phase 2: additional input adapters (not ready to implement)

Goal: the same editor accepts keyboard text and offline voice proposals.

| # | Step | Tests (each checked with a sabotage, as REPO.md requires) |
|---|------|------|
| 2.1 | **Keyboard input adapter.** Define frontend text events and connect SDL text input and a selected headless keyboard transport to TextEntry. Decide the headless transport first. | Identical edits yield identical results; paste limits and secret handling; disconnect does not commit a draft. |
| 2.2 | **Capture and recognition service.** After the engine/capture decision, introduce a core interface with request ids and UI-thread completion; update navigation.md Talk contract first. | Release stops capture; Home/sleep/Hold cancel; late results cannot edit a different field; playback restoration respects intervening user intent. |
| 2.3 | **Voice proposal and spelling.** Add accept/retry/correct and spelling vocabulary, then integrate with the setup flows delivered by HAL work. | Accepted proposal uses normal validation; no recognition result commits credentials implicitly; setup remains possible with buttons alone. |
| 2.4 | **Docs as built.** Update architecture.md, navigation.md and SPEC; record offline device trials. | – |

**Device check (manual):** disconnect the network, dictate and correct non-secret text,
cancel a slow recognition, then finish the same task using only buttons.

## Decisions and open questions

- Button entry is the baseline; draft edits stay in memory. No STT dependency in core.
- Open before phase 2: STT engine/model budgets, microphone/capture backend, headless
  text keyboard transport and localized character/grapheme handling.
- Wi-Fi AP provisioning and USB configuration import need separate transport and
  credential-lifecycle designs; they do not replace the standalone button editor.
