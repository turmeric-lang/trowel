"""Phase 2 — settings.json as the config file."""

import json
import platform
import time

import pytest


def _settings_menu_path(trowel):
    """Find the Settings… menu path, which differs by platform."""
    if platform.system() == "Darwin":
        return ["Settings\u2026"]
    return ["Edit", "Settings\u2026"]


def _rainbow_styles(trowel):
    """Re-lex by resetting the text, then return the four bracket styles."""
    trowel.call("editor.set_text", {"text": "(a (b) c)"})
    return [
        trowel.call("editor.get_style_at", {"pos": p})["style"]
        for p in [0, 3, 5, 8]
    ]


def _wait_rainbow_off(trowel, timeout=3.0):
    """Poll until rainbow is off (all styles < 40), re-lexing each poll.

    The settings reload is debounced 200ms and does not auto-re-lex existing
    text, so each poll resets the text to force a re-lex with current settings.
    Requires the off state to hold across two checks separated by more than the
    debounce, so a not-yet-reloaded state cannot pass by accident — and a
    broken-JSON reload that resets to defaults cannot pass either, because the
    second check would see rainbow back on.
    """
    deadline = time.time() + timeout
    confirmed = False
    styles = _rainbow_styles(trowel)
    while time.time() < deadline:
        styles = _rainbow_styles(trowel)
        if all(s < 40 for s in styles):
            if confirmed:
                return styles
            confirmed = True
            time.sleep(0.25)
            continue
        confirmed = False
        time.sleep(0.05)
    return styles


def test_settings_opens_tab(trowel, trowel_session):
    """Settings… opens settings.json in a tab."""
    path = _settings_menu_path(trowel)
    trowel.call("menu.invoke", {"path": path})

    # The tab is open and showing settings.json.
    tabs = trowel.call("window.list")["windows"][0]["tabs"]
    assert any("settings.json" in t for t in tabs)


def test_rainbow_brackets_off_from_settings(trowel_session):
    """Writing editor.rainbowBrackets: false to settings.json before launch."""
    trowel_session.settings_json.write_text(
        json.dumps({"editor.rainbowBrackets": False})
    )
    t = trowel_session.launch()
    t.call("editor.set_text", {"text": "(a (b) c)"})
    styles = [
        t.call("editor.get_style_at", {"pos": p})["style"]
        for p in [0, 3, 5, 8]
    ]
    # Rainbow styles are 40-46. With rainbow off, none should be in that range.
    assert all(s < 40 for s in styles), f"rainbow styles still active: {styles}"


def test_rainbow_brackets_default_on(trowel):
    """With no rainbow setting in settings.json, rainbow brackets are on."""
    trowel.call("editor.set_text", {"text": "(a (b) c)"})
    open_outer = trowel.call("editor.get_style_at", {"pos": 0})["style"]
    assert open_outer >= 40, f"rainbow not active: {open_outer}"


def test_live_reload_rainbow_off(trowel, trowel_session):
    """Writing editor.rainbowBrackets: false to settings.json from outside
    Trowel applies it without a relaunch."""
    # Start with rainbow on (default, settings.json has only turmeric.path).
    trowel.call("editor.set_text", {"text": "(a (b) c)"})
    assert trowel.call("editor.get_style_at", {"pos": 0})["style"] >= 40

    # Overwrite settings.json with rainbow off (keep turmeric.path too).
    existing = json.loads(trowel_session.settings_json.read_text())
    existing["editor.rainbowBrackets"] = False
    trowel_session.settings_json.write_text(json.dumps(existing))

    # Poll until the 200ms-debounced reload applies (re-lexing each poll).
    styles = _wait_rainbow_off(trowel)
    assert all(s < 40 for s in styles), f"rainbow still on after live reload: {styles}"


def test_broken_json_keeps_last_values(trowel, trowel_session):
    """Broken JSON → previous values stay in effect."""
    # Write valid settings first (with rainbow off).
    existing = json.loads(trowel_session.settings_json.read_text())
    existing["editor.rainbowBrackets"] = False
    trowel_session.settings_json.write_text(json.dumps(existing))

    # Poll until the reload applies.
    styles_before = _wait_rainbow_off(trowel)
    assert all(s < 40 for s in styles_before)

    # Overwrite with broken JSON.
    trowel_session.settings_json.write_text("{ this is not json")

    # Poll: the debounced reload must be attempted (and keep last values).
    # _wait_rainbow_off requires the off state to hold across two checks
    # straddling the 200ms debounce, so a reset-to-defaults would be caught.
    styles_after = _wait_rainbow_off(trowel)
    assert all(s < 40 for s in styles_after), f"values changed on broken json: {styles_after}"


def test_unknown_key_ignored(trowel_session):
    """An unknown key is ignored, and valid keys still apply."""
    trowel_session.settings_json.write_text(
        json.dumps({"editor.rainbowBracket": True, "editor.rainbowBrackets": False})
    )
    t = trowel_session.launch()
    t.call("editor.set_text", {"text": "(a (b) c)"})
    styles = [
        t.call("editor.get_style_at", {"pos": p})["style"]
        for p in [0, 3, 5, 8]
    ]
    # The typo'd key is ignored; the correct key still applies.
    assert all(s < 40 for s in styles), f"unknown key broke valid key: {styles}"


def test_qsettings_values_not_read(trowel_session):
    """QSettings editor/rainbowBrackets=false is NOT read anymore.
    The app reads from settings.json, not QSettings."""
    # Pre-write settings.json so the one-shot migrateFromQSettings() — which
    # only runs when settings.json is absent — short-circuits and never reads
    # QSettings. This tests the steady-state invariant the name promises:
    # the read path ignores QSettings.
    trowel_session.settings_json.write_text("{}")

    # Write the QSettings INI with the old key.
    ini = trowel_session.settings_ini
    ini.parent.mkdir(parents=True, exist_ok=True)
    ini.write_text("[editor]\nrainbowBrackets=false\n")

    t = trowel_session.launch()
    # The app should have rainbow on (default), because it reads from
    # settings.json (which has only turmeric.path), not QSettings.
    t.call("editor.set_text", {"text": "(a (b) c)"})
    styles = [
        t.call("editor.get_style_at", {"pos": p})["style"]
        for p in [0, 3, 5, 8]
    ]
    assert any(s >= 40 for s in styles), f"QSettings value leaked: {styles}"
