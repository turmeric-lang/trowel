#include "editor/scanner.h"

#include <cctype>
#include <string_view>
#include <unordered_set>

namespace trowel {

namespace {

// Keyword sets ported from turmeric/editors/vim-turmeric/syntax/turmeric.vim,
// which is the MAINTAINED one and is deliberately kept in step with
// editors/vscode-turmeric's TextMate grammar. The top-level `vim-syntax/`
// directory these sets originally came from is the older copy, and the two had
// drifted by a dozen `def*` forms and most of the delimited-control and
// concurrency surface.
const std::unordered_set<std::string_view>& DefineKeywords() {
    static const std::unordered_set<std::string_view> s = {
        "def", "defn", "define", "defalias", "defclass", "defdata", "defdynamic",
        "defeffect", "defgadt", "defimage", "defkind", "defmacro", "defmodule",
        "defopaque", "defprotocol", "defrec", "defrecord", "defstruct",
        "deftuple", "deftype", "defworld", "definstance", "defpackage",
        "use", "import", "export",
    };
    return s;
}
const std::unordered_set<std::string_view>& ControlKeywords() {
    static const std::unordered_set<std::string_view> s = {
        "if", "cond", "case", "match", "loop", "while", "for",
        "break", "continue", "return", "and", "or", "not",
        "when", "unless", "recur", "defer",
        // The `cond`/`case` fallback clause head. An ordinary symbol to the
        // reader, so it needs naming explicitly to read as the clause it is.
        "else",
    };
    return s;
}
const std::unordered_set<std::string_view>& TypeKeywords() {
    static const std::unordered_set<std::string_view> s = {
        "type", "typeclass", "impl", "where", "forall", "generic",
        "trait", "any",
    };
    return s;
}
const std::unordered_set<std::string_view>& EffectKeywords() {
    static const std::unordered_set<std::string_view> s = {
        "effect", "handle", "do", "perform", "with", "resume",
        "with-handler", "compose-handlers", "discontinue",
        "reset", "shift", "shift0", "escape",
        "cloneable-reset", "cloneable-shift", "serial-reset", "serial-shift",
        "with-region", "bt-scope", "call/cc",
        "async", "await", "spawn", "yield",
    };
    return s;
}
const std::unordered_set<std::string_view>& ExceptKeywords() {
    static const std::unordered_set<std::string_view> s = {
        "try", "catch", "throw", "finally", "raise",
        "panic", "panic-with", "catch-unwind",
    };
    return s;
}
const std::unordered_set<std::string_view>& SpecialKeywords() {
    static const std::unordered_set<std::string_view> s = {
        "let", "let*", "letrec", "lambda", "fn", "quote", "unquote",
        "quasiquote", "begin", "set!", "use-reader-macros",
    };
    return s;
}
const std::unordered_set<std::string_view>& BuiltinKeywords() {
    static const std::unordered_set<std::string_view> s = {
        "vec", "push", "pop", "len", "nth", "set-nth!", "append",
        "first", "rest", "cons", "reverse", "list", "apply", "map",
        "filter", "reduce", "fold", "print", "println", "read",
        "str", "concat", "split", "coerce", "cast", "type-of",
        "is-a?", "is?",
        "string?", "vector?", "list?", "number?", "symbol?",
        "boolean?", "null?", "atom?", "pair?", "empty?", "extern-c",
    };
    return s;
}
const std::unordered_set<std::string_view>& NilKeywords() {
    static const std::unordered_set<std::string_view> s = {
        "nil", "null", "none", "unit", "nil-value",
    };
    return s;
}

// R7RS. The special-form list is the one in the vim file's `#lang r7rs`-scoped
// block, which is scoped there for the same reason the `scheme` mode below
// gates rules off: two of these lexemes collide with Turmeric syntax.
const std::unordered_set<std::string_view>& SchemeDefineKeywords() {
    static const std::unordered_set<std::string_view> s = {
        "define", "define-syntax", "define-record-type", "define-values",
        "define-library", "import", "export", "include", "include-ci",
    };
    return s;
}
const std::unordered_set<std::string_view>& SchemeSpecialKeywords() {
    static const std::unordered_set<std::string_view> s = {
        "lambda", "case-lambda", "let", "let*", "letrec", "letrec*",
        "let-values", "let*-values", "let-syntax", "letrec-syntax",
        "syntax-rules", "syntax-error", "begin", "if", "cond", "case",
        "and", "or", "when", "unless", "do", "else", "set!",
        "quote", "quasiquote", "unquote", "unquote-splicing",
        "delay", "delay-force", "make-promise", "force",
        "guard", "parameterize", "make-parameter", "cond-expand",
        "dynamic-wind", "call-with-current-continuation", "call/cc",
        "call-with-values", "values", "with-exception-handler",
        "raise", "raise-continuable", "error", "assert",
    };
    return s;
}
const std::unordered_set<std::string_view>& SchemeBuiltinKeywords() {
    static const std::unordered_set<std::string_view> s = {
        "display", "write", "newline", "read", "read-line", "read-char",
        "car", "cdr", "caar", "cadr", "cdar", "cddr", "cons", "list",
        "append", "length", "reverse", "list-tail", "list-ref",
        "map", "for-each", "apply", "vector", "vector-ref", "vector-set!",
        "make-vector", "vector-length", "vector->list", "list->vector",
        "string", "string-ref", "string-length", "string-append",
        "substring", "string->list", "list->string", "string->number",
        "number->string", "string->symbol", "symbol->string",
        "eq?", "eqv?", "equal?", "not", "null?", "pair?", "list?",
        "symbol?", "string?", "number?", "procedure?", "boolean?",
        "vector?", "char?", "zero?", "positive?", "negative?", "odd?",
        "even?", "abs", "quotient", "remainder", "modulo", "gcd", "lcm",
        "floor", "ceiling", "truncate", "round", "sqrt", "expt", "exact",
        "inexact", "min", "max", "assq", "assv", "assoc", "memq", "memv",
        "member", "exit", "eval",
    };
    return s;
}

constexpr bool IsSymbolStart(unsigned char c) {
    return (std::isalpha(c) || c == '_' || c == '+' || c == '-' || c == '*'
            || c == '/' || c == '<' || c == '>' || c == '=' || c == '!'
            || c == '?' || c == '$' || c == '%' || c == '&' || c == '^');
}
constexpr bool IsSymbolCont(unsigned char c) {
    return (std::isalnum(c) || c == '_' || c == '-' || c == '+' || c == '*'
            || c == '/' || c == '<' || c == '>' || c == '=' || c == '!'
            || c == '?' || c == '$' || c == '%' || c == '&' || c == '.'
            || c == ':' || c == '\'');
}

// Which dialect family the walker is running as.
//
// `sweet` and `scheme` are independent: `#lang r7rs/sweet` sets both. What
// `scheme` turns ON is the R7RS lexeme set; what it turns OFF matters just as
// much, because several Turmeric rules actively misread Scheme source -- see
// the gates in the main loop.
struct Mode {
    bool sweet = false;
    bool scheme = false;
};

TurStyle SymbolStyle(std::string_view sym, const Mode& mode) {
    if (sym.empty()) return TurStyle::Identifier;
    if (mode.scheme) {
        if (SchemeDefineKeywords().count(sym))  return TurStyle::Define;
        if (SchemeSpecialKeywords().count(sym)) return TurStyle::Special;
        if (SchemeBuiltinKeywords().count(sym)) return TurStyle::Builtin;
        // Deliberately NOT falling through to the Turmeric sets. `defn`, `fn`,
        // `match` and friends are not Scheme, and painting them as keywords in a
        // `.scm` buffer would claim the file is something it is not.
        return TurStyle::Identifier;
    }
    if (NilKeywords().count(sym))     return TurStyle::Nil;
    if (DefineKeywords().count(sym))  return TurStyle::Define;
    if (ControlKeywords().count(sym)) return TurStyle::Control;
    if (TypeKeywords().count(sym))    return TurStyle::Type;
    if (EffectKeywords().count(sym))  return TurStyle::Effect;
    if (ExceptKeywords().count(sym))  return TurStyle::Except;
    if (SpecialKeywords().count(sym)) return TurStyle::Special;
    if (BuiltinKeywords().count(sym)) return TurStyle::Builtin;
    return TurStyle::Identifier;
}

// `#x1f`, `#b1010`, `#o777`, `#d99`, `#e1.5`, `#i-2/3`, and the doubled forms
// (`#x#e...`). R7RS 7.1.1 lets a radix and an exactness prefix appear in either
// order, so up to two are consumed.
bool IsSchemeNumberPrefixChar(unsigned char c) {
    return c == 'x' || c == 'X' || c == 'o' || c == 'O' || c == 'b' || c == 'B'
        || c == 'd' || c == 'D' || c == 'e' || c == 'E' || c == 'i' || c == 'I';
}

bool IsFenceAt(const char* text, Sci_Position i, Sci_Position end) {
    return i + 3 <= end && text[i] == '`' && text[i + 1] == '`' && text[i + 2] == '`';
}

// Consume datum-comment (`#;`) content on this line, tracking nesting in
// st.turDcDepth. Strings and line comments inside the skipped datum are
// stepped over so their brackets don't disturb the count.
Sci_Position ConsumeDatumBody(const char* text, Sci_Position i, Sci_Position end,
                              LexState& st) {
    while (i < end && st.turDcDepth > 0) {
        const char ch = text[i];
        if (ch == '"') {
            ++i;
            while (i < end && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < end) i += 2;
                else ++i;
            }
            if (i < end) ++i;
            continue;
        }
        if (ch == ';') return end;
        if (ch == '(' || ch == '[' || ch == '{') { ++st.turDcDepth; ++i; continue; }
        if (ch == ')' || ch == ']' || ch == '}') { --st.turDcDepth; ++i; continue; }
        ++i;
    }
    return i;
}

// Consume the part of this line belonging to an open inline-C block, handing
// the body to the C scanner so embedded C is highlighted as C rather than as
// one flat color. Returns the position just past what was consumed.
Sci_Position ConsumeCBlock(const ScanInput& in, LexState& st, Emitter& out,
                           Sci_Position i) {
    Sci_Position fence = i;
    while (fence < in.end && !IsFenceAt(in.text, fence, in.end)) ++fence;

    if (fence > i) {
        const ScanInput body{in.text, i, fence, in.rainbow};
        ScanCLine(body, st, out);
    }
    out.SetGapStyle(static_cast<int>(TurStyle::Default));

    if (fence < in.end) {
        out.Emit(fence, 3, static_cast<int>(TurStyle::CBlock));
        st.turInCBlock = false;
        st.cInComment = false;
        return fence + 3;
    }
    return in.end;
}

// True when `$` at `i` stands alone as the sweet reader's GROUP/SPLIT marker
// rather than opening a symbol like `$foo` (IsSymbolStart accepts '$').
bool IsSweetGroupAt(const char* text, Sci_Position i, Sci_Position end) {
    return text[i] == '$'
        && (i + 1 >= end || !IsSymbolCont(static_cast<unsigned char>(text[i + 1])));
}

// True when `\` at `i` is the sweet reader's SPLIT marker: a lone backslash,
// not the `#\` character-literal prefix (which the main loop consumes first).
bool IsSweetSplitAt(const char* text, Sci_Position i, Sci_Position end) {
    return text[i] == '\\'
        && (i + 1 >= end || text[i + 1] == ' ' || text[i + 1] == '\t');
}

void ScanTurmeric(const ScanInput& in, LexState& st, Emitter& out, const Mode& mode) {
    const char* text = in.text;
    const Sci_Position end = in.end;
    Sci_Position i = in.begin;

    out.SetGapStyle(static_cast<int>(TurStyle::Default));
    auto emit = [&](Sci_Position from, Sci_Position len, TurStyle s) {
        out.Emit(from, len, static_cast<int>(s));
    };

    // --- Header lines: shebang and #lang -----------------------------------
    //
    // Both are position-sensitive in reader.c, so they are handled before the
    // main loop rather than as another `#` case inside it.
    //
    // A Racket-style shebang counts only at byte 0 of the file and must be
    // `#!` followed by `/`, a blank, or end of line — that is what keeps a
    // future `#!fold-case`-style dispatch form from being eaten. It runs to
    // end of line and nothing more, so a `#lang` on the next line is still
    // seen, both here and by the reader.
    if (in.line == 0 && i < end
        && text[i] == '#' && i + 1 < end && text[i + 1] == '!'
        && (i + 2 >= end || text[i + 2] == '/' || text[i + 2] == ' '
            || text[i + 2] == '\t')) {
        emit(i, end - i, TurStyle::LineComment);
        st.turAfterShebang = true;
        return;
    }

    // `#lang` is only a directive in the leading position: line 1 of the file,
    // or line 2 when line 1 was a shebang. Anywhere else it is ordinary source
    // and falls through to the main loop, which is what makes the styling agree
    // with LanguageForBuffer()'s choice of reader.
    if (in.line == 0 || (in.line == 1 && st.turAfterShebang)) {
        Sci_Position h = i;
        while (h < end && (text[h] == ' ' || text[h] == '\t')) ++h;
        if (h + 5 <= end && text[h] == '#' && text[h + 1] == 'l' && text[h + 2] == 'a'
            && text[h + 3] == 'n' && text[h + 4] == 'g'
            && (h + 5 >= end || text[h + 5] == ' ' || text[h + 5] == '\t')) {
            emit(h, 5, TurStyle::LangDir);
            h += 5;
            while (h < end && (text[h] == ' ' || text[h] == '\t')) ++h;
            const Sci_Position nameStart = h;
            while (h < end && text[h] != ' ' && text[h] != '\t') ++h;
            emit(nameStart, h - nameStart, TurStyle::Type);
            // Trailing layer tokens (`stringed`, `refined`, ...) are part of
            // the directive line, not source.
            if (h < end) emit(h, end - h, TurStyle::LangDir);
            return;
        }
    }

    // --- Continuations from previous lines ---------------------------------

    if (st.turBlockDepth > 0) {
        const Sci_Position start = i;
        while (i < end && st.turBlockDepth > 0) {
            if (i + 1 < end && text[i] == '#' && text[i + 1] == '|') {
                ++st.turBlockDepth; i += 2; continue;
            }
            if (i + 1 < end && text[i] == '|' && text[i + 1] == '#') {
                --st.turBlockDepth; i += 2; continue;
            }
            ++i;
        }
        emit(start, i - start, TurStyle::BlockComment);
    }

    if (st.turInString && i < end) {
        const Sci_Position start = i;
        while (i < end && text[i] != '"') {
            if (text[i] == '\\' && i + 1 < end) i += 2;
            else ++i;
        }
        if (i < end && text[i] == '"') { ++i; st.turInString = false; }
        emit(start, i - start, TurStyle::String);
    }

    if (st.turInDatumComment && i < end) {
        const Sci_Position start = i;
        i = ConsumeDatumBody(text, i, end, st);
        emit(start, i - start, TurStyle::LineComment);
        if (st.turDcDepth == 0) st.turInDatumComment = false;
    }

    if (st.turInCBlock) {
        i = ConsumeCBlock(in, st, out, i);
    }

    // --- Main loop ---------------------------------------------------------

    while (i < end) {
        const char c = text[i];

        if (c == ' ' || c == '\t') { ++i; continue; }  // backfilled as Default

        // Line / doc comment.
        if (c == ';') {
            const bool doc3 = (i + 2 < end && text[i + 1] == ';' && text[i + 2] == ';');
            emit(i, end - i, doc3 ? TurStyle::DocComment : TurStyle::LineComment);
            i = end;
            continue;
        }

        // Nested block comment.
        if (c == '#' && i + 1 < end && text[i + 1] == '|') {
            const Sci_Position start = i;
            i += 2;
            st.turBlockDepth = 1;
            while (i < end && st.turBlockDepth > 0) {
                if (i + 1 < end && text[i] == '#' && text[i + 1] == '|') {
                    ++st.turBlockDepth; i += 2; continue;
                }
                if (i + 1 < end && text[i] == '|' && text[i + 1] == '#') {
                    --st.turBlockDepth; i += 2; continue;
                }
                ++i;
            }
            emit(start, i - start, TurStyle::BlockComment);
            continue;
        }

        // Datum comment: #; skips the next s-expression.
        if (c == '#' && i + 1 < end && text[i + 1] == ';') {
            const Sci_Position start = i;
            i += 2;
            while (i < end && (text[i] == ' ' || text[i] == '\t')) ++i;
            if (i < end) {
                const char dc = text[i];
                if (dc == '(' || dc == '[' || dc == '{') {
                    ++i;
                    st.turDcDepth = 1;
                    st.turInDatumComment = true;
                    i = ConsumeDatumBody(text, i, end, st);
                    if (st.turDcDepth == 0) st.turInDatumComment = false;
                } else if (dc == '"') {
                    ++i;
                    while (i < end && text[i] != '"') {
                        if (text[i] == '\\' && i + 1 < end) i += 2;
                        else ++i;
                    }
                    if (i < end) ++i;
                } else {
                    while (i < end && text[i] != ' ' && text[i] != '\t'
                           && text[i] != '(' && text[i] != ')'
                           && text[i] != '[' && text[i] != ']'
                           && text[i] != '{' && text[i] != '}') {
                        ++i;
                    }
                }
            }
            emit(start, i - start, TurStyle::LineComment);
            continue;
        }

        // Inline C block: ```c ... ``` or ``` ... ```. The fence markers and
        // language tag stay CBlock-colored; the body goes to the C scanner.
        if (IsFenceAt(text, i, end)) {
            const Sci_Position start = i;
            i += 3;
            while (i < end && std::isalnum(static_cast<unsigned char>(text[i]))) ++i;
            emit(start, i - start, TurStyle::CBlock);
            st.turInCBlock = true;
            st.cInComment = false;
            i = ConsumeCBlock(in, st, out, i);
            continue;
        }

        // String.
        if (c == '"') {
            const Sci_Position start = i;
            ++i;
            st.turInString = true;
            while (i < end && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < end) i += 2;
                else ++i;
            }
            if (i < end && text[i] == '"') { ++i; st.turInString = false; }
            emit(start, i - start, TurStyle::String);
            continue;
        }

        // Character literal: #\name or #\<char>, plus Scheme's #\x<hex>.
        if (c == '#' && i + 1 < end && text[i + 1] == '\\') {
            const Sci_Position start = i;
            i += 2;
            // `#\x41` is a hex scalar value in R7RS (7.1.1). The alpha run
            // below would otherwise stop after the `x`, leaving `41` to be
            // painted as a separate number.
            if (mode.scheme && i + 1 < end && (text[i] == 'x' || text[i] == 'X')
                && std::isxdigit(static_cast<unsigned char>(text[i + 1]))) {
                ++i;
                while (i < end && std::isxdigit(static_cast<unsigned char>(text[i]))) ++i;
            } else if (i < end) {
                if (std::isalpha(static_cast<unsigned char>(text[i]))) {
                    while (i < end && std::isalpha(static_cast<unsigned char>(text[i]))) ++i;
                } else {
                    ++i;
                }
            }
            emit(start, i - start, TurStyle::CharLit);
            continue;
        }

        // Booleans: #t / #f, and Scheme's #true / #false, at a word boundary.
        if (c == '#' && i + 1 < end) {
            const std::string_view tail(text + i + 1,
                                        static_cast<size_t>(end - i - 1));
            Sci_Position blen = 0;
            if (tail.rfind("true", 0) == 0)       blen = 5;
            else if (tail.rfind("false", 0) == 0) blen = 6;
            else if (tail[0] == 't' || tail[0] == 'f') blen = 2;
            if (blen > 0
                && (i + blen >= end
                    || !IsSymbolCont(static_cast<unsigned char>(text[i + blen])))) {
                emit(i, blen, TurStyle::Boolean);
                i += blen;
                continue;
            }
        }

        if (mode.scheme) {
            // `#(` vector and `#u8(` bytevector prefixes. Only the prefix is
            // painted; the `(` falls through to the bracket handler below so it
            // still counts toward nesting depth and keeps rainbow colouring and
            // the unmatched-closer check honest.
            if (c == '#' && i + 1 < end && text[i + 1] == '(') {
                emit(i, 1, TurStyle::SchemeVector);
                ++i;
                continue;
            }
            if (c == '#' && i + 3 < end && text[i + 1] == 'u' && text[i + 2] == '8'
                && text[i + 3] == '(') {
                emit(i, 3, TurStyle::SchemeVector);
                i += 3;
                continue;
            }
            // Radix and exactness prefixes, up to two of them.
            if (c == '#' && i + 1 < end
                && IsSchemeNumberPrefixChar(static_cast<unsigned char>(text[i + 1]))) {
                const Sci_Position start = i;
                i += 2;
                if (i + 1 < end && text[i] == '#'
                    && IsSchemeNumberPrefixChar(static_cast<unsigned char>(text[i + 1]))) {
                    i += 2;
                }
                if (i < end && (text[i] == '+' || text[i] == '-')) ++i;
                while (i < end
                       && (std::isxdigit(static_cast<unsigned char>(text[i]))
                           || text[i] == '.' || text[i] == '/')) {
                    ++i;
                }
                emit(start, i - start, TurStyle::Number);
                continue;
            }
            // `|a bar symbol|`. Scoped to Scheme because in a Turmeric buffer
            // this would swallow the `|` of `#refine{x : T | pred}` and the `|`
            // of the `|>` operator.
            if (c == '|') {
                const Sci_Position start = i;
                ++i;
                while (i < end && text[i] != '|') {
                    if (text[i] == '\\' && i + 1 < end) i += 2;
                    else ++i;
                }
                if (i < end) ++i;  // the closing bar
                emit(start, i - start, TurStyle::BarSymbol);
                continue;
            }
            // Scheme's unquote and unquote-splicing. Turmeric spells these `~`
            // and `~@`, and leaves a bare `,` unstyled.
            if (c == ',') {
                const Sci_Position len = (i + 1 < end && text[i + 1] == '@') ? 2 : 1;
                emit(i, len, TurStyle::Quote);
                i += len;
                continue;
            }
        }

        // Everything from here to the sweet markers is Turmeric-only syntax, and
        // in a Scheme buffer each rule is not merely useless but wrong: `#?`,
        // `^attr`, `:keyword`, `::`, `|>` and `~` are all ordinary characters or
        // ordinary identifier bytes to the Scheme reader. A leading-colon
        // identifier in particular is a legal Scheme symbol (there is an
        // upstream report, `r7rs-leading-colon-identifiers.md`), so painting it
        // as a keyword literal would be a lie about the language.
        if (!mode.scheme) {

        // Reader conditional prefix #?
        if (c == '#' && i + 1 < end && text[i + 1] == '?') {
            emit(i, 2, TurStyle::LangDir);
            i += 2;
            continue;
        }

        // Metadata annotation: ^foo
        if (c == '^' && i + 1 < end && std::isalpha(static_cast<unsigned char>(text[i + 1]))) {
            const Sci_Position start = i;
            ++i;
            while (i < end && (std::isalnum(static_cast<unsigned char>(text[i]))
                               || text[i] == '_' || text[i] == '-')) {
                ++i;
            }
            emit(start, i - start, TurStyle::Metadata);
            continue;
        }

        // Keyword literal: :foo
        if (c == ':' && i + 1 < end
            && (std::isalpha(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '_')) {
            const Sci_Position start = i;
            ++i;
            while (i < end && IsSymbolCont(static_cast<unsigned char>(text[i]))) ++i;
            emit(start, i - start, TurStyle::KeywordLit);
            continue;
        }

        // Special ops :: and |>
        if (c == ':' && i + 1 < end && text[i + 1] == ':') {
            emit(i, 2, TurStyle::Operator); i += 2; continue;
        }
        if (c == '|' && i + 1 < end && text[i + 1] == '>') {
            emit(i, 2, TurStyle::Operator); i += 2; continue;
        }

        // Reader macros: ~@ ~
        if (c == '~' && i + 1 < end && text[i + 1] == '@') {
            emit(i, 2, TurStyle::Quote); i += 2; continue;
        }
        if (c == '~') {
            emit(i, 1, TurStyle::Quote); ++i; continue;
        }

        }  // !mode.scheme

        // Quote and quasiquote are shared, so they sit outside the gate.
        if (c == '`' || c == '\'') {
            emit(i, 1, TurStyle::Quote); ++i; continue;
        }

        // Sweet-expression reader markers. Checked before the identifier path
        // because IsSymbolStart accepts '$', which would otherwise swallow a
        // standalone GROUP marker as a one-character symbol.
        if (mode.sweet && (IsSweetGroupAt(text, i, end) || IsSweetSplitAt(text, i, end))) {
            emit(i, 1, TurStyle::SweetMarker);
            ++i;
            continue;
        }

        // Delimiters. With rainbow coloring on, every bracket type is painted
        // by its nesting depth so matching pairs share a color; an unmatched
        // closer is flagged. Without it, brackets fall back to the flat
        // Delim / CurlyInfix styles.
        if (c == '(' || c == '[' || c == '{') {
            const TurStyle flat = (c == '{') ? TurStyle::CurlyInfix : TurStyle::Delim;
            emit(i, 1, in.rainbow ? RainbowStyleForDepth(st.turBracketDepth) : flat);
            if (st.turBracketDepth < kMaxTurBracketDepth) ++st.turBracketDepth;
            ++i;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            const TurStyle flat = (c == '}') ? TurStyle::CurlyInfix : TurStyle::Delim;
            TurStyle s = flat;
            if (in.rainbow) {
                if (st.turBracketDepth > 0) {
                    --st.turBracketDepth;
                    s = RainbowStyleForDepth(st.turBracketDepth);
                } else {
                    s = TurStyle::BracketError;
                }
            }
            emit(i, 1, s);
            ++i;
            continue;
        }

        // Numbers.
        if (std::isdigit(static_cast<unsigned char>(c))
            || ((c == '-' || c == '.') && i + 1 < end
                && std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
            const Sci_Position start = i;
            if (c == '-') ++i;
            if (i + 1 < end && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
                i += 2;
                while (i < end && std::isxdigit(static_cast<unsigned char>(text[i]))) ++i;
            } else if (i + 1 < end && text[i] == '0'
                       && (text[i + 1] == 'b' || text[i + 1] == 'B')) {
                i += 2;
                while (i < end && (text[i] == '0' || text[i] == '1')) ++i;
            } else {
                while (i < end && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
                if (i < end && text[i] == '.') {
                    ++i;
                    while (i < end && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
                }
                if (i < end && (text[i] == 'e' || text[i] == 'E')) {
                    ++i;
                    if (i < end && (text[i] == '+' || text[i] == '-')) ++i;
                    while (i < end && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
                }
                // Scheme rationals: `1/2` is one number, not `1` followed by
                // the symbol `/2`. Turmeric has no rational literal and `/` is
                // an ordinary symbol byte there, so this is scoped.
                if (mode.scheme && i + 1 < end && text[i] == '/'
                    && std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
                    ++i;
                    while (i < end && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
                }
            }
            emit(start, i - start, TurStyle::Number);
            continue;
        }

        // Identifier / keyword.
        if (IsSymbolStart(static_cast<unsigned char>(c))
            // `:` starts a symbol in Scheme but a keyword literal in Turmeric,
            // and the keyword-literal branch above is gated off in Scheme mode,
            // so without this a leading-colon identifier would fall to the
            // unknown-byte default one character at a time.
            || (mode.scheme && c == ':')) {
            const Sci_Position start = i;
            while (i < end && IsSymbolCont(static_cast<unsigned char>(text[i]))) ++i;
            // Guarantee progress. `IsSymbolStart` and `IsSymbolCont` are two
            // separate lists and they do not agree: `^` starts a symbol but
            // does not continue one, so a bare `^` matched the branch, consumed
            // zero characters, emitted a zero-length token and `continue`d with
            // `i` unchanged — an infinite loop that hung the whole UI thread.
            //
            // Typing `^` and pausing before the `m` of `^mut` was enough. The
            // guard is on the invariant rather than on the character, because
            // any future divergence between the two lists is the same bug.
            if (i == start) ++i;
            const std::string_view sym(text + start, static_cast<size_t>(i - start));
            TurStyle s = SymbolStyle(sym, mode);
            if (s == TurStyle::Identifier && i < end && text[i] == '(') {
                s = TurStyle::NeotericCall;
            }
            emit(start, i - start, s);
            continue;
        }

        // Fallback: unknown byte.
        emit(i, 1, TurStyle::Default);
        ++i;
    }
}

}

void ScanTurmericLine(const ScanInput& in, LexState& st, Emitter& out) {
    ScanTurmeric(in, st, out, Mode{/*sweet=*/false, /*scheme=*/false});
}

void ScanTurmericSweetLine(const ScanInput& in, LexState& st, Emitter& out) {
    ScanTurmeric(in, st, out, Mode{/*sweet=*/true, /*scheme=*/false});
}

void ScanR7rsLine(const ScanInput& in, LexState& st, Emitter& out) {
    ScanTurmeric(in, st, out, Mode{/*sweet=*/false, /*scheme=*/true});
}

void ScanR7rsSweetLine(const ScanInput& in, LexState& st, Emitter& out) {
    ScanTurmeric(in, st, out, Mode{/*sweet=*/true, /*scheme=*/true});
}

}
