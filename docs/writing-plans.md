# Writing a plan for `/implement-phase`

A plan is a Markdown file in `docs/` (see [streaming.md](streaming.md) and
[queue.md](queue.md)). The skill in
[.claude/skills/implement-phase/SKILL.md](../.claude/skills/implement-phase/SKILL.md)
reads it, gives each step to its own agent in order, reviews the phase and lists
the manual checks. Run it with `/implement-phase docs/<plan>.md <phase>`.

## Layout

```markdown
# <Topic>: plan

> Status: what is built, what is a plan. Links to architecture.md and navigation.md.

<What the device does today and what the plan adds.>

## <Design sections: behaviour, menus, files, what is spoken>

## Phase 1: <name>

Goal: <what the user can do at the end, in one or two sentences>.

| # | Step | Tests (each checked with a sabotage, as CLAUDE.md requires) |
|---|------|------|
| 1.1 | **Short title.** What to build: classes, files, keys, spoken text. | What the tests show. |
| 1.2 | ... | ... |
| 1.N | **Docs for phase 1 as built:** architecture.md, navigation.md, SPEC. | – |

**Device check (manual):** <what a person does on the device and what they hear>.

## Phase 2: <name>
...

## Decisions

## Open questions
```

The skill depends on these:

- **Phases** are `## Phase N: <name>`. A sub-phase such as `2a` is fine (`2a.1`).
- **Step ids** are `N.M` in the first column of the table. A finished step is ticked
  `✅ N.M`. Don't tick by hand unless the step really is built.
- **The Tests column is the step's done criteria.** The agent writes a test for each
  item and checks it with a sabotage.
- **The Device check (manual)** paragraph becomes the manual test list at the end of
  the phase. Without one, the skill uses the Goal line.
- **Decisions** are binding. An agent stops (BLOCKED) when a step contradicts them.
- **Open questions:** a step that depends on an undecided one stops the skill. Write
  down the option that was chosen, even if it is still to be confirmed on a device.

## A good step

- **One commit, one feature.** An agent with a fresh context does one step. If the
  Step cell needs two "and"s for different features, split it.
- **The device works after every step.** When a step changes a key or a menu that a
  later step finishes, say what it does in between ("Up / Down still change the
  volume until 1.3", "says 'Not available yet.' until 2.9").
- **Concrete enough that nothing is guessed:** the names of classes and files, the
  spoken text in quotes, the keys of saved files, the numbers (timeouts, limits).
  The agent stops on anything it would have to make up.
- **Tests describe what is heard and saved,** not internals: "Down from Skip says
  'Track'", "a second exit writes nothing".
- **Cross-references by step id** ("step 2.5"), and keep them right when steps are
  renumbered, here and in the other docs (`grep -rn "step [0-9]" docs`).
- **Last step of a phase updates the docs** as built: architecture.md, navigation.md
  where the build differs, SPEC.

## Before you run it

- The design sections, navigation.md and streaming.md agree with each other. A
  contradiction makes an agent stop.
- For menus and keys, the first step of the plan updates
  [navigation.md](navigation.md) before any code.
- The working tree is clean: commit the plan first.
- After a phase, add a "Phase N as built" section if the build turned out different
  from the plan (see streaming.md).
