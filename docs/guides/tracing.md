# Tracing

Trowel can record an execution trace of the current file with `tur trace`,
then explain what the recording means. Tracing is the first half of
time-travel debugging — it records the run; the debugger (see
[Debugging](debugging.md)) can then replay it in both directions.

## How to run it

**Run → Trace Buffer.** The file must be saved first — `tur trace` reads from
disk, so an unsaved edit would trace the previous version. If the buffer is
modified, Trowel prompts to save.

Tracing is only available for Turmeric files (any dialect). Scheme-only files
that are not also Turmeric are declined.

## What you see

The trace output appears in the REPL pane — the same terminal you already
watch. When the trace finishes, the status bar shows a one-sentence summary of
the outcome, not a bare step count. A bare count is misleading on its own (see
below), so Trowel explains the recording rather than printing a number.

## The five outcomes

| Outcome | What it means |
|---|---|
| **Failed** | `tur trace` could not start, or no toolchain was found. |
| **Compile error** | The file does not compile. Fix the errors and trace again. |
| **Nothing to run** | The file has no executable content (e.g. only comments). Add an expression or a `(defn main [] …)` and trace again. |
| **Short recording** | The program ran but evaluated very little. The explanation distinguishes two causes (below). |
| **Recorded** | A real recording. The summary reports step count, peak call depth, and whether it hit the step cap. |

### Why a short recording is usually not about the program

A low step count is routinely an artifact of how the source is punctuated, not
of what the program does. This depends on which `tur` you are running:

- **Per-line granularity** (older `tur`): the recorder counts one step per
  source *line*. A form written on a single line collapses to one step however
  much it evaluates — a one-line loop records its counter jumping straight to
  its final value. Reformatting the same code across lines gives a fuller
  recording.
- **Per-expression granularity** (newer `tur`): the recorder counts one step
  per expression, and the summary line says so. A short recording here means
  the program genuinely evaluated very little.

Trowel reads the granularity from the summary line and tailors the
explanation, so you do not have to know which `tur` you have.

## After the trace

The recording is written to a temp directory beside the source file. Trowel
keeps the path of the last recording so a replay session can open it without
re-running. To step through a recording in both directions, use
**Run → Time-Travel Debug** (see [Debugging](debugging.md)).

## See also

- [Debugging](debugging.md) — time-travel debugging replays a recording
  forward and backward.
- [Settings](settings.md) — `turmeric.path` to override the `tur` binary.
