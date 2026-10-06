# Plugin system and Turmeric integration — plan

> **Status:** P0–P7 shipped. All seven milestones landed across commits
> `dacdb2e` ("WIP: Plugins") and `8a3546b` ("Plugins"). `libturi.a` is linked
> into `trowel_lib` (P0); `CommandRegistry` + `CommandPalette` with `Ctrl+P`
> are wired (P1); the full v1 native API — 21 natives covering registration,
> state reading, buffer manipulation, UI, snippets, keybinding, and background
> tasks — is registered in `src/plugin/plugin_natives.cpp` (P2); the OSC 517
> REPL command bridge is live (P3); the snippets plugin with tab-stop parsing
> ships as a smoke-tested `plugin.tur` (P4); data-driven syntax plugins via
> `trowel-define-syntax` are built and smoke-tested (P5); `keymap.tur` loading
> is wired (P6); `trowel-spawn` + the 16 ms `QTimer` event-loop pump are live
> (P7). Five smoke tests cover it: `test_snippets.py`, `test_syntax_plugin.py`,
> `test_keymap.py`, `test_background.py`, `test_bookmarks.py`.
>
> **Bundled plugins:** bookmarks and snippets ship as `:/plugins/` Qt
> resources. The keymap is data-driven: bundled `:/keymap/keymap-mac.tur`
> and `:/keymap/keymap-other.tur` set default shortcuts (generated from
> `shortcuts.cpp`), and `~/.trowel/keymap.tur` overrides them. The
> `trowel-set-keybinding` native now updates the backing `QAction` directly,
> so overrides take effect immediately. `CommandRegistry::setShortcut()`
> was fixed to propagate to the `QAction` (previously only the registry
> entry was updated). Startup order fixed: `registerBuiltinCommands()` and
> `loadKeymap()` now run in `startSession()` after `commandRegistry_` and
> `pluginHost_` exist (previously called from `setupMenus()` where both
> were null, making them no-ops).
>
> **Still open:** per-plugin isolation (§4.4) is split into its own plan,
> [`plugin-isolation.md`](plugin-isolation.md).
> **Related:** [`PLAN.md`](PLAN.md) (architecture), [`socket-api.md`](socket-api.md)
> (control socket — the existing command dispatch this builds on),
> [`button-bar.md`](button-bar.md) (side bar — the existing button surface this
> extends), [`multi-window.md`](multi-window.md) (WindowManager),
> [`editor-intelligence.md`](editor-intelligence.md) (LSP / buffer model),
> [`plugin-isolation.md`](plugin-isolation.md) (per-plugin sandboxing — split
> from §4.4).

Two goals, one plan, because they are inseparable: the plugin system's
scripting language is Turmeric, and embedding Turmeric is what makes the
plugin system possible without a foreign FFI bridge.

**Goal 1 — Plugin system.** A way for code (written in Turmeric) to extend
Trowel at runtime: register commands in a command palette, register buttons in
the side bar, read editor/REPL state, manipulate buffers, and run code in the
background or in the REPL. Start with a small API surface and grow it.

**Goal 2 — Gradual Turmeric replacement.** Replace parts of Trowel's C++ code
with Turmeric code where it is sensible to do so. The plugin host is the first
piece of Turmeric to live inside Trowel; over time, features that are currently
hardcoded C++ (command definitions, keymap logic, snippet expansion, bookmark
storage) migrate to Turmeric scripts that the host loads.

---

## 1. Why Turmeric as the plugin language

Trowel already ships a prebuilt Turmeric toolchain (the `tur` binary + stdlib,
fetched via `FetchContent` at `TROWEL_TURMERIC_VERSION`). The release archive
also contains `lib/libturi.a` and `include/turi/{eval,env,value,fiber}.h` — the
C embedding API. Trowel currently uses only the `tur` binary (spawning `tur
repl`, `tur lsp`, `tur dap`, `tur trace` as child processes). Linking against
`libturi.a` lets Trowel evaluate Turmeric source in-process, with full access
to the stdlib, without spawning a child or crossing a process boundary.

This is the same API the `tur_eval_basic` and `tur_eval_sandbox` tests in the
Turmeric repo exercise (`tests/turi/eval-basic.c`, `tests/turi/sandbox-eval.c`).
The embedding surface is small and stable:

| Function | Purpose |
|---|---|
| `turi_init(bool use_color)` | Initialise diagnostics. Call once at startup. |
| `turi_env_new()` | Create an unrestricted eval environment. |
| `turi_env_new_sandboxed()` | Create a capability-restricted env (no I/O, FFI, etc.). |
| `turi_env_new_with_natives(specs, n)` | Create an env with native functions pre-registered. |
| `turi_eval(env, src)` → `TuriValue` | Evaluate a source string. Definitions persist across calls. |
| `turi_eval_file(env, path)` | Evaluate a file. |
| `turi_eval_typed(env, src, tag_buf, cap)` | Same, plus the elaborated type name of the result. |
| `turi_env_register_native(env, name, fn, ud)` | Register a C function callable from Turmeric. |
| `turi_env_register_native_caps(env, name, fn, ud, caps)` | Same, with an explicit capability requirement. |
| `turi_env_set_module_base_dir(env, dir)` | Set the import search root for `(import ...)`. |
| `turi_env_set_fuel(env, steps)` | Bound execution (step-fuel). |
| `turi_env_allow(env, cap)` / `turi_env_deny(env, cap)` | Grant/revoke capabilities. |
| `turi_env_reset(env)` | Clear user definitions; keep registered natives. |
| `turi_env_free(env)` | Tear down. |
| `turi_value_repr(buf, cap, v)` | Render a value to a C string. |
| `turi_call(env, fn, args, n)` | Call a closure value directly. |
| `turi_run_event_loop(env)` | Pump the cooperative async scheduler. |
| `turi_task_spawn(env, src)` | Spawn an async task; returns a `TURI_FUTURE`. |

`TuriValue` is a tagged union (`TURI_NIL`, `TURI_BOOL`, `TURI_INT`,
`TURI_FLOAT`, `TURI_CSTR`, `TURI_CLOSURE`, `TURI_ERROR`, `TURI_STRUCT`,
`TURI_FUTURE`, `TURI_GEN`, ...). A native function (`TuriNativeFn`) is
`TuriValue (*)(TuriEnv*, TuriValue*, uint32_t, void*)` — it receives the env,
the argument array, the count, and a user-data pointer.

Capabilities (`TuriCaps`) gate what a sandboxed env may do: `TURI_CAP_IO`,
`TURI_CAP_FFI`, `TURI_CAP_INLINE_C`, `TURI_CAP_ASYNC`, `TURI_CAP_UNSAFE`,
`TURI_CAP_IMPORT`, `TURI_CAP_FS`, `TURI_CAP_PROC`, `TURI_CAP_ENV`. An
unrestricted env (`turi_env_new()`) holds all of them; a sandboxed env holds
none. The plugin host grants only what each plugin needs.

### Alternatives considered

- **Lua / LuaJIT.** The classic editor-embedding choice. Rejected because
  Trowel's entire reason to exist is Turmeric — using a second language for
  extensibility would split the ecosystem and deny the dogfooding benefit.
- **Python.** Heavyweight, GIL-bound, and the same ecosystem-split problem.
- **Child-process plugins (JSON-RPC over stdio).** What the control socket
  already is. Rejected for plugins because the latency and lifecycle overhead
  is wrong for interactive features (command palette filtering, snippet
  expansion, buffer mutation on every keystroke). The control socket stays
  for external automation; plugins run in-process.

---

## 2. Architecture

```
+----------------------------------------------------------+
|  Trowel (Qt6)                                            |
|                                                          |
|  MainWindow                                              |
|    ├── TabBar / EditorStack (EditorView, DirectoryView)  |
|    ├── ReplPane (TerminalView + ReplSession)             |
|    ├── SideBar (button bar)                              |
|    ├── CommandPalette (new — see §3)                     |
|    └── PluginHost (new — see §4)                          |
|          ├── TuriEnv (libturi, in-process)               |
|          │     ├── registered natives (Trowel API)        |
|          │     └── loaded plugin scripts                  |
|          ├── PluginRegistry (manifests, lifecycles)      |
|          └── HookBus (event dispatch)                    |
|                                                          |
|  ControlServer (existing — unchanged)                    |
+----------------------------------------------------------+
```

The `PluginHost` owns one `TuriEnv` (or one per plugin — see §4.4) and a set
of registered native functions that expose Trowel's C++ surface to Turmeric
code. Plugins are Turmeric source files that call those natives to register
commands, buttons, and hooks.

### 2.1 What is a plugin

A plugin is a directory under `~/.trowel/plugins/<name>/` (or a bundled
`<app>/Contents/Resources/plugins/<name>/`) containing:

```
my-plugin/
├── plugin.tur          # entry point — evaluated by the host on load
├── commands.tur        # optional: additional command definitions
└── snippets.tur        # optional: plugin-specific data/logic
```

`plugin.tur` is the manifest and entry point in one. It calls host natives to
declare what it provides:

```turmeric
;; plugin.tur — a snippets plugin
(trowel:register-command
  :id "snippets.expand"
  :title "Expand Snippet"
  :key "Cmd+Shift+V"
  :fn (fn [] (snippets:expand-at-cursor)))

(trowel:register-button
  :icon "nf-md-playlist_play"
  :tooltip "Expand Snippet"
  :command "snippets.expand")

(defmodule Snippets
  (defn expand-at-cursor []
    (let [word (trowel:word-at-cursor)]
      (case word
        "defn" (trowel:insert-snippet
                 "(defn ${1:name} [${2:args}]\n  ${0:body})")
        "defmodule" (trowel:insert-snippet
                      "(defmodule ${1:Name}\n  ${0:body})")
        (trowel:status-message (str "No snippet for: " word))))))
```

### 2.2 What is a syntax plugin

Syntax plugins are a **separate kind**. They do not use the same registration
surface as command/button plugins. A syntax plugin provides:

- A Scintilla lexer name (e.g. `"rust"`, `"go"`).
- Keyword lists, token-class patterns, fold points.
- Optionally, a Turmeric function that classifies a line for indentation.

The host installs a syntax plugin's lexer into Scintilla via the same
`CreateLexer` / `ILexer5` mechanism the built-in turmeric lexer uses (see
`src/editor/scanner_*.cpp`). A syntax plugin cannot register commands or
buttons — it is purely a highlighting/indentation definition. This keeps the
security surface small (no buffer mutation, no REPL access) and the API
focused.

Syntax plugins live under `~/.trowel/syntax/<name>/` and contain a
`syntax.tur` that returns a syntax descriptor (keywords, operators, comment
delimiters, string delimiters, fold-open/fold-close characters). The host
reads the descriptor and builds a Scintilla lexer from it. For v1 this is a
data-driven lexer (keyword lists + simple token classes); a full
programmatic lexer (arbitrary tokenization logic in Turmeric) is a future
direction.

---

## 3. Command palette

A fuzzy-filtering command palette, invoked by `Cmd-P` (matching VSCode).
Shows all registered commands — built-in and plugin-provided — in a single
searchable list. Selecting an entry runs the command.

### 3.1 UI

- A `QFrame` popup centered horizontally, ~40% down from the top of the
  window. Overlays the editor (not a dock widget — it appears and dismisses).
- A `QLineEdit` at the top for the filter. Below it, a `QListWidget` (or a
  custom `QWidget` with `QListView`) showing matching commands.
- Filter is fuzzy: subsequence match (like VSCode / Sublime). Score by
  match density and recency. The first match is preselected; Enter runs it.
- `Esc` dismisses. `Up`/`Down` navigate. `Cmd-P` again cycles to the next
  match (VSCode behavior).
- Each row shows: the command title, a dimmed category prefix (e.g.
  `Snippets: Expand Snippet`), and the keyboard shortcut if one is bound.

### 3.2 Command registry

A `CommandRegistry` (new, `src/app/command_registry.{h,cpp}`) holds every
command the palette can show. Each entry:

```cpp
struct Command {
    QString id;          // "snippets.expand", "trowel.run-buffer"
    QString title;       // "Expand Snippet", "Run Buffer"
    QString category;    // "Snippets", "Run"
    QKeySequence shortcut;  // optional
    std::function<void()> handler;  // what runs when selected
};
```

Built-in commands (Run Buffer, Save, Go to Definition, Toggle REPL, etc.)
register themselves at startup by lifting the existing `QAction*`s from
`MainWindow` into the registry. A command's handler is the same slot the
menu/toolbar already uses — no duplicated logic.

Plugin commands register through the `trowel:register-command` native, which
calls into the `CommandRegistry` on the Qt thread.

### 3.3 Calling commands from the REPL

The REPL already speaks to `tur repl` over a PTY. A Turmeric form like
`(trowel:run-command "snippets.expand")` is not available in the REPL's env
— the REPL is a separate process. Instead, the command palette and the REPL
share the command registry through a **command dispatch bridge**:

- A new REPL meta-command, `:cmd <id>`, sends a control message to Trowel
  (via the existing OSC 133 / OSC 7 channel the REPL already uses for
  prompt/cwd reporting, or via a dedicated OSC code). Trowel receives it,
  looks up the command by id in the `CommandRegistry`, and runs it.
- This is a thin extension of the existing `ReplSession` data-scan loop
  (`scanCwdReports`, `scanDialectReports` in `repl_session.cpp`): add a
  `scanCommandRequests` that recognises an OSC sequence carrying a command
  id, and emits a signal `commandRequested(QString)` that `MainWindow`
  connects to `CommandRegistry::run(id)`.
- The REPL side is a one-line `(trowel:cmd "snippets.expand")` that prints
  the OSC sequence. This can be defined in the user's `~/.tur/replrc.tur` or
  shipped as a stdlib helper once the protocol is stable.

This keeps the REPL as a real `tur repl` process (not an in-process eval)
while still letting it drive Trowel commands. The in-process `TuriEnv` is for
plugins; the REPL stays out-of-process.

---

## 4. Plugin host

### 4.1 TuriEnv lifecycle

`PluginHost` (`src/plugin/plugin_host.{h,cpp}`) owns:

- One `TuriEnv*` created at startup via `turi_env_new_with_natives()`, with
  all Trowel API natives pre-registered.
- The env is **unrestricted** (`turi_env_new`, not sandboxed) for v1. A
  sandboxed mode (per-plugin envs with capability grants) is a future
  direction (§4.4). The trade-off: v1 plugins run with full trust, which is
  acceptable because plugins are user-installed local files (same trust
  model as shell aliases or editor config).
- `turi_env_set_module_base_dir` is set to `~/.trowel/plugins/` so
  `(import ...)` resolves plugin-to-plugin.
- The stdlib is available via the bundled `tur` binary's stdlib directory
  (already staged by the CMake `turmeric_prebuilt` fetch). The host sets
  `TUR_STDLIB_DIR` (or the equivalent env field) to point at it.

### 4.2 Native API surface (v1)

The host registers a set of C functions (via `turi_env_register_native`) that
constitute the plugin API. Each is a thin wrapper over existing Trowel C++
methods, marshaling `TuriValue` args to Qt types and back.

**Registration:**

| Native | Signature | Maps to |
|---|---|---|
| `trowel:register-command` | `(id :cstr title :cstr category :cstr key :cstr fn)` | `CommandRegistry::add(...)` |
| `trowel:register-button` | `(icon :cstr tooltip :cstr command :cstr)` | `MainWindow::addSideBarAction` |
| `trowel:register-hook` | `(event :cstr fn)` | `HookBus::subscribe(event, fn)` |

**State reading:**

| Native | Returns | Maps to |
|---|---|---|
| `trowel:buffer-text` | `:cstr` | `EditorView::text()` |
| `trowel:buffer-text-range` | `(start :int end :int) → :cstr` | `EditorView::textInRange` |
| `trowel:cursor-pos` | `:int` | `EditorView::cursorPos()` |
| `trowel:cursor-line-col` | `(line :int col :int)` | `EditorView::lineColFromPos` |
| `trowel:selection` | `(start :int end :int)` | `EditorView::selectionRange()` |
| `trowel:file-path` | `:cstr` | `EditorView::filePath()` |
| `trowel:repl-screen` | `(lines :int) → :cstr` | `TerminalView::screenLines(n)` |
| `trowel:repl-busy?` | `:bool` | `ReplSession::isBusy()` |
| `trowel:word-at-cursor` | `:cstr` | new helper on `EditorView` |

**Buffer manipulation:**

| Native | Maps to |
|---|---|
| `trowel:set-text` | `EditorView::setText` |
| `trowel:insert-text` | `ScintillaEdit::insertText` at pos |
| `trowel:replace-range` | `EditorView::replaceRange` |
| `trowel:set-cursor` | `EditorView::setCursorPos` |
| `trowel:set-selection` | `EditorView::setSelection` |
| `trowel:insert-snippet` | new: snippet expansion with tab-stops (§5.1) |
| `trowel:begin-edit-group` | `EditorView::beginEditGroup` |
| `trowel:end-edit-group` | `EditorView::endEditGroup` |

**REPL / execution:**

| Native | Maps to |
|---|---|
| `trowel:repl-send` | `ReplSession::sendCommand` |
| `trowel:run-buffer` | `RunBuffer(editor, repl)` |
| `trowel:run-selection` | `RunRange(editor, repl, start, end)` |
| `trowel:eval` | `turi_eval` on the plugin env (run Turmeric in-process) |
| `trowel:spawn` | `turi_task_spawn` (async background task) |
| `trowel:pump-events` | `turi_run_event_loop` (drain async scheduler) |

**UI:**

| Native | Maps to |
|---|---|
| `trowel:status-message` | `MainWindow::statusBar()->showMessage` |
| `trowel:focus-editor` | `MainWindow::focusEditor` |
| `trowel:focus-repl` | `MainWindow::focusRepl` |
| `trowel:open-file` | `MainWindow::openPath` |

**Hook events (v1):**

| Event | Fires when |
|---|---|
| `buffer:changed` | Editor text changed (debounced) |
| `cursor:moved` | Caret position changed |
| `buffer:saved` | File saved |
| `file:opened` | File opened |
| `repl:busy` | REPL entered/exited busy state |
| `command:run` | A command was invoked (pre-run, can veto) |

### 4.3 Hook dispatch

The `HookBus` (`src/plugin/hook_bus.{h,cpp}`) is a simple
`QHash<QString, QVector<TuriValue>>` of event name → list of registered
closure values. When a C++ signal fires (e.g. `EditorView::contentChanged`),
the host calls `HookBus::emit("buffer:changed", args)`, which:

1. Looks up the event name.
2. For each registered closure, calls `turi_call(env, closure, args, n)`.
3. If a closure returns `TURI_ERROR`, logs it to the status bar and continues
   (one bad plugin does not kill the hook chain).
4. For `command:run` (vetoable), a closure returning `false` prevents the
   command from running.

All hook dispatch happens on the Qt main thread (the same thread that owns
the widgets). This is correct: plugins manipulate widgets, and Qt widgets
are not thread-safe. Background work (§4.5) runs on the Turmeric async
scheduler, which is cooperative and single-threaded within the `TuriEnv`.

### 4.4 Per-plugin isolation (future)

Split into its own plan: [`plugin-isolation.md`](plugin-isolation.md).

v1 uses a single shared `TuriEnv` for all plugins. This is simple and lets
plugins share definitions (one plugin's `defmodule` is visible to another's
`(import ...)`). The trade-off: a plugin that calls `turi_env_reset` would
wipe every other plugin's state. The isolation plan covers the per-plugin
env management, capability grants, and manifest format.

### 4.5 Background code

Plugins can run code in the background via `trowel:spawn`, which calls
`turi_task_spawn(env, src)`. The Turmeric async scheduler is cooperative
(single-threaded, fiber-based), so "background" means "interleaved with the
Qt event loop," not "on another OS thread."

The host pumps the scheduler on a `QTimer` (every ~16ms, or on idle) by
calling `turi_run_event_loop(env)`. This drains ready fibers and timers
without blocking. A plugin that spawns a long-running task (e.g. a file
watcher) yields cooperatively; the Qt UI never stalls because the scheduler
returns to the host between fiber switches.

For truly blocking work (e.g. a network request), a plugin can use the
Turmeric stdlib's async I/O (`async_file.tur`, `async_socket.tur`) if the
host grants `TURI_CAP_IO`. The scheduler integrates these via the same
`QTimer` pump.

### 4.6 Plugin discovery and loading

At startup, `PluginHost::loadAll()` scans:

1. `<app>/Contents/Resources/plugins/` (bundled, read-only).
2. `~/.trowel/plugins/` (user-installed).

For each `<name>/plugin.tur` found, the host:

1. Reads the file.
2. Calls `turi_eval_with_path(env, source, path)` (so diagnostics report the
   real file path).
3. If the result is `TURI_ERROR`, shows the diagnostic in the status bar and
   a dialog, and skips the plugin (does not abort other plugins).
4. The plugin's top-level forms execute, calling `trowel:register-*` natives
   that populate the `CommandRegistry`, side bar, and `HookBus`.

Plugins can be reloaded at runtime (`trowel:reload-plugin "name"`) by
calling `turi_env_reset` (clears user definitions but keeps natives) and
re-evaluating the plugin file. This is the development loop.

---

## 5. First plugin: snippets

The first plugin dogfoods the entire system: it registers a command, reads
the buffer, manipulates it, and is written in Turmeric.

### 5.1 Snippet expansion

A snippet is a template string with tab-stops (`${1:default}`, `${0:final}`)
in the TextMate style. Expansion:

1. Read the word at the cursor.
2. Look up a snippet for that word.
3. Insert the template at the cursor position.
4. Enter snippet mode: the first tab-stop is selected. `Tab` cycles to the
   next stop; `Tab` at the last stop exits snippet mode and places the cursor
   at `${0}`.

Snippet mode is a small state machine in the plugin (Turmeric), not in C++.
The C++ side installs an event filter on the Scintilla widget (see §11.1):
when `Key_Tab` arrives and a `SnippetSession` is active, the filter consumes
the event and calls `snippetAdvance()` instead of letting Scintilla insert
an indent. When no snippet is active, Tab falls through to Scintilla's
default behavior unchanged. The plugin queries `trowel:snippet-active?` and
calls `trowel:snippet-advance` to drive the state machine.

The C++ side provides `trowel:insert-snippet`, which:

1. Parses the template into text + tab-stop ranges (a simple regex pass).
2. Inserts the text via `ScintillaEdit::insertText`.
3. Selects the first tab-stop range.
4. Records the stop ranges in a `SnippetSession` struct on the
   `EditorView` (so the plugin can query `trowel:snippet-active?` and
   `trowel:snippet-advance`).

### 5.2 Snippet storage

Snippets are defined in Turmeric, in the plugin's own module:

```turmeric
(defmodule Snippets
  (def snippets
    { "defn"    "(defn ${1:name} [${2:args}]\n  ${0:body})"
      "defmodule" "(defmodule ${1:Name}\n  ${0:body})"
      "defdata"  "(defdata ${1:Name}\n  ${0:body})"
      "let"      "(let [${1:var} ${2:val}]\n  ${0:body})"
      "match"    "(match ${1:expr}\n  ${2:pat} ${3:body}\n  ${0:else})" }))
```

This is the dogfooding moment: the snippet table is a Turmeric map, the
expansion logic is Turmeric code, and the C++ side only provides the
low-level buffer primitives. A user who wants to add a snippet edits a
`.tur` file, not C++.

### 5.3 Bookmarks

Implemented as a bundled Turmeric plugin at
`resources/plugins/bookmarks/plugin.tur`, loaded from the `:/plugins/` Qt
resource path (bundled plugins are now scanned alongside `~/.trowel/plugins/`).
Registers "Toggle Bookmark", "Next Bookmark", "Prev Bookmark", and "Clear All
Bookmarks" commands plus four side-bar buttons.

The C++ side owns the bookmark state (`QHash<QString, QSet<int>>` in
`PluginContext`) and persistence (`~/.trowel/bookmarks.tur`, one
`file:line` per line). Four bookmark natives — `trowel-bookmark-toggle`,
`trowel-bookmark-next`, `trowel-bookmark-prev`, `trowel-bookmark-clear` —
handle the state, marker, and persistence in one call. The Turmeric plugin
is a thin wrapper that registers commands and buttons and calls the
natives. Visual markers use `kBookmarkMarker` (Scintilla marker 8) in the
shared gutter, with lowest precedence in `refreshGutterMarkers()`. Also
added general-purpose natives `trowel-cursor-line`, `trowel-goto-line`,
`trowel-set-bookmarks` (comma-separated line string), `trowel-read-file`,
and `trowel-write-file` (restricted to `~/.trowel/`). Covered by
`tests/smoke/test_bookmarks.py` (4 tests: toggle, next/prev, clear,
persistence across restart).

---

## 6. Gradual Turmeric replacement

The plugin host is the beachhead. Once Turmeric code runs in-process, C++
features that are pure logic (no Qt widget creation) can migrate. The
principle: **C++ owns the widgets and the event loop; Turmeric owns the
logic.**

### 6.1 What stays in C++

- Qt widget creation, layout, painting (Scintilla, terminal, splitter, tabs).
- The event loop and signal/slot wiring.
- PTY management, process lifecycle.
- Scintilla SCI message marshaling.
- LSP / DAP transport (binary protocols over stdio/sockets).

These are performance-critical, widget-coupled, or binary-protocol work that
does not benefit from being in a scripting language.

### 6.2 What can migrate to Turmeric

| Feature | Current C++ location | Migration path |
|---|---|---|
| Command definitions | `MainWindow::setupMenus()` (hardcoded `QAction`s) | Lift into `CommandRegistry`; a bundled `commands.tur` plugin registers them. The `QAction`s become thin wrappers that call `CommandRegistry::run(id)`. |
| Keymap | Hardcoded `QKeySequence` in `QAction` setup | **Done.** Bundled `keymap-mac.tur` / `keymap-other.tur` (Qt resources) set defaults; `~/.trowel/keymap.tur` overrides. `trowel-set-keybinding` updates the `QAction` directly. |
| Snippet definitions | (none yet) | Pure Turmeric from day one (§5). |
| Bookmark storage | (none yet) | Pure Turmeric from day one (§5.3). |
| Recent files logic | `MainWindow::loadRecentFiles` / `rememberRecentFile` | A `recent-files.tur` plugin that hooks `file:opened` and `file:saved`, stores the list, and registers "Open Recent" commands. |
| Run-buffer protocol | `run_buffer.cpp` (the `(load ...)` encoding) | The encoding logic (scratch file, `#lang` header, extension) moves to a Turmeric function `trowel:run-buffer-impl`. The C++ `RunBuffer` calls it via `turi_call` and sends the result to the PTY. |
| Dialect detection | `dialect.cpp` (`Dialect::fromPath`, `#lang` parsing) | A Turmeric function that classifies a path + `#lang` line. The C++ `EditorView::dialect()` calls it. |
| Theme loading | `theme_loader.cpp` (JSON → Scintilla styles) | A Turmeric function that reads the theme JSON and returns a style map. The C++ side applies it via SCI messages. The JSON parsing is already available in the Turmeric stdlib. |
| Preferences logic | `preferences_view.cpp` | The preference schema and defaults move to Turmeric; the C++ side renders the UI from the schema. |

### 6.3 Migration order

1. **Commands + keymap** (with the command palette). This is the structural
   prerequisite: until commands are in a registry, nothing else can reference
   them by id.
2. **Snippets** (the first plugin). Proves the native API and the hook bus.
3. **Run-buffer protocol.** Small, self-contained, and already has a clean
   C++ interface (`RunBuffer` / `RunRange`). Moving the encoding to Turmeric
   is low-risk and makes it scriptable.
4. **Dialect detection.** Pure logic, no widgets.
5. **Recent files.** Hooks into the event system, exercises persistence.
6. **Theme loading.** Exercises JSON parsing in Turmeric.

Each migration is independently shippable. None is required for the plugin
system to work — they are opportunities, not dependencies.

### 6.4 What does not migrate (v1)

- The LSP client (`lsp_client.cpp`, `lsp_manager.cpp`). Binary JSON-RPC over
  stdio; the transport stays in C++. The LSP *configuration* (which server,
  which args) could move to Turmeric, but the protocol implementation stays.
- The DAP client (`dap_client.cpp`, `debug_session.cpp`). Same reasoning.
- The terminal emulator (`terminal_view.cpp`). Performance-critical byte
  processing on every PTY read.
- Scintilla lexers (`scanner_*.cpp`). These are C++ implementations of the
  Scintilla `ILexer5` interface; they cannot be Turmeric until Scintilla
  supports external lexers via a non-C++ ABI (it does not). Syntax *plugins*
  (§2.2) are the workaround: a data-driven lexer built from a Turmeric
  descriptor, implemented in C++ but configured from Turmeric.

---

## 7. CMake integration

Trowel already fetches the prebuilt Turmeric archive via `FetchContent`. The
archive contains `lib/libturi.a`, `lib/libturt_runtime.a`,
`lib/libturt_preamble.a`, `lib/libtur_mir.a`, and
`include/turi/{eval,env,value,fiber}.h`.

Changes to `CMakeLists.txt`:

```cmake
# After the existing turmeric_prebuilt FetchContent block:

# Link libturi into trowel_lib for the plugin host.
target_link_libraries(trowel_lib PRIVATE
    ${turmeric_prebuilt_SOURCE_DIR}/lib/libturi.a
    ${turmeric_prebuilt_SOURCE_DIR}/lib/libturt_runtime.a
    ${turmeric_prebuilt_SOURCE_DIR}/lib/libturt_preamble.a
)
target_include_directories(trowel_lib PRIVATE
    ${turmeric_prebuilt_SOURCE_DIR}/include
)
# libturi needs pthread and dl (already propagated by its own CMake, but
# we link statically so we repeat them here).
target_link_options(trowel_lib PRIVATE -pthread)
find_library(DL_LIBRARY dl)
if(DL_LIBRARY)
    target_link_libraries(trowel_lib PRIVATE ${DL_LIBRARY})
endif()
```

The exact archive layout (flat vs. prefix) is already handled by the existing
`_turmeric_key` / `_turmeric_ext` / staging logic — the `lib/` and
`include/` directories sit at the archive root in the prefix layout (the
current one, post-v0.46.0).

New source files:

```
src/plugin/
├── plugin_host.{h,cpp}       # TuriEnv lifecycle, native registration, plugin loading
├── plugin_natives.{h,cpp}    # the C functions exposed to Turmeric (§4.2)
├── hook_bus.{h,cpp}          # event subscription/dispatch
├── command_registry.{h,cpp}  # command palette data model
├── command_palette.{h,cpp}   # the QFrame popup UI
└── snippet_session.{h,cpp}   # tab-stop state for snippet expansion
```

---

## 8. Security model

- **v1: full trust.** Plugins are local files the user chose to install, same
  as shell config or editor plugins in any other editor. The `TuriEnv` is
  unrestricted. A malicious plugin can do anything the user can do — this is
  the same trust boundary as a `.vimrc` or a VSCode extension.
- **Future: sandboxed plugins.** `turi_env_new_sandboxed` +
  `turi_env_allow(env, cap)` per plugin, with capabilities declared in the
  plugin manifest. The host checks the manifest against a permission prompt
  on first load (like VSCode's "This extension wants to access X"). The
  capability system already exists in libturi; the work is the per-plugin env
  management and the UI, not the security primitive.
- **No network by default.** A v1 plugin env does not get `TURI_CAP_IO`
  unless the user opts in. The `trowel:*` natives are registered with
  `TURI_CAP_NONE` (no capability requirement) because they are the host's
  own API, not the stdlib's I/O surface.
- **Hook failures are isolated.** A hook that throws `TURI_ERROR` is logged
  and skipped; it does not abort the event chain or crash Trowel.

---

## 9. Milestones

### P0 — Link libturi, eval in-process (1–2 days)

- CMake changes to link `libturi.a` into `trowel_lib`.
- `PluginHost` skeleton: `turi_init`, `turi_env_new`, `turi_eval("(+ 1 2)")`,
  assert the result is `3`. No UI, no natives, just proof the linker works.
- A debug menu action "Eval Turmeric..." that takes a string, evals it, and
  shows the result in the status bar.

### P1 — Command registry + palette (2–3 days)

- `CommandRegistry` with all built-in commands lifted from `MainWindow`'s
  existing `QAction`s.
- `CommandPalette` UI: `QFrame` popup, fuzzy filter, `Cmd-P` to open.
- Commands are runnable from the palette. No plugin commands yet.

### P2 — Plugin host core (2–3 days)

- Register the v1 native API (§4.2) — at least the registration, state-reading,
  and buffer-manipulation subsets.
- `HookBus` with `buffer:changed`, `cursor:moved`, `file:opened`, `file:saved`.
- Plugin discovery: scan `~/.trowel/plugins/` and bundled dir, eval each
  `plugin.tur`, report errors gracefully.
- `trowel:reload-plugin` for the development loop.

### P3 — REPL command bridge (1 day)

- OSC-based `:cmd <id>` from the REPL to Trowel.
- `ReplSession::commandRequested` signal → `CommandRegistry::run`.

### P4 — Snippets plugin (2–3 days)

- `trowel:insert-snippet` native with tab-stop parsing.
- `SnippetSession` state on `EditorView`.
- The snippets plugin itself (§5): `plugin.tur`, snippet table, expansion
  logic, `Tab` cycling.
- Dogfood: write this plan's code snippets using the snippets plugin.

### P5 — Syntax plugins (2–3 days)

- `syntax.tur` descriptor format.
- Data-driven Scintilla lexer built from the descriptor.
- One example syntax plugin (e.g. a simple JSON or TOML highlighter that
  replaces the hardcoded `scanner_json.cpp` / `scanner_toml.cpp`).

### P6 — First migration: commands + keymap to Turmeric (1–2 days)

- ~~A bundled `commands.tur` that registers every built-in command.~~
  Built-in commands stay in C++ (`registerBuiltinCommands()` walks the menu
  bar and registers each `QAction`); the keymap overrides their shortcuts.
- **Done.** Bundled `keymap-mac.tur` / `keymap-other.tur` (Qt resources)
  set default shortcuts; `~/.trowel/keymap.tur` overrides. The
  `trowel-set-keybinding` native updates the backing `QAction` directly.
  `CommandRegistry::setShortcut()` propagates to the `QAction`.

### P7 — Background tasks (1–2 days)

- `trowel:spawn` + `QTimer`-driven `turi_run_event_loop` pump.
- One plugin that uses it (e.g. a file-watcher that refreshes a directory
  view on change).

---

## 10. Testing

- **Unit tests** (Catch2, existing test harness): `CommandRegistry` add/run,
  `HookBus` subscribe/emit, snippet tab-stop parsing, native marshaling
  (TuriValue → QString and back).
- **Integration test** (existing offscreen Qt smoke harness): load a test
  plugin, assert it registered a command, invoke it from the palette, assert
  the buffer changed. The control socket's `menu.invoke` can trigger
  `Cmd-P` → type → Enter programmatically.
- **Plugin host test**: eval a known Turmeric expression in-process, check
  the `TuriValue` result. This is the `tur_eval_basic` pattern, ported to
  Trowel's test runner.

---

## 11. Resolved questions

1. **Snippet tab-stop key binding — resolved: Qt event filter on the
  Scintilla widget.** `Tab` currently goes straight to Scintilla (no Qt-level
  interception; `setUseTabs(false)`, `setTabWidth(2)`). `EditorView` already
  has an `eventFilter`, but it only watches the rename `QLineEdit` for
  Escape. The plan: install an event filter on `sci_` (the `ScintillaEdit`
  widget). When `Key_Tab` arrives and a `SnippetSession` is active, consume
  the event (return `true`) and call `snippetAdvance()`. Otherwise let it
  fall through to Scintilla's default indent. This is simpler and lower
  latency than routing Tab through the command registry — a keystroke that
  needs to feel instant should not cross a hook dispatch boundary. The
  `command:run` veto hook stays useful for other key bindings (e.g. a plugin
  intercepting `Cmd+Shift+V`), but Tab is handled at the event-filter layer.
  The C++ side owns the interception; the Turmeric snippet plugin owns the
  logic (queries `trowel:snippet-active?`, calls `trowel:snippet-advance`).

2. **Plugin env vs. REPL env — resolved: stay separate.** The plugin host's
  in-process `TuriEnv` and the REPL's out-of-process `tur repl` are
  intentionally separate environments. The REPL is the user's scratchpad for
  their own code; plugins are editor extensions. A plugin that defines
  `(defmodule Snippets ...)` is not callable from the REPL by design. If a
  user wants to invoke a plugin function from the REPL, they use the command
  palette or `:cmd <id>`. If a plugin wants to push definitions into the
  REPL, `trowel:repl-send` lets it send arbitrary text (e.g. `(load
  "path/to/snippets.tur")`) — but this is explicit, not implicit namespace
  sharing.

3. **Syntax plugin lexer performance — non-issue for v1.** A data-driven
  lexer reads the Turmeric descriptor once at load time and compiles it into
  Scintilla keyword lists. The per-keystroke lexing happens in C++, not in
  Turmeric — the descriptor is configuration, not runtime logic. Performance
  is equivalent to the hardcoded scanners. The concern only arises if we
  later add programmatic lexers (arbitrary tokenization logic in Turmeric,
  called per-line), which is a future direction that would need
  benchmarking before committing.

4. **libturi ABI stability — covered by existing version pin.** Trowel
  already pins `TROWEL_TURMERIC_VERSION` and recompiles when it bumps (the
  `FetchContent` hash enforces it). Linking `libturi.a` statically means the
  same constraint applies — no new risk. The `turi/eval.h` API is the
  Turmeric project's public embedding surface (phase S0, install targets,
  exercised by `tur_eval_basic` and `tur_eval_sandbox`). If it changes, the
  `tur_eval_basic` test breaks first. The `tur` binary and `libturi.a`
  come from the same release archive, so they cannot drift.
