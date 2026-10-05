#pragma once

#include <ILexer.h>

// Forward-declared rather than including <QString>: pulling in a Qt header
// here would also drag in Qt's `emit` keyword macro, which collides with the
// natural name for a style-emitting helper throughout the scanners.
class QString;
class QByteArray;

namespace trowel {

// Languages Trowel can highlight. A document is lexed by one root language;
// Markdown additionally delegates fenced-code bodies to a guest language, and
// Turmeric delegates its inline ``` blocks to C.
// New languages must be appended: the value is packed into a 4-bit field when
// Markdown records its guest (see kMdGuestShift in lexer_adapter.cpp), and 15
// is taken as the "no recognized tag" sentinel, so ids must stay in 0..14.
enum class Language : int {
    Turmeric = 0,
    C,
    Markdown,
    Json,
    Just,
    // Sweet-expression Turmeric: same tokens and keywords, plus the
    // indentation-sensitive reader's marker syntax. Its own language rather
    // than a flag so path/fence dispatch and the run path can switch on it.
    TurmericSweet,
    CMake,
    Toml,
    Sh,
    Python,
    // R7RS Scheme (`#lang r7rs`, `.scm`), and the same under SRFI-110
    // sweet-expressions (`#lang r7rs/sweet`). Its own scanners rather than a
    // flag on the Turmeric one: the lexeme set genuinely differs (`#true`,
    // `#\x41`, `#(`, `#u8(`, `|bar symbols|`, radix prefixes, `,@`) and two of
    // those rules collide with Turmeric syntax, so they have to be gated off
    // rather than merely added. See editor/dialect.h for which `#lang` bases
    // map onto which of these.
    R7rs,
    R7rsSweet,
    // Plain text: no highlighting, no line comment.  `.txt`, `.text`, `.rst`,
    // `.adoc`, `.org`, and common extensionless names like LICENSE, COPYING.
    // The fallback for unknown files stays Turmeric, so extensionless Turmeric
    // scripts and shebang files are not misclassified.
    PlainText,
    // Data-driven lexer configured from a Turmeric syntax descriptor
    // (§2.2 syntax plugins). The descriptor index is stored in the
    // leaf-language bit field (jsonDepth slot) at scan time.
    PluginSyntax,

    // Keep last: the count, which the Markdown guest field's width is asserted
    // against in lexer_adapter.cpp. The guest field is four bits with 15 as its
    // "no recognized tag" sentinel, so ids must stay in 0..14.
    LanguageCount,
};

// Pick a language from a file path alone. Unrecognized (and empty, i.e.
// untitled) paths fall back to Turmeric, matching Trowel's pre-existing
// behavior.
Language LanguageForPath(const QString& path);

// Pick a language from a path *and* the buffer's contents, so that a `#lang`
// directive on line 1 selects the reader the way the toolchain does. This is
// what an editor buffer should use; LanguageForPath is the extension half of
// it. Untitled buffers get a language this way too.
Language LanguageForBuffer(const QString& path, const QByteArray& text);

// The buffer's `#lang` line, newline-terminated, or empty if it has none.
// Running a *selection* would otherwise drop it, losing any layers and any
// base the file extension cannot express.
QByteArray LangDirectiveLine(const QByteArray& text);

// Style IDs applied by the Turmeric scanner. Names mirror the theme keys in
// resources/turmeric-dark.theme.json.
enum class TurStyle : int {
    Default = 0,
    LineComment,
    DocComment,
    BlockComment,
    String,
    StringEscape,
    Number,
    Boolean,
    Nil,
    KeywordLit,
    CharLit,
    Metadata,
    Quote,
    Operator,
    Define,
    Control,
    Type,
    Effect,
    Except,
    Special,
    Builtin,
    CBlock,
    LangDir,
    Delim,
    CurlyInfix,
    NeotericCall,
    Identifier,
    Invalid,
    // Sweet-expression reader markers ($ and a lone \). Only ever emitted by
    // the sweet variant of the Turmeric scanner.
    SweetMarker,
    // Scheme's vector and bytevector prefixes, `#(` and `#u8(`.
    SchemeVector,
    // A Scheme `|bar symbol|`. R7RS mode only: in a Turmeric buffer `|` opens
    // neither a symbol nor anything else, and painting it as one would swallow
    // the `|` of `#refine{x : T | pred}`.
    BarSymbol,

    Count,

    // Rainbow bracket styles. These deliberately start above Scintilla's
    // predefined style range (STYLE_DEFAULT=32 .. STYLE_FOLDDISPLAYTEXT=39) so
    // they never collide with those reserved slots. Brackets are colored by
    // their nesting depth, cycling through Rainbow0..Rainbow6; an unmatched
    // closing bracket is painted with BracketError.
    Rainbow0 = 40,
    Rainbow1,
    Rainbow2,
    Rainbow3,
    Rainbow4,
    Rainbow5,
    Rainbow6,
    BracketError,
};

// Each additional language gets its own contiguous block of style IDs, clear
// of both the Turmeric range above and Scintilla's reserved 32..39.
enum class CStyle : int {
    Default = 48,
    Comment,
    DocComment,
    Preproc,
    Keyword,
    Type,
    String,
    StringEscape,
    Char,
    Number,
    Operator,
    Identifier,
};

enum class MdStyle : int {
    Default = 64,
    Heading,
    Emphasis,
    Strong,
    CodeSpan,
    Fence,
    CodeBlock,
    LinkText,
    LinkUrl,
    Blockquote,
    ListMarker,
    Rule,
    Html,
    Escape,
};

enum class JsonStyle : int {
    Default = 80,
    Key,
    String,
    StringEscape,
    Number,
    Literal,
    Operator,
    Error,
};

enum class JustStyle : int {
    Default = 91,
    Comment,
    RecipeName,
    Dependency,
    Parameter,
    Assign,
    Interpolation,
    Backtick,
    Keyword,
    String,
    Number,
    Body,
    Attribute,
    Operator,
};

enum class CMakeStyle : int {
    Default = 112,
    Comment,
    Command,
    Keyword,
    Variable,
    String,
    StringEscape,
    Number,
    Operator,
    Identifier,
};

enum class TomlStyle : int {
    Default = 128,
    Comment,
    Table,
    Key,
    String,
    StringEscape,
    Number,
    Boolean,
    DateTime,
    Operator,
    Error,
};

enum class ShStyle : int {
    Default = 144,
    Comment,
    Keyword,
    Builtin,
    Function,
    String,
    StringEscape,
    Variable,
    Number,
    Operator,
    Backtick,
    Identifier,
};

enum class PyStyle : int {
    Default = 160,
    Comment,
    Keyword,
    Builtin,
    Decorator,
    ClassName,
    FuncName,
    String,
    StringEscape,
    TripleString,
    Number,
    Operator,
    Identifier,
};

// Data-driven syntax plugin styles. These are the Scintilla style IDs the
// plugin syntax scanner paints with. The theme maps them to colors the same
// way it maps the built-in language styles.
enum class PluginSyntaxStyleId : int {
    Default = 174,
    Keyword,
    Comment,
    String,
    Number,
    Operator,
    Identifier,
    Preprocessor,
    Type,
    Function,
    Variable,
    Constant,
    Error,
};

// Number of distinct colors the rainbow-bracket cycle uses.
inline constexpr int kRainbowLevels = 7;
// Highest style id any scanner can emit (inclusive). EditorView applies the
// editor font across 0..kMaxStyleId.
inline constexpr int kMaxStyleId = static_cast<int>(PluginSyntaxStyleId::Error);

// The line-comment token for `lang`, or empty when the language has no
// line comment (JSON, Markdown, PlainText).  Used by Edit > Toggle Comment.
QByteArray LineCommentToken(Language lang);

// Whether `lang` has a fold strategy (bracket depth, indentation, or
// headings).  Languages without a strategy get no fold margin.
bool HasFoldStrategy(Language lang);

// Construct a fresh lexer for `lang`. Ownership passes to Scintilla — release
// happens via ILexer5::Release(). When `rainbow` is true, brackets are styled
// by nesting depth (Rainbow0..); otherwise they use flat per-language styles.
Scintilla::ILexer5* CreateLexerForLanguage(Language lang, bool rainbow = true);

// Set the global syntax registry and descriptor index for plugin syntax lexers.
// Called by PluginHost before creating a PluginSyntax lexer.
class SyntaxRegistry;
void SetSyntaxRegistry(const SyntaxRegistry* reg);
void SetPluginSyntaxDescriptorIndex(int index);

}
