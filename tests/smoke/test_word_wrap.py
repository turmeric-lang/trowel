"""Phase 4 — Word wrap."""

import json
import time


def _settings_path(tmp_path):
    return tmp_path / "home" / "config" / "settings.json"


def test_md_wraps_by_default(trowel, tmp_path):
    """Opening a .md file -> wrap is on."""
    f = tmp_path / "doc.md"
    f.write_text("# Title\n\nSome prose here.\n")
    trowel.call("editor.open", {"path": str(f)})
    state = trowel.call("editor.state")
    assert state["wrap"] is True


def test_tur_does_not_wrap_by_default(trowel, tmp_path):
    """Opening a .tur file -> wrap is off."""
    f = tmp_path / "code.tur"
    f.write_text("(define x 42)\n")
    trowel.call("editor.open", {"path": str(f)})
    state = trowel.call("editor.state")
    assert state["wrap"] is False


def test_toggle_wrap_on_tur(trowel, tmp_path):
    """View > Word Wrap on a .tur buffer turns it on; other tabs do not change."""
    a = tmp_path / "a.tur"
    a.write_text("(define a 1)\n")
    b = tmp_path / "b.tur"
    b.write_text("(define b 2)\n")
    trowel.call("editor.open", {"path": str(a)})
    trowel.call("editor.open", {"path": str(b)})

    # Toggle wrap on tab b (the active tab)
    trowel.call("menu.invoke", {"path": ["View", "Word Wrap"]})
    assert trowel.call("editor.state")["wrap"] is True

    # Switch to tab a via Previous Tab — wrap should still be off
    trowel.call("menu.invoke", {"path": ["View", "Previous Tab"]})
    assert trowel.call("editor.state")["wrap"] is False


def test_wrap_setting_prose_false(trowel, tmp_path):
    """editor.wrap.prose=false -> .md buffers without override stop wrapping."""
    sp = _settings_path(tmp_path)
    sp.parent.mkdir(parents=True, exist_ok=True)
    sp.write_text(json.dumps({"editor.wrap.prose": False}))

    # Wait for the 200ms debounce + reload
    time.sleep(1.0)

    f = tmp_path / "doc.md"
    f.write_text("# Title\n\nSome prose.\n")
    trowel.call("editor.open", {"path": str(f)})
    state = trowel.call("editor.state")
    assert state["wrap"] is False
    assert state["wrapOverride"] == "default"


def test_wrap_override_stays_after_setting_change(trowel, tmp_path):
    """An explicit on override stays on even if the setting changes."""
    f = tmp_path / "code.tur"
    f.write_text("(define x 42)\n")
    trowel.call("editor.open", {"path": str(f)})

    # Override to on
    trowel.call("menu.invoke", {"path": ["View", "Word Wrap"]})
    assert trowel.call("editor.state")["wrap"] is True
    assert trowel.call("editor.state")["wrapOverride"] == "on"

    # Change the code wrap setting — override should stay
    sp = _settings_path(tmp_path)
    sp.parent.mkdir(parents=True, exist_ok=True)
    sp.write_text(json.dumps({"editor.wrap.code": True}))
    time.sleep(1.0)
    assert trowel.call("editor.state")["wrap"] is True
    assert trowel.call("editor.state")["wrapOverride"] == "on"


def test_txt_is_plain_text(trowel, tmp_path):
    """A .txt file is PlainText, not Turmeric."""
    f = tmp_path / "notes.txt"
    f.write_text("(define x 42)\n")
    trowel.call("editor.open", {"path": str(f)})
    state = trowel.call("editor.state")
    # PlainText wraps by default (prose default is true)
    assert state["wrap"] is True
