"""Per-plugin isolation: one sandboxed env per plugin, manifest capabilities."""

import hashlib
import json

import pytest

from conftest import ControlError


def _plugin(tmp_path, name, source, manifest=None):
    d = tmp_path / "home" / ".trowel" / "plugins" / name
    d.mkdir(parents=True, exist_ok=True)
    (d / "plugin.tur").write_text(source)
    if manifest is not None:
        (d / "manifest.tur").write_text(manifest)
    return d


def _cmd(plugin_id, body):
    return f'''(trowel-register-command "{plugin_id}.cmd" "{plugin_id}" "Test" "" (fn [] {body}))\n'''


def _run(trowel, cid):
    trowel.call("command.run", {"id": cid})
    return trowel.call("editor.get_text")["text"]


def test_same_definition_name_does_not_clash(tmp_path, trowel_session):
    _plugin(tmp_path, "pa", '(defn who [] "A")\n' + _cmd("pa", '(trowel-set-text (who))'))
    _plugin(tmp_path, "pb", '(defn who [] "B")\n' + _cmd("pb", '(trowel-set-text (who))'))
    trowel = trowel_session.launch()
    assert _run(trowel, "pa.cmd") == "A"
    assert _run(trowel, "pb.cmd") == "B"


def test_plugin_cannot_see_another_plugins_definitions(tmp_path, trowel_session):
    _plugin(tmp_path, "pa", '(defn only-in-a [] "A")\n' + _cmd("pa", '(trowel-set-text (only-in-a))'))
    _plugin(tmp_path, "pb", _cmd("pb", '(trowel-set-text (only-in-a))'))
    trowel = trowel_session.launch()
    assert _run(trowel, "pa.cmd") == "A"
    # pb's command must not have resolved pa's function.
    trowel.call("editor.set_text", {"text": "untouched"})
    try:
        trowel.call("command.run", {"id": "pb.cmd"})
    except ControlError:
        pass
    assert trowel.call("editor.get_text")["text"] == "untouched"


def test_reload_command_reloads_one_plugin(tmp_path, trowel_session):
    pa_dir = _plugin(tmp_path, "pa", '(defn who [] "A")\n' + _cmd("pa", '(trowel-set-text (who))'))
    _plugin(tmp_path, "pb", '(def counter 7)\n' + _cmd("pb", '(trowel-set-text (int->str counter))')
            + '(trowel-register-command "pb.reload-a" "r" "Test" "" (fn [] (trowel-reload-plugin "pa")))\n')
    trowel = trowel_session.launch()
    assert _run(trowel, "pa.cmd") == "A"
    (pa_dir / "plugin.tur").write_text('(defn who [] "A2")\n' + _cmd("pa", '(trowel-set-text (who))'))
    trowel.call("command.run", {"id": "pb.reload-a"})
    # The reload is deferred to the event loop; poll briefly.
    import time
    deadline = time.time() + 5
    text = ""
    while time.time() < deadline:
        text = _run(trowel, "pa.cmd")
        if text == "A2":
            break
        time.sleep(0.1)
    assert text == "A2"
    assert _run(trowel, "pb.cmd") == "7"


FS_PROBE = '''(trowel-register-command "fsp.cmd" "fsp" "Test" ""
  (fn []
    (do
      (trowel-set-text "start")
      (io-fopen-read "/etc/hosts")
      (trowel-set-text "reached"))))
'''


def test_no_manifest_means_no_stdlib_io(tmp_path, trowel_session):
    _plugin(tmp_path, "fsp", FS_PROBE)
    trowel = trowel_session.launch()
    try:
        trowel.call("command.run", {"id": "fsp.cmd"})
    except ControlError:
        pass
    assert trowel.call("editor.get_text")["text"] == "start"


MANIFEST = '(defplugin "fsp"\n  :capabilities [fs:read]\n  :description "probe")\n'


def test_manifest_capability_granted_via_override(tmp_path, monkeypatch, trowel_session):
    monkeypatch.setenv("TROWEL_PLUGIN_PERMISSIONS", "allow")
    _plugin(tmp_path, "fsp", FS_PROBE, MANIFEST)
    trowel = trowel_session.launch()
    assert _run(trowel, "fsp.cmd") == "reached"


def test_manifest_capability_denied_via_override(tmp_path, monkeypatch, trowel_session):
    monkeypatch.setenv("TROWEL_PLUGIN_PERMISSIONS", "deny")
    _plugin(tmp_path, "fsp", FS_PROBE, MANIFEST)
    trowel = trowel_session.launch()
    try:
        trowel.call("command.run", {"id": "fsp.cmd"})
    except ControlError:
        pass
    assert trowel.call("editor.get_text")["text"] == "start"


def _write_permissions(tmp_path, name, manifest, granted):
    digest = hashlib.sha256(manifest.encode()).hexdigest()
    p = tmp_path / "home" / ".trowel" / "plugin-permissions.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({name: {"hash": digest, "granted": granted}}))


def test_cached_grant_skips_prompt(tmp_path, trowel_session):
    _plugin(tmp_path, "fsp", FS_PROBE, MANIFEST)
    _write_permissions(tmp_path, "fsp", MANIFEST, True)
    trowel = trowel_session.launch()
    assert _run(trowel, "fsp.cmd") == "reached"


def test_cached_denial_skips_prompt(tmp_path, trowel_session):
    _plugin(tmp_path, "fsp", FS_PROBE, MANIFEST)
    _write_permissions(tmp_path, "fsp", MANIFEST, False)
    trowel = trowel_session.launch()
    try:
        trowel.call("command.run", {"id": "fsp.cmd"})
    except ControlError:
        pass
    assert trowel.call("editor.get_text")["text"] == "start"


HELPER = '(defmodule helper\n  (export answer)\n  (def answer 42))\n'
LIB_HELPER = HELPER.replace('defmodule helper', 'defmodule lib/helper')
IMPORTER = '''(defmodule importer
  (import {path} :refer [answer])
  (defn run [] (trowel-set-text (int->str answer))))
(trowel-register-command "imp.cmd" "imp" "Test" "" (fn [] (importer/run)))
'''


def test_import_needs_capability(tmp_path, trowel_session):
    d = _plugin(tmp_path, "imp", IMPORTER.format(path="helper"))
    (d / "helper.tur").write_text(HELPER)
    trowel = trowel_session.launch()
    # The plugin fails to load: no capability, no (import ...).
    with pytest.raises(ControlError):
        trowel.call("command.run", {"id": "imp.cmd"})


def test_sibling_import_with_capability(tmp_path, monkeypatch, trowel_session):
    monkeypatch.setenv("TROWEL_PLUGIN_PERMISSIONS", "allow")
    d = _plugin(tmp_path, "imp", IMPORTER.format(path="helper"),
                '(defplugin "imp" :capabilities [import])\n')
    (d / "helper.tur").write_text(HELPER)
    trowel = trowel_session.launch()
    assert _run(trowel, "imp.cmd") == "42"


def test_plain_import_cannot_reach_other_plugins(tmp_path, monkeypatch, trowel_session):
    monkeypatch.setenv("TROWEL_PLUGIN_PERMISSIONS", "allow")
    _plugin(tmp_path, "lib", "")
    (tmp_path / "home" / ".trowel" / "plugins" / "lib" / "helper.tur").write_text(LIB_HELPER)
    _plugin(tmp_path, "imp", IMPORTER.format(path="lib/helper"),
            '(defplugin "imp" :capabilities [import])\n')
    trowel = trowel_session.launch()
    with pytest.raises(ControlError):
        trowel.call("command.run", {"id": "imp.cmd"})


def test_import_plugins_reaches_other_plugins(tmp_path, monkeypatch, trowel_session):
    monkeypatch.setenv("TROWEL_PLUGIN_PERMISSIONS", "allow")
    _plugin(tmp_path, "lib", "")
    (tmp_path / "home" / ".trowel" / "plugins" / "lib" / "helper.tur").write_text(LIB_HELPER)
    _plugin(tmp_path, "imp", IMPORTER.format(path="lib/helper"),
            '(defplugin "imp" :capabilities [import:plugins])\n')
    trowel = trowel_session.launch()
    assert _run(trowel, "imp.cmd") == "42"
