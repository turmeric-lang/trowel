#include "editor/scanner.h"

#include <cctype>

namespace trowel {

namespace {

// Scan a JSON string, painting escapes separately. Returns the position just
// past the closing quote (or end of line for an unterminated string).
Sci_Position ScanString(const char* text, Sci_Position i, Sci_Position end,
                        Emitter& out, JsonStyle body) {
    Sci_Position seg = i;
    ++i;
    while (i < end && text[i] != '"') {
        if (text[i] == '\\' && i + 1 < end) {
            out.Emit(seg, i - seg, static_cast<int>(body));
            out.Emit(i, 2, static_cast<int>(JsonStyle::StringEscape));
            i += 2;
            seg = i;
            continue;
        }
        ++i;
    }
    if (i < end && text[i] == '"') ++i;
    out.Emit(seg, i - seg, static_cast<int>(body));
    return i;
}

// A string is an object key when the next non-blank character is a colon.
// Looking ahead within the line covers every realistic layout; a key split
// from its colon by a newline falls back to being styled as a plain string.
bool LooksLikeKey(const char* text, Sci_Position i, Sci_Position end) {
    while (i < end && (text[i] == ' ' || text[i] == '\t')) ++i;
    return i < end && text[i] == ':';
}

bool MatchWord(const char* text, Sci_Position i, Sci_Position end, const char* word) {
    for (Sci_Position k = 0; word[k] != '\0'; ++k) {
        if (i + k >= end || text[i + k] != word[k]) return false;
    }
    return true;
}

}

void ScanJsonLine(const ScanInput& in, LexState& st, Emitter& out) {
    const char* text = in.text;
    const Sci_Position end = in.end;
    Sci_Position i = in.begin;

    out.SetGapStyle(static_cast<int>(JsonStyle::Default));
    auto emit = [&](Sci_Position from, Sci_Position len, JsonStyle s) {
        out.Emit(from, len, static_cast<int>(s));
    };

    while (i < end) {
        const char c = text[i];

        if (c == ' ' || c == '\t') { ++i; continue; }

        if (c == '"') {
            // Peek at where the string ends so we can tell a key from a value
            // before painting it.
            Sci_Position probe = i + 1;
            while (probe < end && text[probe] != '"') {
                if (text[probe] == '\\' && probe + 1 < end) probe += 2;
                else ++probe;
            }
            if (probe < end) ++probe;
            const bool key = LooksLikeKey(text, probe, end);
            i = ScanString(text, i, end, out, key ? JsonStyle::Key : JsonStyle::String);
            continue;
        }

        // Containers. With rainbow on they share the Turmeric bracket palette,
        // so nesting reads the same way across both languages.
        if (c == '{' || c == '[') {
            out.Emit(i, 1, in.rainbow
                               ? static_cast<int>(RainbowStyleForDepth(st.jsonDepth))
                               : static_cast<int>(JsonStyle::Operator));
            if (st.jsonDepth < kMaxJsonDepth) ++st.jsonDepth;
            ++i;
            continue;
        }
        if (c == '}' || c == ']') {
            int style = static_cast<int>(JsonStyle::Operator);
            if (st.jsonDepth > 0) {
                --st.jsonDepth;
                if (st.jsonDepth < st.minBracketDepth)
                    st.minBracketDepth = st.jsonDepth;
                if (in.rainbow) style = static_cast<int>(RainbowStyleForDepth(st.jsonDepth));
            } else if (in.rainbow) {
                style = static_cast<int>(TurStyle::BracketError);
            }
            out.Emit(i, 1, style);
            ++i;
            continue;
        }

        if (c == ':' || c == ',') {
            emit(i, 1, JsonStyle::Operator);
            ++i;
            continue;
        }

        if (MatchWord(text, i, end, "true"))  { emit(i, 4, JsonStyle::Literal); i += 4; continue; }
        if (MatchWord(text, i, end, "false")) { emit(i, 5, JsonStyle::Literal); i += 5; continue; }
        if (MatchWord(text, i, end, "null"))  { emit(i, 4, JsonStyle::Literal); i += 4; continue; }

        if (std::isdigit(static_cast<unsigned char>(c))
            || (c == '-' && i + 1 < end
                && std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
            const Sci_Position start = i;
            if (c == '-') ++i;
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
            emit(start, i - start, JsonStyle::Number);
            continue;
        }

        // Anything else is not valid JSON — flag it rather than hiding it.
        emit(i, 1, JsonStyle::Error);
        ++i;
    }
}

}
