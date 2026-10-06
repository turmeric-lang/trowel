# Debugging

Trowel has a built-in debugger backed by `tur dap`, the Debug Adapter Protocol
server inside the bundled Turmeric toolchain. It supports breakpoints, stepping,
call stack inspection, variable evaluation, and — over a recording —
time-travel debugging that steps both forward and backward.

The debugger surfaces as a **Debugger tab** in the right-hand pane, beside the
REPL. It is not a separate window.

## Starting a session

| Command | Shortcut | What it does |
|---|---|---|
| **Run → Debug Buffer** | `F5` | Run the current file under `tur dap`, stopping at the first breakpoint (or at the end if none). |
| **Run → Time-Travel Debug** | `Ctrl+F5` | Record the run first, then replay it with stop-on-entry at step 0. |
| **Run → Restart Debug Session** | `Ctrl+Shift+F5` | Respawn `tur dap` with the same program and mode. A restart is a fresh process, not a rewind. |
| **Run → Toggle Breakpoint** | `F9` | Set or clear a breakpoint on the caret's line (or click the gutter). |

`F5` doubles as Continue: when a session is already paused, `F5` resumes it
instead of starting a new one.

### Requirements

- The file must be **saved** — `tur dap` reads from disk, and breakpoints bind
  by file path. An unsaved buffer is prompted to save.
- Debugging is only available for Turmeric files (any dialect). Scheme-only
  files are declined.
- One session per window. Starting a new session stops the previous one.

## Breakpoints

Click the gutter to toggle a breakpoint on a line, or use `F9`. Breakpoints
carry an optional **condition** (empty means unconditional): right-click a
breakpoint in the breakpoints pane to edit it.

A caveat: `tur dap` matches breakpoints by **basename**, so if two open buffers
share the same filename, a breakpoint in one silently applies to both. Trowel
warns about this in the breakpoints pane when it detects colliding basenames.

## The Debugger tab

The tab has four areas:

```
┌─────────────────────────────────────────────┐
│  Stepping toolbar                           │
├──────────────┬──────────────────────────────┤
│  Call stack  │  Variables                   │
├──────────────┴──────────────────────────────┤
│  Breakpoints                                 │
├─────────────────────────────────────────────┤
│  Output console                              │
│  turi> [evaluate line]                      │
└─────────────────────────────────────────────┘
```

- **Stepping toolbar** — Continue, Step Over, Step In, Step Out, Stop. In a
  replay session, reverse-execution buttons (Step Back, Reverse Step Over,
  Reverse Continue) appear as well.
- **Call stack** — the frames from the last `stopped` event. Clicking a frame
  re-issues scopes and variables for that frame and highlights it.
- **Variables** — the locals for the selected frame.
- **Breakpoints** — a view over the breakpoint model. Edit conditions, toggle,
  or remove breakpoints here.
- **Output console** — debuggee output from DAP `output` events, plus
  evaluation results. The `turi>` line at the bottom evaluates an expression
  against the selected frame.

### Stepping

| Action | Forward | Reverse (replay only) |
|---|---|---|
| Continue / resume | Continue | Reverse continue |
| Step over | Step Over | Reverse step over |
| Step in | Step In | — |
| Step out | Step Out | — |
| Step back | — | Step Back |

Stepping commands are no-ops unless the session is Paused.

### Evaluating expressions

Type an expression at the `turi>` prompt and press Enter. The result appears
in the output console, marked so it is not mistaken for program output.
Evaluation is refused in a replay session — there is no live frame to evaluate
against.

## Time-travel debugging

Time-Travel Debug records the run first (the same recording `tur trace`
produces — see [Tracing](tracing.md)), then replays it with stop-on-entry at
step 0. You can then step forward and backward through the recording, and the
output console rewinds with the cursor.

A timeline scrubber appears at the top of the Debugger tab when the `tur`
binary advertises the timeline extension. It is hidden otherwise — a scrubber
for a recording that does not exist would be misleading.

Reverse-execution buttons are hidden (not disabled) in a live session, because
the adapter only serves them from a recording. They appear when a replay
session is running.

### Restarting a replay

Because `tur dap` runs one program per session and has no `restart` request,
restarting a replay session means **re-recording** the program. Restart Debug
Session (`Ctrl+Shift+F5`) handles this automatically: it re-runs the recording
and drops you back at step 0.

## See also

- [Tracing](tracing.md) — record an execution trace without entering the
  debugger. The same recording feeds time-travel debugging.
- [Keyboard shortcuts](keyboard-shortcuts.md) — the debug shortcuts.
- [Scripting Trowel](scripting.md) — driving the debugger over the control
  socket (`debug.*` commands).
