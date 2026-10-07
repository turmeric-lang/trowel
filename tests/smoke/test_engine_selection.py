"""Engine selection — the run/engine setting and run.status control command.

Tests that writing ``run.engine`` to settings.json before launch causes
``run.status`` to report the engine and a non-empty ``tur_engine`` env value,
and that the default (no key) produces an empty ``tur_engine`` — proving the
manifest is not overridden.

See docs/plans/engine-selection.md.
"""

import json


def test_default_engine_no_override(trowel):
    """With no run/engine key, tur_engine is empty (manifest not overridden)."""
    status = trowel.call("run.status")
    assert status["engine"] == "default"
    assert status["tur_engine"] == ""


def test_engine_interp_injected(trowel_session):
    """Writing run.engine=interp injects TUR_ENGINE=interp into the env."""
    trowel_session.settings_json.write_text(
        json.dumps({"run.engine": "interp"})
    )
    t = trowel_session.launch()
    status = t.call("run.status")
    assert status["engine"] == "interp"
    assert status["tur_engine"] == "interp"
    assert "build" in status["argv"]


def test_engine_cc_injected(trowel_session):
    """Writing run.engine=cc injects TUR_ENGINE=cc into the env."""
    trowel_session.settings_json.write_text(
        json.dumps({"run.engine": "cc"})
    )
    t = trowel_session.launch()
    status = t.call("run.status")
    assert status["engine"] == "cc"
    assert status["tur_engine"] == "cc"


def test_engine_jit_injected(trowel_session):
    """Writing run.engine=jit injects TUR_ENGINE=jit into the env."""
    trowel_session.settings_json.write_text(
        json.dumps({"run.engine": "jit"})
    )
    t = trowel_session.launch()
    status = t.call("run.status")
    assert status["engine"] == "jit"
    assert status["tur_engine"] == "jit"


def test_run_status_reports_binary(trowel):
    """run.status reports a non-empty binary path when tur is resolvable."""
    status = trowel.call("run.status")
    assert status["binary"], "binary path should not be empty"
