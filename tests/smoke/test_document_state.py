"""Phase 6 — Per-document state.

Each test quits through File > Quit and relaunches, the same way
test_session_restore.py does, because document state is persisted in
closeEvent.
"""

import json
import time

import pytest


def _make_file(tmp_path, name, lines):
    """Create a file with the given number of non-empty lines."""
    f = tmp_path / name
    f.write_text("\n".join(f"line {i}" for i in range(lines)) + "\n")
    return f


def _pos(t, line, col=0):
    """Get byte position for a line:col via the control API."""
    return t.call("editor.pos_from_linecol", {"line": line, "col": col})["pos"]


def test_caret_and_selection_restored(trowel_session, tmp_path):
    """Caret and selection come back after quit and relaunch."""
    f = _make_file(tmp_path, "doc.tur", 60)

    t = trowel_session.launch([str(f)])
    # Put the caret on line 40 with a selection.
    start = _pos(t, 40, 0)
    end = _pos(t, 40, 5)
    t.call("editor.set_selection", {"start": start, "end": end})
    time.sleep(0.1)

    cursor_before = t.call("editor.get_cursor")
    assert cursor_before["line"] == 40

    trowel_session.quit(t)

    t2 = trowel_session.launch([str(f)])
    time.sleep(0.3)
    cursor_after = t2.call("editor.get_cursor")
    assert cursor_after["line"] == 40
    # Selection should be restored too.
    sel = cursor_after["selection"]
    assert sel[0] != sel[1]  # has a selection


def test_close_tab_and_reopen_restores_state(trowel_session, tmp_path):
    """Closing a tab and reopening it restores state without a relaunch."""
    f = _make_file(tmp_path, "doc.tur", 40)

    t = trowel_session.launch([str(f)])
    # Move caret to line 20, col 3.
    p = _pos(t, 20, 3)
    t.call("editor.set_cursor", {"pos": p})
    time.sleep(0.1)

    # Close the tab.
    t.call("menu.invoke", {"path": ["File", "Close Tab"]})
    time.sleep(0.3)

    # Reopen the file.
    t.call("editor.open", {"path": str(f)})
    time.sleep(0.3)

    cursor = t.call("editor.get_cursor")
    assert cursor["line"] == 20
    assert cursor["col"] == 3


def test_fold_all_restored_after_quit(trowel_session, tmp_path):
    """Fold All, quit, relaunch -> the same headers are folded."""
    f = tmp_path / "doc.tur"
    f.write_text(
        "(define (f x)\n  (let ((y 1))\n    (+ x y)))\n"
        "(define (g x)\n  (* x 2))\n"
        "(define h 42)\n"
    )

    t = trowel_session.launch([str(f)])
    t.call("menu.invoke", {"path": ["View", "Folding", "Fold All"]})
    time.sleep(0.2)
    folds_before = t.call("editor.folds")
    headers_before = [fd for fd in folds_before if fd["header"]]
    assert all(not fd["expanded"] for fd in headers_before)

    trowel_session.quit(t)

    t2 = trowel_session.launch([str(f)])
    time.sleep(0.3)
    folds_after = t2.call("editor.folds")
    headers_after = [fd for fd in folds_after if fd["header"]]
    # Same headers should be folded.
    assert len(headers_after) == len(headers_before)
    assert all(not fd["expanded"] for fd in headers_after)


def test_folds_not_restored_when_file_changed(trowel_session, tmp_path):
    """Fold a region, quit, modify the file, relaunch -> folds are not restored."""
    f = tmp_path / "doc.tur"
    f.write_text(
        "(define (f x)\n  (let ((y 1))\n    (+ x y)))\n"
        "(define (g x)\n  (* x 2))\n"
    )

    t = trowel_session.launch([str(f)])
    t.call("menu.invoke", {"path": ["View", "Folding", "Fold All"]})
    time.sleep(0.2)

    trowel_session.quit(t)

    # Modify the file after quit.
    f.write_text(
        "(define (f x)\n  (let ((y 1))\n    (+ x y)))\n"
        "(define (g x)\n  (* x 2))\n"
        "(define (z x)\n  (- x 1))\n"
    )

    t2 = trowel_session.launch([str(f)])
    time.sleep(0.3)
    folds = t2.call("editor.folds")
    headers = [fd for fd in folds if fd["header"]]
    # Folds should NOT be restored — all headers should be expanded.
    assert all(fd["expanded"] for fd in headers)


def test_wrap_override_restored(trowel_session, tmp_path):
    """Turn wrap on for a .tur buffer, quit, relaunch -> still wrapped."""
    f1 = tmp_path / "a.tur"
    f1.write_text("(define (main) (println \"hello\"))\n")
    f2 = tmp_path / "b.tur"
    f2.write_text("(define (other) (println \"world\"))\n")

    t = trowel_session.launch([str(f1)])
    # Turn wrap on for f1.
    t.call("menu.invoke", {"path": ["View", "Word Wrap"]})
    time.sleep(0.1)
    state = t.call("editor.state")
    assert state["wrap"] is True

    trowel_session.quit(t)

    t2 = trowel_session.launch([str(f1)])
    time.sleep(0.3)
    state = t2.call("editor.state")
    assert state["wrap"] is True
    assert state["wrapOverride"] == "on"

    # A different .tur file should not be wrapped.
    t2.call("editor.open", {"path": str(f2)})
    time.sleep(0.1)
    state2 = t2.call("editor.state")
    assert state2["wrapOverride"] == "default"


def test_remember_document_state_false(trowel_session, tmp_path):
    """editor.rememberDocumentState: false -> nothing is restored."""
    f = _make_file(tmp_path, "doc.tur", 40)

    # Write settings with rememberDocumentState: false.
    trowel_session.settings_json.write_text(
        json.dumps({"editor.rememberDocumentState": False})
    )

    t = trowel_session.launch([str(f)])
    p = _pos(t, 30, 0)
    t.call("editor.set_cursor", {"pos": p})
    time.sleep(0.1)

    trowel_session.quit(t)

    # doc-state.json should not contain our file.
    ds = trowel_session.doc_state_json
    if ds.exists():
        data = json.loads(ds.read_text())
        for key in data:
            assert "doc.tur" not in key

    t2 = trowel_session.launch([str(f)])
    time.sleep(0.3)
    cursor = t2.call("editor.get_cursor")
    # Caret should be at line 0, not line 30.
    assert cursor["line"] == 0


def test_clear_menu_clears_document_state(trowel_session, tmp_path):
    """File > Open Recent > Clear Menu clears document-state.json."""
    f = _make_file(tmp_path, "doc.tur", 40)

    t = trowel_session.launch([str(f)])
    p = _pos(t, 30, 0)
    t.call("editor.set_cursor", {"pos": p})
    time.sleep(0.1)
    trowel_session.quit(t)

    # doc-state.json should have our file.
    ds = trowel_session.doc_state_json
    assert ds.exists()
    data = json.loads(ds.read_text())
    assert len(data) > 0

    # Relaunch and clear menu.
    t2 = trowel_session.launch([str(f)])
    t2.call("menu.invoke", {"path": ["File", "Open Recent", "Clear Menu"]})
    time.sleep(0.5)

    # doc-state.json should be empty now.
    data2 = json.loads(ds.read_text())
    assert len(data2) == 0

    trowel_session.quit(t2)
