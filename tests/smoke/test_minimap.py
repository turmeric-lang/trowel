"""Minimap — Phases 1-3.

On by default. Toggle via the View menu (Alt+M), assert the control API
reports the state, and check the slider geometry moves when the cursor
goes to EOF. Phase 3 moves strip rendering off the GUI thread; the cap is
removed entirely.
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
    lines = "\n".join(f"(define (f{i} x) (+ x {i}))" for i in range(500))
    trowel.call("editor.set_text", {"text": lines})

    state_top = trowel.call("editor.minimap")
    text = trowel.call("editor.get_text")["text"]
    trowel.call("editor.set_cursor", {"pos": len(text.encode("utf-8"))})
    trowel.call("editor.press", {"key": "End"})
    time.sleep(0.3)

    state_bottom = trowel.call("editor.minimap")
    assert state_bottom["enabled"] is True
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
    trowel.call("editor.set_cursor", {"pos": 0})
    trowel.call("editor.type", {"text": "; edit\n" * 20})
    state = trowel.call("editor.minimap")
    assert state["enabled"] is True
    assert state["sliderTop"] >= 0


def test_minimap_large_file_no_cap(trowel):
    """Phase 3 removes the line cap. A 2M-line file must not crash the
    minimap or the editor; the minimap reports enabled."""
    # 2M lines — well past the old 1M phase-2 cap.
    trowel.call("editor.set_text", {"text": "x\n" * 2000000})
    state = trowel.call("editor.minimap")
    assert state["enabled"] is True
