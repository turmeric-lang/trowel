"""Phase 5 — Folding."""

import pytest


def test_turmeric_fold_headers(trowel):
    """A .tur file with three top-level defines and a nested let."""
    trowel.call("editor.set_text", {
        "text": "(define (f x)\n  (let ((y 1))\n    (+ x y)))\n"
                "(define (g x)\n  (* x 2))\n"
                "(define h 42)\n"
    })
    folds = trowel.call("editor.folds")
    headers = [f for f in folds if f["header"]]
    # Three top-level forms open folds (lines 0, 3, 5).
    # The nested let on line 1 also opens a fold.
    assert len(headers) >= 3
    # The first header is at line 0 (the first define).
    assert headers[0]["line"] == 0


def test_fold_all(trowel):
    """Fold All -> every header reports expanded: false."""
    trowel.call("editor.set_text", {
        "text": "(define (f x)\n  (let ((y 1))\n    (+ x y)))\n"
                "(define (g x)\n  (* x 2))\n"
    })
    trowel.call("menu.invoke", {"path": ["View", "Folding", "Fold All"]})
    folds = trowel.call("editor.folds")
    headers = [f for f in folds if f["header"]]
    assert all(not f["expanded"] for f in headers)


def test_unfold_all(trowel):
    """Unfold All -> every header is expanded."""
    trowel.call("editor.set_text", {
        "text": "(define (f x)\n  (let ((y 1))\n    (+ x y)))\n"
                "(define (g x)\n  (* x 2))\n"
    })
    # First fold all
    trowel.call("menu.invoke", {"path": ["View", "Folding", "Fold All"]})
    # Then unfold all
    trowel.call("menu.invoke", {"path": ["View", "Folding", "Unfold All"]})
    folds = trowel.call("editor.folds")
    headers = [f for f in folds if f["header"]]
    assert all(f["expanded"] for f in headers)


def test_fold_top_level(trowel):
    """Fold Top-Level Forms -> base-level headers folded, nested keeps state."""
    trowel.call("editor.set_text", {
        "text": "(define (f x)\n  (let ((y 1))\n    (+ x y)))\n"
                "(define (g x)\n  (* x 2))\n"
    })
    trowel.call("menu.invoke", {"path": ["View", "Folding", "Fold Top-Level Forms"]})
    folds = trowel.call("editor.folds")
    headers = [f for f in folds if f["header"]]
    # Top-level headers (level == SC_FOLDLEVELBASE) should be folded.
    base_level = 0x400  # SC_FOLDLEVELBASE
    top_level = [f for f in headers if f["level"] == base_level]
    assert all(not f["expanded"] for f in top_level)


def test_toggle_fold(trowel):
    """Toggle Fold at the caret folds and unfolds."""
    trowel.call("editor.set_text", {
        "text": "(define (f x)\n  (+ x 1))\n"
    })
    # Caret is at position 0, on the header line.
    trowel.call("menu.invoke", {"path": ["View", "Folding", "Toggle Fold"]})
    folds = trowel.call("editor.folds")
    headers = [f for f in folds if f["header"]]
    assert any(not f["expanded"] for f in headers)

    # Toggle again to unfold.
    trowel.call("menu.invoke", {"path": ["View", "Folding", "Toggle Fold"]})
    folds = trowel.call("editor.folds")
    headers = [f for f in folds if f["header"]]
    assert all(f["expanded"] for f in headers)


def test_rainbow_off_same_levels(trowel, tmp_path):
    """Fold levels are identical with rainbow on and off."""
    text = "(define (f x)\n  (let ((y 1))\n    (+ x y)))\n"

    # Rainbow on (default)
    trowel.call("editor.set_text", {"text": text})
    folds_on = trowel.call("editor.folds")

    # Rainbow off via settings
    import json, time
    sp = tmp_path / "home" / "config" / "settings.json"
    sp.parent.mkdir(parents=True, exist_ok=True)
    sp.write_text(json.dumps({"editor.rainbowBrackets": False}))
    time.sleep(1.0)
    trowel.call("editor.set_text", {"text": text})
    folds_off = trowel.call("editor.folds")

    # Same number of headers at the same lines and levels.
    assert len(folds_on) == len(folds_off)
    for a, b in zip(folds_on, folds_off):
        assert a["line"] == b["line"]
        assert a["level"] == b["level"]


def test_markdown_heading_folds(trowel, tmp_path):
    """Markdown: ## under # nests; # inside code fence is not a header."""
    f = tmp_path / "doc.md"
    f.write_text("# Title\n\n## Section\n\nSome text.\n\n"
                 "```python\n# this is a comment, not a heading\nx = 1\n```\n")
    trowel.call("editor.open", {"path": str(f)})
    folds = trowel.call("editor.folds")
    headers = [f for f in folds if f["header"]]
    # # Title is a header, ## Section is a header, the code fence opening is a header.
    # The # inside the fence is NOT a header.
    header_lines = [f["line"] for f in headers]
    assert 0 in header_lines  # # Title
    assert 2 in header_lines  # ## Section
    # The # comment inside the fence (line 7) should not be a header.
    assert 7 not in header_lines


def test_python_indentation_folds(trowel, tmp_path):
    """Python indentation folding with blank lines inside a block."""
    f = tmp_path / "code.py"
    f.write_text("def foo():\n    x = 1\n\n    y = 2\n\nbar = 3\n")
    trowel.call("editor.open", {"path": str(f)})
    folds = trowel.call("editor.folds")
    headers = [f for f in folds if f["header"]]
    # The line "    x = 1" at indent 4 is a header (more indented than def foo()).
    assert len(headers) >= 1
    # The header should be the first indented line.
    assert headers[0]["line"] == 1
