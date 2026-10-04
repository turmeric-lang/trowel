# Upstream reports — filed, fixed, and two of them wrong

Defects in the Turmeric toolchain found while bringing Trowel onto `v0.60.1`.

**All six were filed as [turmeric-lang/turmeric#1063][pr1063] (merged
2026-10-03) and fixed in [#1064][pr1064], released as `v0.61.0`.** The report
bodies here are kept as the local record of what was measured and of what was
got wrong; upstream's rewritten versions live in its `docs/archive/`.

[pr1063]: https://github.com/turmeric-lang/turmeric/pull/1063
[pr1064]: https://github.com/turmeric-lang/turmeric/pull/1064

## Two were misdiagnosed, both mine

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

## The four that held as filed

| Report | Upstream root cause |
|---|---|
| [lsp-ignores-the-file-extension](lsp-ignores-the-file-extension.md) | `run_doc_analysis` wrote the buffer to `tur_lsp_XXXXXX.tur` — the compiler *does* read the extension, but the extension it saw was always `.tur`. Scratch file now takes the document's suffix. |
| [fmt-reprints-sweet-as-s-expressions](fmt-reprints-sweet-as-s-expressions.md) | `fmt_format_buffer` kept text as written only for `READER_R7RS[_SWEET]`; `READER_SWEET` fell through to `fmt_print`. Sweet now shares that branch. (Upstream's correction to my filing: the old output still *ran* — a silent change of syntax, not corruption.) |
| [format-subcommand-shreds-a-sweet-buffer](format-subcommand-shreds-a-sweet-buffer.md) | `cmd_format` hard-coded `READER_TURMERIC` even for file paths, so `.scm` and `#lang` files were affected too — wider than I filed. Now resolves `--lang`, then extension, then the `#lang` line. |
| [lang-flags-take-different-vocabularies](lang-flags-take-different-vocabularies.md) | Two separate name tables. Both `--lang` flags now fall back to `lang_base_lookup`, so every `tur dialects` row is accepted. |

## Two new reports upstream found on the way

- `lsp-relative-load-resolves-against-scratch-dir` (medium) — a relative
  `(load "b.tur")` fails under `tur lsp` but works in `tur check`.
- `lsp-publishes-other-files-diagnostics-under-one-uri` (low-medium) — the
  "secondary observation" I filed as *latent* is **live**: a loaded file's error
  is drawn on the open buffer. I under-called it; it needed a repro, not a
  caveat.

## Trowel-side follow-ups — done

The pin is on `v0.61.0` and every workaround these reports justified is gone:
`LspManager::shiftedText` and its line shift, `DialectIsFormattable`, and the
separate `DialectFmtLangFlag` vocabulary. Signature help auto-triggers on space.
The smoke test that encoded the `:run` misdiagnosis is deleted and replaced by
one asserting the definitions persist — which they always did.

Each deletion was gated on re-verifying the fix against the newly pinned binary
rather than on the fix being reported, because three of these workarounds
originally outlived their bugs by several releases. See the status block in
`docs/plans/dialects-saffron-and-r7rs.md` for the per-item evidence.

**Still open upstream, and live:**
`lsp-publishes-other-files-diagnostics-under-one-uri` — a loaded file's error is
drawn on the open buffer. Trowel keys diagnostics by the published URI, so it
inherits this. A client-side filter on the non-standard `"file"` key would hide
it; not added, because that is a fresh workaround for a report that is already
filed and being worked, and this effort has just finished deleting three of
those.
