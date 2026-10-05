# Sweet-expression support: run + highlight

> **Status:** Complete. `ScanTurmericSweetLine` and the `SweetMarker`
> style are live in `scanner_turmeric.cpp`; `Dialect::TurmericSweet` in
> `dialect.h`; `run_buffer.cpp` handles sweet dialects via `:run`
> rather than `load`. Generalized beyond this plan into a full dialect
> system — see [`dialects-saffron-and-r7rs.md`](dialects-saffron-and-r7rs.md).
> Prerequisite: [`multi-language-highlighting.md`](multi-language-highlighting.md)
> (complete).

**Sequenced after `docs/plans/multi-language-highlighting.md`** — sweet mode
becomes a variant of the `TurmericScanner` inside the scanner framework that
plan introduces, and reuses its `Language` dispatch and markdown fence
info-string mapping. Do not start this plan until that refactor has landed.

## Context

`.tur.sweet` support today is vestigial: the pieces around it exist, but the
two that matter — running and highlighting — were never finished.

What already works:

- File dialogs, macOS `Info.plist.in`, and Linux MIME registration recognize
  `.tur.sweet` / `.sweet`.
- `run_buffer.cpp:23-28` picks a `.tur.sweet` extension for scratch files
  written from sweet buffers.
- Loading a *saved, clean* `.tur.sweet` file delegates to the bundled `tur`,
  which dispatches on the file's extension.

What doesn't:

- **Highlighting**: `CreateTurmericLexer(sweetExp)` is plumbed
  (`turmeric_lexer.h:65`) but the flag only changes `GetName()`
  (`turmeric_lexer.cpp:153`); every call site passes `false`. Sweet-specific
  tokens (`$`, `\\`, `<*`, `*>`) get no styling.
- **Running dirty/untitled sweet buffers**: `docs/plans/PLAN.md:269` planned
  prepending a `#lang sweet-exp` header to the scratch file; that was never
  implemented (`writeScratchAndLoad`, `run_buffer.cpp:45-73`, writes contents
  verbatim and relies on extension alone — verify whether `tur` honors the
  extension for `load`ed scratch files, see Part A).
- **Testing**: `test_run_sweet_buffer`, planned in
  `docs/plans/smoke-tests.md:134`, was never written. There is no coverage
  that sweet evaluation works at all.

## Part A — make sweet buffers run correctly

1. **Establish ground truth** with the bundled toolchain
   (`build/<preset>/…/turmeric/tur`): does `(load "x.tur.sweet")` parse the
   file as sweet-exp based on extension alone, or does it require a
   `#lang sweet-exp` first line? Test both a bare `def sweet-x 7` file and one
   with the `#lang` header, from the REPL.
2. **Fix `writeScratchAndLoad`** (`src/repl/run_buffer.cpp`) accordingly: if
   the header is required, prepend `#lang sweet-exp\n` when the target
   extension is `.tur.sweet` and the buffer does not already start with a
   `#lang` line (per the original PLAN.md §269 intent). Applies to both
   `RunBuffer` (dirty path) and `RunRange` (selections from sweet buffers).
3. **Stop re-deriving language from the path string**: `extensionFor()`
   duplicates suffix sniffing. Once `EditorView` tracks its `Language`
   (from the multi-language plan), expose it and switch on that instead, so
   untitled buffers the user is writing sweet code in can be handled once a
   language override exists (until then, untitled → `.tur`, unchanged).

## Part B — sweet highlighting

Finally honor the sweet flag by teaching `TurmericScanner` a `sweet` mode and
adding `Language::TurmericSweet`:

- **New tokens** (styled via one new style slot, `TurStyle::SweetMarker`,
  which fits in the free 27–31 gap below Scintilla's reserved 32–39 range):
  - `\\` — SPLIT operator (word boundary)
  - `$` — GROUP/SPLIT operator (standalone)
  - `<*` and `*>` — collecting brackets (recognized *before* the generic
    bracket handling so `*>` is not flagged as an unmatched-closer by rainbow
    mode)
- **Already correct, verify with tests**: keyword styling is
  position-independent (`SymbolStyle`), so paren-less `def sweet-x 7` lines
  highlight today; neoteric `f(x)` → `NeotericCall`; curly-infix `{a + b}` →
  `CurlyInfix`/rainbow; `#lang sweet-exp` → `LangDir`; inline ```` ```c ````
  blocks reuse the shared C delegation from the multi-language plan.
- **No new line-state bits**: sweet mode is a static per-lexer flag, not
  per-line state.

Dispatch and theme wiring (same mechanical pattern as the other languages):

- `LanguageForPath`: `.tur.sweet` / `.sweet` → `TurmericSweet` (instead of
  plain Turmeric).
- Markdown fence info strings `sweet`, `turmeric-sweet`, `tur-sweet` → sweet
  guest scanner.
- `StyleKeyMap()` + `resources/turmeric-dark.theme.json`: add `sweetMarker`
  (suggest the quote/reader-macro purple `#C4A0E8`, matching how `$`-like
  structural operators read elsewhere in the theme).

## Part C — optional ergonomics (do last, or defer)

- Auto-indent on Enter for sweet buffers (copy previous line's leading
  whitespace). Sweet-exp is indentation-sensitive, so this is the single
  biggest editing-comfort win; Trowel currently has no auto-indent for any
  language, so implement it generically in `EditorView` and enable it for
  sweet first if that's simpler to validate.

## Files touched

- `src/repl/run_buffer.cpp` — `#lang` prepend + language-based dispatch
- `src/editor/scanner_turmeric.cpp`, `lexers.h` (post-refactor names) — sweet
  mode, `SweetMarker` style, `Language::TurmericSweet`
- `src/editor/scanner_markdown.cpp` — fence info-string mapping
- `src/editor/theme_loader.cpp`, `resources/turmeric-dark.theme.json`
- `tests/smoke/test_run_buffer.py`, `tests/smoke/test_lexer_theme.py` (or the
  language-lexing test module from the multi-language plan),
  `tests/smoke/fixtures/`

## Verification

1. `just build && just smoke` green.
2. New smoke tests:
   - `test_run_sweet_buffer` (the one promised in smoke-tests.md §): open
     `fixtures/sweet_hello.tur.sweet`, Run Buffer, assert `sweet-x` evaluates
     to `7` in the REPL — **and** a dirty-buffer variant (edit first, run, so
     the scratch-file path is exercised).
   - Lexer assertions on a new `fixtures/sweet_syntax.tur.sweet` exercising
     `$`, `\\`, `<* … *>`, `{a + b}`, `f(x)`, and a paren-less `def` line:
     `editor.get_style_at` returns `SweetMarker` / `Define` / `NeotericCall` /
     rainbow styles at the expected positions.
   - Markdown fixture gains a ```` ```sweet ```` fence; assert sweet styles
     inside it.
3. Manual: open a `.tur.sweet` file, edit without saving, Run Buffer, confirm
   the binding is visible in the REPL; Run Selection on a sweet region;
   confirm `*>` is not painted as a bracket error.
