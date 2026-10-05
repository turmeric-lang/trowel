#include "snippet_session.h"

#include <QRegularExpression>
#include <QRegularExpressionMatch>

namespace trowel {

void SnippetSession::parse(const QByteArray& template_, int insertPos)
{
    stops_.clear();
    text_.clear();
    current_ = 0;

    // Parse TextMate-style tab-stops: ${1:default}, ${0:final}, $1, $0.
    // We use a regex to find all tab-stop markers, then build the expanded
    // text by replacing each marker with its default text.
    //
    // ${n:default} — tab-stop n with default text
    // ${n}         — tab-stop n with no default (zero-width)
    // $n           — tab-stop n with no default (zero-width)
    //
    // The final cursor position is ${0} or $0.
    static const QRegularExpression re(
        QStringLiteral("\\$(\\{([0-9]+)(?::([^}]*))?\\}|([0-9]+))"));

    const QString src = QString::fromUtf8(template_);
    int lastEnd = 0;
    auto it = re.globalMatch(src);

    while (it.hasNext()) {
        const auto m = it.next();
        // Append the text before this marker.
        text_.append(template_.mid(lastEnd, m.capturedStart() - lastEnd));

        // Extract the tab-stop number and default text.
        int index;
        QString defaultText;
        if (!m.captured(2).isEmpty()) {
            // ${n:default} or ${n}
            index = m.captured(2).toInt();
            defaultText = m.captured(3);
        } else {
            // $n
            index = m.captured(4).toInt();
        }

        const int stopStart = insertPos + text_.size();
        text_.append(defaultText.toUtf8());
        const int stopEnd = insertPos + text_.size();

        stops_.append({stopStart, stopEnd, index});
        lastEnd = m.capturedEnd();
    }

    // Append any remaining text after the last marker.
    text_.append(template_.mid(lastEnd));

    // Sort stops by index (ascending), with 0 (final) last.
    std::sort(stops_.begin(), stops_.end(),
              [](const SnippetStop& a, const SnippetStop& b) {
                  // Non-zero stops sorted ascending; 0 goes last.
                  if (a.index == 0) return false;
                  if (b.index == 0) return true;
                  return a.index < b.index;
              });
}

SnippetStop SnippetSession::advance()
{
    if (current_ + 1 >= stops_.size()) {
        // Past the last stop — snippet is done.
        // If there's a ${0} stop, return it; otherwise return empty.
        if (!stops_.isEmpty() && stops_.last().index == 0) {
            SnippetStop final = stops_.last();
            clear();
            return final;
        }
        clear();
        return {-1, -1, -1};
    }
    current_++;
    return stops_[current_];
}

SnippetStop SnippetSession::firstStop() const
{
    if (stops_.isEmpty()) return {-1, -1, -1};
    return stops_.first();
}

}  // namespace trowel
