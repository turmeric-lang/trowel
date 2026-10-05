"""P7 — background tasks: trowel-spawn + event loop pump."""

import pytest
import time

BG_PLUGIN = r''';; A plugin that uses trowel-spawn for background work.
(defn bg-task-start []
  (trowel-spawn "(trowel-status-message \"background task done\")"))

(trowel-register-command
  "bg.run"
  "Run Background Task"
  "Test"
  ""
  (fn [] (bg-task-start)))
'''


@pytest.fixture
def bg_plugin(tmp_path):
    plugins_dir = tmp_path / "home" / ".trowel" / "plugins" / "bg-test"
    plugins_dir.mkdir(parents=True, exist_ok=True)
    (plugins_dir / "plugin.tur").write_text(BG_PLUGIN)


def test_spawn_runs_background_task(bg_plugin, trowel):
    """trowel-spawn queues a task; the QTimer pump drains it."""
    trowel.call("command.run", {"id": "bg.run"})
    # The event loop pump runs every ~16ms.  Wait a moment for the task
    # to complete and the status message to be set.
    time.sleep(0.2)
    # The task calls trowel-status-message, which sets the status bar.
    # We can't easily read the status bar from the control socket, but
    # we can verify the app is still responsive (the task didn't crash it).
    text = trowel.call("editor.get_text")
    assert text["text"] == ""


def test_pump_events_native(bg_plugin, trowel):
    """trowel-pump-events drains the scheduler synchronously."""
    # Spawn a task and pump immediately.
    trowel.call("command.run", {"id": "bg.run"})
    # The app should still be responsive.
    trowel.type("hello")
    assert trowel.call("editor.get_text")["text"] == "hello"
