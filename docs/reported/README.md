# Reported issues

Two kinds of report live here: defects found in the **upstream Turmeric
toolchain** while bringing Trowel onto a new pin, and **test failures** found
in Trowel's own smoke suite. Both are local records of what was measured; the
individual report files hold the detail.

---

## Upstream reports — filed, fixed, and two of them wrong

Defects in the Turmeric toolchain found while bringing Trowel onto `v0.60.1`.

**All six were filed as [turmeric-lang/turmeric#1063][pr1063] (merged
2026-10-03) and fixed in [#1064][pr1064], released as `v0.61.0`.** The report
bodies here are kept as the local record of what was measured and of what was
got wrong; upstream's rewritten versions live in its `docs/archive/`.

[pr1063]: https://github.com/turmeric-lang/turmeric/pull/1063
[pr1064]: https://github.com/turmeric-lang/turmeric/pull/1064

### Two were misdiagnosed, both mine

**`run-on-sweet-file-drops-its-definitions` — the definitions were never
dropped.** `:run` on a file whose reader comes from its extension applies that
reader to the *session*, so a Turmeric prompt reads sweet-exp afterwards. A bare
`both-x` was then an *incomplete* sweet expression waiting for its terminating
blank line, and the REPL answered `(cancelled)`. I read that as "unbound".
Re-measured: `(+ both-x 1)` gives `=> 8`, and `both-x` followed by a blank line
gives `=> 7`. The real bug was the session reader changing, which is what
upstream fixed.

*Lesson, and it is the same one three times in this effort: absence of the
output I expected is not the presence of the failure I assumed. The control I
skipped — the same probe on a `.tur` file — would have shown the reader
difference immediately.*

**`signature-help-declines-at-its-own-trigger-character` — my suggested fix
could not work.** I argued the server should answer at the `(` because "the
callee is already known at that point". In a lisp it is not: the callee is typed
*after* the paren, and `lsp_enclosing_call` reads only up to the cursor. My
cost argument against a space trigger was real but avoidable — signature help
did not need to be in the flush-before-answering group at all. Upstream moved
the trigger to `" "` and took it out of that group.

### The four that held as filed

| Report | Upstream root cause |
|---|---|
| [lsp-ignores-the-file-extension](lsp-ignores-the-file-extension.md) | `run_doc_analysis` wrote the buffer to `tur_lsp_XXXXXX.tur` — the compiler *does* read the extension, but the extension it saw was always `.tur`. Scratch file now takes the document's suffix. |
| [fmt-reprints-sweet-as-s-expressions](fmt-reprints-sweet-as-s-expressions.md) | `fmt_format_buffer` kept text as written only for `READER_R7RS[_SWEET]`; `READER_SWEET` fell through to `fmt_print`. Sweet now shares that branch. (Upstream's correction to my filing: the old output still *ran* — a silent change of syntax, not corruption.) |
| [format-subcommand-shreds-a-sweet-buffer](format-subcommand-shreds-a-sweet-buffer.md) | `cmd_format` hard-coded `READER_TURMERIC` even for file paths, so `.scm` and `#lang` files were affected too — wider than I filed. Now resolves `--lang`, then extension, then the `#lang` line. |
| [lang-flags-take-different-vocabularies](lang-flags-take-different-vocabularies.md) | Two separate name tables. Both `--lang` flags now fall back to `lang_base_lookup`, so every `tur dialects` row is accepted. |

### Two new reports upstream found on the way

- `lsp-relative-load-resolves-against-scratch-dir` (medium) — a relative
  `(load "b.tur")` fails under `tur lsp` but works in `tur check`.
- `lsp-publishes-other-files-diagnostics-under-one-uri` (low-medium) — the
  "secondary observation" I filed as *latent* is **live**: a loaded file's error
  is drawn on the open buffer. I under-called it; it needed a repro, not a
  caveat.

### Trowel-side follow-ups — done

The pin is on `v0.61.0` and every workaround these reports justified is gone:
`LspManager::shiftedText` and its line shift, `DialectIsFormattable`, and the
separate `DialectFmtLangFlag` vocabulary. Signature help auto-triggers on space.
The smoke test that encoded the `:run` misdiagnosis is deleted and replaced by
one asserting the definitions persist — which they always did.

Each deletion was gated on re-verifying the fix against the newly pinned binary
rather than on the fix being reported, because three of these workarounds
originally outlived their bugs by several releases. See the status block in
`docs/plans/dialects-saffron-and-r7rs.md` for the per-item evidence.

### A diagnostic on the `(load ...)` line is correct, not a bug

I got this one wrong twice, so it is worth stating plainly.

I filed the "secondary observation" in the LSP report as a latent bug: a
`publishDiagnostics` carrying another file's diagnostics under the open
document's URI. Upstream filed it as
`lsp-publishes-other-files-diagnostics-under-one-uri`, and I then "reproduced"
it against v0.61.0 and reported it as live.

**It was not the bug. It was the fix.** `tur lsp` analyses one document at a
time, so each analysis owns only that document's diagnostics, and `load` is
textual inclusion. The model is clangd's: an error from an included header is
shown on the `#include` line ("In included file: ...") with a note pointing into
the header. v0.61.0 does exactly that:

- the range is on the `"b.tur"` STRING in the load form (column 6), not at
  b.tur's own column (19), which is what the old bug used;
- `relatedInformation` points at b.tur's URI and the real span;
- the message is prefixed `in b.tur:1:20:`.

My probe printed only the line, the `file` key and the message — and both the
bug and the fix land on line 0 in my repro, so it could not tell them apart.
The discriminating evidence (the message prefix) was in my own output and I
read past it. The mistake was not insufficient caution; it was checking against
"is there a diagnostic on a.tur", which BOTH outcomes satisfy. Decide what
would distinguish the two BEFORE running the probe.

Two shapes I proposed as the "correct fix" are both wrong, for reasons worth
keeping: publishing under b.tur's URI would have a.tur's analysis and b.tur's
own analysis overwriting each other's diagnostics whenever both are open (that
shape belongs to whole-project servers like rust-analyzer or gopls); and
sending nothing for a.tur would show it clean while it loads code that does not
compile.

**Do not filter on the `file` key** — that hides a real error in the load.

#### What Trowel does now — done

Rendered as an error in a DEPENDENCY, with the related location as a jump:

1. `initialize` declares `relatedInformation: true`. It declared **false**, so
   Trowel was telling the server it could not use the note — and the server's
   one actionable field went unsent and unread.
2. `LspDiagnostic` carries `related` (the spans) and `relatedMessages`
   (the server's note per span), plus `isFromDependency()`.
3. The status bar prefixes such a diagnostic
   `Error in a dependency — in b.tur:1:24: … (Shift+F12 to open it)`, and
   **Run > Go to Diagnostic Source** (Shift+F12) jumps to the real site through
   the existing `jumpToSpan` and nav history. Its own action rather than a case
   inside Go to Definition: F12 sometimes meaning "jump to an error in another
   file" would be a surprise, and the two have different preconditions.

Four smoke tests in `test_lsp.py`. Note what they had to work around: a
RELATIVE `(load "b.tur")` under `tur lsp` resolves against the server's scratch
directory, so it fails `load: cannot open ...` and never reaches the dependency
case — the other open report,
`lsp-relative-load-resolves-against-scratch-dir`. The first draft of these
tests used a relative load and one of them **passed anyway**, asserting a
"cannot open" diagnostic while claiming to test a dependency error. They now
load by absolute path and assert the `in <file>:<line>:<col>:` prefix first, so
they cannot pass on the wrong diagnostic.

Upstream notes one gap still to come: a file loaded BY a loaded file falls back
to line 0 of the open document, and a follow-up will move it onto the open
document's own load line and add `(via mid.tur)` to the message. Trowel needs
no change for that — it renders whatever range and related location arrive.

---

## Reported test failures

Failures in the smoke suite found during the minimap work. All are
pre-existing — they reproduce on the clean tree with the minimap changes
stashed — and none are caused by the minimap changes.

All five were reproduced 2026-10-05 against `main` (9616c3a) and then fixed.
See each report's **Status** / **Fixed** lines for details.

### Summary

| Report | Test | Category | Status |
|---|---|---|---|
| [step-back-lands-on-the-same-line-in-suite-context](step-back-lands-on-the-same-line-in-suite-context.md) | `test_debugger.py::test_step_back_moves_the_cursor_backwards` | flaky (timing) | fixed |
| [restart-stop-count-race-in-suite-context](restart-stop-count-race-in-suite-context.md) | `test_debugger.py::test_restart_respawns_the_same_program` | flaky (timing) | fixed |
| [settings-json-not-created-on-startup](settings-json-not-created-on-startup.md) | `test_settings.py::test_live_reload_rainbow_off` | pre-existing (product gap) | fixed |
| [broken-json-test-settings-json-missing](broken-json-test-settings-json-missing.md) | `test_settings.py::test_broken_json_keeps_last_values` | pre-existing (product gap) | fixed |
| [qsettings-value-migrated-on-first-launch](qsettings-value-migrated-on-first-launch.md) | `test_settings.py::test_qsettings_values_not_read` | pre-existing (test premise) | fixed |

### The debugger tests are flaky, not broken

Both debugger tests pass in isolation and fail when run as part of the full
`test_debugger.py` suite. They share one root cause: `DebugSession::onStopped`
sets `state = Paused` synchronously but increments `stop_count` (and populates
frames) inside an async `stackTrace` callback. A test that reads `stop_count`
or `frames` right after `_wait_state("paused")` returns can see stale or
empty data. The fix in both cases is to wait for `stop_count` to actually
increment before reading — `_step` polls `debug.frames` until non-empty, and
`_wait_state` now accepts a `min_stop_count` parameter.

### The settings tests share one product gap

Two of the three settings tests crash with `FileNotFoundError` because the
app does not create `settings.json` during normal startup.
`Settings::ensureFileExists()` is called only from `MainWindow::openSettings()`,
not from the startup path. Calling it during startup fixes both.

The third settings test, `test_qsettings_values_not_read`, fails because
`migrateFromQSettings()` reads QSettings on first launch (when `settings.json`
does not exist) and writes `editor.rainbowBrackets` into `settings.json`.
The test's premise — that QSettings is not read — holds for the steady-state
read path but not for the one-shot migration. On macOS, cfprefsd isolation
complicates this further.
