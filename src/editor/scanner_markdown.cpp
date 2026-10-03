#include "editor/scanner.h"

#include <cctype>
#include <string>
#include <string_view>

namespace trowel {

namespace {

// A fenced-code delimiter line, per CommonMark: up to three leading spaces,
// then a run of at least three backticks or tildes, then an optional info
// string.
struct Fence {
    bool valid = false;
    bool tilde = false;
    Sci_Position runStart = 0;
    Sci_Position runLen = 0;
    Sci_Position infoStart = 0;
    Sci_Position infoEnd = 0;
};

Fence DetectFence(const char* text, Sci_Position begin, Sci_Position end) {
    Fence f;
    Sci_Position j = begin;
    int indent = 0;
    while (j < end && text[j] == ' ' && indent < 4) { ++j; ++indent; }
    if (indent >= 4 || j >= end) return f;

    const char ch = text[j];
    if (ch != '`' && ch != '~') return f;

    f.runStart = j;
    while (j < end && text[j] == ch) ++j;
    f.runLen = j - f.runStart;
    if (f.runLen < 3) return f;

    while (j < end && (text[j] == ' ' || text[j] == '\t')) ++j;
    f.infoStart = j;
    f.infoEnd = end;
    while (f.infoEnd > f.infoStart
           && (text[f.infoEnd - 1] == ' ' || text[f.infoEnd - 1] == '\t')) {
        --f.infoEnd;
    }

    // A backtick fence's info string may not itself contain a backtick —
    // that is what stops "```" inside a paragraph from opening a block.
    if (ch == '`') {
        for (Sci_Position k = f.infoStart; k < f.infoEnd; ++k) {
            if (text[k] == '`') return f;
        }
    }

    f.tilde = (ch == '~');
    f.valid = true;
    return f;
}

// Map a fence info string to the guest scanner that should highlight the
// block's body. Returns false when the tag is absent or unrecognized, in which
// case the body is painted as undifferentiated code.
bool GuestForInfo(const char* text, Sci_Position begin, Sci_Position end,
                  Language& guest) {
    // Only the first word matters: ```c title="x" is still C.
    Sci_Position wordEnd = begin;
    while (wordEnd < end && text[wordEnd] != ' ' && text[wordEnd] != '\t') ++wordEnd;
    if (wordEnd <= begin) return false;

    std::string tag;
    tag.reserve(static_cast<size_t>(wordEnd - begin));
    for (Sci_Position k = begin; k < wordEnd; ++k) {
        tag.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(text[k]))));
    }

    // `sweet-exp` first, and it is the one that matters: it is the tag
    // Turmeric's own guides use for every sweet-expression example -- 996 of
    // them against zero for the three spellings this list used to hold -- so
    // until it was added here, every sweet example in the shipped documentation
    // rendered as undifferentiated code in Trowel's Markdown view.
    if (tag == "sweet-exp" || tag == "sweet" || tag == "turmeric-sweet"
        || tag == "tur-sweet" || tag == "saffron-sweet") {
        guest = Language::TurmericSweet;
        return true;
    }
    // `scheme` used to land on the Turmeric scanner, which is the wrong lexeme
    // set; the guides use it 16 times. `r7rs/sweet` has no conventional tag, so
    // it is spelled out both ways people would reach for.
    if (tag == "scheme" || tag == "r7rs" || tag == "scm") {
        guest = Language::R7rs;
        return true;
    }
    if (tag == "r7rs-sweet" || tag == "scheme-sweet") {
        guest = Language::R7rsSweet;
        return true;
    }
    // Saffron is Turmeric to a scanner; see editor/dialect.h.
    if (tag == "turmeric" || tag == "tur" || tag == "lisp" || tag == "saffron") {
        guest = Language::Turmeric;
        return true;
    }
    if (tag == "c" || tag == "h" || tag == "cpp" || tag == "c++" || tag == "cc") {
        guest = Language::C;
        return true;
    }
    if (tag == "json") {
        guest = Language::Json;
        return true;
    }
    if (tag == "just" || tag == "justfile" || tag == "make" || tag == "makefile") {
        guest = Language::Just;
        return true;
    }
    if (tag == "cmake") {
        guest = Language::CMake;
        return true;
    }
    if (tag == "toml") {
        guest = Language::Toml;
        return true;
    }
    if (tag == "sh" || tag == "bash" || tag == "shell" || tag == "zsh"
        || tag == "console" || tag == "shell-session") {
        guest = Language::Sh;
        return true;
    }
    if (tag == "python" || tag == "py" || tag == "python3") {
        guest = Language::Python;
        return true;
    }
    return false;
}

bool IsBlank(const char* text, Sci_Position begin, Sci_Position end) {
    for (Sci_Position j = begin; j < end; ++j) {
        if (text[j] != ' ' && text[j] != '\t') return false;
    }
    return true;
}

// ` {0,3}` then three or more of -, *, or _ separated only by spaces.
bool IsThematicBreak(const char* text, Sci_Position begin, Sci_Position end) {
    Sci_Position j = begin;
    int indent = 0;
    while (j < end && text[j] == ' ' && indent < 4) { ++j; ++indent; }
    if (indent >= 4 || j >= end) return false;
    const char ch = text[j];
    if (ch != '-' && ch != '*' && ch != '_') return false;
    int count = 0;
    for (; j < end; ++j) {
        if (text[j] == ch) ++count;
        else if (text[j] != ' ' && text[j] != '\t') return false;
    }
    return count >= 3;
}

// A setext underline: a run of '=' (or '-') alone on the line. '-' runs are
// claimed by IsThematicBreak first, which is checked before this.
bool IsSetextUnderline(const char* text, Sci_Position begin, Sci_Position end) {
    Sci_Position j = begin;
    int indent = 0;
    while (j < end && text[j] == ' ' && indent < 4) { ++j; ++indent; }
    if (indent >= 4 || j >= end || text[j] != '=') return false;
    while (j < end && text[j] == '=') ++j;
    while (j < end && (text[j] == ' ' || text[j] == '\t')) ++j;
    return j == end;
}

// Length of a list marker at `begin` ("- ", "* ", "1. ", "12) "), or 0.
Sci_Position ListMarkerLen(const char* text, Sci_Position begin, Sci_Position end) {
    Sci_Position j = begin;
    if (j < end && (text[j] == '-' || text[j] == '*' || text[j] == '+')) {
        ++j;
    } else {
        Sci_Position digits = j;
        while (digits < end && std::isdigit(static_cast<unsigned char>(text[digits]))) ++digits;
        if (digits == j || digits >= end) return 0;
        if (text[digits] != '.' && text[digits] != ')') return 0;
        j = digits + 1;
    }
    if (j >= end) return j - begin;               // marker alone on the line
    if (text[j] != ' ' && text[j] != '\t') return 0;
    return j - begin;
}

void ScanInline(const char* text, Sci_Position begin, Sci_Position end, Emitter& out);

// Find a closing run of exactly `len` backticks starting at or after `from`.
Sci_Position FindCodeSpanEnd(const char* text, Sci_Position from, Sci_Position end,
                             Sci_Position len) {
    for (Sci_Position j = from; j < end; ++j) {
        if (text[j] != '`') continue;
        Sci_Position runEnd = j;
        while (runEnd < end && text[runEnd] == '`') ++runEnd;
        if (runEnd - j == len) return j;
        j = runEnd - 1;
    }
    return -1;
}

// Find the closing delimiter for emphasis/strong: the next occurrence of
// `len` copies of `ch` on this line.
Sci_Position FindDelim(const char* text, Sci_Position from, Sci_Position end,
                       char ch, Sci_Position len) {
    for (Sci_Position j = from; j + len <= end; ++j) {
        bool match = true;
        for (Sci_Position k = 0; k < len; ++k) {
            if (text[j + k] != ch) { match = false; break; }
        }
        if (match) return j;
    }
    return -1;
}

void ScanInline(const char* text, Sci_Position begin, Sci_Position end, Emitter& out) {
    Sci_Position i = begin;
    auto emit = [&](Sci_Position from, Sci_Position len, MdStyle s) {
        out.Emit(from, len, static_cast<int>(s));
    };

    while (i < end) {
        const char c = text[i];

        // Backslash escape.
        if (c == '\\' && i + 1 < end) {
            emit(i, 2, MdStyle::Escape);
            i += 2;
            continue;
        }

        // Code span.
        if (c == '`') {
            Sci_Position runEnd = i;
            while (runEnd < end && text[runEnd] == '`') ++runEnd;
            const Sci_Position len = runEnd - i;
            const Sci_Position close = FindCodeSpanEnd(text, runEnd, end, len);
            if (close >= 0) {
                emit(i, close + len - i, MdStyle::CodeSpan);
                i = close + len;
                continue;
            }
            i = runEnd;
            continue;
        }

        // Strong then emphasis, so ** wins over *.
        if (c == '*' || c == '_') {
            const bool isDouble = (i + 1 < end && text[i + 1] == c);
            const Sci_Position len = isDouble ? 2 : 1;
            const Sci_Position close = FindDelim(text, i + len, end, c, len);
            if (close >= 0 && close > i + len) {
                emit(i, close + len - i, isDouble ? MdStyle::Strong : MdStyle::Emphasis);
                i = close + len;
                continue;
            }
            i += len;
            continue;
        }

        // Links and images: [text](url) / ![alt](url) / [text][ref].
        if (c == '[' || (c == '!' && i + 1 < end && text[i + 1] == '[')) {
            const Sci_Position start = i;
            Sci_Position j = (c == '!') ? i + 2 : i + 1;
            int depth = 1;
            while (j < end && depth > 0) {
                if (text[j] == '\\' && j + 1 < end) { j += 2; continue; }
                if (text[j] == '[') ++depth;
                else if (text[j] == ']') --depth;
                ++j;
            }
            if (depth == 0) {
                emit(start, j - start, MdStyle::LinkText);
                if (j < end && (text[j] == '(' || text[j] == '[')) {
                    const char close = (text[j] == '(') ? ')' : ']';
                    Sci_Position k = j + 1;
                    while (k < end && text[k] != close) ++k;
                    if (k < end) ++k;
                    emit(j, k - j, MdStyle::LinkUrl);
                    j = k;
                }
                i = j;
                continue;
            }
            i = start + 1;
            continue;
        }

        // Autolink or inline HTML.
        if (c == '<') {
            Sci_Position j = i + 1;
            while (j < end && text[j] != '>' && text[j] != ' ') ++j;
            if (j < end && text[j] == '>') {
                const std::string_view inner(text + i + 1, static_cast<size_t>(j - i - 1));
                const bool isUrl = inner.find("://") != std::string_view::npos
                                   || inner.find('@') != std::string_view::npos;
                emit(i, j + 1 - i, isUrl ? MdStyle::LinkUrl : MdStyle::Html);
                i = j + 1;
                continue;
            }
        }

        ++i;  // plain text — backfilled with the gap style
    }
}

}

void ScanMarkdownLine(const ScanInput& in, LexState& st, Emitter& out) {
    const char* text = in.text;
    const Sci_Position begin = in.begin;
    const Sci_Position end = in.end;

    out.SetGapStyle(static_cast<int>(MdStyle::Default));
    auto emit = [&](Sci_Position from, Sci_Position len, MdStyle s) {
        out.Emit(from, len, static_cast<int>(s));
    };

    // Hand the line to the guest scanner and restore our own gap style, so the
    // line terminator the adapter paints belongs to the guest's code block.
    auto delegate = [&]() {
        if (st.mdGuestPlain) {
            out.SetGapStyle(static_cast<int>(MdStyle::CodeBlock));
            out.FillTo(end);
            return;
        }
        const ScanInput sub{text, begin, end, in.rainbow, in.line};
        ScanLine(st.mdGuest, sub, st, out);
        out.FillTo(end);
    };

    // --- Inside a fenced code block ----------------------------------------
    if (st.mdInFence) {
        // Guest-aware close suppression. While the Turmeric guest is inside
        // its own inline ``` C block, a bare ``` line belongs to the guest as
        // *its* closer — not to us. Without this, an equal-length inner fence
        // (``` inside a ```turmeric block) would tear the outer block open.
        // Both Turmeric guests delegate their inline ``` blocks to C, so both
        // can own a bare fence line.
        const bool guestOwnsFence =
            !st.mdGuestPlain
            && (st.mdGuest == Language::Turmeric || st.mdGuest == Language::TurmericSweet)
            && st.turInCBlock;

        if (!guestOwnsFence) {
            const Fence f = DetectFence(text, begin, end);
            const bool closes = f.valid
                                && f.tilde == st.mdFenceTilde
                                && f.runLen >= 3 + st.mdFenceExtra
                                && f.infoEnd == f.infoStart;
            if (closes) {
                emit(begin, end - begin, MdStyle::Fence);
                st.mdInFence = false;
                ResetGuestState(st);
                return;
            }
        }
        delegate();
        return;
    }

    // --- Opening fence ------------------------------------------------------
    {
        const Fence f = DetectFence(text, begin, end);
        if (f.valid) {
            emit(begin, end - begin, MdStyle::Fence);
            st.mdInFence = true;
            st.mdFenceTilde = f.tilde;
            st.mdFenceExtra = static_cast<int>(f.runLen) - 3;
            if (st.mdFenceExtra > kMaxMdFenceExtra) st.mdFenceExtra = kMaxMdFenceExtra;
            Language guest = Language::Turmeric;
            st.mdGuestPlain = !GuestForInfo(text, f.infoStart, f.infoEnd, guest);
            st.mdGuest = guest;
            // Start the guest from a clean slate so no state leaks in from an
            // earlier block in a different language.
            ResetGuestState(st);
            return;
        }
    }

    if (IsBlank(text, begin, end)) return;

    // --- Block-level constructs --------------------------------------------

    Sci_Position i = begin;
    int indent = 0;
    while (i < end && (text[i] == ' ' || text[i] == '\t') && indent < 4) { ++i; ++indent; }

    // Blockquote markers, possibly stacked ("> > text").
    while (i < end && text[i] == '>') {
        emit(i, 1, MdStyle::Blockquote);
        ++i;
        while (i < end && (text[i] == ' ' || text[i] == '\t')) ++i;
    }
    if (i >= end) return;

    // ATX heading.
    if (text[i] == '#') {
        Sci_Position j = i;
        while (j < end && text[j] == '#') ++j;
        if (j - i <= 6 && (j >= end || text[j] == ' ' || text[j] == '\t')) {
            emit(i, end - i, MdStyle::Heading);
            return;
        }
    }

    if (IsThematicBreak(text, i, end)) {
        emit(i, end - i, MdStyle::Rule);
        return;
    }
    if (IsSetextUnderline(text, i, end)) {
        emit(i, end - i, MdStyle::Heading);
        return;
    }

    // List marker, then inline content after it.
    if (const Sci_Position marker = ListMarkerLen(text, i, end); marker > 0) {
        emit(i, marker, MdStyle::ListMarker);
        i += marker;
    }

    // Raw HTML block.
    if (i < end && text[i] == '<' && i + 1 < end
        && (std::isalpha(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '/'
            || text[i + 1] == '!')) {
        emit(i, end - i, MdStyle::Html);
        return;
    }

    ScanInline(text, i, end, out);
}

}
