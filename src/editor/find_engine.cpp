#include "editor/find_engine.h"

#include <ScintillaEdit.h>

#include <Scintilla.h>

namespace trowel {

namespace {

int SearchFlags(const FindQuery& q) {
    int flags = 0;
    if (q.matchCase) flags |= SCFIND_MATCHCASE;
    if (q.wholeWord) flags |= SCFIND_WHOLEWORD;
    if (q.regex)    flags |= SCFIND_REGEXP | SCFIND_CXX11REGEX;
    return flags;
}

// One-shot search: find the first match at or after `start` within the
// document (or the selection range). Returns {-1,-1} if none.
FindMatch SearchOnce(ScintillaEdit* sci, const FindQuery& q, int start) {
    const int docEnd = static_cast<int>(sci->textLength());
    int rangeStart, rangeEnd;
    if (q.inSelection) {
        rangeStart = q.rangeStart;
        rangeEnd = q.rangeEnd;
    } else {
        rangeStart = 0;
        rangeEnd = docEnd;
    }
    if (start < rangeStart) start = rangeStart;
    if (start >= rangeEnd) return {-1, -1};

    sci->setSearchFlags(SearchFlags(q));
    sci->setTargetRange(start, rangeEnd);
    const QByteArray textBytes = q.text.toUtf8();
    const int pos = static_cast<int>(sci->searchInTarget(textBytes.length(), textBytes.constData()));
    if (pos < 0) return {-1, -1};
    const int end = static_cast<int>(sci->targetEnd());
    return {pos, end};
}

}  // namespace

FindResult FindAll(ScintillaEdit* sci, const FindQuery& query, int startPos) {
    FindResult result;

    if (query.text.isEmpty()) return result;

    const int docEnd = static_cast<int>(sci->textLength());
    int rangeStart, rangeEnd;
    if (query.inSelection) {
        rangeStart = query.rangeStart;
        rangeEnd = query.rangeEnd;
    } else {
        rangeStart = 0;
        rangeEnd = docEnd;
    }

    sci->setSearchFlags(SearchFlags(query));

    // For regex, validate the pattern by doing one search.
    if (query.regex) {
        sci->setTargetRange(rangeStart, rangeEnd);
        const QByteArray qb = query.text.toUtf8();
        const int test = static_cast<int>(sci->searchInTarget(qb.length(), qb.constData()));
        // searchInTarget returns -1 for no match, but also for an invalid
        // pattern. Scintilla doesn't expose the error directly, so we rely
        // on the caller checking whether matches are empty with a non-empty
        // pattern. For now, treat -1 as "no match or error".
        (void)test;
    }

    int pos = rangeStart;
    bool foundCurrent = false;
    constexpr int kMaxMatches = 10000;

    while (pos < rangeEnd && static_cast<int>(result.matches.size()) < kMaxMatches) {
        sci->setTargetRange(pos, rangeEnd);
        const QByteArray qb = query.text.toUtf8();
        const int matchPos = static_cast<int>(sci->searchInTarget(qb.length(), qb.constData()));
        if (matchPos < 0) break;

        const int matchEnd = static_cast<int>(sci->targetEnd());
        if (matchEnd <= matchPos) break;  // zero-length match, avoid loop

        FindMatch m{matchPos, matchEnd};
        result.matches.append(m);

        if (!foundCurrent && matchPos >= startPos) {
            result.currentIndex = static_cast<int>(result.matches.size()) - 1;
            foundCurrent = true;
        }

        pos = matchEnd;
    }

    // If no match was at or after startPos, wrap to the first match.
    if (!foundCurrent && !result.matches.isEmpty()) {
        result.currentIndex = 0;
    }

    result.count = static_cast<int>(result.matches.size());
    return result;
}

FindMatch FindNext(ScintillaEdit* sci, const FindQuery& q, int startPos) {
    return SearchOnce(sci, q, startPos);
}

FindMatch FindPrevious(ScintillaEdit* sci, const FindQuery& q, int startPos) {
    // Search backward: find all matches before startPos and take the last one.
    const int docEnd = static_cast<int>(sci->textLength());
    int rangeStart, rangeEnd;
    if (q.inSelection) {
        rangeStart = q.rangeStart;
        rangeEnd = q.rangeEnd;
    } else {
        rangeStart = 0;
        rangeEnd = docEnd;
    }

    if (startPos <= rangeStart) return {-1, -1};
    if (startPos > rangeEnd) startPos = rangeEnd;

    sci->setSearchFlags(SearchFlags(q));

    FindMatch last{-1, -1};
    int pos = rangeStart;
    while (pos < startPos) {
        sci->setTargetRange(pos, startPos);
        const QByteArray qb = q.text.toUtf8();
        const int matchPos = static_cast<int>(sci->searchInTarget(qb.length(), qb.constData()));
        if (matchPos < 0) break;
        const int matchEnd = static_cast<int>(sci->targetEnd());
        if (matchEnd <= matchPos) break;
        last = {matchPos, matchEnd};
        pos = matchEnd;
    }

    return last;
}

void ReplaceTarget(ScintillaEdit* sci, const QString& replacement, bool regex) {
    const QByteArray bytes = replacement.toUtf8();
    if (regex)
        sci->replaceTargetRE(bytes.length(), bytes.constData());
    else
        sci->replaceTarget(bytes.length(), bytes.constData());
}

}  // namespace trowel
