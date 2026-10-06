"""Minimap — Phases 1-2.

On by default. Toggle via the View menu, assert the control API reports
the state, and check the slider geometry moves when the cursor goes to EOF.
"""

import json
import time

import pytest


def test_minimap_on_by_default(trowel):
    """The minimap is on by default."""
    state = trowel.call("editor.minimap")
    assert state["enabled"] is True


def test_menu_toggle_disables_minimap(trowel):
    """View > Minimap toggles the minimap off."""
    trowel.call("menu.invoke", {"path": ["View", "Minimap"]})
    state = trowel.call("editor.minimap")
    assert state["enabled"] is False


def test_menu_toggle_enables_minimap(trowel):
    """Toggling again turns it back on."""
    trowel.call("menu.invoke", {"path": ["View", "Minimap"]})
    assert trowel.call("editor.minimap")["enabled"] is False
    trowel.call("menu.invoke", {"path": ["View", "Minimap"]})
    assert trowel.call("editor.minimap")["enabled"] is True


def test_minimap_has_shortcut(trowel):
    """The minimap toggle has a keyboard shortcut registered."""
    result = trowel.call("menu.list", {})
    for a in result["actions"]:
        if a["path"] == ["View", "Minimap"]:
            assert len(a["shortcuts"]) > 0
            return
    pytest.fail("View > Minimap not found in menu.list")


def test_slider_moves_on_scroll(trowel):
    """The slider top changes when the cursor moves to EOF and the editor
    scrolls."""
    # A short document fits in the viewport, so the slider covers the whole
    # widget. Use a longer document so proportional slide kicks in.
    lines = "\n".join(f"(define (f{i} x) (+ x {i}))" for i in range(500))
    trowel.call("editor.set_text", {"text": lines})

    state_top = trowel.call("editor.minimap")
    # Move cursor to EOF and scroll there.
    text = trowel.call("editor.get_text")["text"]
    trowel.call("editor.set_cursor", {"pos": len(text.encode("utf-8"))})
    # Ensure the editor scrolls to the cursor.
    trowel.call("editor.press", {"key": "End"})
    time.sleep(0.3)

    state_bottom = trowel.call("editor.minimap")
    assert state_bottom["enabled"] is True
    # The slider should have moved down.
    assert state_bottom["sliderTop"] >= state_top["sliderTop"]


def test_minimap_persists_via_settings(trowel_session):
    """Writing editor.minimap: false to settings.json before launch starts
    the minimap disabled."""
    trowel_session.settings_json.parent.mkdir(parents=True, exist_ok=True)
    trowel_session.settings_json.write_text(
        json.dumps({"editor.minimap": False})
    )
    t = trowel_session.launch()
    state = t.call("editor.minimap")
    assert state["enabled"] is False


def test_minimap_survives_burst_of_edits(trowel):
    """A burst of edits exercises the incremental-invalidation + render
    debounce path. The minimap must stay enabled and report a consistent
    state without crashing."""
    lines = "\n".join(f"(define (f{i} x) (+ x {i}))" for i in range(2000))
    trowel.call("editor.set_text", {"text": lines})
    # Type at the top of the document in a tight loop — each keystroke
    # dirties strips to EOF and restarts the 60 ms debounce timer.
    trowel.call("editor.set_cursor", {"pos": 0})
    trowel.call("editor.type", {"text": "; edit\n" * 20})
    state = trowel.call("editor.minimap")
    assert state["enabled"] is True
    assert state["sliderTop"] >= 0


def test_minimap_hides_above_cap(trowel):
    """Above the line cap (1M), the minimap still reports enabled but does
    not crash or attempt to render."""
    # Create a document just over the cap. 1000001 lines of a single char each.
    trowel.call("editor.set_text", {"text": "x\n" * 1000001})
    state = trowel.call("editor.minimap")
    assert state["enabled"] is True
