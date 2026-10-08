# Experiment flags — plan

> **Status:** Shipped. Trowel creates and opens
> `~/.config/turmeric/experiments.tur`, the REPL banner shows the resolved
> experiment set, and an `experiments.status` control command reports the
> resolved names and source for smoke tests. `tur` (v0.62.0) reads the user
> file and `build.tur` `:enable` itself, so Trowel does not forward
> `--enable=` — it reads the files only for display.
> **Related:** [`engine-selection.md`](engine-selection.md) (shipped; shares
> the `MakeTurInvocation` seam).

Let the user pick which Turmeric experimental features are on when Trowel
launches the REPL or evaluates a buffer. Three sources, in order of
priority:

1. **CLI overrides** — an explicit `--enable=` passed by a launcher script
   or a debug menu action always wins. Trowel does not synthesize these;
   it just passes them through when present.
2. **Project's `build.tur`** — if the current project's manifest declares
   `:enable [...]`, that is the project owner's decision and Trowel is a
   viewer for it.
3. **User settings file** — a hand-editable turmeric file at
   `$XDG_CONFIG_HOME/turmeric/experiments.tur` (falls back to
   `QStandardPaths::AppConfigLocation` / turmeric). Applied only when the
   project has no `:enable` key.

There is no in-app checkbox UI for v1. A gear icon on the toolbar opens
the settings file in the OS default editor; the user edits it and
restarts the REPL. Trowel never enumerates flag names itself — the
turmeric registry is the source of truth (`tur experiments`) and any
listing UI would drift the moment a flag graduates or shelves.

## Why a `.tur` file, not TOML

The manifest already has an `:enable`/`:allow-experimental` vocabulary
for exactly this concept. Reusing it, in a file that lives outside any
particular project, buys:

- **One parser.** Turmeric's manifest reader already validates names
  against `EXPERIMENTS[]`, emits TUR-W0060/W0061 lifecycle warnings, and
  honors `expires_at`. A TOML file forces Trowel to reimplement each of
  those checks and keep them in sync as flags come and go.
- **One mental model.** A user who has read the `build.tur` docs already
  knows the syntax. "Same keys, no project context" is a smaller ask than
  "learn a second schema."
- **A tool-neutral file.** Placing it at `~/.config/turmeric/` (not
  `~/.config/turmeric/trowel/`) means the same file can eventually be
  honored by `tur check` in a scratch directory, the LSP, and the REPL
  when launched outside any project. Trowel is the first consumer, not
  the owner.

The Trowel-specific config (window layout, recent files, font, theme)
still lives under `~/.config/turmeric/trowel/` in whatever form Qt
prefers via `QSettings`. Experiments are compiler semantics, not editor
state — they belong on the turmeric side of the split.

## Settings file

```turmeric
;; ~/.config/turmeric/experiments.tur
;;
;; Names must match `tur experiments`. Unknown names surface a first-class
;; diagnostic from turmeric — Trowel does not validate them.

:enable [forall-kinds
         forall-constraints
         hkt-hrt
         forall-dict-pass]
:allow-experimental true
```

- Created on first open with the block above but with `:enable []` empty,
  and a header comment linking to `docs/guides/experimental-flags-guide.md`.
- Malformed file: log to the status bar, treat as "no user-level flags",
  do not crash. If turmeric's own reader is invoked (see below), its
  diagnostic is surfaced verbatim.

### Precedence with `build.tur`

`build.tur`'s `:enable` list wins outright when present — including an
explicit empty `:enable []`, which means "this project opted out." No
merging: a project that carries the manifest key has decided which
experiments its code needs, and silently unioning the user's set would
turn a green suite red for reasons the project owner never signed off
on.

## Wiring into the REPL  [shipped — preferred path]

Turmeric v0.62.0 reads `~/.config/turmeric/experiments.tur` and `build.tur`'s
`:enable` key itself. Trowel does not forward `--enable=` on the command line
— `tur repl` and `tur build` pick up the files on their own. Trowel's job is
limited to:

1. Creating and opening the user file (the gear icon / menu item).
2. Showing the resolved set in the REPL banner (read-only, for display).
3. The `experiments.status` control command (read-only, for testing).

`resolveExperiments(workingDir)` in `src/repl/experiment_flags.cpp` reads
`build.tur` and `experiments.tur` for display only. It returns
`{names, source, optedOut}`:

1. If a `build.tur` upward from `workingDir` has an `:enable` list,
   return it (possibly empty — an explicit `:enable []` means "opted out").
2. Else return the user file's `:enable`.
3. Else return `{}`.

The `build.tur` reader is a small scanner (skip `;` comments and
`#| ... |#` blocks, find `:enable` followed by `[`, collect symbol tokens
until `]`). The user file uses the same scanner. If either doesn't match
the trivial shape (e.g. computed at read time), fall through to the next
source — do not attempt to embed a full turmeric evaluator in Trowel.

`--allow-experimental` was retired in v0.42.2 — enabling an experiment (via
`--enable=<name>`, `build.tur`, or `experiments.tur`) is now the acknowledgment.
Trowel does not pass it.

Re-read on: REPL start/restart. Not on every keystroke.

Show the resolved set once in the REPL banner:

```
[trowel] tur repl started in ~/proj  (experiments: forall-kinds, hkt-hrt — from build.tur)
```

The `from build.tur` / `from user settings` suffix tells the user which
source won without them having to guess. When no experiments are set, the
banner says `(experiments: none)`.

## Toolbar: gear icon  [shipped]

The settings popup (the cog button at the bottom of the sidebar) now
includes "Experiment Flags…" alongside "Settings…" and "Turmeric
Settings…". The same action is also in the Edit menu as "Experiment
Flags…" (non-macOS) and in the app menu (macOS).

- `QAction* editExperimentsAction_` — tooltip "Experiment Flags…".
- Icon: reuses the existing `NF::Cog` settings button popup — no new
  icon needed.
- Handler (`openExperimentsFile()`):
    - Ensure `~/.config/turmeric/experiments.tur` exists (create with the
      commented stub if not).
    - `openPath(path)` — opens in Trowel's own editor (not the OS default,
      since Trowel is a text editor).
    - Status bar: `Editing <path> — restart the REPL to apply.`

## Non-goals for v1

- **No in-app checkbox list of known experiments.** The turmeric
  registry can change; letting `tur experiments` be the source of truth
  avoids Trowel drifting out of sync.
- **No live-apply.** Restart-to-apply is fine; experiments affect
  compiler behavior from process start.
- **No writing to `build.tur`.** Project-level flags belong to whoever
  owns the project; Trowel is read-only against `build.tur`.
- **No merging of project and user sets.** `build.tur` wins outright
  when present.

## Test plan  [shipped]

`tests/smoke/test_experiment_flags.py` covers:

- `experiments.status` reports empty when no source is set.
- User file with `:enable [reflected-measures]`: `experiments.status` reports
  the name and `source: "user settings"`.
- `build.tur` with `:enable [repl-jit-inline-c]` overrides the user file:
  `experiments.status` reports `source: "build.tur"`.
- `build.tur` with explicit empty `:enable []`: `experiments.status` reports
  `opted_out: true`, `source: "build.tur"`.
- REPL banner shows the experiment names and source.
- REPL banner shows `experiments: none` when no flags are set.
- Malformed `experiments.tur`: treated as no user-level flags, no crash.

The `experiments.status` control command reports `{names, source,
opted_out, file_path, working_dir}`, consumed by the smoke tests.

## Smoke test: Van Laarhoven lens  [deferred]

The plan's original smoke test used `forall-kinds, forall-constraints,
hkt-hrt, forall-dict-pass` — flags that existed in earlier turmeric
versions but are not present in v0.62.0 (which has `reflected-measures`
and `repl-jit-inline-c`). The test is deferred until a turmeric version
with flags that exercise a meaningful compile-time difference is bundled,
or until the VL lens fixture is updated to use available flags.

The skip condition from the original plan applies: skip if `tur
experiments` reports the required flags as unknown.
