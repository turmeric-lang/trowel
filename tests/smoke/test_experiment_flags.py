"""Experiment flags — resolveExperiments scanner, REPL banner, control API.

Tests that writing ``:enable [name]`` to ``~/.config/turmeric/experiments.tur``
causes ``experiments.status`` to report the names and source, that a
``build.tur`` ``:enable`` overrides the user file, and that the REPL banner
shows the resolved set.

See docs/plans/experiment-flags.md.
"""


def test_default_no_experiments(trowel):
    """With no experiments.tur and no build.tur, status reports empty."""
    status = trowel.call("experiments.status")
    assert status["names"] == []
    assert status["source"] == ""


def test_user_settings_experiments(trowel_session):
    """Writing :enable [reflected-measures] to experiments.tur."""
    trowel_session.experiments_file.write_text(
        ":enable [reflected-measures]\n"
    )
    t = trowel_session.launch()
    status = t.call("experiments.status")
    assert "reflected-measures" in status["names"]
    assert status["source"] == "user settings"
    assert status["opted_out"] is False


def test_user_settings_multiple_experiments(trowel_session):
    """Multiple experiment names in the user file."""
    trowel_session.experiments_file.write_text(
        ":enable [reflected-measures repl-jit-inline-c]\n"
    )
    t = trowel_session.launch()
    status = t.call("experiments.status")
    assert "reflected-measures" in status["names"]
    assert "repl-jit-inline-c" in status["names"]
    assert status["source"] == "user settings"


def test_build_tur_overrides_user_file(trowel_session, tmp_path):
    """build.tur :enable wins over the user file."""
    # Write a user file with one experiment.
    trowel_session.experiments_file.write_text(
        ":enable [reflected-measures]\n"
    )

    # Create a project dir with a build.tur that has a different :enable.
    proj = tmp_path / "proj"
    proj.mkdir()
    (proj / "build.tur").write_text(
        "(defpackage proj\n  (:enable [repl-jit-inline-c]))\n"
    )
    (proj / "main.tur").write_text('(defn main [] (println "hello"))\n')

    t = trowel_session.launch()
    t.call("editor.open", {"path": str(proj / "main.tur")})
    status = t.call("experiments.status")
    assert "repl-jit-inline-c" in status["names"]
    assert "reflected-measures" not in status["names"]
    assert status["source"] == "build.tur"


def test_build_tur_explicit_empty_opts_out(trowel_session, tmp_path):
    """build.tur with :enable [] explicitly opts out of user-level flags."""
    trowel_session.experiments_file.write_text(
        ":enable [reflected-measures]\n"
    )

    proj = tmp_path / "optout"
    proj.mkdir()
    (proj / "build.tur").write_text(
        "(defpackage optout\n  (:enable []))\n"
    )
    (proj / "main.tur").write_text('(defn main [] (println "hello"))\n')

    t = trowel_session.launch()
    t.call("editor.open", {"path": str(proj / "main.tur")})
    status = t.call("experiments.status")
    assert status["names"] == []
    assert status["source"] == "build.tur"
    assert status["opted_out"] is True


def test_repl_banner_shows_experiments(trowel_session):
    """The REPL banner includes the resolved experiment set."""
    trowel_session.experiments_file.write_text(
        ":enable [reflected-measures]\n"
    )
    t = trowel_session.launch()
    t.wait_output("turmeric>", timeout_ms=8000)
    screen = t.call("repl.get_screen", {"lines": 200})["text"]
    banner_lines = [l for l in screen.splitlines() if "repl started" in l]
    assert banner_lines, "no startup banner found"
    banner = banner_lines[-1]
    assert "reflected-measures" in banner
    assert "from user settings" in banner


def test_repl_banner_shows_none(trowel):
    """The REPL banner shows 'experiments: none' when no flags are set."""
    trowel.wait_output("turmeric>", timeout_ms=8000)
    screen = trowel.call("repl.get_screen", {"lines": 200})["text"]
    banner_lines = [l for l in screen.splitlines() if "repl started" in l]
    assert banner_lines, "no startup banner found"
    assert "experiments: none" in banner_lines[-1]


def test_malformed_experiments_file(trowel_session):
    """A malformed experiments.tur is treated as no user-level flags."""
    trowel_session.experiments_file.write_text(
        "this is not valid turmeric\n"
    )
    t = trowel_session.launch()
    status = t.call("experiments.status")
    assert status["names"] == []
    assert status["source"] == ""
