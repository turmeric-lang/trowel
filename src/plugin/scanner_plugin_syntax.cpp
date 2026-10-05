#include "scanner_plugin_syntax.h"

#include <cctype>

namespace trowel {

namespace {

// Check if `text` at position `i` matches `prefix`.
bool MatchPrefix(const char* text, Sci_Position i, Sci_Position end,
                 const QByteArray& prefix)
{
    if (prefix.isEmpty()) return false;
    const int n = prefix.size();
    if (i + n > end) return false;
    for (int k = 0; k < n; ++k)
        if (text[i + k] != prefix[k]) return false;
    return true;
}

// Check if a word character is alphanumeric or underscore.
bool IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Check if a character is a digit.
bool IsDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c));
}

}  // namespace

void ScanPluginSyntaxLine(const SyntaxDescriptor& desc,
                          const ScanInput& in, LexState& st, Emitter& out)
{
    const char* text = in.text;
    const Sci_Position end = in.end;
    Sci_Position i = in.begin;

    const auto style = [](PluginSyntaxStyleId s) {
        return static_cast<int>(s);
    };

    out.SetGapStyle(style(PluginSyntaxStyleId::Default));

    // Block comment state persists across lines via the leaf-language field.
    // We use `jsonDepth` as a "in block comment" flag (1 = in comment).
    const bool inBlockComment = st.jsonDepth > 0;

    const QByteArray lineComment = desc.lineComment.toUtf8();
    const QByteArray blockOpen = desc.blockCommentOpen.toUtf8();
    const QByteArray blockClose = desc.blockCommentClose.toUtf8();
    const QByteArray stringDelim = desc.stringDelim.isEmpty()
        ? QByteArray("\"") : desc.stringDelim.toUtf8();
    const QByteArray stringEscape = desc.stringEscape.isEmpty()
        ? QByteArray("\\") : desc.stringEscape.toUtf8();
    const QByteArray operators = desc.operators.toUtf8();

    if (inBlockComment) {
        // Continue scanning inside a block comment until we find the close.
        while (i < end) {
            if (!blockClose.isEmpty() && MatchPrefix(text, i, end, blockClose)) {
                out.Emit(i, blockClose.size(),
                         style(PluginSyntaxStyleId::Comment));
                i += blockClose.size();
                st.jsonDepth = 0;  // exit block comment
                break;
            }
            // Skip to the end of the line; the rest is comment.
            Sci_Position seg = i;
            while (i < end && !(!blockClose.isEmpty() &&
                                MatchPrefix(text, i, end, blockClose))) {
                ++i;
            }
            out.Emit(seg, i - seg, style(PluginSyntaxStyleId::Comment));
        }
        if (i >= end && st.jsonDepth > 0) {
            // Still in block comment at end of line.
            return;
        }
    }

    while (i < end) {
        const char c = text[i];

        // Whitespace.
        if (c == ' ' || c == '\t') { ++i; continue; }

        // Line comment.
        if (!lineComment.isEmpty() && MatchPrefix(text, i, end, lineComment)) {
            out.Emit(i, end - i, style(PluginSyntaxStyleId::Comment));
            i = end;
            continue;
        }

        // Block comment open.
        if (!blockOpen.isEmpty() && MatchPrefix(text, i, end, blockOpen)) {
            st.jsonDepth = 1;  // enter block comment
            out.Emit(i, blockOpen.size(), style(PluginSyntaxStyleId::Comment));
            i += blockOpen.size();
            // Scan to close or end of line.
            while (i < end) {
                if (!blockClose.isEmpty() &&
                    MatchPrefix(text, i, end, blockClose)) {
                    out.Emit(i, blockClose.size(),
                             style(PluginSyntaxStyleId::Comment));
                    i += blockClose.size();
                    st.jsonDepth = 0;  // exit block comment
                    break;
                }
                Sci_Position seg = i;
                while (i < end && !(!blockClose.isEmpty() &&
                                    MatchPrefix(text, i, end, blockClose))) {
                    ++i;
                }
                out.Emit(seg, i - seg, style(PluginSyntaxStyleId::Comment));
            }
            continue;
        }

        // String.
        if (MatchPrefix(text, i, end, stringDelim)) {
            Sci_Position seg = i;
            i += stringDelim.size();
            while (i < end) {
                if (!stringEscape.isEmpty() &&
                    MatchPrefix(text, i, end, stringEscape) &&
                    i + stringEscape.size() < end) {
                    i += stringEscape.size() + 1;  // skip escape + next char
                    continue;
                }
                if (MatchPrefix(text, i, end, stringDelim)) {
                    i += stringDelim.size();
                    break;
                }
                ++i;
            }
            out.Emit(seg, i - seg, style(PluginSyntaxStyleId::String));
            continue;
        }

        // Number.
        if (desc.hasNumbers && IsDigit(c)) {
            Sci_Position seg = i;
            // Handle hex/octal/binary prefixes.
            if (c == '0' && i + 1 < end &&
                (text[i + 1] == 'x' || text[i + 1] == 'X' ||
                 text[i + 1] == 'o' || text[i + 1] == 'O' ||
                 text[i + 1] == 'b' || text[i + 1] == 'B')) {
                i += 2;
                while (i < end && (IsDigit(text[i]) ||
                                   (text[i] >= 'a' && text[i] <= 'f') ||
                                   (text[i] >= 'A' && text[i] <= 'F'))) {
                    ++i;
                }
            } else {
                while (i < end && (IsDigit(text[i]) || text[i] == '.')) {
                    ++i;
                }
            }
            out.Emit(seg, i - seg, style(PluginSyntaxStyleId::Number));
            continue;
        }

        // Operators.
        if (!operators.isEmpty() && operators.indexOf(c) >= 0) {
            out.Emit(i, 1, style(PluginSyntaxStyleId::Operator));
            ++i;
            continue;
        }

        // Identifier / keyword.
        if (IsWordChar(c)) {
            Sci_Position seg = i;
            while (i < end && IsWordChar(text[i])) ++i;
            const QByteArray word = QByteArray(text + seg, i - seg);

            // Check against keyword lists.
            if (desc.keywords.contains(QString::fromUtf8(word),
                                        Qt::CaseSensitive)) {
                out.Emit(seg, i - seg, style(PluginSyntaxStyleId::Keyword));
            } else {
                out.Emit(seg, i - seg,
                         style(PluginSyntaxStyleId::Identifier));
            }
            continue;
        }

        // Anything else: default style, skip one char.
        ++i;
    }
}

}  // namespace trowel
