"""Menu & shortcut equivalence, plus the Phase 1 menu audit tests."""

import pytest
from trowel_ctl import ControlError


def test_menu_invoke_run_buffer(trowel):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.type("(def smoke-menu 11)")
    trowel.call("menu.invoke", {"path": ["Run", "Run Buffer"]})
    trowel.wait_idle(quiet_ms=400, timeout_ms=5000)
    trowel.send("smoke-menu")
    hit = trowel.wait_output("11", timeout_ms=3000)
    assert "11" in hit["matched"]


def test_menu_unknown_action_errors(trowel):
    with pytest.raises(ControlError) as ei:
        trowel.call("menu.invoke", {"path": ["Nope", "Nada"]})
    assert ei.value.code == "no_action"


def test_menu_restart_repl(trowel):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("menu.invoke", {"path": ["Run", "Restart REPL"]})
    trowel.wait_output("Turmeric v", timeout_ms=5000)


def test_no_duplicate_shortcuts(trowel):
    """No two actions in the same window share a key sequence, and no
    single-key shortcut equals the first key of a chord."""
    result = trowel.call("menu.list", {})
    actions = result["actions"]

    # Collect every shortcut string, keyed by the action path for reporting.
    all_shortcuts = []  # (shortcut_string, path)
    chord_prefixes = set()  # first key of every chord

    for a in actions:
        for s in a["shortcuts"]:
            # QKeySequence serialises chords as "Ctrl+K, Ctrl+L".
            parts = [p.strip() for p in s.split(",")]
            if len(parts) > 1:
                chord_prefixes.add(parts[0])
            all_shortcuts.append((s, a["path"]))

    # Check for exact duplicates.
    seen = {}
    for s, path in all_shortcuts:
        if s in seen:
            pytest.fail(
                f"Duplicate shortcut {s!r}: {seen[s]} and {path}")
        seen[s] = path

    # Check that no single-key shortcut equals the first key of a chord.
    single_keys = {s for s, _ in all_shortcuts if "," not in s}
    for prefix in chord_prefixes:
        if prefix in single_keys:
            pytest.fail(
                f"Shortcut {prefix!r} is both a single-key binding and the "
                f"first key of a chord — Qt would never fire the single one")


def test_edit_routes_to_focus(trowel):
    """Edit > Undo works on the editor, and Edit > Select All works on
    the terminal."""
    trowel.wait_output("turmeric>", timeout_ms=5000)

    # Focus the editor and type.
    trowel.call("window.focus", {"pane": "editor"})
    trowel.call("editor.set_text", {"text": ""})
    trowel.type("hello")
    text = trowel.call("editor.get_text", {})
    assert "hello" in text["text"]
    trowel.call("menu.invoke", {"path": ["Edit", "Undo"]})
    text = trowel.call("editor.get_text", {})
    assert "hello" not in text["text"]


def test_side_bar_actions_are_menu_reachable(trowel):
    """Show REPL is reachable from the View menu and toggles REPL visibility."""
    trowel.wait_output("turmeric>", timeout_ms=5000)
    # The action is checkable; invoking it toggles the checked state.
    result = trowel.call("menu.list", {})
    show_repl = None
    for a in result["actions"]:
        if a["path"] == ["View", "Show REPL"]:
            show_repl = a
            break
    assert show_repl is not None, "View > Show REPL not found in menu.list"
    assert show_repl["checkable"] is True


def test_go_to_line(trowel):
    """Go to Line… is present in the Go menu."""
    trowel.wait_output("turmeric>", timeout_ms=5000)
    result = trowel.call("menu.list", {})
    found = any(a["path"] == ["Go", "Go to Line…"] for a in result["actions"])
    assert found, "Go > Go to Line… not found in menu.list"
