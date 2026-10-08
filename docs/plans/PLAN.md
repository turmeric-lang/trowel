# Trowel — Project Plan

> **Status:** v1 shipped (current release v0.3.2). All milestones M0–M5
> landed, plus substantial post-v1 work: multi-window, tab bar, directory
> view, LSP support + navigation, debugger support, multi-language
> highlighting, folding, find/replace, word wrap, per-document state,
> dialects (Saffron, R7RS, sweet), and a 29-test smoke suite. macOS, Linux,
> and Windows all build and run. Bundled Turmeric pin is v0.62.0.
> Remaining plans not started:
> [`plugin-isolation.md`](plugin-isolation.md),
> [`docs-pane.md`](docs-pane.md),
> [`examples-menu.md`](examples-menu.md).
> Partially open: [`minimap.md`](minimap.md) (phases 1–3 shipped, phase 4
> polish not started), [`lsp-navigation.md`](lsp-navigation.md) (T0–T3
> shipped, Cmd/Ctrl-click to definition deferred).
> Shipped: [`engine-selection.md`](engine-selection.md) (Parts A–E all
> shipped — `TurInvocation` seam, `run.engine` setting, `TUR_ENGINE` env
> injection, build banner, `run.status` command, smoke tests),
> [`experiment-flags.md`](experiment-flags.md) (`resolveExperiments`
> scanner, `experiments.tur` creation/open, REPL banner,
> `experiments.status` command, smoke tests).

Trowel is a C/C++ text editor built with CMake that embeds Scintilla and a
terminal emulator in draggable split panes. It is purpose-built for
[turmeric](https://github.com/turmeric-lang/turmeric): the editor speaks to a
persistent `tur repl` / `tur interpret` session so users can "run" a buffer and
then poke at the resulting environment from the REPL — the same
edit → run → inspect loop that makes Processing and DrRacket productive.

An earlier version of Trowel lived inside the turmeric repo and was removed
(commits `"Remove trowel for now"`, `"Trowel newline issue"`). This project
resurrects the idea as a standalone repo; the git history in turmeric is worth
a look during M0 for prior-art clues but is not authoritative.

---

## 1. Goals

- Fast, native desktop editor for turmeric source: `.tur` (turmeric syntax)
  and `.tur.sweet` (sweet-expression syntax).
- Editor pane driven by Scintilla with a turmeric-aware lexer covering the full
  surface syntax (see §6.2) and the turmeric dark color scheme applied by
  default.
- Terminal pane hosting a live `tur repl` or `tur interpret` process; the
  terminal is a full VT100-ish emulator, not a line-oriented log widget.
- One command ("Run") loads the current buffer into the running REPL via
  turmeric's `(load ...)` directive so that any top-level definitions become
  interactively inspectable.
- Draggable splitter between editor and REPL, persistable across sessions.

### Non-goals (v1)
- Multi-file projects, tabs, project drawer.
- AOT compilation via `tur` (interp only in v1).
- Debugger, step execution, breakpoints — deferred even though `tur dap`
  exists upstream.
- LSP-driven autocomplete and diagnostics — deferred even though `tur lsp`
  exists upstream.
- Plugin system.

### Platform priorities
- **v1:** macOS (primary — where the maintainer works).
- **Near-term future:** Linux. Must not be architecturally precluded. Toolkit,
  build system, and platform-specific code paths are chosen with this in mind.
- **Deferred indefinitely:** Windows. Not supported, not tested, not designed
  for. Sole maintainer — no second contributor to spread platform surface
  across.

---

## 2. High-level architecture

```
+----------------------------------------------------+
|  Trowel main window (Qt6 shell — see §3)           |
|  +------------------+  +------------------------+  |
|  |                  |  |                        |  |
|  |  Scintilla view  |  |  Terminal (QTermWidget)|  |
|  |  (editor buffer) |  |   `tur repl` on a PTY  |  |
|  |                  |  |                        |  |
|  +------------------+  +------------------------+  |
|         ^                     ^                    |
|         |                     |                    |
|         +-------- Run --------+                    |
|                                                    |
+----------------------------------------------------+
```

Three cooperating subsystems:

1. **Editor** — Scintilla widget + turmeric lexer + turmeric-dark theme
   loader.
2. **REPL host** — long-lived `tur repl` child process attached to a PTY;
   the terminal emulator renders its output and forwards keystrokes.
3. **Shell** — window / menu / splitter framework tying the two together and
   owning the "Run buffer" command.

---

## 3. Toolkit choice: Qt6

Chosen because it is the only mainstream option that runs first-class on both
macOS (v1) and Linux (near-term future) with the same code path — the platform
constraint from §1 rules out anything else.

Components:
- **Qt6 Widgets** — window, splitter (`QSplitter`), menus, file dialogs,
  settings persistence (`QSettings`).
- **ScintillaEdit** — the official Qt binding to Scintilla; fetched via
  `FetchContent` and built as a static library.
- **QTermWidget** — Konsole's terminal widget, extracted as a standalone
  library. Handles PTY, xterm compatibility, colors, scrollback, selection.
  Available on Homebrew (macOS) and every Linux distro.

Rejected alternatives:
- **GTK3 + VTE:** VTE has no viable macOS story; would force a full toolkit
  rewrite to reach Linux parity later, or vice versa.
- **Native Cocoa (macOS-only):** would preclude Linux entirely — violates §1.
- **Electron / web-based:** wrong end of the perf and native-feel spectrum for
  what wants to feel like SciTE.

Platform-specific code (menu bar placement, PTY spawn details, keyboard
shortcut modifiers) is isolated in `src/platform/` behind small interfaces so
the Linux port is a matter of adding a sibling implementation, not editing
call sites.

---

## 4. Repository layout

```
trowel/
├── CMakeLists.txt
├── cmake/
│   └── FindScintillaEdit.cmake     # if not available via FetchContent
├── docs/
│   ├── PLAN.md                     # this file
│   └── architecture.md             # to be written during v1
├── extern/
│   ├── scintilla/                  # via FetchContent
│   └── qtermwidget/                # via FetchContent or system pkg
├── resources/
│   ├── turmeric.lexer.keywords.h   # generated at build time (see §6.2)
│   └── turmeric-dark.theme.json    # ported from turmeric repo (see §6.3)
├── src/
│   ├── main.cpp
│   ├── app/
│   │   ├── main_window.{h,cpp}     # QMainWindow, splitter, menus
│   │   └── commands.{h,cpp}        # Run, Save, Open, Focus REPL, etc.
│   ├── editor/
│   │   ├── editor_view.{h,cpp}     # ScintillaEdit wrapper
│   │   ├── turmeric_lexer.{h,cpp}  # custom Scintilla ILexer5
│   │   └── theme_loader.{h,cpp}    # load turmeric-dark scheme
│   ├── repl/
│   │   ├── repl_session.{h,cpp}    # PTY + `tur repl` lifecycle
│   │   ├── terminal_view.{h,cpp}   # QTermWidget wrapper
│   │   └── run_buffer.{h,cpp}      # send buffer → REPL via (load ...)
│   └── platform/
│       ├── platform.h              # small interface: PTY spawn, paths
│       ├── platform_macos.mm
│       └── platform_linux.cpp      # stub until we start the Linux port
└── tests/
    ├── test_turmeric_lexer.cpp
    ├── test_theme_loader.cpp
    └── test_run_buffer.cpp
```

---

## 5. Build system (CMake)

- Require CMake ≥ 3.24, C++20.
- `find_package(Qt6 REQUIRED COMPONENTS Widgets)`.
- ScintillaEdit + Scintilla fetched via `FetchContent` from the upstream
  Sourceforge / GitHub mirror.
- QTermWidget: prefer system package (`brew install qtermwidget` /
  `apt install qtermwidget6-dev`); FetchContent fallback.
- Third-party JSON parsing for the color scheme: `nlohmann/json` via
  FetchContent (header-only).
- Warnings-as-errors on our own targets; loose on vendored Scintilla.
- Targets: `trowel` (executable), `trowel_lib` (static, for tests),
  `trowel_tests` (Catch2 via FetchContent).
- `CMakePresets.json` with `macos-debug`, `macos-release`, and (stubbed for
  now) `linux-debug`, `linux-release` presets so the Linux port has a landing
  pad from day one.

---

## 6. Editor subsystem

### 6.1 Scintilla integration
- Instantiate `ScintillaEdit` and pack into the left splitter slot.
- Configure indentation, whitespace display, line numbers, matched-bracket
  highlight, current-line highlight — mirror SciTE defaults where sensible.
- Route text via `SCI_*` messages; wrap the common ones in `editor_view.cpp`.

### 6.2 Turmeric lexer

Reference implementations already in the turmeric repo — do not invent from
scratch, port from these:

- `vim-syntax/syntax/turmeric.vim` — canonical keyword/pattern list.
- `emacs/turmeric-mode.el` — font-lock table, indentation rules, comment
  syntax.
- `vscode-syntax-ext/syntaxes/turmeric.tmLanguage.json` — TextMate grammar,
  covers the same tokens with regex patterns close to what Scintilla can lex.

Token classes to handle (from those references):

- **Comments:** line `;`, doc `;;;` (styled distinctly), nestable block
  `#| ... |#`.
- **Strings:** double-quoted with escape handling; character literals
  `#\newline`, `#\a`, `#\.`.
- **Numbers:** int, hex (`0x...`), binary (`0b...`), float.
- **Keyword literals:** `:foo`.
- **Metadata annotations:** `^mut`, `^linear`, etc.
- **Definition keywords:** `def`, `defn`, `defmacro`, `defmodule`, `defdata`,
  `defgadt`, `defclass`, `definstance`, `defstruct`, `deftuple`, `defpackage`,
  `use`, `import`, `export`.
- **Control flow:** `if`, `cond`, `case`, `match`, `loop`, `while`, `for`,
  `when`, `unless`.
- **Type system:** `type`, `typeclass`, `impl`, `where`, `forall`, `generic`.
- **Effects:** `effect`, `handle`, `do`, `perform`, `try`.
- **Special forms:** `let`, `let*`, `lambda`, `fn`, `quote`.
- **Reader macros / quoting:** `~@`, `~`, `` ` ``, `'`.
- **Operators:** `::`, `|>`.
- **Curly-infix regions:** `{ ... }` (a distinct sub-style is nice).
- **Neoteric calls:** identifier immediately followed by `(` — worth
  distinguishing.
- **C inline blocks:** ` ```c ... ``` ` — style the fence and dim the body;
  full C highlighting inside is a future direction.

Fold points: parentheses, curly braces, block comments, and `#| |#`.

At build time a small script pulls `vim-syntax/syntax/turmeric.vim` (pin a
turmeric commit) and generates `resources/turmeric.lexer.keywords.h` so the
keyword list can't drift.

Register via `Scintilla_LinkLexers` / `CreateLexer("turmeric")` and a second
variant `"turmeric-sweet"` for `.tur.sweet` (see §11).

### 6.3 Turmeric color scheme

Multiple sources exist upstream — pick one canonical and port it:

- `emacs/turmeric-dark-theme.el`
- `vscode-syntax-ext/themes/turmeric-dark-color-theme.json` (recommended
  starting point — closest format to what we want)
- `web/main.js` — `monaco.editor.defineTheme('turmeric-dark', {...})` and
  `'turmeric-light'` (Dark Spice Market: amber keywords, teal types, coral
  strings, sage numbers)

Approach:
- Port the VSCode dark theme JSON to `resources/turmeric-dark.theme.json`
  with a documented mapping: token class → Scintilla `STYLE_*` slot.
- `theme_loader` reads it at startup and applies via `SCI_STYLESETFORE`,
  `SCI_STYLESETBACK`, `SCI_STYLESETFONT`.
- Same palette also applied to the QTermWidget so both panes look cohesive.
- Post-v1: also port the light theme so the user can toggle.

---

## 7. REPL / terminal subsystem

### 7.1 Session lifecycle
- On window open, spawn `tur repl` inside a PTY (`tur interpret` is
  file-oriented — `tur repl` is the interactive REPL with `:type`, `:doc`,
  `:explain` meta-commands and history).
- Working directory = directory of the currently-open file, else `$HOME`.
- Track child pid; on close, SIGTERM then SIGKILL after grace period.
- "Restart REPL" menu item — cheap safety valve when the session gets wedged.
- If `tur` isn't on PATH, show a first-launch dialog with install hints
  (Homebrew tap lives in the turmeric repo's `Formula/`).

### 7.2 Run-buffer protocol

Turmeric already provides the mechanism: the `(load "path")` directive reads
a file and installs its bindings into the current environment (used throughout
the turmeric stdlib, e.g. `(load "stdlib/math.tur")`).

Flow for `Run`:
1. If the buffer is dirty or untitled, save it to a temp file under
   `$XDG_CACHE_HOME/trowel/scratch/` (or macOS equivalent). If it's clean and
   saved, use the real path.
2. Write `(load "<absolute-path>")\n` to the PTY.
3. Terminal shows the REPL's normal echo and any output. Definitions are now
   in the environment — user can immediately type at the REPL to inspect
   them. This is the DrRacket moment.

For `.tur.sweet` files, prepend a `#lang sweet-exp` header to the temp file
(or rely on the file extension if turmeric's `(load ...)` sniffs it — verify
during M4 rather than assuming).

`run_buffer.cpp` is the only place that knows this protocol. Everything else
calls `RunBuffer(editor, repl)`. Also expose `RunRange(editor, repl, start,
end)` — same code path, restricted range — so "evaluate selection" is a
trivial add later.

### 7.3 Terminal
- **v1 (current):** lightweight `QPlainTextEdit`-based terminal + hand-rolled
  PTY loop (`forkpty` + `QSocketNotifier`). Strips CSI/OSC escape sequences,
  honors CR/BS/BEL, forwards keystrokes (including arrows / Ctrl-letter / DEL)
  to the child. No color rendering yet; adequate for a REPL, not for running
  `vim` inside. Isolated behind `TerminalView` so the upgrade path stays open.
- **Future:** swap in QTermWidget (or libvterm-backed widget) when we need
  full xterm compatibility, SGR colors, scrollback search, etc. Deferred from
  v1 because QTermWidget is not packaged on Homebrew and its
  lxqt-build-tools dependency is a heavy yak-shave.
- Font matched to the editor's font for visual consistency (Iosevka →
  Menlo fallback).

---

## 8. Window shell

- `QSplitter` (horizontal, non-collapsible children) hosts editor left,
  terminal right.
- Splitter position, window size, font size, last-opened file persisted via
  `QSettings` (native storage on each platform).
- Menu bar: File (New/Open/Save/Save As/Quit), Edit (standard), View (toggle
  terminal, swap orientation to vertical), Run (Run Buffer ⌘R, Restart REPL,
  Focus REPL ⌘T, Focus Editor ⌘E). macOS gets the native menu bar via Qt's
  default handling.
- No modal dialogs beyond native file picker.

---

## 9. Milestones

**M0 — Skeleton (1–2 days)**
- CMake project, empty Qt6 window, splitter with two placeholder widgets.
- Builds on macOS. `linux-debug` preset defined but not verified.
- Read the removed-Trowel commits from the turmeric git history for any
  reusable ideas.

**M1 — Editor pane (3–5 days)**
- ScintillaEdit embedded, open/save works, default C-like styling as
  placeholder, font configurable.

**M2 — Terminal pane + REPL spawn (2–3 days)**
- QTermWidget embedded, `tur repl` runs inside it interactively.

**M3 — Turmeric lexer + theme (3–5 days)**
- Custom lexer wired up (both `turmeric` and `turmeric-sweet` variants
  registered, though sweet may be a stub reusing the base lexer at first).
- Turmeric-dark color scheme applied to editor and terminal.

**M4 — Run buffer (2–4 days)**
- `Run` command writes `(load "...")` to the live REPL for both `.tur` and
  `.tur.sweet`; verify variables defined in the buffer are visible from
  subsequent REPL input. Do not ship without this working.

**M5 — Polish + persistence (2–3 days)**
- QSettings persistence, keyboard shortcuts, restart REPL, error reporting
  when `tur` isn't on PATH.

**v1 release** after M5, macOS only.

**v1.1 — Linux port**
- Enable the `linux-*` presets, resolve any QTermWidget packaging quirks,
  smoke-test on one mainstream distro.

---

## 10. Testing strategy

- Unit tests (Catch2) for: theme JSON parsing, lexer token boundaries against
  a corpus of turmeric snippets pulled from the turmeric repo's `examples/`
  and `tutorials/`, `run_buffer` command encoding for both dialects.
- Integration test: offscreen Qt platform (`QT_QPA_PLATFORM=offscreen`)
  launches app, opens a fixture file, triggers Run, asserts on REPL output
  captured from the PTY.
- Manual smoke checklist maintained in `docs/manual-smoke.md`.

---

## 11. Future directions (do not preclude)

Design decisions in v1 should leave these paths open:

- **Linux support.** Already covered in §1, §3, §5, §9. Structural, not a
  future direction — but explicitly listed here so nothing in v1 is allowed
  to break the assumption.
- **Multiple file tabs.** Keep the editor pane behind an `EditorView`
  interface that a future `QTabWidget`-based tabbed view can host.
- **Project drawer.** Reserve the left edge for a future `QSplitter` slot;
  splitter code should generalize to a 3-pane layout (drawer / editor / repl).
- **Load between files / imports.** Once we index turmeric's `import` /
  `use` forms, jump-to-definition becomes possible. The lexer should
  distinguish these tokens now so a symbol index can hook in without
  re-lexing later.
- **Run against `tur` (AOT).** `run_buffer.cpp` should be one strategy behind
  a `RunStrategy` interface (`InterpretStrategy`, future `CompileStrategy`).
- **Evaluate selection.** Already covered by `RunRange` in §7.2.
- **Sweet-expression support.** Files ending `.tur.sweet` recognized in v1;
  the lexer variant is registered even if it initially reuses the base
  ruleset. Full sweet-exp indentation handling (curly-infix, indentation to
  parens) is future work.
- **Autocomplete + diagnostics via `tur lsp`.** Upstream already ships
  `tur lsp` (see `src/lsp/` in the turmeric repo) and a lightweight
  completion/calltip backend in `src/cli/lsp_lite.c`. Spawn `tur lsp` as a
  second child process speaking LSP over stdio, wire completions into
  Scintilla's `SCI_AUTOCSHOW`, render diagnostics as squiggle indicators +
  margin markers. Keep the LSP client behind an interface so it can be
  disabled or swapped.
- **Debugger.** Upstream already ships `tur dap` and a VSCode debugger
  extension in `editors/vscode-turmeric/`. Trowel can speak DAP to the same
  binary. Isolate REPL I/O in v1 so the DAP transport slots in without
  touching editor code.
- **Light theme + theme switcher.** Port `turmeric-light` from `web/main.js`
  once the dark theme lands.
- **Turmeric-in-turmeric.** `web/` uses turmeric compiled to WASM; a future
  Trowel could embed the same for offline / no-`tur`-installed use.

---

## 12. Reference material (turmeric repo)

Consulted during planning; keep pinned commits when we start porting:

- `vim-syntax/syntax/turmeric.vim` — lexer keywords.
- `emacs/turmeric-mode.el` — font-lock, indentation rules.
- `emacs/turmeric-dark-theme.el` — dark palette (Emacs form).
- `vscode-syntax-ext/` — tmLanguage grammar, `language-configuration.json`
  (comment delimiters, brackets), `themes/turmeric-dark-color-theme.json`.
- `editors/vscode-turmeric/` — DAP extension; confirms `.tur` + `.tur.sweet`.
- `web/main.js` — Monaco tokenizer + `turmeric-light` / `turmeric-dark`.
- `src/main.c` — dispatch for `tur repl`, `tur interpret`, `tur lsp`,
  `tur dap`.
- `src/lsp/`, `src/cli/lsp_lite.{c,h}` — LSP surface for future integration.
- Git history: `"Remove trowel for now"`, `"Trowel newline issue"` — prior
  Trowel prototype worth reading.
