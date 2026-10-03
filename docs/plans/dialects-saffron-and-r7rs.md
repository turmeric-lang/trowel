# Saffron, R7RS, and the `#lang` dialect axis

> **Status:** proposed 2026-10-03, against Trowel `0943d95` and Turmeric
> `v0.60.1`. Nothing below has landed. Update this line in the same change as
> the work, part by part.

## 0. Summary

Turmeric grew two new base languages since the version Trowel bundles —
**Saffron** (dynamically typed Turmeric) and **R7RS** (Scheme) — each with
sweet and unsweet readers. `#lang` now names one of **ten** base dialects, and
Trowel recognizes four of them. Everything downstream of that — highlighting,
Run Buffer, Format File, the LSP — is wrong or absent for the other six.

Separately, Try Turmeric gained a dialect picker, a docs pane, and a pile of
fixes over the same period. The picker is the one worth porting outright; the
rest is triaged in Part H.

This plan is in nine parts, in dependency order. Parts A–D are the floor:
without them a Saffron or Scheme file in Trowel is mis-highlighted, cannot be
run, and is mangled by Format. Parts E–G are the features. Part H triages the
rest of the Try Turmeric work, and Part I is the three upstream bugs this
survey turned up, which should be reported whatever Trowel does about them.

**The load-bearing discovery** is that a dialect is not just a highlighting
mode. `#lang r7rs` changes which prelude the REPL session has loaded, so
running a Scheme buffer against a Turmeric REPL session fails no matter how
the file is spelled (§1.4). Trowel's REPL has one session and no concept of
its dialect. Part D is about that, and it is the largest piece of real work
here.

---

## 1. What changed upstream

Everything in this section was verified against a `tur` built from the
Turmeric worktree at `v0.59.0`/`v0.60.0`, not read off a changelog. The exact
commands are in §1.6 so they can be re-run against the newly pinned build —
re-run them, because three of the facts Trowel's own source comments assert
have already gone stale this way (§A.3).

### 1.1 Ten bases, one axis

`#lang <base>` takes a single base naming a (language, reader) pair. The
`#lang` *layer* axis that Trowel's code comments still mention (`stringed`,
`refined`) was decommissioned in v0.49.0: `#s"..."` is an unconditional reader
macro now, and a trailing token after the base is a hard error (`TUR-E0330`).

`tur dialects --json` is the registry, and it is the only thing that should
ever be read for this list:

```
turmeric              turmeric  s-expr       stable
turmeric/curly-infix  turmeric  curly-infix  stable
turmeric/neoteric     turmeric  neoteric     stable
turmeric/sweet        turmeric  sweet        stable
saffron               saffron   s-expr       stable
saffron/curly-infix   saffron   curly-infix  stable
saffron/neoteric      saffron   neoteric     stable
saffron/sweet         saffron   sweet        stable
r7rs                  r7rs      scheme       stable
r7rs/sweet            r7rs      sweet        stable
```

All ten are `stable`; `r7rs` graduated out of `EXPERIMENTS[]` at v0.57.0, so
there is nothing to enable and no lifecycle warning to suppress. The JSON form
carries an `"experiment"` key only on a gated base, which is the hook a future
badge hangs on.

The canonical source is `turmeric/src/compiler/lang_dialects.c` (`LANG_BASES[]`)
and `lang_dialects.h` (`LangTraits`). Read `LangTraits` before designing
anything here: it says, per language, which reader the bare token selects,
whether the language is dynamically typed, whose truthiness `if` uses, and
**which prelude it autoloads** — that last field is what Part D turns on.

### 1.2 What the two new languages are

- **Saffron** is Turmeric with annotations optional: an unannotated parameter
  or return is `any` instead of `int`. Same reader, same tokens, same
  compiler, same object files. For highlighting purposes it *is* Turmeric.
  Its own prelude (`stdlib/saffron/prelude.tur`, 113 lines) is a set of
  adaptors over the typed stdlib.
- **R7RS** is Saffron's dynamic substrate under a Scheme reader. Different
  lexemes (`#t`, `#\x41`, `#(...)`, `#u8(...)`, `|bar symbols|`, radix
  prefixes), different special forms, Scheme truthiness (only `#f` is false),
  and a 3916-line prelude whose names are reached through a rename table
  (`scheme_lower.c`) — which is why `display` resolves to `r7rs-display`.

`.scm` selects the Scheme **language** as well as its reader
(`reader_type_from_extension`, `src/compiler/reader.c:5798`). `.tur.sweet` is
still the only other extension that selects a non-default reader; there is no
extension for `r7rs/sweet`, so such a file needs its `#lang` line.

### 1.3 New CLI surface Trowel should be using

| Command | Why it matters here |
|---|---|
| `tur dialects [--json]` | the ten-base registry; Part E reads it |
| `tur repl --lang <base>` | start a REPL session in a dialect — Part D |
| `tur repl --engine <name>` | `cc` / `jit` / `interp` |
| `tur fmt --stdin --lang <base>` | dialect-aware formatting — Part F |
| `tur --engine <name>` | engine selection, with `TUR_ENGINE` and `build.tur :engine` behind it |
| `tur docs --open` / `--serve` | rendered guides + API offline — Part H |

`--engine` existing unblocks `docs/plans/engine-selection.md`, which is
explicitly sequenced behind it ("Do not start Part B or C of this plan until
that flag exists"). That is a separate plan and stays out of scope here, but
its gate is open now and the plan's header should say so.

The JIT is also **on by default** on x86-64 and arm64 as of v0.60.1, with MIR
vendored, and release archives ship `libtur_mir.a`. That is a bundle-size and
staging fact for Part A, not a feature Trowel has to do anything about.

### 1.4 The dialect lives in the REPL *session*, not only in the file

This is the fact the whole run path turns on, and it is asymmetric between the
two new languages.

**R7RS needs the session switched.** Loading a Scheme file into a Turmeric
session fails on the prelude, not the reader — the forms parse, the names get
their `r7rs-` rename, and then nothing answers to them:

```
$ printf '(load "g.scm")\n' | tur repl
g.scm:2:2: error: unknown function or operator 'r7rs-display'
```

Switch the session first and it works:

```
$ printf '#lang r7rs\n(load "g.scm")\n' | tur repl
; language set to r7rs, reader r7rs (session reset)
7
```

`tur repl --lang r7rs` gets there without the switch. `:run` on a `.scm` file
works too — *once the session is r7rs*. The switch is not free: it resets the
session back to the pinned stdlib preload, discarding accumulated user
definitions. Upstream says so in as many words, and the Try Turmeric picker
warns before it happens.

**Saffron mostly does not.** Because `lang_span_is_dynamic` is a per-file
registry lookup, a `#lang saffron` file loaded into a Turmeric session gets
Saffron's defaults for its own forms — `(defn double [x] (* x 2))` applied to
`3.55` printed `7.1` with no session switch at all. The exception is the
prelude: it autoloads only when the **entry** file is `#lang saffron`, so a
buffer reaching for `vec-map` and friends fails in a Turmeric session
(`unknown name 'vec-map'; will runtime-dispatch` and then a runtime error).

So the rule Trowel needs is: **switch the session when the buffer's language
differs from the session's language.** Readers do not need a session switch;
languages do.

### 1.5 The LSP ignores the file extension

`tur lsp` selects its reader from the `#lang` line in the document text and
nothing else. The compiler honors the extension; the LSP does not. Driven
directly over stdio with `didOpen` + `documentSymbol`:

| document | diagnostics | symbols |
|---|---|---|
| `p.scm`, `(define (f x) ...)`, no header | **2 bogus errors** | **none** |
| `p.scm`, same body with `#lang r7rs` | clean | `f`, `main` |
| `p.tur.sweet`, `defn double [x] / {x * 2}`, no header | **bogus `TUR-E0003 unbound symbol 'defn'`** | **none** |
| `p.tur.sweet`, same body with `#lang turmeric/sweet` | clean | `double` |
| `p.tur` with `#lang saffron` | clean | `double` |

The bogus-diagnostic rows are what a Trowel user sees **today** on a
headerless sweet file — Trowel's own fixtures
`tests/smoke/fixtures/sweet_hello.tur.sweet` and `sweet_syntax.tur.sweet` are
both headerless, so this is not hypothetical and it is not new with the new
dialects. It gets worse with them, because `.scm` is precisely the extension
that makes a header unnecessary to `tur`.

This is an upstream bug (§I.1). Part G carries a Trowel-side workaround so the
editor is usable before it is fixed.

### 1.6 Re-run these against the new pin

```sh
TUR=<bundled tur>
$TUR dialects --json
printf '(define (f x) (* x 2))\n(display (f 21))\n' > g.scm
printf '(load "%s/g.scm")\n' "$PWD" | $TUR repl                  # expect: r7rs-display error
printf '#lang r7rs\n(load "%s/g.scm")\n' "$PWD" | $TUR repl      # expect: 7
printf 'defn main []\n  println("x")\n  0\n' > m.tur.sweet
printf ':run %s/m.tur.sweet\n' "$PWD" | $TUR repl                # expect: x  (see A.3)
$TUR format < m.tur.sweet                                        # expect: shredded (see F.1)
$TUR fmt --stdin --lang sweet < m.tur.sweet                      # expect: s-exprs
$TUR fmt --stdin --lang r7rs/sweet < d.sscm                      # expect: verbatim
```

---

## 2. What Trowel assumes today

- **Four bases.** `LanguageForLangBase` (`src/editor/lexer_adapter.cpp:278`)
  accepts `turmeric`, `turmeric/curly-infix`, `turmeric/neoteric`,
  `turmeric/sweet`, and the legacy `sweet-exp` alias. The other six fall
  through to the extension, so `#lang saffron/sweet`, `#lang r7rs` and
  `#lang r7rs/sweet` all highlight as **plain, unsweet Turmeric**. For the two
  sweet ones that means the indentation-sensitive markers go unpainted; for
  the Scheme ones it means the wrong lexeme set entirely.
- **`.scm` is unknown.** Not in the extension table
  (`lexer_adapter.cpp:242`), not in `hasTurmericExtension`
  (`run_buffer.cpp:172`), not in the Info.plist document types, not in
  `resources/linux/trowel.xml`, not in the file-dialog filters
  (`main_window.cpp:1370`). A `.scm` file is highlighted as Turmeric and
  `EvalModeForPath` reports `Disabled`, so Run Buffer is greyed out on it.
- **The REPL has no dialect.** `ReplSession::start` spawns `{"repl"}` with no
  `--lang` (`repl_session.cpp:171`), and nothing tracks or switches what
  language the live session is in.
- **Format File shells out to the wrong subcommand.** `MainWindow::formatFile`
  (`main_window.cpp:1850`) runs `tur format` over stdin with no `--lang`.
- **The keyword sets are two years of drift behind.** `scanner_turmeric.cpp`
  says its sets were "ported from turmeric/vim-syntax/syntax/turmeric.vim" —
  that file still exists but the maintained one is now
  `editors/vim-turmeric/syntax/turmeric.vim`, which is explicitly kept in step
  with the VS Code TextMate grammar and already covers Saffron and R7RS. The
  delta is large (§C.1).
- **Two markdown bugs make Turmeric's own docs render wrong in Trowel**
  (§B.3). One of them hits 996 code fences in the shipped guides.

---

## Part A — bump the bundled Turmeric to v0.60.1

Independent of everything else, and should land first and alone: every
behavioural claim below is about the new toolchain.

**A.1 The pin.** `CMakeLists.txt:161`, `TROWEL_TURMERIC_VERSION "v0.46.0"` →
`"v0.60.1"`, plus the four `_turmeric_sha` values. Take them from the
release's own `sha256sums.txt` asset rather than hashing downloads by hand.
Asset names are unchanged (`turmeric-<tag>-<target>.tar.gz`, Windows `.zip`).

**A.2 The owner moved.** The repos live in the `turmeric-lang` GitHub org as
of v0.60.1. `CMakeLists.txt:219` still builds
`https://github.com/rjungemann/turmeric/releases/download/...`. GitHub
redirects transferred repositories, so this works by accommodation; update it
anyway, in the same change, and check `README.md`, `Casks/`, and the
`com.turmeric_lang.Trowel` metainfo for other spellings. Note for anyone
verifying assets: provenance is bound to the owner that built them, so
`v0.59.0` and earlier need `--owner rjungemann` while this release and later
need `--repo turmeric-lang/turmeric`.

**A.3 Three stale workarounds to retire — verify each, then delete it.**
Trowel carries careful comments about toolchain bugs measured against
v0.42.2. At least one of them is fixed:

- `runCommandIsSafeFor` (`run_buffer.cpp:60`) routes `.tur.sweet` and
  `.sweet` away from `:run` and through `:reset` + `(load ...)`, because
  `:run` on a sweet file used to define nothing silently. **That is fixed at
  v0.59.0**: `:run` on a headerless `.tur.sweet` with a `defn main` printed
  and returned `0` normally. The workaround is now actively harmful — `load`
  does not invoke `main`, so **Run Buffer on a sweet file currently loads the
  program instead of running it**, which is the exact bug commit `6a7d17b`
  fixed for `.tur`. Removing `runCommandIsSafeFor` is a user-visible fix and
  wants its own commit and its own smoke test.
- The archive-layout double probe (`repl_session.cpp:37-56`) is correct and
  stays. v0.60.1 publishes the prefix layout (`bin/`, `lib/`, `include/`,
  `share/`) on all four targets, which is the branch that already wins; keep
  the flat fallback so an older pin keeps working.
- The `extensionFor`/`#lang`-header reasoning in `run_buffer.cpp:23-28` is
  still correct as far as it goes, and Part D rewrites it anyway.

**A.4 Bundle size.** Archives now carry `libtur_mir.a` (vendored MIR, ~2.7 MB
of sources, so a non-trivial static archive) plus `libturt_preamble.a`.
CMake stages the extracted tree verbatim, so nothing breaks — but check the
installer/DMG/AppImage sizes and the Windows zip against whatever limits CI
has, in this commit rather than in the one that trips over them.

**A.5 Re-baseline.** `just build && just smoke` green on the new pin before
any of Part B lands, and run §1.6 against the staged binary. A toolchain jump
of fourteen minor releases is the most likely source of surprise in this whole
plan, and it should be isolated.

---

## Part B — the dialect model

The central design decision. `Language` (`src/editor/lexers.h:19`) is
documented as "languages Trowel can **highlight**", and for that purpose
Saffron is Turmeric — same reader, same tokens. But the run path, the picker,
the formatter and the REPL all need to know Saffron from Turmeric, and
`Language` cannot say it. Conflating the two axes into one enum would mean
four near-identical scanner entries that exist only so other subsystems can
tell them apart.

**B.1 Keep `Language` as the highlighting axis; add a `Dialect` beside it.**

- `Language` gains exactly two members, `R7rs` and `R7rsSweet` (appended —
  the enum's own comment requires it). Saffron and the curly-infix/neoteric
  readers map onto the existing Turmeric entries: `{a + b}` and `f(x)` are
  painted unconditionally already, and `saffron`/`saffron/sweet` are
  token-identical to their Turmeric counterparts.
- A new `Dialect` (suggest `src/editor/dialect.h`) is the (language, reader)
  pair — the thing a `#lang` line names. It carries the base token as
  written, the language, and the reader, and it answers four questions:
  `languageForHighlighting()`, `langFlagForFmt()` (Part F),
  `scratchExtension()` (Part D), and `needsSessionSwitch(from)` (Part D).
- `Dialect` is derived, never stored as UI state: from the buffer's `#lang`
  line, falling back to the extension, falling back to Turmeric. That is the
  same precedence `LanguageForBuffer` already implements
  (`lexer_adapter.cpp:405`, mirroring upstream's
  `chosen = (ext_type != READER_TURMERIC) ? ext_type : lang_type`), so the
  change is to the table it consults, not the rule.

**B.2 Extend `#lang` parsing to all ten bases.** `LanguageForLangBase`
(`lexer_adapter.cpp:278`) becomes a `Dialect` lookup over the ten base tokens
plus the `sweet-exp` alias. Two notes carried forward from upstream: the alias
is accepted on input and never generated, and the trailing-layer tolerance in
the current comment is now wrong — a trailing token is `TUR-E0330`. Keep
*accepting* one in the editor (a half-typed line must not flip the
highlighting to a fallback mid-keystroke) but stop describing it as
meaningful.

**B.3 Two markdown bugs, found while surveying, fixed here.**

- **The guest-language field is masked to three bits on the way in and four
  on the way out.** `PackLexState` writes
  `static_cast<int>(st.mdGuest) & 0x7` (`lexer_adapter.cpp:105`) into a field
  `UnpackLexState` reads with `& 0xF` (`:131`). With ten `Language` members
  that is already broken for the two highest: a ```` ```sh ```` fence (Sh = 8)
  is lexed as Turmeric from its second line on, and ```` ```python ````
  (Python = 9) as C. Adding two members makes it worse. Widen the pack mask to
  `0xF` and add a static assertion that `Language`'s count stays below the
  `0xF` sentinel — the enum comment promises 0..14 and the code delivers 0..7.
- **`sweet-exp` is not a recognized fence tag.** `GuestForInfo` matches
  `sweet`, `turmeric-sweet` and `tur-sweet` (`scanner_markdown.cpp:76`). The
  tag is read as a whole word, so `sweet-exp` matches none of them — and
  `sweet-exp` is what Turmeric's shipped guides use **996 times**, against 0
  for the three Trowel accepts. Every sweet-expression example in the
  documentation currently renders as undifferentiated code in Trowel's
  markdown view. One-line fix, outsized payoff.

  While in there: `scheme` (16 uses in the guides) currently maps to
  `Language::Turmeric` (`:80`) and should map to `R7rs`. Add `saffron` and
  `saffron-sweet` for symmetry, and `r7rs`/`scm` alongside `scheme`.

**B.4 Register `.scm`.** Extension table (`lexer_adapter.cpp:242`),
`hasTurmericExtension` (`run_buffer.cpp:172`), `Info.plist.in`
`CFBundleTypeExtensions`, `resources/linux/trowel.xml` glob, and both
file-dialog filter strings (`main_window.cpp:1370`, `:1414`). Mirror
`reader_type_from_extension` exactly and no further: `.scm` is Scheme,
`.tur.sweet` is sweet Turmeric, and a bare `.sweet` is **not** sweet upstream
— the existing comment at `:238` makes that point and it still holds.

---

## Part C — highlighting

**C.1 Refresh the Turmeric keyword sets first.** Re-port
`scanner_turmeric.cpp:12-70` from
`turmeric/editors/vim-turmeric/syntax/turmeric.vim`, which is the maintained
source and is kept in step with the VS Code grammar. The current sets predate
a lot:

- missing `def*` forms: `defprotocol`, `defdynamic`, `defrecord`, `defopaque`,
  `defeffect`, `defimage`, `defworld`, `deftype`, `defkind`, `defrec`,
  `defalias`, `define`
- missing special forms: `with-handler`, `compose-handlers`,
  `cloneable-reset`/`-shift`, `serial-reset`/`-shift`, `catch-unwind`,
  `discontinue`, `with-region`, `use-reader-macros`, `panic-with`, `bt-scope`,
  `call/cc`, `resume`, `perform`, `reset`, `shift`, `shift0`, `escape`,
  `recur`, `defer`, `await`, `async`, `spawn`, `yield`, `is?`
- missing `#`-dispatch forms: `#map`, `#set`, `#refine`, `#fx`, `#writes`,
  `#reads`, `#s`
- `:else` as a clause head, which is an ordinary symbol to the reader and
  needs its own rule to read as the clause it is

The `^attr` rule matters more than it used to: v0.59.0 moved compiler
attributes **out** of the effect row, so `^construct`, `^byval`,
`^non-exhaustive`, `^capability`, `^extends` and `^fat` are now ordinary
syntax a reader meets constantly. Trowel paints `^foo` as `Metadata` already,
which is right; verify it against the vim file's `\v\^[a-z][a-z0-9-]*`.

Do this as its own commit. It is a large diff with no new structure, it
improves every existing `.tur` buffer, and bisecting it apart from the new
scanners is worth the extra commit.

**C.2 The R7RS scanner.** Add `ScanR7rsLine` / `ScanR7rsSweetLine` as a third
mode of `scanner_turmeric.cpp`'s shared walker rather than a new file: the
overlap is most of the scanner. Already shared and correct —
`#|` nested block comments, `#;` datum comments, strings and escapes, `'`,
`` ` ``, `#t`/`#f`, `#\name`, rainbow brackets, the sweet `$`/`\` markers.
What the Scheme lexeme set adds, all of it taken from the vim file's
`#lang r7rs`-scoped block:

| token | style |
|---|---|
| `#true` / `#false` (long spellings) | `Boolean` |
| `#\x41`, `#\space`, `#\newline`, … | `CharLit` (widen the existing rule past `isalpha`) |
| `#(` and `#u8(` | new `SchemeVector` slot |
| `\|bar symbol\|` | new `BarSymbol` slot |
| `#x`/`#o`/`#b`/`#d`/`#e`/`#i` radix and exactness prefixes, and `1/2` rationals | `Number` |
| `,` and `,@` | `Quote` (Scheme's unquote; Turmeric's is `~`) |
| the Scheme special forms | `Special` / `Define` |

The special-form list is in the vim file and should be copied from it, not
recalled: `define-library`, `define-syntax`, `define-record-type`,
`define-values`, `let-syntax`, `letrec-syntax`, `syntax-rules`, `syntax-error`,
`let*-values`, `let-values`, `letrec*`, `let*`, `begin`, `delay-force`,
`delay`, `make-promise`, `guard`, `parameterize`, `case-lambda`, `cond-expand`,
`include-ci`, `include`, `dynamic-wind`,
`call-with-current-continuation`, `with-exception-handler`,
`raise-continuable`, `raise`, `error`.

**Two collisions the vim file scopes its block to avoid, and Trowel must too.**
A `|...|` symbol would swallow the `|` of `#refine{x : T | pred}`, and `|>` is
a Turmeric operator (`scanner_turmeric.cpp`, the `|>` case). Both rules are
R7RS-mode-only. Conversely `:keyword` literals and `: type` annotations are
Turmeric-only — in Scheme a leading-colon identifier is an ordinary symbol
(there is an archived upstream report, `r7rs-leading-colon-identifiers.md`,
on exactly this) — so gate those off in R7RS mode rather than leaving them on.

**C.3 Two new style slots and the theme.** `TurStyle` has `Count` immediately
after `SweetMarker` and the rainbow block starts at 40, so there is room.
`SchemeVector` and `BarSymbol`, with `schemeVector` and `barSymbol` keys in
`StyleKeyMap()` and `resources/turmeric-dark.theme.json`. Reuse the existing
Turmeric slots for everything else — a whole `r7rs.*` block would mean
maintaining two palettes for one language family, and the point of sharing the
walker is that the tokens really are the same tokens.

**C.4 Saffron needs no scanner.** Say so in a comment where someone will look
for it — `LanguageForLangBase`'s replacement — with the reason (same reader,
same tokens, annotations merely optional) so the absence reads as a decision
rather than an omission. If Saffron prelude names are ever worth their own
style, that is a `Builtin`-set addition, not a scanner.

---

## Part D — the run path and the session dialect

The real work. §1.4 is the specification.

**D.1 Give `ReplSession` a dialect.** It tracks the base the live session is
in, starting from whatever `tur repl` defaults to (Turmeric) or from an
explicit `--lang` at spawn. It is updated on two events: Trowel sending a
`#lang` line, and the REPL printing its own
`; language set to <lang>, reader <reader> (session reset)` /
`; reader already set to <reader>` acknowledgement. Parse the acknowledgement
rather than assuming the send worked — the user can type `#lang saffron` into
the REPL pane directly, and then Trowel's idea of the session is stale.

**D.2 Run Buffer switches the session when the language differs.** Decision
table, from §1.4:

| buffer vs session | action |
|---|---|
| same base | run as today |
| same language, different reader | run as today; the reader travels with the file |
| different language | send `#lang <base>`, wait for the acknowledgement, then run |
| different language, session has user state | **confirm first** — the switch resets it |

The confirmation is the part that needs judgement. Try Turmeric uses a
one-line note in the picker footer ("Switching resets the REPL environment")
rather than a modal, and that is the right weight for the picker (Part E).
For Run Buffer, a prompt on every cross-language run would be intolerable;
suggest a status-bar line after the fact (`[trowel] session switched to r7rs
(reset)`) and a confirmation only when the user has evaluated something since
the last reset. Track that bit in `ReplSession` — it is one boolean set by
`sendCommand` and cleared by a reset.

**D.3 Run Selection carries the `#lang` line, and now needs to mean it.**
`RunRange` already prepends `editor->langDirectiveLine()` for a selection that
does not start at the top of the buffer (`run_buffer.cpp:239`) — good, and
more necessary now: for `r7rs/sweet` there is no extension that can express
the dialect, so the line is the only carrier. Synthesize one from the derived
`Dialect` when the buffer has no `#lang` line but its extension implies a
non-default base (a `.scm` or `.tur.sweet` file). The existing comment's
reasoning about layers is obsolete (§B.2) and should be rewritten to the
"extension cannot express the base" reason, which is the one that still holds.

**D.4 Scratch extensions.** `extensionFor` (`run_buffer.cpp:27`) becomes
`Dialect::scratchExtension()`: `.tur.sweet` for the sweet Turmeric and Saffron
readers, `.scm` for `r7rs`, `.tur` otherwise. For `r7rs/sweet` there is no
extension — write `.tur` and rely on the prepended `#lang r7rs/sweet` line,
and leave a comment saying that is deliberate rather than an oversight.

**D.5 `EvalModeForPath` learns `.scm`.** Also check whether a `build.tur`
manifest can name a Scheme or Saffron entry point before extending
`isBuildManifestName`; `build.tur` and `build.tur.sweet` are the two spellings
the "developing spices" guide documents, and if a `.scm` manifest is not a
thing then nothing changes there.

**D.6 Restart REPL In… gains a dialect.** `tur repl --lang <base>` exists, so
the natural home for "start the session in this dialect" is the existing
Restart REPL In… flow plus one more control. Cheaper and more honest than a
mid-session switch for a user who knows which language they are working in,
and it is how a Scheme-only project should start.

---

## Part E — the dialect picker

Port Try Turmeric's picker, including the two revisions it went through, since
both were corrections worth inheriting.

**E.1 The design, as it settled upstream.** Rows grouped by **language** with
a heading each — Turmeric, Saffron, Scheme — and the readers under it, each row
labelled by the `#lang` line it writes. Curly-infix and neoteric are **not**
offered as rows: `{a + b}` is enabled in every dialect and neoteric is one of
sweet-exp's three tools, so offering them as dialects misrepresents them. Both
stay spellable, and a buffer that names one gets its row back. That is six
rows normally, seven when the buffer names a hidden reader.

**E.2 The `#lang` line stays the source of truth.** The picker is a text edit,
not a hidden mode. Flip it and the header is written; type the header and the
picker reconciles. Nothing about the dialect is stored in UI state. The whole
edit goes through one undo step — in Scintilla terms, one
`SCI_BEGINUNDOACTION`/`SCI_ENDUNDOACTION` pair — so one Ctrl+Z undoes a
language switch.

Writer rules, which are worth copying verbatim because each one is a bug
someone already hit:

- no header, selection is the default (`turmeric`) → write nothing; do not
  decorate a plain file with a redundant header
- no header, non-default selection → insert `#lang <base>` as line 1 plus a
  blank line
- header present → replace exactly that line, preserve everything after
- selection returns to the default → remove the line, and the blank line after
  it if the insert added one

**E.3 Read the registry, do not hardcode it.** `tur dialects --json` at
startup, cached for the process. The hardcoded copy is exactly what drifted on
the web side: the picker went on offering four bases after there were eight,
so `#lang saffron` worked when typed but could not be selected. Trowel has the
same hazard and the same cheap fix, and it has an advantage the web side does
not — the registry comes from *the bundled toolchain*, so a Trowel built
against a newer Turmeric picks up a new base with no code change. Fall back to
a built-in list of the ten if the probe fails, and say in the fallback's
comment that it is a fallback.

**E.4 Where it goes.** Per-tab, reflecting the active editor, re-read on tab
switch — the dialect is per-file. A status-bar button showing the current base
is the natural surface given Trowel's chrome, with the full list in the Run
menu so it is keyboard-reachable and discoverable. The footer carries the
"switching the REPL session resets it" note when the selection would change
the language (D.2).

**E.5 Control API, for the tests.** `lang.bases`, `lang.get`, `lang.set` in
`control_handlers.cpp`, which is how everything else in Trowel is
smoke-tested. `editor.get_style_at` already exists for the lexer assertions.

---

## Part F — the formatter

**F.1 Switch to `tur fmt --stdin --lang <base>`.** `MainWindow::formatFile`
(`main_window.cpp:1850`) runs `tur format`, the older entry point, over stdin
with no dialect. Measured, all three on the same two-function sweet buffer:

- `tur format` (what Trowel calls) **shreds it** — one token per line,
  `defn` / `double` / `[x]` each on their own line separated by blanks. Format
  File on a sweet buffer destroys it today.
- `tur fmt --stdin --lang sweet` **converts it to s-expressions**, leaving any
  `#lang turmeric/sweet` header in place. The file still parses; the author's
  syntax is gone.
- `tur fmt --stdin --lang r7rs/sweet` returns it **verbatim** — checked and
  kept as written, which is what the upstream comment says it does and what
  the other two sweet bases should do.

On Scheme the dialect flag changes the answer outright: without `--lang r7rs`,
`(define (f x)\n(* x 2))` comes back collapsed onto one line with a blank
inserted; with it, correctly re-indented.

**F.2 So: pass `--lang`, and refuse the sweet bases for now.** Format File on
`turmeric/sweet` or `saffron/sweet` should say what it is declining and why
("`tur fmt` reprints sweet-expressions as s-expressions; formatting is
disabled for this dialect") rather than silently rewriting the buffer into
another syntax. `r7rs/sweet` is safe and goes through. Revisit when the
upstream asymmetry is resolved (§I.2).

**F.3 Or route formatting through the LSP instead.** `tur lsp` advertises
`documentFormattingProvider` and upstream deliberately moved the pipeline into
`fmt.c` so the LSP handler reaches the same code without shelling out. That
is the better long-term shape — no process spawn, no stdin, and the server
already holds the document — with one caveat: the handler picks its reader
with `reader_type_from_extension(doc->path)` (`src/lsp/lsp.c:2111`), so it
inherits the extension/header split of §1.5 from the other side. Verify
against the new pin before committing to it, and keep F.2's sweet refusal
either way. Part G already adds the client plumbing.

---

## Part G — LSP gaps

**G.1 Three unimplemented capabilities.** `tur lsp` advertises hover,
definition, documentSymbol, documentHighlight, rename (with
`prepareProvider`), references, workspaceSymbol, formatting, signatureHelp and
completion. Trowel sends ten requests and is missing three:

- **`textDocument/signatureHelp`** — trigger `(`, retrigger space. The
  highest-value one: in a lisp, the thing you want while typing a call is the
  parameter list, and Try Turmeric has had it since the LSP plan landed.
- **`textDocument/formatting`** — Part F.3.
- **`workspace/symbol`** — project-wide symbol search. Trowel has
  `lsp.symbols` (per-document) and a directory view; this is the missing
  "jump to anything in the project".

**G.2 Negotiate the position encoding instead of assuming it.**
`src/lsp/lsp_position.h:12-28` calls Trowel's byte-offset `character` a
"deliberate, contained lie" and says upstream would eventually advertise
`utf-8` via LSP 3.17 `general.positionEncoding`, "at which point bytes become
the *correct* answer and this file needs no change at all." **Upstream has
done it** — `tur lsp` now sends `"positionEncoding":"utf-8"` in its
capabilities (`src/lsp/lsp.c:737`). So:

- the comment needs rewriting; what it describes as a lie is now the
  negotiated truth, and leaving it as-is invites someone to "fix" a
  non-problem
- Trowel should *actually negotiate*: send
  `general.positionEncoding: ["utf-8", "utf-16"]` in `initialize`
  (`lsp_manager.cpp:156`) and read the server's answer out of the initialize
  result, which `onInitializeReply` currently discards entirely
  (`[this](const QJsonValue&, ...)`). `kPositionEncoding` becomes a member
  rather than a `constexpr`; the `Utf16` branch is already written and tested,
  so this is plumbing, not new logic.

Low urgency — it only bites on a line containing non-ASCII — but it is free
now and it is the kind of thing that stays wrong for years once the comment
explaining it has gone stale.

**G.3 Work around the headerless-dialect-file diagnostics (§1.5).** Until
upstream fixes it, a headerless `.scm` or `.tur.sweet` file produces a wall of
false errors in Trowel's gutter. Two candidate workarounds, in preference
order:

1. **Send a synthesized header.** When the derived `Dialect` comes from the
   extension rather than from a `#lang` line, send `didOpen`/`didChange` text
   with `#lang <base>\n` prepended, and shift every incoming diagnostic,
   location and symbol range down one line on the way back. Correct results,
   contained cost, one off-by-one to get right and to test.
2. **Suppress diagnostics for such files.** Honest and cheap, loses the
   feature.

Pick (1); it keeps the editor's intelligence working on the files the new
dialects make idiomatic, and the line-shift is a single seam in
`lsp_position.cpp`. Delete it when §I.1 lands, and leave the upstream report's
name in the comment so the deletion is findable.

---

## Part H — the rest of the Try Turmeric improvements

Surveyed from the changelog across v0.46.1–v0.60.1 and the archived
`try-turmeric-*` plans. Most are web-platform fixes with no Trowel analogue
(service worker, PWA install, iOS safe area, CSP, share links, project zip).
What is left, triaged:

**Worth porting, and the obvious next plan after this one:**

- **A docs pane.** Try Turmeric's is the best thing it has that Trowel lacks:
  the full guides and API reference in-app, searchable, with the pane leading
  on a quickstart and a "Recently Added" list. Trowel is better placed for it
  than the browser was — `tur docs --open`/`--serve` exists, the release
  publishes a `turmeric-docs-<tag>.tar.gz`, and the **docs pack** is a
  specified artifact: `index.json` (version, nav tree, search strings) plus
  chrome-free article bodies under `guides/`, `api/`, `spices/`. Both the
  website and Try Turmeric render from that one pack, which is what keeps them
  from drifting; a third consumer costs nothing new. Trowel's existing
  "Show Doc" is hover-level and is not this. Deserves its own plan, including
  whether to bundle the pack (another `FetchContent`, more bundle weight) or
  fetch it on demand.
- **An Examples menu.** Trowel has no discovery path for the language at all.
  Upstream's examples now exist in both s-expression and sweet form across the
  guides, and `tutorials/` ships in the repo. Cheap, and it is the other half
  of the picker: the picker teaches the syntax exists, examples show it.

**Already in Trowel, in its own idiom — nothing to do:**

- Outline, go-to-definition, hover, completion, rename, references
  (the Run menu).
- The time-travel tracer. Try's `trace-*` panel and Trowel's
  `timeline_strip.cpp` + Replay are the two consumers of the same
  `tur trace` format and the same DAP reverse execution; T1 and T2 landed
  upstream and Trowel consumes both.
- Minimap: `docs/plans/minimap.md` exists and `editor_view.h` references it;
  check its actual state against the code before treating it as open work.
- Sweet auto-indent: `editor_view.h` already has indentation handling "only
  active for languages where indentation is load-bearing", so the sweet-exp
  plan's Part C landed. Extend it to `r7rs/sweet`.

**Fixes Trowel inherits for free by bumping (Part A):**

- A later REPL turn can redefine any `def*` form (v0.49.1) — directly visible
  in Trowel's REPL pane.
- Output that stops mid-line reaches the consumer (v0.56.2); `#lang r7rs`
  plus `(display "x")` used to print nothing until the next newline. Trowel
  reads a pty rather than Emscripten's TTY, so it was probably never affected
  — but it is worth one smoke test now that Scheme buffers are runnable, since
  `display` without a trailing newline is idiomatic Scheme.
- `tur lsp` / `tur dap` reject malformed or oversized `Content-Length`
  (v0.58.0); a `-1` was a heap overflow. Trowel speaks both protocols.

**Deliberately not porting:** share links, project zip import/export, the
PWA/mobile work, the in-page tutorial overlay (`:tutorial` already works in the
REPL pane, which is the native idiom), and the "Solve this" button.

---

## Part I — upstream reports to file

Three findings belong in `turmeric/docs/reported/` regardless of what Trowel
does about them. All three are reproducible with the commands in §1.6.

**I.1 `tur lsp` ignores the file extension when selecting a reader.** A
headerless `.scm` or `.tur.sweet` document gets diagnostics from the wrong
reader and no symbols. The compiler honors the extension
(`reader_type_from_extension`); the LSP consults only the `#lang` line, except
in the formatting handler, which consults only the extension. The two halves
disagree with each other and the compiler disagrees with both. Evidence table
in §1.5.

**I.2 `tur fmt` reprints sweet-expressions as s-expressions, and leaves the
`#lang turmeric/sweet` header on the result.** `--lang r7rs/sweet` checks and
keeps the buffer as written; `--lang sweet` does not, and the header the
formatter preserves verbatim then contradicts the body. Either sweet should be
kept as written like its Scheme counterpart, or `tur fmt` should decline it
out loud.

**I.3 `tur format` destroys a sweet buffer.** The legacy stdin entry point
emits one token per line. If it is deprecated in favour of
`tur fmt --stdin`, it should say so and refuse a non-default reader rather than
shredding it.

---

## Files touched

Part A — `CMakeLists.txt`, `README.md`, `Casks/`,
`resources/linux/com.turmeric_lang.Trowel.metainfo.xml`,
`src/repl/run_buffer.cpp` (delete `runCommandIsSafeFor`)

Part B — `src/editor/dialect.h` (new), `src/editor/lexers.h`,
`src/editor/lexer_adapter.cpp`, `src/editor/scanner_markdown.cpp`,
`resources/Info.plist.in`, `resources/linux/trowel.xml`,
`src/app/main_window.cpp` (dialog filters)

Part C — `src/editor/scanner_turmeric.cpp`, `src/editor/scanner.h`,
`src/editor/lexers.h`, `src/editor/theme_loader.cpp`,
`resources/turmeric-dark.theme.json`

Part D — `src/repl/repl_session.{h,cpp}`, `src/repl/run_buffer.{h,cpp}`,
`src/app/main_window.cpp`

Part E — `src/app/main_window.cpp`, `src/editor/editor_view.{h,cpp}`,
`src/control/control_handlers.cpp`, new picker widget under `src/app/`

Part F — `src/app/main_window.cpp`, and `src/lsp/lsp_manager.{h,cpp}` if F.3

Part G — `src/lsp/lsp_manager.{h,cpp}`, `src/lsp/lsp_position.cpp`,
`src/app/main_window.cpp`

Tests — `tests/smoke/test_lexer_languages.py`, `test_lexer_theme.py`,
`test_run_buffer.py`, `test_repl.py`, `test_lsp.py`, new
`test_dialects.py`, and fixtures: `hello.scm`, `scheme_syntax.scm`,
`saffron_hello.tur`, `sweet_r7rs.tur`, a `#lang`-carrying sweet file, and a
markdown fixture with ```` ```sweet-exp ````, ```` ```scheme ````,
```` ```sh ```` and ```` ```python ```` fences (the last two for §B.3).

---

## Verification

Per part, not at the end.

**A** — `just build && just smoke` green on the new pin; §1.6 re-run against
the staged binary and any answer that differs from this document written back
into it; Run Buffer on `sweet_hello.tur.sweet` *runs* rather than loads.

**B** — fence fixtures assert sweet styles inside a ```` ```sweet-exp ````
block, Scheme styles inside ```` ```scheme ````, sh styles inside
```` ```sh ```` and Python styles inside ```` ```python ```` — the last two
being the §B.3 mask regression, and both must be asserted on the **second**
line of the fence, because the first line is the one that works today. A
`.scm` file reports `Language::R7rs` and `EvalMode::Buffer`.

**C** — `editor.get_style_at` assertions over `scheme_syntax.scm` covering
`#true`, `#\x41`, `#(`, `#u8(`, `|a b|`, `#xff`, `1/2`, `,@`,
`define-record-type`, and a `[` `]` pair; plus a negative test that `|>` in a
`.tur` buffer is still `Operator` and `|a b|` in a `.tur` buffer is **not** a
bar symbol.

**D** — Run Buffer on a `.scm` file from a fresh (Turmeric) REPL prints the
program's output, and the REPL transcript shows the `#lang r7rs` switch and its
acknowledgement. Run Selection from the middle of a `#lang r7rs/sweet` buffer
evaluates under the right reader. A cross-language run after the user has
evaluated something prompts; one after a reset does not.

**E** — picker on an empty buffer: choose `saffron/sweet` → header appears;
choose `turmeric` → header disappears; type a header by hand → picker
reconciles; one Ctrl+Z after a switch restores the previous text exactly;
switch tabs → picker follows the tab. `lang.bases` returns ten rows and
matches `tur dialects --json` from the bundled toolchain.

**F** — Format File on a `.scm` buffer produces the same bytes as
`tur fmt --stdin --lang r7rs` on the same input; on a sweet buffer it declines
with a message and **leaves the buffer byte-identical** (assert the bytes, not
the message — the bytes are the bug).

**G** — signature help appears on `(` and updates on space; a headerless
`.scm` fixture reports **zero** diagnostics and non-empty symbols, with
definition and hover landing on the right lines (the off-by-one in G.3.1).

**Manual, once** — open a Turmeric guide with sweet-exp fences in Trowel's
markdown view and confirm it reads the way it reads on the website. That is
the acceptance test for Part B that no assertion captures.

---

## Explicitly not doing

- **Inventing dialects, labels, or a "convert this file between dialects"
  command.** Trowel offers what `tur dialects` offers. Translating
  s-expressions to sweet-expressions is a formatter feature and, per §F.1, the
  formatter does not yet do it without losing the author's syntax.
- **Offering the legacy `sweet-exp` spelling as a choice.** Accepted on input,
  never written.
- **A Saffron-specific scanner** (§C.4), or an `r7rs.*` theme block (§C.3).
- **Engine selection.** `--engine` exists now and
  `docs/plans/engine-selection.md` is unblocked; that plan should get a
  one-line header update saying so, and nothing else here touches it.
- **Per-dialect bracket/auto-indent configuration beyond extending the
  existing sweet handling to `r7rs/sweet`.** Upstream recorded the same scope
  cut for Monaco so it would not be mistaken for an oversight; same here.
