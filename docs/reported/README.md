# Reported test failures

Failures in the smoke suite found during the minimap work. All four are
pre-existing — they reproduce on the clean tree with the minimap changes
stashed — and none are caused by the minimap changes.

## Summary

| Report | Test | Category |
|---|---|---|
| [step-back-lands-on-the-same-line-in-suite-context](step-back-lands-on-the-same-line-in-suite-context.md) | `test_debugger.py::test_step_back_moves_the_cursor_backwards` | flaky (timing) |
| [settings-json-not-created-on-startup](settings-json-not-created-on-startup.md) | `test_settings.py::test_live_reload_rainbow_off` | pre-existing (product gap) |
| [broken-json-test-settings-json-missing](broken-json-test-settings-json-missing.md) | `test_settings.py::test_broken_json_keeps_last_values` | pre-existing (product gap) |
| [qsettings-value-migrated-on-first-launch](qsettings-value-migrated-on-first-launch.md) | `test_settings.py::test_qsettings_values_not_read` | pre-existing (test premise) |

## The debugger test is flaky, not broken

`test_step_back_moves_the_cursor_backwards` passes in isolation (3/3 runs)
and fails when run as part of the full `test_debugger.py` suite. The
replay step-back works — it is exercised by other tests that poll on output
rather than reading frames immediately after a step. The failure is a race
between `stop_count` incrementing and frames being populated, widened by
suite load.

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
