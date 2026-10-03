# REPL working directory — plan

Give the user explicit control over, and visibility into, the working
directory of a window's `tur repl` session: an indicator showing where
the REPL is rooted, a GUI command to change it, and a `:cd`-style
meta-command in the REPL itself.

Independent of [`multi-window.md`](multi-window.md) — it can land before
or after. The two compose cleanly: each `MainWindow` owns its own
`ReplSession`, so everything here is inherently per-window.

## Background: how the cwd works today

- `MainWindow::replWorkingDir()` returns the active file's directory, or
  `$HOME` when there is no file.
- It is consulted at exactly two moments: `ReplSession::start()` on
  window construction, and `ReplSession::restart()` from the **Restart
  REPL** action. It does not chase tab switches — deliberately, since a
  REPL that silently `cd`s underneath you breaks relative paths and
  in-flight work.
- `ReplSession` already stores `lastWorkingDir_`, but does not expose
  it.
- `PtySession::start()` takes the working directory and applies it in
  the child before `exec`.

**The load-bearing constraint:** you cannot change a running process's
cwd from outside it. So any Trowel-side "set working directory" is
necessarily a **restart** of the REPL, which clears its state. That is
not a workaround to be papered over — it should be visible in the UI
labeling. It also means the genuinely *live* version of this feature can
only be implemented inside `tur repl`, which shapes the staging below.

Two pieces of existing machinery make this cheap:

- `TerminalView::showBanner(QString)` injects a display line into the
  terminal **without** going through the PTY — so the indicator costs
  almost nothing.
- `tur repl` **already emits OSC escape sequences that Trowel
  consumes**: `ReplSession` scans for OSC 133;A prompt markers to drive
  `isBusy()`, including a `scanTail_` for sequences split across reads.
  So there is precedent — and a parser — for the REPL reporting state to
  the editor out-of-band.

## B1 — CWD indicator ✅ Done

- `ReplSession::workingDir()` exposes the existing `lastWorkingDir_`, so
  the indicator and the commands below share one source of truth.
- The startup banner now names the directory, with `$HOME` abbreviated
  to `~` the way a shell prompt would:

  ```
  [trowel] tur repl started in ~/projects/foo
  ```

**Deviation from the plan:** rather than adding a separate `▸ cwd:` line,
this extends the banner `ReplSession::onStarted()` already emitted. One
line instead of two, and it matches the surrounding `[trowel] …` house
style (`restarting REPL…`, `repl exited with code …`) instead of
introducing a second visual idiom. `restart()` routes through
`onStarted()`, so re-rooting reports itself for free.

A blank window shows `~`; a window opened on a file shows that file's
directory — which is the per-window REPL rule from
[`multi-window.md`](multi-window.md) made visible.

## B2 — GUI command ✅ Done

**Run ▸ Restart REPL In…** opens `QFileDialog::getExistingDirectory`
starting at the REPL's current directory (falling back to the active
file's), then restarts there. Per-window, like the REPL itself. The
label says "Restart" rather than "Set" so the state-clearing is honest —
"Set REPL Directory…" would imply a live `cd` that is not happening.

**Only one action, not two.** The plan's second entry, "Restart REPL in
Current File's Directory", turned out to already exist: `restartRepl()`
calls `repl_->restart(replWorkingDir())`, which *is* the active file's
directory. Adding it would have shipped two menu entries doing the same
thing. Instead the existing **Restart REPL** kept its behavior and got a
tooltip that says what it does ("Restart the REPL in the current file's
directory"), which it never advertised.

Control commands `repl.get_cwd` → `{cwd, running}` and `repl.set_cwd
{path}` → `{cwd}` back the smoke tests. `set_cwd` rejects a
non-directory with `bad_path` and leaves the running REPL untouched.

### The directory is not sticky

An explicit `set_cwd` lasts until the next plain **Restart REPL**, which
re-roots to the active file's directory as documented. So the rule stays
"Restart REPL puts you where the file is", and Restart REPL In… is a
one-off override rather than a mode.

That is the conservative reading — it introduces no new state and keeps
the shipped user guide true. The alternative (a sticky per-window
override that plain Restart REPL preserves) is arguably friendlier for
someone who deliberately chose a scratch directory and then needs to
clear a wedged REPL, and it is close to the "persisted override" listed
under Non-goals. Worth revisiting if the one-off behavior annoys in
practice.

## B3 — REPL meta-command

A `:cd` command *feels* like it belongs in Trowel, but the mechanics
push the other way. Two options were considered.

### Option A — intercept in Trowel (rejected as the primary path)

`TerminalView::keyPressEvent` would shadow-buffer the typed line and, on
Enter while idle, match `:cd <path>`, consume it, and restart.

This is fragile. Terminal input goes straight to the PTY, where `tur`'s
libedit does the line editing — history recall with ↑, `Ctrl-U`,
mid-line cursor movement, and so on. Trowel's shadow buffer would have
to stay in sync with all of it, and any desync means either swallowing a
real command or missing the meta-command. That is a bad trade for a
feature whose whole point is being simple and predictable.

### Option B — implement in `tur repl`, Trowel listens (recommended)

The REPL owns its own `:cd` / `:pwd`, alongside the `:type`, `:doc`, and
`:explain` meta-commands it already has. The payoff is real: being
in-process, it can `chdir()` **live, with no restart**, so REPL state
survives. Trowel stays out of the input path entirely.

Pair it with the REPL emitting **OSC 7** — the standard "report current
working directory" escape, already understood by most terminals — on any
cwd change. Trowel is parsing tur's OSC stream anyway, so this slots
into `ReplSession`'s existing scanner.

This makes B1's indicator *live* rather than start-only, and keeps
`repl.get_cwd` honest, with no shadow buffer anywhere.

### Staging ✅ Both done

- **B3a (Trowel).** `ReplSession` scans for OSC 7 and updates the cached
  cwd, announcing the move with a banner and keeping `repl.get_cwd`
  honest. Inert when nothing emits it.
- **B3b (Turmeric).** `:cd` / `:pwd` in `tur repl`, emitting OSC 7 on
  change and at startup —
  [turmeric#738](https://github.com/turmeric-lang/turmeric/pull/738).

Option B proved out end-to-end: `:cd` moves the live REPL, Trowel follows
the directory, the process id is unchanged, and a value defined before
the `:cd` is still bound after it. That is the payoff over B2's restart.

**Two implementation notes.**

The OSC 7 scan is *separate* from the existing OSC 133 prompt-marker
scan rather than folded into one generic OSC parser. The two carry very
different payloads — a 7-byte marker versus a full path — and the
prompt-marker path is load-bearing for busy/idle tracking with no direct
test coverage. Rewriting it to accommodate a longer carry buffer was not
worth the risk; the duplication is a few lines.

`displayPath()` had to learn to compare canonically. The REPL reports
`getcwd()`, which resolves symlinks (`/private/var/…` on macOS), while
`QDir::homePath()` does not — so the literal prefix test missed and the
banner printed the home directory in full. Caught by the end-to-end
test, not by reading.

## Milestones

1. **B1** — `workingDir()` accessor + banner line on start/restart.
2. **B2** — the two Run-menu actions; `repl.set_cwd` / `repl.get_cwd`
   control commands; smoke coverage.
3. **B3a** — OSC 7 consumption, live indicator updates.
4. **B3b** — upstream `:cd` / `:pwd` in `tur repl` (Turmeric repo).

## Non-goals

- Changing the cwd of a running REPL from Trowel's side. Not possible;
  B3b is the answer.
- Auto-following the active tab's directory. Explicitly rejected — see
  the predictability argument in `multi-window.md`.
- A persisted per-window cwd override that survives restart. The cwd is
  derived from the file or set explicitly per session; adding a sticky
  override is a separate decision.
- Shell-style `cd -`, `~user` expansion, or path completion in the GUI
  dialog.

## Future

- A clickable cwd indicator in the window chrome (status bar or REPL
  header) rather than a banner line, once there is a natural home for
  it.
- Project-root detection (nearest VCS root or manifest) as the default
  offered in the directory picker.
- Remembering the last explicitly-set cwd per window across sessions,
  if the derived-from-file default proves insufficient.
