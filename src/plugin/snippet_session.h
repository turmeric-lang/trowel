#pragma once

#include <QByteArray>
#include <QVector>

namespace trowel {

// One tab-stop in a snippet expansion.  `start` and `end` are absolute
// positions in the buffer.  When start == end, the stop is a zero-width
// cursor position; when start < end, the stop's default text is selected.
struct SnippetStop {
    int start = 0;
    int end = 0;
    int index = 0;  // tab-stop number (0 = final cursor position)
};

// State for an active snippet expansion on an EditorView.  The C++ side
// owns the tab-stop parsing and the event-filter-level Tab interception;
// the Turmeric snippet plugin owns the expansion logic (which snippet to
// insert for a given word).
//
// Lifecycle: insertSnippet() parses the template, inserts the text, and
// selects the first tab-stop.  advance() moves to the next stop.  When
// the last stop is reached, the session is cleared (snippet mode exits).
class SnippetSession {
public:
    SnippetSession() = default;

    // True when a snippet is active (tab-stops remain).
    bool active() const { return !stops_.isEmpty() && current_ < stops_.size(); }

    // Parse a template with TextMate-style tab-stops (${1:default}, ${0:final})
    // and return the expanded text with tab-stops replaced by their defaults.
    // Fills `stops_` with the absolute positions of each tab-stop in the
    // expanded text (relative to `insertPos`).
    // `insertPos` is where the text will be inserted in the buffer.
    void parse(const QByteArray& template_, int insertPos);

    // The expanded text (tab-stop markers stripped, defaults filled in).
    const QByteArray& text() const { return text_; }

    // The tab-stops, sorted by index (0 last).
    const QVector<SnippetStop>& stops() const { return stops_; }

    // The current tab-stop index (0-based into stops_).
    int current() const { return current_; }

    // Advance to the next tab-stop.  Returns the stop to select, or an
    // empty stop (start == end == -1) if the snippet is done.
    SnippetStop advance();

    // The first stop (what to select after insertion).
    SnippetStop firstStop() const;

    // Clear the session (snippet mode exits).
    void clear() { stops_.clear(); text_.clear(); current_ = 0; }

private:
    QByteArray text_;
    QVector<SnippetStop> stops_;
    int current_ = 0;
};

}  // namespace trowel
