# Engine selection — plan

> **Status:** Parts A–E shipped. The `TurInvocation` seam
> (`src/repl/tur_invocation.{h,cpp}`) routes all six `tur` call sites through
> `MakeTurInvocation`, which injects the engine as the `TUR_ENGINE` env var
> (not `--engine` argv, which `tur build` rejects). A `run/engine` setting in
> settings.json drives the injection; a `run.status` control command reports
> the resolved engine, binary, and env for smoke tests.
> **Related:** [`experiment-flags.md`](experiment-flags.md) (also not
> started; shares the same argv seam).

Turmeric v0.59.0 added `--engine` to `tur run` and the `TUR_ENGINE` env var to
both `tur run` and `tur build`. The bundled binary is v0.62.0, so all three
engines (`cc`, `jit`, `interp`) are available. `tur build` does NOT accept
`--engine` on argv (exit 2, usage error) but DOES honor `TUR_ENGINE` in the
environment. The setting is therefore injected as an env var, not an argv
flag.

Let the user pick which Turmeric execution engine Trowel uses when it runs a
project — the C emitter (`cc`), the MIR JIT, or the tree-walking interpreter.

## Context: what this can and cannot cover

The obvious design — a dropdown that switches "the engine Trowel uses" — does
not survive contact with how Trowel actually talks to `tur`.

**The REPL is always tree-walked.** `tur repl` sets `g_interpret_mode = true`
unconditionally (`turmeric/src/main.c:6989`). With `--enable=jit` on a
`-DTUR_JIT=ON` build, only *spice loading* goes through the engine; the REPL's
own expression evaluation stays tree-walked. So an engine setting applied to
the REPL would change nothing observable.

**"Evaluate" is not a `tur` invocation at all.** `run_buffer.cpp` never spawns
a process — `RunBuffer` (`run_buffer.cpp:109`) and `RunRange` (`:144`) write a
scratch file and type `(load "<path>")` into the *already-running* REPL pty
(`:65`, `:130`), after a `:reset` (`:43`). Evaluate therefore inherits the
REPL's engine by construction, and no per-eval choice is possible without a
`tur` REPL meta-command that does not exist.

**That leaves one surface: Build Project / Run.** `ProjectRunner` is the only
place Trowel spawns a fresh `tur` to execute user code
(`project_runner.cpp:86`, `{"build", projectDir}`). It is the only invocation an
engine setting can meaningfully reach.

Scoping the feature to that is not a compromise — it is the honest extent of
what the setting can do. A dropdown labelled as though it governs the REPL
would be a lie in the UI.

## Goals

- One user-visible setting choosing the engine for **Build Project / Run**.
- The choice is *offered* only when the bundled `tur` can actually satisfy it.
- The resolved engine is visible, not inferred — the user can tell which one
  ran without reading the manifest.
- A single seam through which `tur` argv is constructed, so this and
  `docs/plans/experiment-flags.md` stop being two independent argv rewrites.

## Non-goals

- Changing the REPL's or evaluate's engine (see Context — not possible today).
- Applying the engine to `tur lsp` or `tur format`. Neither executes user code;
  `lsp/serverPath` can even point at a *different binary* entirely
  (`lsp_manager.cpp:54`).
- Overriding a project's `build.tur` `:engine` by default. The manifest is the
  project owner's decision; Trowel is a viewer for it unless the user
  explicitly overrides. Same posture `experiment-flags.md` takes for
  `:experiments`.
- Per-buffer or per-tab engine state.

## Context: how Trowel invokes `tur` today

There is one shared resolver for *locating* `tur` and **four independent call
sites** that each build their own argv. No shared argv helper exists.

`ResolveTurBinary()` — `repl_session.cpp:64-70`, declared `repl_session.h:11-15`:

```cpp
QString ResolveTurBinary() {
    const QString override = QSettings().value("repl/turBinary").toString();
    QString resolved = ifExecutable(override);
    if (resolved.isEmpty()) resolved = ifExecutable(bundledTurPath());
    if (resolved.isEmpty()) resolved = QStandardPaths::findExecutable("tur");
    return resolved;
}
```

Callers: `project_runner.cpp:49`, `lsp_manager.cpp:61`, `main_window.cpp:1117`.

| # | Call site | argv today | Env |
|---|---|---|---|
| A | REPL — `repl_session.cpp:138` | `{"repl"}` | `TUR_STDLIB_DIR` (`:131-136`) |
| B | Build Project — `project_runner.cpp:86` | `{"build", projectDir}` | `TUR_STDLIB_DIR` (`:65-71`) |
| C | LSP — `lsp_manager.cpp:136` | `{"lsp"}` | `TUR_STDLIB_DIR` (`:129-134`) |
| D | Format — `main_window.cpp:1123` | `{"format"}` | **none** |

Two warts this plan should clean up rather than route around:

- **Binary resolution is duplicated.** `ReplSession::start` does *not* call
  `ResolveTurBinary()`; it re-implements the same three-step search inline at
  `repl_session.cpp:105-122` so it can print a "tried these three things"
  banner.
- **Stdlib pinning is triplicated** in three mutually incompatible spellings
  (env-string list at A, `QProcessEnvironment` at B, another list at C, nothing
  at D).

`docs/plans/experiment-flags.md:93-101` already sketches the fix for a
different flag:

```cpp
QStringList args{"repl"};
const ResolvedExperiments r = resolveExperiments(workingDir);
if (!r.names.isEmpty()) {
    args << QString("--enable=%1").arg(r.names.join(","));
```

**That plan is unimplemented** — `grep -rn 'resolveExperiments' src` returns
nothing. Both features want the same seam. Build it once, here.

## Part A — the `TurInvocation` seam  [shipped]

Independent of Turmeric; worth landing on its own merits.

```cpp
// src/repl/tur_invocation.h
struct TurInvocation {
    QString     binary;      // resolved, or empty with `error` set
    QStringList args;
    QProcessEnvironment env; // one spelling of TUR_STDLIB_DIR, finally
    QString     error;       // human-readable "tried these three things"
};

// `subcommand` is the leading argv (e.g. {"build", dir}); global flags are
// appended by the resolver so every call site gets them consistently.
TurInvocation MakeTurInvocation(const QStringList& subcommand,
                                const QString& workingDir = {});
```

1. Move the three-step search into one function; have `ReplSession::start`
   consume the `error` string for its banner instead of re-searching.
2. Fold the three stdlib-pinning copies into `MakeTurInvocation`. Site D gains
   the pinning it currently lacks — verify separately whether `tur format`
   needs it, and if not, say so in a comment rather than leaving it accidental.
3. Route all four sites through it. Each becomes one line.

No behavior change. This is the commit that should be reviewable on its own.

## Part B — the setting  [shipped]

Follows the `commitTurmericPath()` pattern: changes what gets executed, emits
no signal, takes effect on the next process start. The setting lives in
settings.json (not QSettings) as `run.engine`, read by `Settings::runEngine()`
and written by `Settings::setRunEngine()`.

But the closer analogue is `commitTurmericPath()`
(`preferences_view.cpp:83-92`): it changes what gets executed, emits **no**
signal, and takes effect on the next process start. An engine setting is the
same shape — there is nothing live to fan out to.

- **Key:** `run.engine` in settings.json, values `"default" | "cc" | "jit" | "interp"`.
- **`"default"` is the default and passes no `TUR_ENGINE` env var**, letting
  the project's `build.tur` `:engine` decide. Anything else overrides via
  `TUR_ENGINE`.
- **Stored as a string** (combo item data), never the combo index.
- Added to `restoreDefaults()` — resets to `"default"`, combo index 0.
- **Injection is via `TUR_ENGINE` env var**, not `--engine` argv. `tur build`
  rejects `--engine` on the command line (exit 2, usage error) but honors
  `TUR_ENGINE` in the environment. `MakeTurInvocation` reads the setting and
  inserts `TUR_ENGINE` into `inv.env` when the value is not `"default"`.

### First enum setting — QComboBox

This is the app's first `QComboBox`. The hand-rolled stylesheet in
`preferences_view.cpp` now includes `QComboBox` and `QComboBox
QAbstractItemView` rules matching the dark theme. The combo stores the engine
string as item data (not the index) so reordering or filtering by Part C
cannot break the mapping.

## Part C — capability detection  [deferred]

The dropdown **cannot be a static list of three.** Whether the bundled `tur`
can JIT is a property of how that binary was compiled: `-DTUR_JIT=ON` vendors
MIR at configure time, and a default build carries no engine at all. Offering
`jit` against such a binary produces a hard error at Run time, which is a
terrible way to learn.

Trowel therefore needs to ask the binary what it supports. Turmeric has no
machine-readable capability output today (`tur experiments` is
human-formatted), so **this part needs a small upstream addition** — request it
alongside the `--engine` flag. Until then, the honest fallbacks are, in order
of preference:

1. Query a capability report (`tur --capabilities --json`, or equivalent) once
   per resolved binary, cache it against the binary's path + mtime.
2. Offer all three, and surface the failure clearly when Run fails — inferior,
   because the error arrives late and looks like a Trowel bug.

Do not parse `tur experiments` output for this. It is formatted for humans, it
answers the wrong question (experiment enablement, not engine presence), and it
will drift when the JIT graduates.

## Part D — making the choice visible  [shipped]

Two surfaces, both cheap:

- **Build output.** `ProjectRunner` already echoes `tur` output into the
  terminal. The resolved engine belongs in the leading line, so the transcript
  records which engine produced the result.
- **The REPL banner is *not* the right place** — it would imply the setting
  governs the REPL, which it does not (see Context). Resist this; it is the
  obvious move and it is wrong here. `experiment-flags.md:122-129` proposes the
  banner for *its* feature, where it is correct because experiments do affect
  the REPL.

## Part E — control API and smoke tests  [shipped]

There is **no generic settings command** in the control API — the dispatch
table (`control_handlers.cpp:848-897`) has `ping`, `window.*`, `menu.invoke`,
`editor.*`, `lsp.*`, `repl.*`, `run.*`, `wait.*`, and `grep -rn QSettings
src/control/` returns zero hits.

The precedent for exposing a setting is to fold it into a domain status
command — `HandleLspStatus` (`control_handlers.cpp:447-455`) reports
`{state, error, server_path, enabled}`, consumed as a skip-guard in
`tests/smoke/test_lsp.py:22-27`.

**Do the same for Run:** a `run.status` reporting
`{"engine": "...", "tur_engine": "...", "argv": [...], "binary": "..."}`.
`tur_engine` is the `TUR_ENGINE` env var value (empty when `"default"`), which
is what proves the setting reached the process env — the assertion that would
actually catch a regression in Part A's seam. Tests write `run.engine` to
settings.json (via `Session.settings_json`) before launch, not the QSettings
INI.

Tests seed settings two ways, both already in place:

- Per-test QSettings isolation via `TROWEL_SETTINGS_DIR`
  (`tests/smoke/conftest.py:107-111`, honored at `src/main.cpp:45-53`, which
  flips QSettings to `IniFormat`).
- Writing the INI directly before launch — `Session.settings_ini`
  (`conftest.py:208-210`), as `test_session_restore.py:64-70` does.

The smoke tests (`tests/smoke/test_engine_selection.py`) exercise the
**setting** (writing `run.engine` to settings.json before launch and asserting
`run.status`), not the **dropdown widget** — `menu.invoke` can open the
Preferences tab but there is no command to manipulate widgets inside it.

## Files to add / change

**Add**
- `src/repl/tur_invocation.h` / `.cpp`
- `tests/smoke/test_engine_selection.py`

**Change**
- `src/repl/repl_session.h` / `.cpp` — move resolution into the seam; drop the
  inline duplicate at `:105-122`, keeping the banner text.
- `src/repl/project_runner.cpp` — route through the seam; engine shown in banner.
- `src/lsp/lsp_manager.cpp`, `src/app/main_window.cpp` (`formatFile`) — route
  through the seam; **no** engine flag.
- `src/app/preferences_view.h` / `.cpp` — the combo, `commitEngine()`,
  `restoreDefaults()`, a `QComboBox` stylesheet rule.
- `src/app/settings.h` / `.cpp` — `runEngine()` getter, `setRunEngine()` setter.
- `src/control/control_handlers.cpp` — `run.status`.
- `CMakeLists.txt` — add `tur_invocation.cpp` / `.h` to `trowel_lib`.

## Risks

- **Shipping Part B before Turmeric lands `--engine`.** Resolved: `tur run --engine` and `TUR_ENGINE` env var shipped in v0.59.0; the bundled binary is v0.62.0.
- **The setting silently overriding `build.tur`.** Mitigated by defaulting to
  `"default"` and passing no flag; easy to regress by "helpfully" defaulting to
  `cc`.
- **Offering an engine the binary lacks.** Part C exists for this. Without it
  the feature actively misleads on any default-built `tur`.
- **Scope creep back toward the REPL.** Every reviewer will ask why the
  dropdown does not affect the REPL. The answer is in Context; link it from the
  preference's tooltip so the question is answered in the UI, not just the doc.
- **`experiment-flags.md` drifting from this seam.** Two plans now depend on
  `MakeTurInvocation`. Whichever lands second must not fork it — and note that
  `experiment-flags.md` names the manifest key `:enable`, while the real key is
  `:experiments`; that doc needs a correction pass independent of this work.
- **Per-window settings.** `applyX()` fan-out iterates `this->buffers_` only
  (`main_window.cpp:1304-1310`); there is no cross-window broadcast.
  `WindowManager::notifyWindowsChanged()` (`window_manager.h:71-72`) exists but
  only relists titles/menus. Harmless while the setting is read at spawn time —
  a trap if anyone later makes it live.

## Verification

1. `just build` — clean under `-Wall -Wextra -Wpedantic -Werror`.
2. `just smoke` — existing suite must not regress. New
   `tests/smoke/test_engine_selection.py`:
   - write `run.engine=interp` to settings.json, launch, assert `run.status`
     reports `engine: "interp"` **and** `tur_engine: "interp"`;
   - default (`"default"`) produces `tur_engine: ""`, proving the manifest
     is not overridden;
   - all three engines (`cc`, `jit`, `interp`) are tested.
3. Part A regression check — all four call sites still work after the seam
   lands: REPL starts, Build Project runs, LSP connects, Format File formats.
   This is the change most likely to break something silently, and the smoke
   suite covers all four (`test_repl.py`, `test_run_buffer.py`, `test_lsp.py`,
   `test_startup.py`).
4. `just run` and drive it by hand: switch the engine with a project open,
   Build Project, confirm the transcript names the engine that ran and the
   result matches.
