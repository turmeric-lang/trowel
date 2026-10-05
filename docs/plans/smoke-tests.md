# Trowel Smoke Test Suite — Plan

> **Status:** Complete. `tests/smoke/conftest.py` and 29 test files
> covering startup, editor, REPL, run buffer, LSP, debugger,
> multi-window, drag-and-drop, folding, find, format, dialects, and
> more. The suite runs on macOS and Linux via `just smoke`.
> **Related:** [`socket-api.md`](socket-api.md) (complete, the control
> socket the suite drives).

An end-to-end smoke suite that drives a real Trowel process through the
control socket ([socket-api.md](socket-api.md)) and asserts on observable
state. Complements the Catch2 unit tests in `PLAN.md §10` — these tests
launch the actual GUI application under `QT_QPA_PLATFORM=offscreen`, spawn
a real `tur repl`, and verify that user-visible behavior works.

Smoke tests are cheap (single binary launch, ~2–5s per test) but broad:
they catch regressions that unit tests miss because the failure mode lives
at a widget/PTY/process boundary rather than inside a single class.

---

## 1. Goals

- Every test exercises the same paths a user would — no whitebox hooks,
  no direct method calls on internal classes. Everything flows through the
  control socket.
- Deterministic: no `sleep`. Every wait uses `wait.repl_output` /
  `wait.repl_idle` / `wait.editor_signal` with an explicit timeout.
- Fast enough to run on every PR: <60s wall for the whole suite locally.
- Runs on macOS (v1) and Linux (v1.1) without conditional code paths in
  test bodies — differences hidden in the harness.
- Failure output is diagnostic: on assertion failure, dump `state.dump`
  and the last 40 lines of the terminal into the test log automatically.

### Non-goals (v1)
- Pixel/screenshot comparison.
- Load/performance testing.
- Testing turmeric itself — we assume `tur repl` is well-behaved and skip
  tests that would fail if it isn't (with a clear skip reason).

---

## 2. Harness

- **Language:** Python 3.11+ with `pytest`. Chosen over C++ because the
  tests are I/O and orchestration, not tight loops; iteration speed matters.
- **Location:** `tests/smoke/`. Not built by CMake; run with
  `just smoke` / `pytest tests/smoke -q`.
- **Fixtures** (in `tests/smoke/conftest.py`):
  - `trowel` — session-scoped. Builds if needed, then spawns the app with
    `--control-socket`, waits for the socket path on stdout, yields a
    connected client. Kills on teardown.
  - `fresh_trowel` — function-scoped. Same, but a new process per test.
    Used for tests that mutate persistent settings (`QSettings`) or leave
    the REPL wedged on purpose.
  - `editor` — helper wrapping `trowel` with editor-focused verbs
    (`type`, `press`, `expect_text`, `expect_cursor`).
  - `repl` — same for the REPL pane (`send`, `expect_output`, `idle`).
  - `fixture_files` — points at `tests/smoke/fixtures/` containing
    `hello.tur`, `defs.tur`, `syntax_error.tur`, `sweet_hello.tur.sweet`.
- **Env:** `QT_QPA_PLATFORM=offscreen`, `TROWEL_CONTROL_SOCKET=1`,
  isolated `HOME`/`XDG_CONFIG_HOME` per test so `QSettings` writes are
  scoped. `PATH` prepended with a directory containing a real `tur` (or a
  minimal fake — see §5).

---

## 3. Test cases

Organized by subsystem. Each bullet is a discrete test function. Tests are
independent; ordering-dependent behavior is a bug in the harness.

### 3.1 Startup & window
- `test_launches_with_empty_buffer` — window opens, editor is empty, REPL
  child is running (`repl.is_running.running == True`).
- `test_repl_prompt_appears_on_launch` — `wait.repl_output "> "` inside
  3s.
- `test_focus_toggle` — `window.focus editor` then `terminal`; verify
  keystrokes land in the right pane.
- `test_splitter_resize_persists` — set splitter, restart with the
  `fresh_trowel` fixture using the same `HOME`, verify position restored.

### 3.2 Editor — text & cursor
- `test_type_inserts_text` — `editor.type "hello"`, `get_text == "hello"`,
  cursor at col 5.
- `test_arrow_navigation` — insert `abc\ndef`, press `Up`, `Home`,
  `Right`; assert cursor at (0,1).
- `test_home_end` — `Home` goes to col 0, second `Home` toggles to first
  non-whitespace (SciTE behavior — verify Trowel matches; if it doesn't,
  the test tells us to decide explicitly).
- `test_selection_by_shift_arrow` — type `hello world`, shift-select last
  5 chars, `editor.get_selection.text == "world"`.
- `test_backspace_and_delete` — checks both keys against a mixed-line
  buffer.
- `test_undo_redo` — type, ⌘Z, ⌘⇧Z; buffer round-trips.
- `test_multiline_indent` — press Enter at end of `(defn foo ()`; the
  next line's cursor column matches Scintilla's auto-indent for turmeric
  (verify what it *is*, don't prescribe).
- `test_bracket_matching_highlight` — position at `(`, verify Scintilla
  reports a matched brace at the closing `)` via `editor.get_cursor`
  (needs a small extension to the API — worth adding).

### 3.3 Editor — file I/O
- `test_open_file` — `editor.open fixtures/hello.tur`; text matches disk.
- `test_save_marks_clean` — type, save to tmp path, `modified == False`
  after a `wait.editor_signal modifiedChanged`.
- `test_save_as_updates_path` — save-as, `filePath` changes, window title
  reflects it (fetch via `window.geometry` + title — extend API).
- `test_reopen_recent` — open, close, open via Recent Files menu path.

### 3.4 REPL interaction
- `test_repl_echoes_input` — `repl.send "(+ 1 2)"`,
  `wait.repl_output "3"` within 2s.
- `test_repl_multiline_paste` — send a multi-line form; verify it
  evaluates as one expression, not line-by-line.
- `test_repl_arrow_up_history` — send `(+ 1 1)`, wait for result, send
  Up-arrow via `repl.press`, verify last line of terminal shows
  `(+ 1 1)` reconstituted from history.
- `test_repl_ctrl_c_interrupts` — send an infinite loop, `repl.press
  {key:"c", mods:["ctrl"]}`, expect prompt back within 1s.
- `test_repl_restart` — call `repl.restart`, expect `stopped` signal then
  a fresh prompt; `pid` changes.
- `test_repl_survives_syntax_error` — send garbage, expect an error line,
  expect prompt back, next real expression evaluates fine.

### 3.5 Run buffer (turmeric integration)
This is the core edit → run → inspect loop from `PLAN.md §7.2`. If any of
these fail, the product doesn't work.

- `test_run_buffer_loads_definitions` — open `fixtures/defs.tur`
  containing `(def x 42)`, `run.buffer`, `wait.repl_idle`, then
  `repl.send "x"` → expect `42` in the output.
- `test_run_buffer_saves_dirty_untitled_to_scratch` — type into an empty
  buffer, `run.buffer`, verify the scratch file exists under the cache
  dir and `(load ...)` referenced it (grep terminal output for the path).
- `test_run_selection` — select one form out of a multi-form buffer,
  `run.selection`; only that form's bindings become defined.
- `test_run_syntax_error_shows_diagnostic` — buffer with a broken paren;
  Run; terminal shows a turmeric parse/error diagnostic, prompt returns,
  editor is unchanged.
- `test_run_sweet_buffer` — `fixtures/sweet_hello.tur.sweet` loads and
  its top-level binding is visible from the REPL. (Verify the sweet
  variant path in `run_buffer.cpp` actually fires.)

### 3.6 Menu & shortcuts
- `test_run_menu_action` — `menu.invoke ["Run", "Run Buffer"]` is
  equivalent to `run.buffer`.
- `test_cmd_r_shortcut` — `editor.press {key:"r", mods:["meta"]}` fires
  Run.
- `test_disabled_action_errors` — Save when clean returns
  `error.code == "action_disabled"` (nice, unambiguous failure mode).

### 3.7 Lexer / theme (light touch)
Full lexer testing lives in Catch2. Smoke-level checks that the styling
pipeline is wired up at all:
- `test_lexer_registered` — after open, request a small extension to the
  API: `editor.get_style_at {pos}` → non-default style number for a
  keyword like `def`. If the number is 0 for every token, the lexer
  wasn't loaded.
- `test_theme_applied` — same helper returning the foreground color for
  a keyword; assert it matches the ported dark-theme palette entry.

### 3.8 Shutdown
- `test_clean_quit_kills_repl` — send a `menu.invoke ["File","Quit"]`,
  verify the `tur` PID no longer exists within 2s (SIGTERM grace period
  per `PLAN.md §7.1`).
- `test_unsaved_prompt_on_quit` — modified buffer + Quit → dialog
  suppressed by an env flag (`TROWEL_TEST_SILENT_DIALOGS=1`) and the
  discard path taken; assert the pending save signal never fires.

---

## 4. Determinism playbook

Rules for keeping the suite from becoming flaky:

- **Never `sleep`.** Every wait is a `wait.*` call with an explicit
  timeout. Default 3s; raise per-test for known-slow ones with a comment
  explaining why.
- **Always `wait.repl_idle` after `run.buffer`.** `(load ...)` can print
  interleaved output; asserting on terminal state before it settles is
  the #1 flake source.
- **Isolated `HOME` per test.** No shared `QSettings` state.
- **Fresh REPL for tests that depend on empty environment.** Cheap; use
  `fresh_trowel`.
- **No timing-based assertions.** If a test would say "under 100ms", it
  belongs in a benchmark, not the smoke suite.

On assertion failure, the harness automatically:
1. Calls `state.dump` and writes it to `tests/smoke/artifacts/<test>.json`.
2. Snapshots the terminal (last 200 lines) alongside.
3. If the app died, includes its stderr and exit code.

---

## 5. `tur` binary in CI

The suite needs a working `tur` on PATH. Options:

1. **Prebuilt.** Publish a `tur` binary from the turmeric repo's release
   pipeline; download it in CI. Preferred — tests exercise the real
   thing.
2. **Fake tur.** A ~50-line Python script pretending to be a REPL:
   prints `> `, evaluates a whitelist of forms (`(+ ...)`,
   `(def name val)`, `x` → prints last def), acknowledges `(load ...)`.
   Fast, self-contained, but drifts from real turmeric semantics.

Ship with (1) as the default and (2) as `--fake-tur` for offline runs and
the "editor-only" subset of tests (§3.1–3.3 don't require a real `tur`).

---

## 6. CI integration

- New Just target: `just smoke`.
- New CMake preset already tested `Release`; the smoke job uses it.
- GitHub Actions matrix: `{macos-latest, ubuntu-latest}` × `{Release}`.
  Linux run stays advisory until v1.1 (§9 in `PLAN.md`).
- Artifacts: on failure, upload `tests/smoke/artifacts/`.

---

## 7. Milestones

**T0 — Harness + first test (0.5 day).** conftest, one test:
`test_launches_with_empty_buffer`. Depends on socket-api §S0.

**T1 — Editor tests (1 day).** §3.1–3.3. Depends on socket-api §S1.

**T2 — REPL & run tests (1 day).** §3.4–3.5. Depends on socket-api §S2/S3.

**T3 — Menu/shortcut/shutdown (0.5 day).** §3.6, §3.8.

**T4 — Lexer/theme probes + minor API extensions (0.5 day).** §3.7 plus
the small `editor.get_style_at` and title-in-geometry additions called
out inline above. Land these back in socket-api.md as they're added.

**T5 — CI wiring (0.5 day).** Just target, GH Actions job, artifact
upload.

Total: ~4 days after the socket API's S0–S3 land.

---

## 8. Future directions

- **Golden terminal snapshots.** Record the terminal buffer after a
  known sequence, diff on regression. Needs a stable `tur` version pin.
- **Screenshot diffing** (once socket API grows a screenshot verb).
- **Fuzz-style typing tests.** Random ASCII into the editor for N
  seconds; assert app doesn't crash and undo restores. Catches escape /
  encoding regressions in the editor.type synth-input path.
- **Cross-platform matrix.** Linux moves from advisory to required at
  v1.1 (`PLAN.md §9`).
- **Reuse for demos.** The NDJSON transcript of a passing test is a
  demo script — replay it against a real Trowel for a screencast.
