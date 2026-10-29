---
name: implement-phase
description: Implement one phase of a plan file (default docs/streaming.md) end to end. Spawns one agent per step (N.1, N.2, …) in order; each implements, tests, sabotage-checks, reviews, fixes and commits its step. Then reviews the whole phase, commits the fixes and closes the phase. Use when the user says "implement phase N" or invokes $implement-phase.
---

# Implement a phase

You are the **orchestrator**. You do not write feature code yourself. You pick the phase, spawn one agent per step **one at a time**, check each agent's result, then run the phase review and close the phase.

`CHECK` below means the full local check, run from the repo root:

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

Steps that touch the build, dependencies or Docker files also run the Docker targets they affect (`docker build -f docker/Dockerfile --target native .` and/or `--target pi`).

## Codex conventions

Invoke as `$implement-phase docs/streaming.md 2` or ask in plain language.
Read arguments from the user message, not a Claude command variable.
Discover the repository root from the workspace; do not assume a user-specific path.
Follow applicable `AGENTS.md` instructions and explicitly read `REPO.md` for
this repository's build, layering, testing and Git conventions. These do not
override higher-priority instructions or the user's explicit choices.

This workflow requests sequential delegation when available and permitted. Use
Codex collaboration tools such as `spawn_agent`, `send_message` and
`followup_task`, waiting for each step to finish before starting another. Use a
fresh phase reviewer. Inherit model and reasoning settings; do not guess a
Codex equivalent of the Claude model ladder. If delegation is unavailable or
prohibited, do the work sequentially yourself and disclose that review was not
independent.

For code review, read and follow an applicable `code-review` skill if available.
Otherwise review the diff explicitly using the criteria below and disclose the
fallback. Do not assume a Claude Skill tool or `/code-review high` command exists.
Do not fabricate model names, versions or co-author email addresses; follow any
actual repository attribution requirements.

## 0. Preflight

1. Parse the user's message (either order, both optional):
   - A path (contains `/` or ends in `.md`) is the plan file, `PLAN`. Default: `docs/streaming.md`. If the file doesn't exist, stop and say so.
   - A phase identifier (including sub-phases such as `2a`) is the phase.
   Examples: `$implement-phase 1`, `$implement-phase docs/streaming.md`, `$implement-phase docs/streaming.md 2`.
2. Read applicable `AGENTS.md`, `REPO.md`, `PLAN` (the target phase, the one before it, its intro and its **Decisions** / **Open questions** sections) and skim `docs/architecture.md` sections the phase touches.
3. Work out how `PLAN` marks steps and progress, and tell the user how you read it (the expected format is in `docs/writing-plans.md`):
   - Step ids are `N.M`, usually rows of a `| # | Step | Tests |` table; the Tests column is that step's done criteria.
   - A step is done when its id cell is ticked as `✅ N.M` (or its `- [ ]` box is `- [x]` if the plan uses boxes). This is `MARK` below.
   - A phase written as bullets without step ids has no steps yet: **stop and ask** the user to break it into `N.M` steps first (offer to draft the table).
   - The phase's manual check is its **Device check (manual)** paragraph, or its "Goal" line if it has none.
4. Pick the phase:
   - If a phase number was given, use it.
   - Otherwise use the first phase that has unfinished steps.
   - If the previous phase still has unfinished steps, **stop and ask**. Don't skip ahead.
   - If a step depends on an open question in `PLAN` that isn't decided, **stop and ask**.
5. `git status` must be clean. If not, report the paths and ask how to handle them; never stash, reset or commit unrelated work. Verify the branch is `main`; if not, ask for direction before switching or committing.
6. Record the base commit: `BASE=$(git rev-parse HEAD)`. The phase review diffs against it.
7. List the unfinished steps of the phase in order (`N.1`, `N.2`, …). Finished steps are skipped.
8. Tell the user in one short block: the plan file, the phase, the steps you will run, how you read `MARK`, the base commit, and the delegation/review mode. Then start. Don't wait for confirmation.

## 1. One agent per step (sequential)

For each unfinished step, in order, spawn **one** Codex step agent using inherited model settings and wait for it to finish. Never run two step agents at once: each step builds on the previous commit.

Fill in this prompt (keep the placeholders' content specific, not a summary of the whole plan):

```
You are implementing step {N.M} "{step title}" of {PLAN} in {REPO_ROOT}.

Read first: applicable AGENTS.md and REPO.md (repository rules, especially "Layering rules" and "Every test must be checked with a sabotage"), SPEC.md where the step touches product behaviour, docs/navigation.md if it touches menus or keys, the docs/architecture.md sections this step touches, the Decisions section of {PLAN}, and the full text of step {N.M} (both the Step and the Tests column).

Context from earlier steps in this phase:
{one or two lines per finished step: what it added, file names, anything the next step relies on. "none" for the first step.}

Do, in this order:
1. Implement the step exactly as the plan describes. Keep the layering: app/core/ is plain C++20 with no third-party or OS calls beyond std::filesystem, talks out only through app/core/services.h interfaces, and never reads the real time (use clock_). Don't build later steps or later-phase features.
2. Write tests for all new or changed behaviour, covering everything in the step's Tests column. Core tests use tests/fakes.h and drive App through keyDown / keyUp / tick; check what is spoken, which file plays and what is saved, not private state.
3. Sabotage-check every new or changed test as REPO.md describes, in the Ninja build-sabotage dir: green → smallest plausible bug in production code → red for the expected reason (read the message) → restore → green. Cover each meaningful branch. Never leave sabotage in the tree (check git diff).
4. Run the full check until it passes:
   cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
   If the step touches CMakeLists.txt, cmake/, docker/ or dependencies, also run the Docker targets it affects (docker build -f docker/Dockerfile --target native . / --target pi .), and the core-only configure (-DOAP_BUILD_SIM=OFF -DOAP_BUILD_HEADLESS=OFF).
5. If your diff has no code changes (it touches only `.md` files), skip the `code-review` skill: re-read the diff yourself for wrong claims, leaked secrets and docs that no longer match the code, and write `REVIEW: skipped, no code changes`. Otherwise, before committing, use an available `code-review` skill on your uncommitted working tree: read and follow the available code-review skill. If no applicable skill is available, review the diff explicitly and say so in REVIEW. Either way, look for: correctness bugs, layering violations (media/ or platform/ headers in core, real time in App), threading bugs (UI vs decoder vs worker threads, callbacks after cancel or destruction), blocking calls on the UI thread, anything that could leave the user without speech or with a hung device, menus that break docs/navigation.md rules, magic numbers, missing or weak tests, dead code.
6. Fix every real finding, add tests for bug fixes (red first, then green, sabotage-checked), and rerun the full check.
7. Verify the step's Tests column and goal. Anything that needs a human (listening to audio, real buttons, the Pi, a real server, Wi-Fi) can't be verified by you: list it as a manual check, don't claim it.
8. Mark the step done in {PLAN}: {MARK}. If the step changed a build or run command, update REPO.md's "Build and test" section. Update docs/architecture.md if the step changed what it describes.
9. Commit everything to main (no branch) with message `{N.M}: <short summary>` in the style of `git log` (plain sentence, capitalised, no trailing period); add a body when the change needs explaining, and follow actual repository attribution requirements without inventing a co-author
   Don't push.

Stop instead of guessing if: the code and docs disagree, the step contradicts the Decisions section or REPO.md, it depends on an undecided open question, or you can't make the check pass. In that case commit nothing, leave the tree as is, and report BLOCKED with the reason.

Final report (this is all the orchestrator sees). Keep it terse, no prose outside these lines:
STATUS: DONE | BLOCKED <reason if blocked>
COMMIT: <sha or none>
SUMMARY: <one line>
CHECKS: <commands and outcomes, including any skipped or blocked checks>
SABOTAGE: <each new/changed test and the production bug that made it fail>
REVIEW: <first line: code-review ran (or why not), N findings, M fixed. Then only unfixed findings, one line each with why>
MANUAL CHECKS: <one per line: setup → what you should hear. Omit if none>
NOTES: <only what later steps must know. Omit if none>
```

### After each agent returns

1. If `STATUS: BLOCKED`: stop the loop, show the user the reason and `git status`, and ask how to proceed. Don't spawn the next step.
2. Verify, don't trust:
   - `git status` is clean and `git log -1` is the reported commit with the `N.M:` prefix.
   - The step is marked done in the plan.
   - `git show --stat HEAD` touches `tests/` when it touches logic.
   - The REVIEW line says the `code-review` skill ran, or `skipped, no code changes` and `git show --stat HEAD` touches only `.md` files. An explicit diff review is also acceptable when no applicable review skill exists. If review was omitted, send it back to run the review and fix the findings in a follow-up commit.
   If anything is off, send the same agent a follow-up (using the available follow-up task tool and its id) to fix it. If it still isn't right, stop and ask the user.
3. Post one short progress line to the user (`1.3 done: <summary> (abc1234)`) and keep the agent's MANUAL CHECKS and NOTES for later.

## 2. Phase review

When every step is committed:

1. Run `CHECK` yourself. If it fails, spawn a sequential fix agent with the failure output before going on.
2. Spawn a **fresh** Codex review agent using inherited model settings (not one of the step agents) to review the whole phase:

```
Review phase {N} of {PLAN} in {REPO_ROOT}: the diff `git diff {BASE}..HEAD` (commits: `git log --oneline {BASE}..HEAD`).
Read REPO.md, the phase and Decisions sections of {PLAN}, and the docs/architecture.md sections the diff touches first. Use an applicable code-review skill on that range when available; otherwise review the diff explicitly and disclose the fallback. If the range has no code changes (only `.md` files), skip the skill and review the docs yourself: claims against the code, consistency between sections, leaked secrets.
Look especially for problems that only show up across steps: interfaces in app/core/services.h that one side implements differently, fakes in tests/fakes.h that behave unlike the real implementation, callbacks that can arrive after a menu or player was left, edge cases at step boundaries, duplicated helpers, layering violations, constants that ended up hard-coded, docs (architecture.md, navigation.md, help texts) that no longer match, tests that don't pin the behaviour, gaps against the phase's goal and device check.
Then fix every real finding: tests first for bugs (red, then green), sabotage-check new tests as REPO.md describes, and the full check passes (cmake --build build -j && ctest --test-dir build --output-on-failure).
Commit to main as `Phase {N} review fixes` with a body listing each fix as a `- ` bullet (see `git show f5fa32d` for the style), following actual repository attribution requirements without inventing a co-author
If there is nothing worth fixing, commit nothing.
Report tersely, no prose: COMMIT (sha or none), UNRESOLVED (only unfixed findings, one line each with why; omit if none), MANUAL CHECKS (omit if none).
```

3. Verify as in section 1 (clean tree, commit message, `CHECK` green).

## 3. Close the phase

1. Confirm every step of the phase is marked done.
2. Go through the phase's goal and **Device check (manual)**. Mark which parts were verified by tests and which need a human.
3. Build the **manual test list** from every agent's MANUAL CHECKS, the device check, and every test criterion that only a human can confirm (how it sounds, real buttons, the Pi, a real server, Wi-Fi on and off). Merge duplicates and order them so as few sessions as possible cover them: simulator first, then headless, then the device. Each item gives:
   - **Setup:** the exact command (e.g. `./tools/make_sample_library.sh library music && ./build/oap-sim library`, `./build/oap-headless library`, a Docker or Pi step) and any config needed (e.g. `server:` in `config.yaml`).
   - **Do:** which keys to press, or what to change (pull the Wi-Fi, stop the server).
   - **Expect:** what you should hear, concrete enough to say pass or fail.
   - **Step:** which step it comes from.
4. Final report to the user, in this order:
   - Commits of the phase (`git log --oneline {BASE}..HEAD`).
   - One line per step, plus the review fixes commit (if any).
   - Anything left unverified, skipped or unresolved (including any REVIEW / UNRESOLVED items), stated plainly.
   - **Last, so it's what the user sees at the bottom:** a `## Manual checks` section with the list from item 3 as `- [ ]` checkboxes, numbered, and a closing line asking the user to run them and reply with the numbers that failed and what they heard.
5. Don't push and don't start the next phase. The phase is closed only once the user confirms the manual checks.

## 4. When the user reports manual check results

- All pass: say the phase is closed. Nothing else to do.
- Some fail: for each failure, spawn one Codex agent using inherited model settings (one at a time) with the failing check, what the user heard, and the related step's commit. It reproduces the problem in a test where possible (red first), fixes it, sabotage-checks, runs the full check, and commits to main as `Phase {N} manual-check fixes: <summary>` following actual repository attribution requirements. Verify as in section 1, then show the user **only the checks that need re-testing** in the same `## Manual checks` format, again at the end of the message.

## Verification boundaries

Respect sandbox permissions and request execution approval when required. Report
checks that cannot run as blocked or unverified; never claim they passed or mark
a step done before required automated checks pass. Keep manual verification
pending separately from implementation completion. Do not run ASan or TSan unless
asked. Never speak, log or print credential-bearing stream URLs.

Use a separate build directory for core-only checks, and configure, build and test
it with the applicable OAP_BUILD_NET setting from REPO.md. Run affected Docker
targets for build/dependency changes. Inspect actual test coverage and sabotage
evidence, not just whether the commit touches tests/. Do not repeat unchanged
checks without a new change, failure or unresolved concern. If no unfinished
steps remain, report that without manufacturing work. If no manual checks apply,
say so instead of inventing checks. Never close a phase with unresolved findings.
