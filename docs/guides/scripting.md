# Scripting Trowel

Trowel can be driven from an external process through a local **control
socket**: a Unix domain socket that speaks NDJSON (one JSON object per line,
both directions). It reproduces anything a keyboard/mouse user can do — focus a
pane, type text, press keys, open and save files, run the buffer, read editor
and REPL state — and can wait on events instead of sleeping.

This is the surface the smoke suite (`tests/smoke/`) uses, and the one to reach
for when you want to automate Trowel: scripted demos, editor automation,
external tooling. It is **not** the plugin API — plugins run in-process in
Turmeric (see [`docs/plans/plugin-system.md`](../plans/plugin-system.md)); the
control socket is for out-of-process automation.

The socket is **off by default**. There is no always-on port, and the API can
only trigger existing editor/REPL slots — it cannot `system()` or load code.
See [Safety](#safety).

## Enabling the socket

Pick one. On startup Trowel prints the resolved path to stdout as
`trowel-control-socket: <path>`, so a harness can pick it up without guessing.

| How | Effect |
|---|---|
| `--control-socket` | Enable, default per-pid path (see below). |
| `--control-socket-path=PATH` | Enable and listen at `PATH` (implies `--control-socket`). |
| `TROWEL_CONTROL_SOCKET=1` | Enable, default per-pid path. |
| `TROWEL_CONTROL_SOCKET=PATH` | Enable and listen at `PATH`. |

The default path lives under the writable runtime location
(`$XDG_RUNTIME_DIR` on Linux, otherwise the system temp dir), in a `trowel/`
subdirectory, named `ctl-<pid>.sock` — e.g.
`/run/user/1000/trowel/ctl-12345.sock`. There is one socket per running
instance; the pid in the name keeps concurrent instances (and the smoke suite)
from colliding.

> On non-macOS platforms a second launch normally forwards its files to the
> already-running instance and exits. That single-instance routing is
> **disabled** whenever an explicit control socket is requested, so each
> scripted run stays isolated. (macOS never uses it — LaunchServices delivers
> file-open events instead.)

## The protocol

Framing is NDJSON: one UTF-8 JSON object per line, `\n`-terminated, in both
directions. One in-flight request at a time per connection; multiple concurrent
connections are allowed. `id` is chosen by the client and echoed verbatim in
the reply.

Request:

```json
{"id": 1, "cmd": "editor.type", "args": {"text": "(def x 1)"}}
```

Response (success):

```json
{"id": 1, "ok": true, "result": {"cursor": {"line": 0, "col": 9}}}
```

Response (error):

```json
{"id": 1, "ok": false, "error": {"code": "no_editor", "message": "the active tab is not an editor"}}
```

All positions are 0-based. Editor positions are UTF-8 byte offsets; line/col
come straight from Scintilla's own APIs.

## Client tooling

You do not need a library — a socket and one line of JSON is enough — but two
helpers ship in the repo:

- `tests/support/trowel_ctl.py` — a tiny, dependency-free Python client:
  ```python
  from trowel_ctl import TrowelCtl, ControlError

  with TrowelCtl("/run/user/1000/trowel/ctl-12345.sock") as ctl:
      ctl.call("ping")
      ctl.call("editor.type", {"text": "(+ 1 2)"})
  ```
  `ctl.call(cmd, args)` returns `result` on success and raises `ControlError`
  (with `.code` and `.message`) on failure.

- `scripts/trowel-ctl` — a shell wrapper for poking a running instance by hand:
  ```
  scripts/trowel-ctl /run/user/1000/trowel/ctl-12345.sock editor.get_text
  scripts/trowel-ctl /run/user/1000/trowel/ctl-12345.sock editor.type '{"text":"hi"}'
  ```
  With no command, it reads `cmd [json-args]` lines from stdin — handy with
  `socat` or a pipe.

For a raw, dependency-free check:

```
echo '{"id":1,"cmd":"ping"}' | socat - UNIX-CONNECT:/run/user/1000/trowel/ctl-12345.sock
```

## Command surface

Commands are flat on the wire (`cmd: "editor.type"`). A few are
**registry-level** — `ping`, `window.new`, `window.list` — and work with zero
windows open. `editor.open` and `window.activate` create a window when none is
open; everything else targets the active window and fails with `no_window`
when there isn't one. `editor.*` commands additionally fail with `no_editor`
when the active tab is not an editor.

### Window & focus

| Command | Args | Result |
|---|---|---|
| `ping` | — | `{pong: true}` |
| `window.list` | — | `{count, windows: [{title, active, file_path, tabs, tab_count}]}` |
| `window.new` | — | `{count}` |
| `window.activate` | — | activates the target window |
| `window.focus` | `{pane: "editor"\|"terminal"\|"repl"}` | focuses a pane |
| `window.geometry` | `{width?, height?, x?, y?}` (optional, sets if present) | `{x, y, w, h, splitter, title, file_path}` |
| `window.set_splitter` | `{pos}` or `{sizes: [...]}` | sets the splitter |
| `window.drop` | `{paths: [...]}` | synthesizes a file drag/drop; `{accepted}` |
| `window.screenshot` | `{path}` | writes a PNG; `{path, width, height}` |

### Menu & commands

| Command | Args | Result |
|---|---|---|
| `menu.invoke` | `{path: ["Run", "Run Buffer"]}` | triggers the menu action |
| `menu.list` | — | `{actions: [...]}` (titles, shortcuts, enabled/checked state) |
| `command.run` | `{id}` | runs a registered command by id |

### Editor

| Command | Args | Result |
|---|---|---|
| `editor.open` | `{path}` | opens a file (creates a window if none) |
| `editor.save` | — | saves the current file |
| `editor.save_as` | `{path}` | saves to `path` |
| `editor.set_text` | `{text}` | replaces the whole buffer |
| `editor.type` | `{text}` | synthesized keystrokes (auto-indent, bracket-match fire) |
| `editor.press` | `{key, mods?: ["ctrl","shift","alt","meta"]}` | one keystroke |
| `editor.get_text` | — | `{text, modified, path}` (truncated over 4 MB) |
| `editor.get_cursor` | — | `{pos, line, col, anchor, selection: [start,end]}` |
| `editor.set_cursor` | `{pos}` or `{line, col}` | moves the caret |
| `editor.pos_from_linecol` | `{line, col}` | `{pos}` |
| `editor.get_selection` | — | `{text, start, end}` |
| `editor.set_selection` | `{start, end}` | sets the selection |
| `editor.get_style_at` | `{pos}` | style at a position |
| `editor.state` | — | `{path, wrap, ...}` |
| `editor.minimap` | `{show?}` | minimap state / toggle |
| `editor.folds` | — | fold structure |
| `editor.fold` | `{line, action: "toggle"\|"expand"\|"collapse"}` | folds a line |
| `editor.is_read_only` | — | read-only flag |
| `editor.rename_input` | — | rename-box input |
| `editor.begin_rename` | — | starts an LSP rename |
| `editor.call_tip` | — | call tip |

Key names for `editor.press` follow Qt's `Qt::Key` enum stripped of `Key_`
(`"return"`, `"backspace"`, `"tab"`, `"escape"`, `"up"`, `"home"`, `"delete"`,
`"pageup"`, ...). `mods` accepts `"ctrl"`, `"shift"`, `"alt"`, `"meta"` (or
`"cmd"`).

### Language server

All act on the active editor tab. See
[`docs/guides/`](.) and [`docs/plans/lsp-support.md`](../plans/lsp-support.md).

| Command | Args | Result |
|---|---|---|
| `lsp.status` | — | `{state, error, server_path, enabled}` |
| `lsp.diagnostics` | — | `{diagnostics: [...], count}` |
| `lsp.decorations` | — | squiggle ranges read back out of Scintilla |
| `lsp.completions` | `{pos?, timeout_ms?}` | `{labels, count}` |
| `lsp.hover` | `{pos?, timeout_ms?}` | `{text}` |
| `lsp.definition` | `{pos?}` | location |
| `lsp.symbols` | — | document symbols |
| `lsp.highlights` | `{pos?}` | highlight ranges |
| `lsp.references` | `{pos?}` | reference locations |
| `lsp.workspace_symbols` | `{query}` | workspace symbols |
| `lsp.signature_help` | `{pos?}` | signature info |
| `lsp.prepare_rename` | `{pos?}` | rename readiness |
| `lsp.rename` | `{new_name}` | performs the rename |
| `lsp.restart` | — | respawns `tur lsp` |

### REPL / terminal

| Command | Args | Result |
|---|---|---|
| `repl.send` | `{text, newline?: bool = true}` | writes to the PTY |
| `repl.press` | `{key, mods?}` | arrow keys, `Ctrl-C`, `Ctrl-D`, ... |
| `repl.get_screen` | `{lines?: int}` | last N lines of the terminal buffer |
| `repl.get_cursor` | — | `{line, col}` in the terminal grid |
| `repl.restart` | — | restarts the REPL |
| `repl.get_cwd` | — | `{cwd, running}` |
| `repl.set_cwd` | `{path}` | `{cwd}` (restarts the REPL in `path`) |
| `repl.is_running` | — | `{running, pid}` |
| `run.buffer` | — | Run → Run Buffer |
| `run.selection` | — | Run → Run Selection |

### Language & dialect

| Command | Args | Result |
|---|---|---|
| `lang.get` | — | current language |
| `lang.set` | `{lang}` | sets the language |
| `lang.set_session` | `{lang}` | sets the session dialect |
| `lang.menu` | — | dialect menu state |
| `lang.bases` | — | language bases |

### Find & document state

| Command | Args | Result |
|---|---|---|
| `find.state` | — | find-bar state |
| `find.set` | `{...}` | configures find |
| `find.replace` | `{...}` | replace |
| `doc_state.path` | — | per-file document-state path |

### Navigation

| Command | Args | Result |
|---|---|---|
| `nav.history` | — | navigation history |
| `nav.goto_definition` | — | jumps to definition |

### Trace

| Command | Args | Result |
|---|---|---|
| `trace.run` | `{...}` | runs a trace |

### Debugger

| Command | Args | Result |
|---|---|---|
| `debug.start` | `{...}` | starts a debug session |
| `debug.stop` | — | stops it |
| `debug.status` | — | `{state: "idle"\|"starting"\|"running"\|"paused"\|...}` |
| `debug.continue` | — | continues |
| `debug.step` | `{...}` | steps |
| `debug.breakpoint.toggle` | `{...}` | toggles a breakpoint |
| `debug.breakpoint.set` | `{...}` | sets a breakpoint |
| `debug.breakpoints` | — | breakpoint list |
| `debug.frames` | — | stack frames |
| `debug.variables` | `{...}` | variables |
| `debug.select_frame` | `{...}` | selects a frame |
| `debug.evaluate` | `{...}` | evaluates an expression |
| `debug.reverse_continue` | — | reverse-continues |
| `debug.timeline` | — | execution timeline |
| `debug.seek` | `{...}` | seeks in the timeline |
| `debug.sites` | `{...}` | debug sites |

### Waiters (events)

Waiters **block the response** until a condition is met or the timeout fires.
Every waiter takes an explicit `timeout_ms` — there is no server-side default.
While a waiter is yielded to the event loop, other commands can run on a second
connection; keep that in mind when you script against a shared instance.

| Command | Args | Resolves when |
|---|---|---|
| `wait.repl_output` | `{pattern, regex?: bool = false, timeout_ms}` | terminal buffer matches `pattern` |
| `wait.repl_idle` | `{quiet_ms = 200, timeout_ms}` | no new PTY bytes for `quiet_ms` |
| `wait.editor_signal` | `{signal: "modifiedChanged"\|"filePathChanged", timeout_ms}` | the editor signal fires |
| `wait.process_exit` | `{timeout_ms}` | the REPL child exits |
| `wait.diagnostics` | `{min_count = 1, max_count?, timeout_ms}` | the LSP publishes a batch in range (`max_count: 0` waits for diagnostics to clear) |

## A worked example

Open a file, type into it, run the buffer, and wait for the REPL to print the
result — the same shape the smoke tests use:

```python
import subprocess, sys
from pathlib import Path
sys.path.insert(0, "tests/support")
from trowel_ctl import TrowelCtl

# Launch Trowel with its own isolated socket.
sock = "/tmp/trowel-demo.sock"
proc = subprocess.Popen(["./build/trowel.app/Contents/MacOS/trowel",
                         f"--control-socket-path={sock}"])
try:
    with TrowelCtl(sock) as ctl:
        ctl.call("editor.open", {"path": str(Path("examples/hello.tur").resolve())})
        ctl.call("editor.set_cursor", {"line": 0, "col": 0})
        ctl.call("run.buffer")
        matched = ctl.call("wait.repl_output",
                           {"pattern": "hello", "timeout_ms": 5000})
        print("REPL printed:", matched["matched"])
finally:
    proc.terminate()
    proc.wait(timeout=5)
```

Prefer `editor.type` / `editor.press` over `editor.set_text` when you want the
real input path (auto-indent, bracket matching, snippet triggers) to fire, and
prefer `menu.invoke` or `command.run` over re-deriving a shortcut — they hit
the same slots the human UI does, so they break when the human UI breaks.

## Safety

- **Off by default.** No flag, no socket file, no listening code path.
- **Local only.** Unix domain socket, `chmod 0600`; the server refuses to start
  if the parent directory is world-writable. No TCP, no remote control.
- **No code execution.** The API triggers existing editor/REPL slots and
  widget events. It cannot `system()`, load libraries, or spawn arbitrary
  binaries. Adding such a command later requires an explicit opt-in flag so it
  never sneaks in.
- **Single trusted client.** No auth or ACLs — assume anything that can reach
  the socket can drive the editor. Keep the path private to your harness.

## Reference

- [`docs/plans/socket-api.md`](../plans/socket-api.md) — the design and
  rationale behind the control socket.
- [`docs/plans/smoke-tests.md`](../plans/smoke-tests.md) — the smoke suite that
  exercises it end to end.
- `tests/smoke/conftest.py` — a real launch-and-drive harness to copy from.
- `src/control/control_handlers.cpp` — the authoritative command list; if this
  guide and the source disagree, the source wins.
