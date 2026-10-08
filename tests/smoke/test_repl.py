"""§3.4 — REPL interaction."""

import pytest

from trowel_ctl import ControlError


def test_repl_echoes_result(trowel):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.send("(+ 1 2)")
    hit = trowel.wait_output("3", timeout_ms=3000)
    assert "3" in hit["matched"]


def test_repl_multiple_expressions(trowel):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.send("(+ 40 2)")
    trowel.wait_output("42", timeout_ms=3000)
    trowel.send("(- 10 3)")
    hit = trowel.wait_output("7", timeout_ms=3000)
    assert "7" in hit["matched"]


def _cwd_banner(trowel):
    """The most recent 'repl started in <dir>' line on the REPL screen."""
    lines = [
        line.strip()
        for line in trowel.call("repl.get_screen", {"lines": 200})["text"].splitlines()
        if "repl started in" in line
    ]
    if not lines:
        return None
    # Strip the "(experiments: ...)" suffix so endswith(dir) still works.
    return lines[-1].split("  (experiments")[0].strip()


def test_repl_banner_reports_working_directory(trowel):
    """Each REPL says where it is rooted — otherwise the cwd is invisible."""
    trowel.wait_output("turmeric>", timeout_ms=5000)
    banner = _cwd_banner(trowel)
    assert banner is not None, "no startup banner found on the REPL screen"
    # A window with no file roots at home, shown shell-style as `~`.
    assert banner.endswith("~"), banner


def test_repl_banner_follows_the_file_a_window_opened(trowel, fixture_files):
    """A REPL restarted with a file open roots in that file's directory."""
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(fixture_files / "hello.tur")})
    trowel.call("repl.restart")
    trowel.wait_output("Turmeric v", timeout_ms=5000)

    banner = _cwd_banner(trowel)
    assert banner is not None
    assert banner.endswith(fixture_files.name), banner


def test_repl_set_cwd_reroots(trowel, fixture_files):
    """`repl.set_cwd` restarts the REPL in the given directory.

    Both calls are synchronous — set_cwd restarts before replying — so no
    waiting is needed for the new value to be visible.
    """
    trowel.wait_output("turmeric>", timeout_ms=5000)
    assert trowel.call("repl.get_cwd")["cwd"]  # rooted somewhere to begin with

    assert trowel.call("repl.set_cwd", {"path": str(fixture_files)})["cwd"] == str(
        fixture_files
    )
    assert trowel.call("repl.get_cwd")["cwd"] == str(fixture_files)
    assert trowel.call("repl.is_running")["running"] is True


def test_repl_set_cwd_rejects_non_directories(trowel, fixture_files):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    before = trowel.call("repl.get_cwd")["cwd"]

    with pytest.raises(ControlError, match="bad_path"):
        trowel.call("repl.set_cwd", {"path": str(fixture_files / "hello.tur")})
    with pytest.raises(ControlError, match="bad_path"):
        trowel.call("repl.set_cwd", {"path": str(fixture_files / "no-such-dir")})

    # A rejected path must leave the running REPL alone.
    assert trowel.call("repl.get_cwd")["cwd"] == before
    assert trowel.call("repl.is_running")["running"] is True


def test_restart_repl_returns_to_the_file_directory(trowel, fixture_files):
    """Plain Restart REPL re-roots to the active file, discarding an explicit
    set_cwd — the working directory is not sticky."""
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(fixture_files / "hello.tur")})
    trowel.call("repl.set_cwd", {"path": str(fixture_files.parent)})
    assert trowel.call("repl.get_cwd")["cwd"] == str(fixture_files.parent)

    # menu.invoke triggers the action synchronously, unlike repl.restart.
    trowel.call("menu.invoke", {"path": ["Run", "Restart REPL"]})
    assert trowel.call("repl.get_cwd")["cwd"] == str(fixture_files)


def test_repl_restart_changes_state(trowel):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("repl.restart")
    # After restart, the banner reappears.
    trowel.wait_output("Turmeric v", timeout_ms=5000)
    assert trowel.call("repl.is_running")["running"] is True


def test_repl_survives_syntax_error(trowel):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    # Unbalanced close-paren — a complete but invalid expression.
    trowel.send(")))")
    trowel.wait_idle(quiet_ms=300, timeout_ms=3000)
    # A real expression should still evaluate afterwards.
    trowel.send("(+ 5 5)")
    hit = trowel.wait_output("10", timeout_ms=3000)
    assert "10" in hit["matched"]


def test_repl_ctrl_c_returns_prompt(trowel):
    """Ctrl-C on a running REPL should get back to a prompt."""
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.repl_press("c", mods=["ctrl"])
    trowel.wait_idle(quiet_ms=300, timeout_ms=3000)
    # Ensure REPL is still alive and responds.
    trowel.send("(+ 1 1)")
    hit = trowel.wait_output("2", timeout_ms=3000)
    assert "2" in hit["matched"]
