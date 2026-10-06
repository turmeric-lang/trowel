"""P6 — keymap customization via keymap.tur.

The bundled keymap (loaded from Qt resources) sets default shortcuts.
The user keymap(~/.trowel/keymap.tur) overrides them.  The override
must actually change the QAction's shortcut, not just the registry entry.
"""

import pytest

KEYMAP = r''';; ~/.trowel/keymap.tur — override keyboard shortcuts.
;;
;; Each line sets a keyboard shortcut for a registered command by id.
;; Command ids are slugified from the menu text: "trowel.runbuffer",
;; "trowel.save", etc.

(trowel-set-keybinding "trowel.runbuffer" "Ctrl+Shift+R")
'''

INVALID_KEYMAP = '(trowel-set-keybinding "nonexistent.command" "Ctrl+X")'


def _write_keymap(tmp_path, content):
    trowel_dir = tmp_path / "home" / ".trowel"
    trowel_dir.mkdir(parents=True, exist_ok=True)
    (trowel_dir / "keymap.tur").write_text(content)


@pytest.fixture
def keymap(tmp_path):
    """Create a keymap.tur before Trowel launches."""
    _write_keymap(tmp_path, KEYMAP)


@pytest.fixture
def invalid_keymap(tmp_path):
    """Create a keymap.tur with an invalid command id before Trowel launches."""
    _write_keymap(tmp_path, INVALID_KEYMAP)


def _find_action(menus, *name_fragments):
    """Find a menu action whose path contains all the given fragments."""
    for action in menus.get("actions", []):
        path = " ".join(action.get("path", []))
        if all(frag.lower() in path.lower() for frag in name_fragments):
            return action
    return None


def test_keymap_overrides_shortcut(keymap, trowel):
    """The user keymap overrides the QAction shortcut for Run Buffer."""
    menus = trowel.call("menu.list")
    action = _find_action(menus, "Run", "Buffer")
    assert action is not None, "Run Buffer action not found in menu.list"
    shortcuts = action.get("shortcuts", [])
    # The default is Ctrl+R; the keymap overrides it to Ctrl+Shift+R.
    assert "Ctrl+Shift+R" in shortcuts, (
        f"Expected Ctrl+Shift+R in shortcuts, got {shortcuts}")
    assert "Ctrl+R" not in shortcuts, (
        f"Ctrl+R should have been replaced, got {shortcuts}")


def test_bundled_keymap_loads(trowel):
    """The bundled keymap sets default shortcuts even without a user keymap."""
    menus = trowel.call("menu.list")
    action = _find_action(menus, "Run", "Buffer")
    assert action is not None, "Run Buffer action not found in menu.list"
    shortcuts = action.get("shortcuts", [])
    # The bundled keymap should set Ctrl+R as the default for Run Buffer.
    assert "Ctrl+R" in shortcuts, (
        f"Expected Ctrl+R from bundled keymap, got {shortcuts}")


def test_keymap_invalid_command(invalid_keymap, trowel):
    """An invalid command id in keymap.tur is handled gracefully."""
    # The app should still be running (the error is logged, not fatal).
    text = trowel.call("editor.get_text")
    assert text["text"] == ""
