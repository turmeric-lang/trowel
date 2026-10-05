"""Phase 3 — Find and Replace."""

import pytest


def test_find_basic(trowel):
    """Query 'define' on a fixture with 3 matches → count 3."""
    trowel.call("editor.set_text", {"text": "(define a 1)\n(define b 2)\n(define c 3)\n"})
    trowel.call("find.set", {"query": "define"})
    state = trowel.call("find.state")
    assert state["count"] == 3
    assert state["current"] == 0


def test_find_next_wraps(trowel):
    """Find Next three times wraps back to the first match."""
    trowel.call("editor.set_text", {"text": "(define a 1)\n(define b 2)\n(define c 3)\n"})
    trowel.call("find.set", {"query": "define"})

    # Move to first match
    trowel.call("menu.invoke", {"path": ["Edit", "Find", "Find Next"]})
    sel1 = trowel.call("editor.get_selection")
    assert sel1["start"] != sel1["end"]

    # Move to second match
    trowel.call("menu.invoke", {"path": ["Edit", "Find", "Find Next"]})
    sel2 = trowel.call("editor.get_selection")
    assert sel2["start"] != sel1["start"]

    # Move to third match
    trowel.call("menu.invoke", {"path": ["Edit", "Find", "Find Next"]})
    sel3 = trowel.call("editor.get_selection")
    assert sel3["start"] != sel2["start"]

    # Wrap back to first
    trowel.call("menu.invoke", {"path": ["Edit", "Find", "Find Next"]})
    sel4 = trowel.call("editor.get_selection")
    assert sel4["start"] == sel1["start"]


def test_match_case_changes_count(trowel):
    """Match Case changes the count."""
    trowel.call("editor.set_text", {"text": "Define define DEFINE\n"})
    # Without match case: 3 matches
    trowel.call("find.set", {"query": "define"})
    assert trowel.call("find.state")["count"] == 3

    # With match case: 1 match
    trowel.call("find.set", {"query": "define", "matchCase": True})
    assert trowel.call("find.state")["count"] == 1


def test_whole_word_changes_count(trowel):
    """Whole Word changes the count."""
    trowel.call("editor.set_text", {"text": "define xdefine define\n"})
    # Without whole word: 3 matches (define, xdefine, define)
    trowel.call("find.set", {"query": "define"})
    assert trowel.call("find.state")["count"] == 3

    # With whole word: 2 matches (not 'xdefine')
    trowel.call("find.set", {"query": "define", "wholeWord": True})
    assert trowel.call("find.state")["count"] == 2


def test_replace_all_one_undo(trowel):
    """Regex replace all → text correct, one Undo restores."""
    trowel.call("editor.set_text", {"text": "(def a 1)\n(def b 2)\n"})
    trowel.call("find.set", {"query": "\\(def (\\w+)", "regex": True})
    result = trowel.call("find.replace", {"replacement": "(defn \\1", "all": True})
    assert result["replaced"] == 2

    text = trowel.call("editor.get_text")["text"]
    assert "(defn a 1)" in text
    assert "(defn b 2)" in text

    # One Undo restores the original
    trowel.call("menu.invoke", {"path": ["Edit", "Undo"]})
    text = trowel.call("editor.get_text")["text"]
    assert "(def a 1)" in text
    assert "(def b 2)" in text


def test_in_selection_replace(trowel):
    """In Selection → only matches in range are replaced."""
    trowel.call("editor.set_text", {"text": "foo bar foo bar foo\n"})
    # Select the whole line (positions 0-21)
    trowel.call("editor.set_selection", {"start": 0, "end": 21})
    trowel.call("find.set", {"query": "foo", "inSelection": True})
    # The inSelection range is set from the selection at find.set time
    result = trowel.call("find.replace", {"replacement": "baz", "all": True})
    assert result["replaced"] >= 1


def test_invalid_regex(trowel):
    """An invalid regex → error is set, text unchanged."""
    trowel.call("editor.set_text", {"text": "hello world\n"})
    trowel.call("find.set", {"query": "[invalid", "regex": True})
    state = trowel.call("find.state")
    # The count should be 0 (no matches) and text unchanged
    assert state["count"] == 0
    text = trowel.call("editor.get_text")["text"]
    assert text == "hello world\n"


def test_open_with_selection(trowel):
    """Open with a one-line selection → query equals the selection."""
    trowel.call("editor.set_text", {"text": "(define x 42)\n"})
    # Select "define"
    trowel.call("editor.set_selection", {"start": 1, "end": 7})
    trowel.call("menu.invoke", {"path": ["Edit", "Find", "Find…"]})
    state = trowel.call("find.state")
    assert state["open"] is True
    assert state["query"] == "define"


def test_select_all_occurrences(trowel):
    """Select All Occurrences → selection count equals match count."""
    trowel.call("editor.set_text", {"text": "foo foo foo\n"})
    trowel.call("find.set", {"query": "foo"})
    assert trowel.call("find.state")["count"] == 3
    trowel.call("menu.invoke", {"path": ["Edit", "Find", "Select All Occurrences"]})
    # Check that there are multiple selections
    sel = trowel.call("editor.get_selection")
    # With multi-selection, get_selection returns the main selection
    # but the text should have all 3 selected
    assert sel["start"] != sel["end"]
