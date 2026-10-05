# Experiment flags — plan

> **Status:** Not started. No `resolveExperiments` or experiment-flag
> handling anywhere in the codebase. The plan's interim path (Trowel
> reads `experiments.tur` and forwards `--enable=`) is unimplemented;
> the preferred path (turmeric reads the user file itself) has not been
> filed upstream.
> **Related:** [`engine-selection.md`](engine-selection.md) (also not
> started; shares the same `MakeTurInvocation` seam).

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

## Wiring into the REPL

Two paths, depending on how far turmeric has come:

**Preferred, once turmeric reads the user file itself.** File an issue /
plan against turmeric for a `XF_SRC_USER_CONFIG` source tag next to
`XF_SRC_CLI` / `XF_SRC_MANIFEST` / `XF_SRC_WEB`, with the CLI, LSP, and
REPL entry points reading `$XDG_CONFIG_HOME/turmeric/experiments.tur`
before applying any project-level `:enable`. Trowel then does nothing
special — `tur repl` picks up the file on its own. Trowel's job shrinks
to the gear icon and the banner (below).

**Interim, until that lands.** Trowel reads the file directly and
forwards it:

```cpp
QStringList args{"repl"};
const ResolvedExperiments r = resolveExperiments(workingDir);
if (!r.names.isEmpty()) {
    args << QString("--enable=%1").arg(r.names.join(","));
    if (r.allowExperimental) args << "--allow-experimental";
}
pty_->start(resolved, args, workingDir);
```

`resolveExperiments(workingDir)` returns `{names, allowExperimental,
source}`:

1. If a `build.tur` upward from `workingDir` has an `:enable` list,
   return it (possibly empty).
2. Else return the user file's `:enable`.
3. Else return `{}`.

The `build.tur` reader is a small scanner (skip `;` comments and
`#| ... |#` blocks, find `:enable` followed by `[`, collect symbol tokens
until `]`). The user file uses the same scanner. If either doesn't match
the trivial shape (e.g. computed at read time), fall through to the next
source and log once — do not attempt to embed a full turmeric evaluator
in Trowel.

Re-read on: REPL start/restart, and on save of `build.tur` or
`experiments.tur` from within Trowel. Not on every keystroke.

Show the resolved set once in the REPL banner:

```
[trowel] tur repl started (experiments: forall-kinds, hkt-hrt — from build.tur)
```

The `from build.tur` / `from user settings` / `from CLI override` suffix
tells the user which source won without them having to guess.

## Toolbar: gear icon

Extend `MainWindow::setupToolBar` (see `src/app/main_window.cpp:136`)
with a new action after the two run actions, separated by
`addSeparator()`:

- `QAction* editExperimentsAction_` — tooltip
  **"Edit experiment flags…"**.
- Icon: `NerdIcon(NF::Cog, glyphSize, iconColor)`. Add
  `constexpr char32_t Cog = 0xF0493; // nf-md-cog` to
  `src/app/icon_font.h` alongside `Play` / `PlaylistPlay`; verify the
  codepoint against the Nerd Font cheat sheet at implementation time.
- Handler:
    - Ensure `~/.config/turmeric/experiments.tur` exists (create with the
      commented stub above if not).
    - `QDesktopServices::openUrl(QUrl::fromLocalFile(path))` — opens in
      the OS default text editor. No in-app editor, no modal.
    - Status bar: `Editing <path> — restart the REPL to apply.`

Also expose the same action under **Run → Experiment flags…** so
keyboard-only users can find it without hunting the toolbar.

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

## Test plan

- REPL banner shows `experiments: none` when neither source is set.
- User file with `:enable [foo]`, no `build.tur`: `tur repl` receives
  `--enable=foo --allow-experimental`; banner says `from user settings`.
- `build.tur` with `:enable [bar]`, user file with `[foo]`:
  `--enable=bar`, banner says `from build.tur`.
- `build.tur` with `:enable []` (explicit empty): no `--enable` flag,
  banner says `from build.tur` (project explicitly opted out).
- Malformed `experiments.tur`: status bar warning, no crash, empty set.
- Gear icon opens the file; if it didn't exist, a stub is created first.

## Smoke test: Van Laarhoven lens compiles under user-configured flags

Adds one test to the smoke suite (`tests/smoke/`, see
[smoke-tests.md](smoke-tests.md)) that proves the end-to-end path: user
edits `experiments.tur`, restarts the REPL, evaluates a VL lens example,
and turmeric accepts it. If the flag plumbing regresses, this test fails
at the type checker before ever producing output.

Fixture: `tests/smoke/fixtures/van_laarhoven_lens.tur`. Copy the
concrete form from turmeric's own suite
(`tests/fixtures/van-laarhoven-lens-concrete/input.tur`) — it prints
`3\n30\n4\n99`. The concrete form is deliberate; the generic form has
extra inference caveats that aren't what this test is exercising.

Flags required, taken verbatim from that fixture's `flags` file:

```
forall-kinds, forall-constraints, hkt-hrt, forall-dict-pass
```

`--allow-experimental` is set from the file's own `:allow-experimental
true` and forwarded automatically; the user does not repeat it on the
command line.

### Test body (`tests/smoke/test_experiment_flags.py`)

```python
def test_van_laarhoven_lens_compiles(fresh_trowel, fixture_files, tmp_path):
    cfg = tmp_path / "turmeric" / "experiments.tur"
    cfg.parent.mkdir(parents=True)
    cfg.write_text(
        ":enable [forall-kinds forall-constraints"
        " hkt-hrt forall-dict-pass]\n"
        ":allow-experimental true\n"
    )
    # fresh_trowel already sets XDG_CONFIG_HOME=tmp_path per test.

    t = fresh_trowel()
    t.file.open(str(fixture_files / "van_laarhoven_lens.tur"))
    t.repl.restart()  # picks up the new settings file
    banner = t.repl.expect_output("tur repl started", timeout=10)
    assert "forall-kinds" in banner
    assert "from user settings" in banner

    t.run_buffer()
    out = t.repl.expect_output("99", timeout=15)
    assert "3\n30\n4\n99" in out
    # Negative check: any TUR-E00xx line = type-check failure = flags
    # didn't reach the compiler.
    assert "TUR-E0" not in out
```

### Companion test: `build.tur` overrides the user file

Same fixture, but drop a `build.tur` next to it with `:enable []`
(explicit empty). Assert the REPL banner says
`experiments: none (from build.tur)` and that running the buffer now
fails with a `TUR-E` diagnostic — proving the project override actually
suppressed the user-level flags rather than silently merging.

### Skip conditions

- Skip (don't fail) if the installed `tur` reports any of these flags as
  unknown via `tur experiments`. This decouples the smoke suite from
  turmeric's flag graduation cadence — when `forall-kinds` becomes
  stable and the flag is retired, the test skips with a clear reason
  instead of turning red across every PR until someone updates it.
  Emit the skip reason: `flags no longer present in tur experiments:
  <names>`.
