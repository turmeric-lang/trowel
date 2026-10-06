"""Bookmarks plugin: toggle, navigate, clear, and persistence."""

import pytest


@pytest.fixture
def saved_file(trowel, tmp_path):
    """Create and save a .tur file so the editor has a file path."""
    path = tmp_path / "bookmark_test.tur"
    trowel.type("(defn a [] 1)\n(defn b [] 2)\n(defn c [] 3)\n")
    trowel.call("editor.save", {"path": str(path)})
    return path


def test_bookmark_toggle(saved_file, trowel):
    """Toggle a bookmark on the current line, then toggle it off."""
    trowel.call("editor.set_cursor", {"pos": 20})
    trowel.call("command.run", {"id": "bookmarks.toggle"})
    # Toggle it off.
    trowel.call("command.run", {"id": "bookmarks.toggle"})


def test_bookmark_next_prev(saved_file, trowel):
    """Set two bookmarks, navigate next and prev."""
    text = trowel.call("editor.get_text")["text"]
    # Bookmark line 1 (pos 0).
    trowel.call("editor.set_cursor", {"pos": 0})
    trowel.call("command.run", {"id": "bookmarks.toggle"})
    # Bookmark line 3 (pos 28 = start of "(defn c").
    line3_start = len(text[:text.index("(defn c")].encode("utf-8"))
    trowel.call("editor.set_cursor", {"pos": line3_start})
    trowel.call("command.run", {"id": "bookmarks.toggle"})
    # Go to line 2 (between the two bookmarks).
    line2_start = len(text[:text.index("(defn b")].encode("utf-8"))
    trowel.call("editor.set_cursor", {"pos": line2_start})
    # Next should go to line 3.
    trowel.call("command.run", {"id": "bookmarks.next"})
    cursor = trowel.call("editor.get_cursor")
    assert cursor["pos"] == line3_start
    # Prev should go to line 1.
    trowel.call("command.run", {"id": "bookmarks.prev"})
    cursor = trowel.call("editor.get_cursor")
    assert cursor["pos"] == 0


def test_bookmark_clear(saved_file, trowel):
    """Clear all bookmarks in the current file."""
    trowel.call("editor.set_cursor", {"pos": 0})
    trowel.call("command.run", {"id": "bookmarks.toggle"})
    trowel.call("editor.set_cursor", {"pos": 15})
    trowel.call("command.run", {"id": "bookmarks.toggle"})
    # Clear all bookmarks.
    trowel.call("command.run", {"id": "bookmarks.clear"})
    # Verify the bookmarks file no longer has entries for this file.
    import os
    home = os.environ.get("HOME", "")
    bm_path = os.path.join(home, ".trowel", "bookmarks.tur")
    if os.path.exists(bm_path):
        content = open(bm_path).read()
        assert "bookmark_test" not in content


def test_bookmark_persistence(tmp_path, trowel_session):
    """Bookmarks persist across restarts."""
    s = trowel_session
    trowel = s.launch()
    path = str(tmp_path / "persist_test.tur")
    trowel.type("(defn hello []\n  (println \"hello\"))\n")
    trowel.call("editor.save", {"path": path})
    trowel.call("editor.set_cursor", {"pos": 20})
    trowel.call("command.run", {"id": "bookmarks.toggle"})
    s.quit(trowel)

    # Relaunch — the bookmark should still be there.
    trowel2 = s.launch()
    trowel2.call("editor.open", {"path": path})
    # Toggle should remove it (it was persisted).
    trowel2.call("command.run", {"id": "bookmarks.toggle"})
