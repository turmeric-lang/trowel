# Multi-language syntax highlighting

> **Status:** Complete. The scanner framework is built: `scanner.h`,
> `lexers.h`, `lexer_adapter.cpp`, and per-language scanners
> (`scanner_turmeric`, `scanner_c`, `scanner_markdown`, `scanner_json`,
> `scanner_just`). Extended beyond the plan with `scanner_python`,
> `scanner_sh`, `scanner_toml`, `scanner_cmake`. Nested-fence
> delegation and per-file dispatch are live.
> **Related:** [`sweet-exp.md`](sweet-exp.md) (complete, builds on this),
> [`dialects-saffron-and-r7rs.md`](dialects-saffron-and-r7rs.md)
> (complete, generalizes the dialect system further).

## Context

Trowel installs the hand-written Turmeric lexer for **every** file,
unconditionally (`src/editor/editor_view.cpp:35`). There is no per-extension
dispatch at all, so `.md`, `.json`, `Justfile`, and `.c`/`.h` files are all
"highlighted" as Turmeric — which is why markdown looks wrong. Additionally,
the Turmeric lexer paints ```` ``` ```` inline-C blocks a single flat `CBlock`
color with no C highlighting inside (`turmeric_lexer.cpp:401-417`).

## Goals

Support the file types in a typical Turmeric repo — `.tur`/`.tur.sweet`,
`.md`, `.json`, `Justfile`, and C files — with correct handling of **nested
fences**: a markdown file containing a ```` ```turmeric ```` fence whose body
itself contains a ```` ```c ```` inline block must highlight all three layers
correctly.

Note: Trowel bundles Scintilla but **not** Lexilla, so there are no stock
lexers to borrow. That's fine — Lexilla couldn't do the nested-fence
delegation anyway (its lexers only run on whole documents, and its Markdown
lexer doesn't highlight embedded languages), and it has no Justfile lexer. We
extend the existing hand-written-lexer approach instead.

## Architecture

### 1. Refactor into composable "scanners" (`src/editor/`)

Restructure lexing so a host language can delegate a sub-range to a guest
language:

- A **scanner** walks a text buffer and emits `(offset, len, styleId)` runs,
  threading an explicit, bit-packable per-line state struct. The existing
  `TurmericLexer::Lex` body becomes `TurmericScanner`.
- New scanners: `CScanner`, `MarkdownScanner`, `JsonScanner`, `JustScanner`.
- One generic `ILexer5` adapter (replacing the class boilerplate in
  `turmeric_lexer.cpp:113-168`) parameterized by root scanner + rainbow flag.
  `CreateLexerForLanguage(Language, rainbow)` replaces `CreateTurmericLexer`.
- Keep the existing per-line `SetLineState` mechanism (as in
  `turmeric_lexer.cpp:225-235`) — all state fits in 31 bits after compressing
  the Turmeric depth fields (block-comment depth 8→4 bits, datum-comment depth
  8→4 bits, bracket depth 7→5 bits; caps are generous in practice). Layout:
  Turmeric core ~16 bits, markdown fence state ~8 bits (in-fence flag, fence
  char, fence length 3 bits, guest language 3 bits), C guest state ~2 bits
  (in block comment, in preprocessor continuation).

### 2. Style ID space + theme

Scintilla styles are one byte; current usage is 0–30 (`TurStyle`) and 40–47
(rainbow), skipping Scintilla's reserved 32–39. Allocate new contiguous blocks
well clear of those:

- C: 48–63 (default, comment, doc comment, preprocessor, keyword, type,
  string, char, number, operator, identifier…)
- Markdown: 64–79 (heading, emphasis, strong, code span, fence delimiter,
  link text, link URL, blockquote, list bullet, hr…)
- JSON: 80–90 (key, string, number, literal, error; braces reuse the existing
  rainbow styles when rainbow is on)
- Justfile: 91–105 (comment, recipe name, dependency, assignment, `{{…}}`
  interpolation, backtick, setting keyword, recipe body…)

Wire-up (mechanical, same pattern per language):

- Extend `StyleKeyMap()` in `theme_loader.cpp:55-95` with namespaced keys
  (`c.keyword`, `md.heading`, `json.key`, `just.recipe`, …).
- Add corresponding entries to `resources/turmeric-dark.theme.json`, reusing
  the existing palette.
- Fix `EditorView::setFont` (`editor_view.cpp:85-94`) to cover the full
  0–`kMaxStyleId` range instead of the two Turmeric loops.

### 3. Nested-fence semantics (the markdown core)

`MarkdownScanner` implements CommonMark fenced-code rules, which is what makes
nesting work:

- Opening fence: ≥3 backticks or tildes at ≤3 spaces indent; remember
  **char, length, and info string**. Info string maps to guest scanner:
  `turmeric`/`tur` → Turmeric, `c` → C, `json` → JSON,
  `just`/`justfile`/`make` → Just; unknown → flat code style.
- Closing fence: same char, **length ≥ opening**, **no info string**. So a
  ```` ````turmeric ```` fence (4 backticks) is never closed by inner ```` ``` ````
  lines, and an inner ```` ```c ```` line can't close anything (it has an info
  string).
- Guest-aware close suppression: while the Turmeric guest reports it is inside
  its own inline-C block, a bare ```` ``` ```` line is consumed by the guest as
  *its* closer rather than closing the markdown fence. This makes the
  equal-length case (``` outer + ``` inner) render properly.
- Fence bodies are delegated to the guest scanner with guest styles; fence
  delimiter lines + info string get the markdown fence-delimiter style.

### 4. Turmeric inline-C upgrade

In `TurmericScanner`, keep the ```` ``` ```` / ```` ```c ```` fence markers
styled as `CBlock` but delegate the body to `CScanner` (both the single-line
and the multi-line continuation paths, `turmeric_lexer.cpp:296-308` and
`401-417`). This is what makes inline C render as C in plain `.tur` files
*and* inside markdown fences (Markdown → Turmeric → C is the deepest stack).
Also fix the existing `i + 2 < readLen` off-by-one that misses a fence at
end-of-buffer.

### 5. Per-file dispatch (`EditorView`)

- Add `Language LanguageForPath(const QString&)`: `.tur`/`.tur.sweet`/`.sweet`
  → Turmeric; `.md`/`.markdown` → Markdown; `.json` → JSON; basename
  `Justfile`/`justfile`/`.justfile`/`*.just` → Just;
  `.c`/`.h`/`.cc`/`.cpp`/`.hpp`/`.hh` → C; everything else (incl. untitled) →
  Turmeric (current behavior).
- `EditorView::setPath` (`editor_view.cpp:219`) swaps the lexer +
  `colourise(0, -1)` when the language changes — this covers open, Save-As,
  and new tabs.
- `setRainbowBrackets` (`editor_view.cpp:101-110`) recreates the lexer for the
  **current** language instead of hardcoding Turmeric.
- Widen the Open/Save dialog filters in `main_window.cpp:667` and `:710` to
  mention the new types.

## Files touched

- `src/editor/turmeric_lexer.{h,cpp}` → refactor into scanner framework
  (likely rename/split: `lexers.h`, `scanner_turmeric.cpp`, `scanner_c.cpp`,
  `scanner_markdown.cpp`, `scanner_json.cpp`, `scanner_just.cpp`,
  `lexer_adapter.cpp`; update the `CMakeLists.txt` source list)
- `src/editor/editor_view.{h,cpp}` — language dispatch, setFont range,
  rainbow toggle
- `src/editor/theme_loader.cpp`, `resources/turmeric-dark.theme.json` — new
  style keys
- `src/app/main_window.cpp` — dialog filters
- `tests/smoke/fixtures/` + `tests/smoke/test_lexer_theme.py` (or a new
  `test_lexer_languages.py`)

## Verification

1. `just build` — compiles clean.
2. New smoke tests (drive via the existing control API, same pattern as
   `tests/smoke/test_lexer_theme.py`):
   - New fixtures: `sample.md` (with a ```` ```turmeric ```` fence containing
     a ```` ```c ```` block — the exact nested case — plus a
     ```` ````turmeric ```` 4-backtick variant), `sample.json`, `Justfile`,
     `sample.c`.
   - Per language: open the fixture, assert `editor.get_style_at` returns the
     expected style band at key positions (e.g. `#` heading → md heading
     style; JSON key vs value; C keyword inside the nested fence → C keyword
     style; text *after* the inner ```` ``` ```` still styles as Turmeric; text
     after the true closing fence styles as markdown prose).
   - Regression: existing Turmeric + rainbow tests still pass (`just smoke`).
3. Manual: `just run`, open a Turmeric repo's `README.md`, `Justfile`,
   `*.json`, `*.c`, `*.tur` side by side; toggle rainbow brackets on a `.md`
   tab; Save-As a buffer from `.txt` to `.md` and confirm re-highlight.

## Out of scope

- Sweet-expression mode (`CreateTurmericLexer(sweetExp)` is plumbed but always
  `false` today; dispatch keeps that behavior). Covered by the follow-up plan
  in `docs/plans/sweet-exp.md`.
- Folding, light theme, user-selectable per-buffer language override.
