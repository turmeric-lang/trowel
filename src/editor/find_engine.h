#pragma once

#include <QString>
#include <QVector>

class ScintillaEdit;

namespace trowel {

// A find query. The engine is stateless — it searches the document as it
// stands when called.
struct FindQuery {
    QString text;
    bool matchCase = false;
    bool wholeWord = false;
    bool regex = false;
    bool inSelection = false;
    // When inSelection is true, only matches within [rangeStart, rangeEnd)
    // are considered. Set by the caller before calling search.
    int rangeStart = 0;
    int rangeEnd = 0;
};

// One match in the document.
struct FindMatch {
    int start = 0;
    int end = 0;
};

// Result of a search.
struct FindResult {
    QVector<FindMatch> matches;
    int currentIndex = -1;   // index into matches, -1 if none
    QString error;           // non-empty if the regex is invalid
    int count = 0;           // capped at 10000; shown as "10000+" in the UI
};

// Search the whole document (or the selection range) for `query`, starting
// from `startPos`. Returns all matches (capped at 10000) and the index of the
// first match at or after `startPos`.
FindResult FindAll(ScintillaEdit* sci, const FindQuery& query, int startPos);

// Find the next match at or after `startPos`. Returns {-1,-1} if none.
FindMatch FindNext(ScintillaEdit* sci, const FindQuery& query, int startPos);

// Find the previous match before `startPos`. Returns {-1,-1} if none.
FindMatch FindPrevious(ScintillaEdit* sci, const FindQuery& query, int startPos);

// Replace the current target range with `replacement`. Uses replaceTargetRE
// when regex is on so \1..\9 refer to capture groups.
void ReplaceTarget(ScintillaEdit* sci, const QString& replacement, bool regex);

}  // namespace trowel
