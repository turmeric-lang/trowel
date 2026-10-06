# Reported test failures

Failures in the smoke suite found during the minimap work. All are
pre-existing — they reproduce on the clean tree with the minimap changes
stashed — and none are caused by the minimap changes.

All five were reproduced 2026-10-05 against `main` (9616c3a) and then fixed.
See each report's **Status** / **Fixed** lines for details.

## Summary

| Report | Test | Category | Status |
|---|---|---|---|
| [step-back-lands-on-the-same-line-in-suite-context](step-back-lands-on-the-same-line-in-suite-context.md) | `test_debugger.py::test_step_back_moves_the_cursor_backwards` | flaky (timing) | fixed |
| [restart-stop-count-race-in-suite-context](restart-stop-count-race-in-suite-context.md) | `test_debugger.py::test_restart_respawns_the_same_program` | flaky (timing) | fixed |
| [settings-json-not-created-on-startup](settings-json-not-created-on-startup.md) | `test_settings.py::test_live_reload_rainbow_off` | pre-existing (product gap) | fixed |
| [broken-json-test-settings-json-missing](broken-json-test-settings-json-missing.md) | `test_settings.py::test_broken_json_keeps_last_values` | pre-existing (product gap) | fixed |
| [qsettings-value-migrated-on-first-launch](qsettings-value-migrated-on-first-launch.md) | `test_settings.py::test_qsettings_values_not_read` | pre-existing (test premise) | fixed |

## The debugger tests are flaky, not broken

Both debugger tests pass in isolation and fail when run as part of the full
`test_debugger.py` suite. They share one root cause: `DebugSession::onStopped`
sets `state = Paused` synchronously but increments `stop_count` (and populates
frames) inside an async `stackTrace` callback. A test that reads `stop_count`
or `frames` right after `_wait_state("paused")` returns can see stale or
empty data. The fix in both cases is to wait for `stop_count` to actually
increment before reading — `_step` polls `debug.frames` until non-empty, and
`_wait_state` now accepts a `min_stop_count` parameter.

## The settings tests share one product gap

Two of the three settings tests crash with `FileNotFoundError` because the
app does not create `settings.json` during normal startup.
`Settings::ensureFileExists()` is called only from `MainWindow::openSettings()`,
not from the startup path. Calling it during startup fixes both.

The third settings test, `test_qsettings_values_not_read`, fails because
`migrateFromQSettings()` reads QSettings on first launch (when `settings.json`
does not exist) and writes `editor.rainbowBrackets` into `settings.json`.
The test's premise — that QSettings is not read — holds for the steady-state
read path but not for the one-shot migration. On macOS, cfprefsd isolation
complicates this further.
