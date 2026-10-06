#pragma once

#include "editor/lexers.h"

#include <ILexer.h>

#include <algorithm>
#include <vector>

namespace trowel {

struct SyntaxDescriptor;

// ---------------------------------------------------------------------------
// StyleSink
// ---------------------------------------------------------------------------

// Abstract destination for style bytes. The scanners write through an
// Emitter, which writes through a StyleSink. DocumentSink paints into
// Scintilla's IDocument (the live editor); BufferSink paints into a plain
// byte array (the off-thread minimap renderer). The sink receives absolute
// positions (base + offset) so each implementation handles the base its own
// way.
class StyleSink {
public:
    virtual ~StyleSink() = default;
    virtual void Paint(Sci_Position pos, Sci_Position len, int style) = 0;
};

// Paints into a flat byte array. Used by the off-thread minimap renderer,
// which cannot touch Scintilla. `pos` is absolute (base + from); for the
// minimap base is 0, so pos is the offset within the strip text.
class BufferSink final : public StyleSink {
public:
    explicit BufferSink(Sci_Position len) : styles_(static_cast<size_t>(len), 0) {}
    void Paint(Sci_Position pos, Sci_Position len, int style) override {
        if (pos < 0) return;
        const size_t start = static_cast<size_t>(pos);
        const size_t end = std::min(start + static_cast<size_t>(len), styles_.size());
        const unsigned char s = static_cast<unsigned char>(style);
        std::fill(styles_.begin() + start, styles_.begin() + end, s);
    }
    const std::vector<unsigned char>& styles() const { return styles_; }
private:
    std::vector<unsigned char> styles_;
};

// ---------------------------------------------------------------------------
// Emitter
// ---------------------------------------------------------------------------

// Paints style runs onto the document. Offsets handed to Emit() are relative
// to the start of the buffer the scanners were handed; the Emitter adds the
// document base itself.
//
// Emit() also backfills: any bytes skipped between the previous run and `from`
// are painted with the current gap style. Scanners therefore only have to emit
// the spans they care about, and a scanner bug can never leave stale styling
// behind.
class Emitter {
public:
    Emitter(StyleSink& sink, Sci_Position base)
        : sink_(&sink), base_(base) {}

    void SetGapStyle(int style) { gapStyle_ = style; }
    int gapStyle() const { return gapStyle_; }

    void Emit(Sci_Position from, Sci_Position len, int style);

    // Paint everything up to (but excluding) `to` with the gap style.
    void FillTo(Sci_Position to);

private:
    void Paint(Sci_Position from, Sci_Position len, int style);

    StyleSink* sink_;
    Sci_Position base_;
    Sci_Position next_ = 0;
    int gapStyle_ = 0;
};

// ---------------------------------------------------------------------------
// Per-line state
// ---------------------------------------------------------------------------

// Everything a scanner needs to carry from one line to the next. Every field
// gets its own slice of the 31 usable bits of Scintilla's int line state — bit
// 31 stays clear so the packed value is never negative. See the bit layout in
// lexer_adapter.cpp; the widths below are what the caps enforce.
//
// The deepest legal nesting is Markdown hosting a Turmeric fence that itself
// hosts an inline C block, so Markdown, Turmeric, and C state must all be live
// simultaneously. Every other language is a *leaf*: it can be a root language
// or a Markdown guest, but never a host, so at most one of them is ever live.
// Their state therefore shares one small bit field (see kLeafShift in
// lexer_adapter.cpp) instead of each claiming its own — there is not room for
// ten languages otherwise. ResetGuestState() clears the whole field whenever
// the active language changes, so stale bits can never leak across a switch.
struct LexState {
    // --- Turmeric ---
    int turBlockDepth = 0;    // nested #| |# depth, capped
    int turDcDepth = 0;       // nested #; datum-comment depth, capped
    int turBracketDepth = 0;  // bracket nesting, for rainbow coloring
    // Minimum bracket depth reached on the current line.  Not packed into
    // line state (the 31 bits are full); used only within Lex() to compute
    // fold levels.  Initialized to the depth at line start before each
    // ScanLine call, updated by the scanner on each closer.
    int minBracketDepth = 0;
    bool turInString = false;
    bool turInCBlock = false;
    bool turInDatumComment = false;
    // Line 0 was a `#!` shebang, so a `#lang` directive on line 1 is still in
    // the leading position the reader accepts it from.
    bool turAfterShebang = false;

    // --- leaf languages (mutually exclusive; share one bit field) ---
    int jsonDepth = 0;
    bool justInRecipe = false;
    int pyStringMode = 0;    // 0 none, 1 ''' , 2 """
    int tomlStringMode = 0;  // 0 none, 1 ''' , 2 """
    int shStringMode = 0;    // 0 none, 1 '...' , 2 "..."

    // --- C (coexists with Turmeric) ---
    bool cInComment = false;

    // --- Markdown (coexists with any guest) ---
    bool mdInFence = false;
    bool mdFenceTilde = false;  // fence uses ~~~ rather than ```
    int mdFenceExtra = 0;       // opening fence length minus 3, capped at 7
    Language mdGuest = Language::Turmeric;
    bool mdGuestPlain = true;   // fence has no recognized language tag
};

// Field caps implied by the bit widths above. Exceeding them saturates rather
// than wrapping, so pathological input degrades gracefully instead of
// corrupting neighbouring fields.
inline constexpr int kMaxTurBlockDepth = 7;
inline constexpr int kMaxTurDcDepth = 7;
inline constexpr int kMaxTurBracketDepth = 127;
inline constexpr int kMaxJsonDepth = 31;
// Opening-fence length minus 3. Two bits: a ``````` fence (six backticks) is
// already well past anything real, and the bit had to go to the widened guest
// field when the language count outgrew three bits.
inline constexpr int kMaxMdFenceExtra = 3;

int PackLexState(const LexState& st);
LexState UnpackLexState(int packed);

// Reset the fields that alias between root/guest languages. Called when the
// active language changes so packing stays lossless.
void ResetGuestState(LexState& st);

// ---------------------------------------------------------------------------
// Scanners
// ---------------------------------------------------------------------------

// One line's worth of text to scan. `begin`/`end` are buffer-relative offsets
// bounding the line's content; `end` excludes the line terminator, which the
// caller styles.
struct ScanInput {
    const char* text = nullptr;
    Sci_Position begin = 0;
    Sci_Position end = 0;
    bool rainbow = true;
    // Zero-based document line number. Only the header-sensitive constructs
    // need it — a `#!` shebang is a shebang at line 0 only, and `#lang` counts
    // only on line 0, or line 1 when line 0 was a shebang.
    Sci_Position line = 0;
};

void ScanTurmericLine(const ScanInput& in, LexState& st, Emitter& out);
// Turmeric with the sweet-expression reader's markers recognized. Identical to
// ScanTurmericLine apart from the extra marker tokens, so both share an impl.
void ScanTurmericSweetLine(const ScanInput& in, LexState& st, Emitter& out);
// R7RS Scheme, and Scheme under SRFI-110 sweet-expressions. A third and fourth
// mode of the same walker: the overlap with Turmeric is most of a lisp scanner
// (`#|` block comments, `#;` datum comments, strings, quote, rainbow brackets),
// so what these add is the Scheme lexeme set and what they SUBTRACT is the
// Turmeric-only syntax that would otherwise misread a Scheme buffer.
void ScanR7rsLine(const ScanInput& in, LexState& st, Emitter& out);
void ScanR7rsSweetLine(const ScanInput& in, LexState& st, Emitter& out);
void ScanCLine(const ScanInput& in, LexState& st, Emitter& out);
void ScanMarkdownLine(const ScanInput& in, LexState& st, Emitter& out);
void ScanJsonLine(const ScanInput& in, LexState& st, Emitter& out);
void ScanJustLine(const ScanInput& in, LexState& st, Emitter& out);
void ScanCMakeLine(const ScanInput& in, LexState& st, Emitter& out);
void ScanTomlLine(const ScanInput& in, LexState& st, Emitter& out);
void ScanShLine(const ScanInput& in, LexState& st, Emitter& out);
void ScanPythonLine(const ScanInput& in, LexState& st, Emitter& out);

// Dispatch to the scanner for `lang`.
void ScanLine(Language lang, const ScanInput& in, LexState& st, Emitter& out);

// The style a language paints unstyled bytes (and line terminators) with.
int DefaultStyleFor(Language lang);

// The current PluginSyntax descriptor, or null. Captured at setLanguage
// time so the off-thread minimap renderer can call ScanPluginSyntaxLine
// directly without the global index lookup (which would race with other
// editors changing it).
const SyntaxDescriptor* CurrentPluginSyntaxDescriptor();

// Map a bracket nesting depth to its rainbow style, cycling through the
// palette. Shared by the Turmeric and JSON scanners.
constexpr TurStyle RainbowStyleForDepth(int depth) {
    const int level = depth % kRainbowLevels;
    return static_cast<TurStyle>(static_cast<int>(TurStyle::Rainbow0) + level);
}

}
