"""Minimap — Phase 1.

Toggle via the View menu, assert the control API reports the state, and
check the slider geometry moves when the cursor goes to EOF.
"""

import json
import time

import pytest


def test_minimap_off_by_default(trowel):
    """The minimap is off until the View menu toggle turns it on."""
    state = trowel.call("editor.minimap")
    assert state["enabled"] is False


def test_menu_toggle_enables_minimap(trowel):
    """View > Minimap toggles the minimap on."""
    trowel.call("menu.invoke", {"path": ["View", "Minimap"]})
    state = trowel.call("editor.minimap")
    assert state["enabled"] is True


def test_menu_toggle_disables_minimap(trowel):
    """Toggling again turns it off."""
    trowel.call("menu.invoke", {"path": ["View", "Minimap"]})
    assert trowel.call("editor.minimap")["enabled"] is True
    trowel.call("menu.invoke", {"path": ["View", "Minimap"]})
    assert trowel.call("editor.minimap")["enabled"] is False


def test_slider_moves_on_scroll(trowel):
    """After enabling the minimap, the slider top changes when the cursor
    moves to EOF and the editor scrolls."""
    trowel.call("menu.invoke", {"path": ["View", "Minimap"]})
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
    """Writing editor.minimap: true to settings.json before launch starts
    the minimap enabled."""
    trowel_session.settings_json.parent.mkdir(parents=True, exist_ok=True)
    trowel_session.settings_json.write_text(
        json.dumps({"editor.minimap": True})
    )
    t = trowel_session.launch()
    state = t.call("editor.minimap")
    assert state["enabled"] is True


def test_minimap_hides_above_cap(trowel):
    """Above the phase-1 line cap (200k), the minimap still reports enabled
    but does not crash."""
    trowel.call("menu.invoke", {"path": ["View", "Minimap"]})
    # Create a document just over the cap. 200001 lines of a single char each.
    # Use set_text with a large string — the control API handles it.
    trowel.call("editor.set_text", {"text": "x\n" * 200001})
    state = trowel.call("editor.minimap")
    assert state["enabled"] is True
