#include "editor/dialect.h"

#include <QByteArray>
#include <QFileInfo>
#include <QString>

namespace trowel {

namespace {

struct Row {
    Dialect         dialect;
    const char*     base;        // the `#lang` token, and `tur repl --lang`
    DialectLanguage language;
    const char*     languageName;
    const char*     readerName;
    Language        highlight;
    bool            sweet;
    const char*     scratchExt;
};

// One row per legal (language, reader) pair, in `tur dialects` order.
//
// Verified against the pinned v0.61.0 with `tur dialects --json`:
//
//   turmeric              turmeric  s-expr       stable
//   turmeric/curly-infix  turmeric  curly-infix  stable
//   turmeric/neoteric     turmeric  neoteric     stable
//   turmeric/sweet        turmeric  sweet        stable
//   saffron               saffron   s-expr       stable
//   saffron/curly-infix   saffron   curly-infix  stable
//   saffron/neoteric      saffron   neoteric     stable
//   saffron/sweet         saffron   sweet        stable
//   r7rs                  r7rs      scheme       stable
//   r7rs/sweet            r7rs      sweet        stable
//
// All ten are `stable`: `r7rs` graduated out of EXPERIMENTS[] at v0.57.0, so
// there is nothing to enable and no lifecycle warning to suppress.
constexpr Row kRows[] = {
    { Dialect::Turmeric,           "turmeric",             DialectLanguage::Turmeric,
      "turmeric", "s-expr",      Language::Turmeric,      false, ".tur"       },
    { Dialect::TurmericCurlyInfix, "turmeric/curly-infix", DialectLanguage::Turmeric,
      "turmeric", "curly-infix", Language::Turmeric,      false, ".tur"       },
    { Dialect::TurmericNeoteric,   "turmeric/neoteric",    DialectLanguage::Turmeric,
      "turmeric", "neoteric",    Language::Turmeric,      false, ".tur"       },
    { Dialect::TurmericSweet,      "turmeric/sweet",       DialectLanguage::Turmeric,
      "turmeric", "sweet",       Language::TurmericSweet, true,  ".tur.sweet" },
    { Dialect::Saffron,            "saffron",              DialectLanguage::Saffron,
      "saffron",  "s-expr",      Language::Turmeric,      false, ".tur"       },
    { Dialect::SaffronCurlyInfix,  "saffron/curly-infix",  DialectLanguage::Saffron,
      "saffron",  "curly-infix", Language::Turmeric,      false, ".tur"       },
    { Dialect::SaffronNeoteric,    "saffron/neoteric",     DialectLanguage::Saffron,
      "saffron",  "neoteric",    Language::Turmeric,      false, ".tur"       },
    { Dialect::SaffronSweet,       "saffron/sweet",        DialectLanguage::Saffron,
      "saffron",  "sweet",       Language::TurmericSweet, true,  ".tur.sweet" },
    { Dialect::R7rs,               "r7rs",                 DialectLanguage::R7rs,
      "r7rs",     "scheme",      Language::R7rs,          false, ".scm"       },
    // No extension selects `r7rs/sweet` upstream, so a scratch copy goes out as
    // `.tur` and carries its `#lang` line. See DialectScratchExtension.
    { Dialect::R7rsSweet,          "r7rs/sweet",           DialectLanguage::R7rs,
      "r7rs",     "sweet",       Language::R7rsSweet,     true,  ".tur"       },
};

static_assert(sizeof(kRows) / sizeof(kRows[0]) == static_cast<size_t>(Dialect::Count),
              "every Dialect needs a row, in enum order");

const Row& RowFor(Dialect d) {
    const size_t i = static_cast<size_t>(d);
    // Out of range degrades to Turmeric rather than reading past the table,
    // matching lang_traits()' own bounds check upstream.
    if (i >= static_cast<size_t>(Dialect::Count)) return kRows[0];
    return kRows[i];
}

}

const char* DialectBaseToken(Dialect d)     { return RowFor(d).base; }
const char* DialectLanguageName(Dialect d)  { return RowFor(d).languageName; }
const char* DialectReaderName(Dialect d)    { return RowFor(d).readerName; }
DialectLanguage LanguageOf(Dialect d)       { return RowFor(d).language; }
Language HighlightLanguageFor(Dialect d)    { return RowFor(d).highlight; }
bool DialectIsSweet(Dialect d)              { return RowFor(d).sweet; }
const char* DialectScratchExtension(Dialect d) { return RowFor(d).scratchExt; }

bool DialectNeedsSessionSwitch(Dialect session, Dialect buffer) {
    return LanguageOf(session) != LanguageOf(buffer);
}

bool DialectFromBaseToken(const QByteArray& token, Dialect& out) {
    for (const Row& r : kRows) {
        if (token == r.base) { out = r.dialect; return true; }
    }
    // The legacy alias. Accepted on input by lang_base_from_name() upstream and
    // deliberately never generated -- it is not a table row there either, so it
    // is not offered as a picker choice here.
    if (token == "sweet-exp") { out = Dialect::TurmericSweet; return true; }
    return false;
}

bool DialectForFileName(const QString& path, Dialect& out) {
    if (path.isEmpty()) return false;
    const QString lower = QFileInfo(path).fileName().toLower();

    // `.tur.sweet` before `.tur`: the second would otherwise match first and
    // claim a sweet file as plain Turmeric.
    if (lower.endsWith(".tur.sweet")) { out = Dialect::TurmericSweet; return true; }
    if (lower.endsWith(".scm"))       { out = Dialect::R7rs;          return true; }
    // A bare `.sweet` is read as ORDINARY Turmeric upstream -- only
    // `.tur.sweet` maps to the sweet reader -- so treating it as sweet here
    // would make the editor disagree with what actually runs. Such a file gets
    // the sweet reader only by carrying a `#lang` line, same as the toolchain.
    if (lower.endsWith(".tur") || lower.endsWith(".sweet")) {
        out = Dialect::Turmeric;
        return true;
    }
    return false;
}

bool LangDirectiveSpan(const QByteArray& text, int& start, int& end) {
    int i = 0;
    const int n = text.size();

    // An optional `#!` shebang may precede the directive, which is then on
    // line 2. Same accept rule as detect_lang_dialect() in reader.c.
    if (n >= 2 && text[0] == '#' && text[1] == '!'
        && (n < 3 || text[2] == '/' || text[2] == ' ' || text[2] == '\t'
            || text[2] == '\n' || text[2] == '\r')) {
        const int nl = text.indexOf('\n');
        if (nl < 0) return false;  // shebang-only file
        i = nl + 1;
    }

    int j = i;
    while (j < n && (text[j] == ' ' || text[j] == '\t')) ++j;
    if (text.mid(j, 5) != "#lang") return false;

    start = i;
    const int nl = text.indexOf('\n', j);
    end = (nl < 0) ? n : nl + 1;
    return true;
}

bool DialectFromDirective(const QByteArray& text, Dialect& out) {
    int start = 0;
    int end = 0;
    if (!LangDirectiveSpan(text, start, end)) return false;

    int i = start;
    const int n = text.size();
    while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
    i += 5;  // past `#lang`; LangDirectiveSpan established it is there

    const int wsStart = i;
    while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i == wsStart) return false;  // `#langfoo` is not a directive

    const int baseStart = i;
    while (i < n && text[i] != ' ' && text[i] != '\t'
           && text[i] != '\n' && text[i] != '\r') {
        ++i;
    }
    // A trailing token after the base is TUR-E0330 upstream -- the `#lang` LAYER
    // axis was decommissioned in v0.49.0 and `#s"..."` is an unconditional
    // reader macro now. It is still ignored rather than rejected here, because a
    // half-typed line must not flip the editor's highlighting to a fallback
    // between keystrokes.
    return DialectFromBaseToken(text.mid(baseStart, i - baseStart), out);
}

Dialect DialectForBuffer(const QString& path, const QByteArray& text) {
    Dialect fromExt = Dialect::Turmeric;
    const bool named = DialectForFileName(path, fromExt);

    // Upstream's precedence: an extension that already names a non-default
    // reader wins and the directive is a redundant hint; otherwise the
    // directive decides.
    if (named && fromExt != Dialect::Turmeric) return fromExt;

    Dialect fromDirective = Dialect::Turmeric;
    if (DialectFromDirective(text, fromDirective)) return fromDirective;

    return fromExt;
}

}
