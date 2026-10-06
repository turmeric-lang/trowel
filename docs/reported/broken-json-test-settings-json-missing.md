# `test_broken_json_keeps_last_values` fails: settings.json does not exist after launch

**Found:** 2026-10-05, against `main` (8a3546b). Pre-existing — reproduces on
the clean tree with the minimap changes stashed.
**Status:** reproduced 2026-10-05 against `main` (9616c3a). Fails in
isolation with `FileNotFoundError` at `test_settings.py:74`
(`json.loads(trowel_session.settings_json.read_text())`) — same crash and
root cause as `test_live_reload_rainbow_off`; `settings.json` is absent
after launch.
**Fixed:** 2026-10-05. Same source fix as the companion report
(`ensureFileExists()` in the `Settings` constructor). The test's fixed
`time.sleep(1.0)` waits were also replaced with `_wait_rainbow_off`, which
polls `editor.get_style_at` (re-lexing each poll) and requires the off
state to hold across two checks straddling the 200ms debounce. Verified:
`test_settings.py` 7/7 pass.
**Impact:** the test cannot run. It crashes in setup with `FileNotFoundError`
before asserting anything about broken-JSON resilience.

## What happens

The test writes valid settings (rainbow off), waits for reload, then
overwrites with broken JSON and asserts the previous values stay in effect:

```python
existing = json.loads(trowel_session.settings_json.read_text())
existing["editor.rainbowBrackets"] = False
trowel_session.settings_json.write_text(json.dumps(existing))
```

`read_text()` raises `FileNotFoundError` on the first line. Same crash as
`test_live_reload_rainbow_off`, same root cause.

## Why it happens

Identical to [settings-json-not-created-on-startup](settings-json-not-created-on-startup.md).
The app does not create `settings.json` during normal startup.
`Settings::ensureFileExists()` is called only from `MainWindow::openSettings()`
(`src/app/main_window.cpp:3186`), not from the startup path. The `Settings`
constructor's `load()` silently treats a missing file as empty values, and
`migrateFromQSettings()` writes the file only when QSettings has values to
migrate. With a fresh test environment and no QSettings migration values,
the file is never created.

## Suggested fix

Same as the companion report: call `Settings::ensureFileExists()` during
startup so `settings.json` exists after launch. Both tests share the same
fix; fixing one fixes the other.

A note on the test itself: even once the file exists, the test relies on a
1-second `time.sleep` for the 200ms debounce reload. Under load this is the
same shape that made the seek tests flaky — a fixed sleep that passes in
isolation and fails when the machine is busy. The seek tests were fixed by
polling (`_wait_output`); this test would benefit from the same treatment,
polling `editor.get_style_at` until the styles reflect the new setting
rather than sleeping a fixed duration.
