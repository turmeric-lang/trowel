# Trowel Control Socket — Plan

> **Status:** Complete. `ControlServer`, `ControlConnection`, and
> `ControlHandlers` are live (`src/control/`). The command surface has
> grown well beyond the original plan — `window.drop`, `window.list`,
> `lsp.*`, `debug.*`, `repl.get_cwd` / `repl.set_cwd`, and more. NDJSON
> framing over a Unix domain socket, off by default.
> **Related:** [`smoke-tests.md`](smoke-tests.md) (complete, drives the
> socket).

A small local IPC surface for driving a running Trowel instance from an
external process. Intended for automated smoke tests (see
[smoke-tests.md](smoke-tests.md)), scripted demos, and eventual editor
automation (macros, external tooling). Not intended as a plugin API or as a
network-exposed control channel.

The socket sits **beside** the existing Qt event loop and mutates the same
widgets a user would. A command like `type` posts synthetic
`QKeyEvent`s to the focused widget; `run_buffer` invokes the same slot as the
`⌘R` menu action. This keeps the API and the human UI in lock-step by
construction — smoke tests fail when real interaction fails.

---

## 1. Goals

- Reproduce any interaction a keyboard/mouse user can perform: focus a pane,
  type text, press keys/chords, click menu items, open/save files, resize the
  splitter.
- Observe state without racing: buffer contents, cursor position, selection,
  terminal screen contents, window/menu state.
- Wait on events (REPL prompt returned, buffer loaded, process exited) rather
  than sleeping.
- Zero dependencies for the client — one Unix domain socket, one line-based
  request/response frame, JSON payloads.
- Off by default; enabled per-launch by a flag or env var. No always-on port.

### Non-goals (v1)
- Windows named-pipe transport (mirrors the platform priority in `PLAN.md §1`).
- TCP / remote control. Local sockets only.
- Auth, ACLs, multi-tenant sessions. Single trusted local client.
- Plugin loading, arbitrary code execution inside Trowel.
- Snapshotting/screenshotting the rendered widgets (image diffing is a
  separate, later concern).

---

## 2. Transport

- **Unix domain socket** at
  `$XDG_RUNTIME_DIR/trowel/ctl-<pid>.sock` (Linux) or
  `$TMPDIR/trowel-ctl-<pid>.sock` (macOS). Symlink
  `$TMPDIR/trowel-ctl.sock` → the most recent instance for convenience.
- Enabled by `--control-socket[=PATH]` CLI flag or `TROWEL_CONTROL_SOCKET=1`.
  Prints the resolved path to stdout on startup so test harnesses can pick it
  up without guessing.
- Framing: **NDJSON** — one JSON object per line, `\n`-terminated, both
  directions. Chose NDJSON over length-prefixed framing because the protocol
  is human-debuggable (`socat - UNIX-CONNECT:...`) and the payloads are small.
- One in-flight request at a time per connection; multiple concurrent
  connections allowed (server multiplexes onto the Qt thread — see §5).

---

## 3. Message shape

Request:
```json
{"id": 42, "cmd": "type", "args": {"target": "editor", "text": "(def x 1)"}}
```

Response (success):
```json
{"id": 42, "ok": true, "result": {"cursor": {"line": 0, "col": 9}}}
```

Response (error):
```json
{"id": 42, "ok": false, "error": {"code": "no_focus_target", "message": "no widget named 'editor'"}}
```

Async event (unsolicited, only for subscriptions — see §4.5):
```json
{"event": "repl.prompt", "data": {"prompt": "> "}}
```

- `id` is echoed verbatim; the client picks it. No server-generated ids.
- `error.code` values are a closed set (documented per command). Free-form
  `message` is for humans.
- All positions are 0-based, UTF-8 byte-offsets and (line, col) computed from
  Scintilla's own APIs — do not reimplement.

---

## 4. Command surface

Grouped for readability; the wire protocol is flat (`cmd: "editor.type"` etc).

### 4.1 Focus & window
- `window.focus {pane: "editor"|"terminal"}` — same as `⌘E` / `⌘T`.
- `window.geometry` → `{x,y,w,h,splitter: <int>}`.
- `window.set_splitter {pos: <int>}` — drag the splitter to a pixel position.
- `window.new` → `{count}` — same as File → New Window.
- `window.drop {paths: [...]}` → `{accepted}` — synthesize a drag/drop of
  local files onto the window, exercising the real drop handlers.
- `window.list` → `{count, windows: [{title, active, file_path, tabs,
  tab_count}]}`.

Targets are resolved per command, so a long-lived connection follows the
user as windows open and close:

- `ping`, `window.new`, `window.list` are registry-level — they work with
  zero windows open and never create one to answer.
- `editor.open` and `window.activate` create a window when none is open
  (rule (a) in [`multi-window.md`](multi-window.md)).
- Everything else targets the active window and fails with `no_window`
  when there isn't one, rather than conjuring a blank window.

`editor.*` commands additionally fail with `no_editor` when the active
tab is not an editor (a directory browser or the preferences pane).

On macOS the app outlives its windows, so the zero-window state is real
and reachable; elsewhere the process exits with its last window.
- `menu.invoke {path: ["Run", "Run Buffer"]}` — resolve a `QAction` by its
  menu path and `trigger()` it. Preferred over duplicating shortcuts.

### 4.2 Editor
- `editor.open {path}` → same as File → Open.
- `editor.save` / `editor.save_as {path}`.
- `editor.set_text {text}` — replace the whole buffer.
- `editor.type {text}` — synthesized keystrokes; goes through Scintilla so
  auto-indent, bracket-match, etc. fire.
- `editor.press {key, mods?: ["ctrl","shift","alt","meta"]}` — one keystroke
  (e.g. `{"key":"Enter"}`, `{"key":"a","mods":["meta"]}`). Key names follow
  Qt's `Qt::Key` enum stripped of `Key_`.
- `editor.get_text` → `{text, modified: bool, path: string|null}`.
- `editor.get_cursor` → `{pos, line, col, anchor, selection: [start,end]}`.
- `editor.set_cursor {pos?, line?, col?, anchor?}`.
- `editor.get_selection` → `{text, start, end}`.
- `editor.set_selection {start, end}`.

### 4.2b Language server
All of these act on the **active editor tab**. See
[`lsp-support.md`](lsp-support.md).

- `lsp.status` → `{state: "disabled"|"stopped"|"starting"|"ready"|"failed",
  error, server_path, enabled}`.
- `lsp.diagnostics` → `{diagnostics: [{severity, start_line, start_char,
  end_line, end_char, message, source}], count}` — the model's current batch.
- `lsp.decorations` → `{error_ranges, warning_ranges, error_marker_lines,
  warning_marker_lines}` — read back out of **Scintilla**, not out of the
  model. Distinguishes "a diagnostic exists" from "a squiggle is painted".
- `lsp.completions {pos?, timeout_ms?}` → `{labels: [string], count}`.
  Defaults to the caret.
- `lsp.hover {pos?, timeout_ms?}` → `{text}`.
- `lsp.restart` — respawns `tur lsp` and drops stale diagnostics.

### 4.3 REPL / terminal
- `repl.send {text, newline?: bool = true}` — same code path as user typing
  into the terminal pane; goes through the PTY.
- `repl.press {key, mods?}` — for arrow keys, `Ctrl-C`, `Ctrl-D`, etc.
- `repl.get_screen {lines?: <int>}` → last N lines of the terminal buffer
  (post-CSI-stripping, matching what the user sees).
- `repl.get_cursor` → `{line, col}` inside the terminal grid.
- `repl.restart` — Run → Restart REPL.
- `repl.is_running` → `{running: bool, pid: <int>|null}`.
- `repl.get_cwd` → `{cwd, running}` — where this window's REPL is rooted.
- `repl.set_cwd {path}` → `{cwd}` — restart the REPL in `path`. Errors with
  `bad_path` for anything that is not an existing directory, leaving the
  running REPL alone. The restart is unavoidable: a live process cannot be
  moved. See [`repl-working-directory.md`](repl-working-directory.md).
- `run.buffer` — Run → Run Buffer (same slot as `MainWindow::runBuffer`).
- `run.selection` — same as `runSelection`.

### 4.4 State inspection
- `state.dump` → union of `window.geometry`, `editor.get_cursor`,
  `editor.get_text` (elided over 64KB), `repl.is_running`,
  `repl.get_screen {lines: 40}`. Used by tests when an assertion fails, to
  attach context to the report.

### 4.5 Waiters (events)
Two shapes: **wait** (blocks the response until condition met or timeout),
**subscribe** (fires async events until unsubscribed).

- `wait.repl_output {pattern, regex?: bool = false, timeout_ms}`
  → resolves when the terminal buffer matches. Returns
  `{matched: string, since_last_ms: <int>}`.
- `wait.repl_idle {quiet_ms = 200, timeout_ms}` — no new PTY bytes for
  `quiet_ms`. Used to wait out a `(load ...)` before asserting.
- `wait.editor_signal {signal: "modifiedChanged"|"filePathChanged", timeout_ms}`.
- `wait.process_exit {timeout_ms}` — REPL child exited.
- `wait.diagnostics {min_count = 1, max_count = ∞, timeout_ms}` — resolves once
  the language server has published a batch for the active buffer whose size is
  within the bounds. Returns the same shape as `lsp.diagnostics`. `max_count: 0`
  is how you wait for diagnostics to **clear**; a bare `min_count: 0` would be
  satisfied by the stale batch that is already there. Errors with `no_uri` for
  an unsaved buffer, which gets no language support at all.
- `subscribe {events: [...]}` / `unsubscribe {events: [...]}` — begins
  streaming `{"event": ..., "data": {...}}` frames on the same connection.

Every waiter takes an explicit `timeout_ms`. There is no server-side default.

---

## 5. Server implementation

- `src/control/control_server.{h,cpp}` — owns a `QLocalServer`; on each
  incoming `QLocalSocket` spawns a `ControlConnection` object parented to the
  main thread.
- All command handlers run on the Qt main thread (they mutate widgets), so
  no locking is needed. `QLocalSocket::readyRead` naturally queues work into
  the event loop.
- Command dispatch: `QHash<QString, Handler>` populated at construction.
  Handlers get a reference to `MainWindow` and return a `QJsonValue` or throw
  a `ControlError{code, message}` — the connection layer marshals both.
- Synthetic input:
  - Editor: build `QKeyEvent` with the parsed key/mods, `QApplication::sendEvent`
    to the Scintilla widget. For `editor.type`, chunk into a `QKeyEvent` per
    grapheme so IME-like behavior isn't required.
  - Terminal: write straight to the PTY master via
    `PtySession::write(QByteArray)`. Key names like `"Up"` map to the same
    escape sequences `TerminalView` already sends on user keystroke — reuse
    that mapping, don't duplicate.
- Menu invocation: walk `MainWindow::menuBar()` matching titles, call
  `QAction::trigger()`. Errors if the action is disabled.
- Waiters: implemented with `QMetaObject::invokeMethod` + a `QTimer` for the
  timeout. `wait.repl_output` hooks into the same signal that already routes
  PTY bytes into the terminal buffer — pattern-check on each chunk, resolve
  or keep waiting.

---

## 6. Client tooling

- Ship a tiny Python client at `tests/support/trowel_ctl.py` — the socket
  path, one class with methods for each command, timeouts wired through.
  No third-party deps.
- Also useful standalone: `scripts/trowel-ctl` shell script wrapping
  `socat` so a human can poke the running instance:
  `echo '{"id":1,"cmd":"editor.get_text"}' | trowel-ctl`.

---

## 7. Milestones

**S0 — Server skeleton (1 day).** `--control-socket` flag, `QLocalServer`,
NDJSON framing, `id`/`ok`/`error` shape verified end-to-end with a `ping`
command. Python client stub.

**S1 — Editor & menu commands (1–2 days).** Focus, menu.invoke,
editor.{get_text,set_text,type,press,get_cursor,set_cursor,get_selection,
set_selection}, editor.open/save. Enough for typing tests.

**S2 — REPL commands (1–2 days).** repl.send/press/get_screen/restart,
run.buffer, run.selection. Reuse the existing PTY write path and terminal
screen buffer — do not fork a second reader.

**S3 — Waiters (1 day).** wait.repl_output, wait.repl_idle,
wait.editor_signal, wait.process_exit. This is what unblocks the smoke tests
described in [smoke-tests.md](smoke-tests.md).

**S4 — Subscribe/unsubscribe + state.dump (0.5 day).** Nice-to-have; land
after the smoke suite has shown what's actually painful without it.

---

## 8. Safety & footguns

- **Off by default.** Absence of the flag means no server, no socket file,
  no code path exercised in normal use.
- **Permission on the socket.** `chmod 0600` on creation; refuse to start if
  the parent directory is world-writable.
- **No `eval`-like commands.** The API can only trigger existing MainWindow
  slots and widget events. It cannot load DLLs, `system()`, or `QProcess`
  arbitrary binaries. Adding such a command later requires an opt-in flag
  (`--control-socket-allow-exec`) so it never sneaks in.
- **Reentrancy.** Handlers run on the Qt thread. `wait.*` yields to the event
  loop; other commands can then run on a second connection. Document this,
  don't paper over it — test authors need to know.
- **Buffer size caps.** `editor.get_text` returns up to 4 MB; larger buffers
  return `{"text_truncated": true, "size": N, "sha256": ...}`. Terminal
  screen fetch is capped at 500 lines.

---

## 9. Future directions

- **Screenshot command.** `window.screenshot {pane?} -> {png_base64}` for
  golden-image tests, once the API is stable and we care.
- **Record/replay.** Log the request/response stream to a file, replay it as
  a scripted demo. The NDJSON transport is already replay-friendly.
- **DAP transport reuse.** When `tur dap` support lands (`PLAN.md §11`), the
  control socket connection layer can host it as a second protocol on a
  second socket — same server infrastructure.
- **Windows named pipe.** Follows if/when Windows support does.
