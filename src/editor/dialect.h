#pragma once

#include "editor/lexers.h"

class QByteArray;
class QString;

namespace trowel {

// The `#lang` BASE axis: which (language, reader) pair a buffer is written in.
//
// WHY THIS IS NOT `Language`. `Language` (lexers.h) is the HIGHLIGHTING axis --
// "which scanner paints this buffer" -- and for that purpose Saffron is
// Turmeric: same reader, same tokens, same keywords, with type annotations
// merely optional. But the run path, the formatter and the dialect picker all
// have to tell Saffron from Turmeric, because the two autoload different
// preludes and `tur` must be told which one it is looking at. One enum cannot
// answer both questions without four near-duplicate scanner entries that exist
// only so other subsystems can read them, so there are two axes: this one is
// derived from the buffer, and `Language` is derived from this.
//
// The rows mirror LANG_BASES[] in turmeric/src/compiler/lang_dialects.c, in the
// order `tur dialects` prints them -- which is also the order the picker shows.
// That table is the upstream source of truth; `tur dialects --json` is its
// machine-readable form and is what the picker reads at runtime rather than
// trusting this copy to still be complete.
enum class Dialect : int {
    Turmeric = 0,
    TurmericCurlyInfix,
    TurmericNeoteric,
    TurmericSweet,
    Saffron,
    SaffronCurlyInfix,
    SaffronNeoteric,
    SaffronSweet,
    R7rs,
    R7rsSweet,

    Count,
};

// The LANGUAGE half. Three values, where there are ten bases: the reader axis
// is the other factor. What hangs off this and not off the base is anything to
// do with the prelude -- which is to say, whether the REPL session has to be
// switched before a buffer can run in it (DialectNeedsSessionSwitch).
enum class DialectLanguage : int {
    Turmeric = 0,
    Saffron,
    R7rs,
};

// The token a `#lang` line spells for this dialect: "turmeric", "saffron/sweet",
// "r7rs/sweet", and so on. Also exactly what `tur repl --lang` accepts -- but
// NOT what `tur fmt --lang` accepts; see DialectFmtLangFlag.
const char* DialectBaseToken(Dialect d);

// "turmeric" | "saffron" | "r7rs", and the reader half unqualified
// ("s-expr" | "curly-infix" | "neoteric" | "sweet" | "scheme"). These match the
// `language` and `reader` columns of `tur dialects`, so a picker row built from
// this table and one built from the JSON read the same.
const char* DialectLanguageName(Dialect d);
const char* DialectReaderName(Dialect d);

DialectLanguage LanguageOf(Dialect d);

// The scanner that paints a buffer in this dialect. Saffron shares Turmeric's,
// and the curly-infix and neoteric readers share it too: `{a + b}` and `f(x)`
// are painted unconditionally by the Turmeric scanner, so those readers only
// *enable* syntax it already knows. Only the sweet readers and the Scheme
// lexeme set need a scanner of their own.
Language HighlightLanguageFor(Dialect d);

// True for the three sweet readers. Sweet-expressions are indentation
// sensitive, which is what makes auto-indent load-bearing and what makes
// `tur fmt` lossy on them (see DialectFmtLangFlag).
bool DialectIsSweet(Dialect d);

// The value to pass `tur fmt --stdin --lang`.
//
// THIS IS A READER SPELLING, NOT A BASE TOKEN, and that is a real trap rather
// than an inconsistency in this file: the two `--lang` flags on the same binary
// take different vocabularies. `tur repl --lang` wants a base and rejects
// `sweet-exp`/`scheme`; `tur fmt --lang` wants a reader and rejects
// `saffron`/`saffron/sweet`/`scheme`. Measured against v0.60.1, both ways
// round. So a Saffron buffer formats under its READER's spelling, which is
// correct -- formatting is a reader concern and Saffron's reader is Turmeric's.
const char* DialectFmtLangFlag(Dialect d);

// Whether `tur fmt` can format this dialect without rewriting it into another
// syntax.
//
// False for `turmeric/sweet` and `saffron/sweet`: `tur fmt --lang sweet`
// reprints a sweet buffer as s-expressions and leaves any `#lang .../sweet`
// header in place, so the result is a file whose header contradicts its body.
// `r7rs/sweet` is checked and returned verbatim, which is what the other two
// should do. Measured against v0.60.1; reported upstream as
// `fmt-reprints-sweet-as-s-expressions`.
bool DialectIsFormattable(Dialect d);

// The extension a scratch copy of such a buffer must be written under, so that
// `tur` picks the right reader off the name. Mirrors
// reader_type_from_extension() in turmeric/src/compiler/reader.c, which
// recognizes exactly two: `.tur.sweet` and `.scm`.
//
// `r7rs/sweet` has NO extension upstream, and that is deliberate here rather
// than an oversight -- such a buffer is written `.tur` and relies on its
// prepended `#lang r7rs/sweet` line, which is the only carrier it has.
const char* DialectScratchExtension(Dialect d);

// Does running a `buffer` dialect inside a `session` dialect require switching
// the session first?
//
// The READER travels with the file: a sweet file is read as sweet in a Turmeric
// session, because the extension or the `#lang` line says so per-file. The
// LANGUAGE does not, because it selects a PRELUDE the session either has loaded
// or does not. Measured against v0.60.1:
//
//   - `(load "x.scm")` in a Turmeric session => "unknown function or operator
//     'r7rs-display'". The forms parse and the names get their `r7rs-` rename;
//     nothing answers to them, because `r7rs/prelude.tur` was never loaded.
//   - the same load after `#lang r7rs` (or under `tur repl --lang r7rs`) => 7.
//   - a `#lang saffron` buffer runs unswitched as far as its own forms go
//     (dynamic typing is a per-FILE registry lookup upstream), but reaches for
//     `vec-map` and friends in vain -- the Saffron prelude autoloads only when
//     the ENTRY file is Saffron.
//
// So: readers no, languages yes.
bool DialectNeedsSessionSwitch(Dialect session, Dialect buffer);

// Resolve a `#lang` base token. Accepts the ten table rows plus the legacy
// `sweet-exp` alias, which upstream still accepts on input and never generates.
bool DialectFromBaseToken(const QByteArray& token, Dialect& out);

// The dialect a FILE NAME implies, for the Turmeric family only. False for a
// name outside the family (a `.md`, a `.c`, an untitled buffer), which is not
// the same answer as "Turmeric" -- the caller needs to tell those apart to
// reproduce upstream's extension-beats-directive precedence.
bool DialectForFileName(const QString& path, Dialect& out);

// The dialect a BUFFER is in: its `#lang` line, falling back to its extension,
// falling back to Turmeric. Same precedence as LanguageForBuffer, which is
// upstream's own (`chosen = (ext_type != READER_TURMERIC) ? ext_type :
// lang_type`, the `load` path in elab_toplevel.c).
Dialect DialectForBuffer(const QString& path, const QByteArray& text);

// The dialect named by `text`'s `#lang` line, or false when it has none.
bool DialectFromDirective(const QByteArray& text, Dialect& out);

// Byte offsets of the `#lang` line within `text` (start inclusive, end past the
// newline), or false when there is none. Exposed because the picker has to
// replace exactly that line and nothing else.
bool LangDirectiveSpan(const QByteArray& text, int& start, int& end);

}
