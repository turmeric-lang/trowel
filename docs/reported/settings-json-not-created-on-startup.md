# `test_live_reload_rainbow_off` fails: settings.json does not exist after launch

**Found:** 2026-10-05, against `main` (8a3546b). Pre-existing — reproduces on
the clean tree with the minimap changes stashed.
**Status:** reproduced 2026-10-05 against `main` (9616c3a). Fails in
isolation with `FileNotFoundError` at `test_settings.py:57`
(`json.loads(trowel_session.settings_json.read_text())`) —
`settings.json` is absent after launch, exactly as described.
**Fixed:** 2026-10-05. The `Settings` constructor (`src/app/settings.cpp`)
now calls `ensureFileExists()` after `migrateFromQSettings()`, so
`settings.json` is created (as `{}`) on startup when no migration values
exist. Verified: `test_settings.py` 7/7 pass.
**Impact:** the test cannot run. It crashes in setup with `FileNotFoundError`
before asserting anything about live reload.

## What happens

The test launches Trowel, then reads `settings.json` to amend it with
`editor.rainbowBrackets: false`:

```python
existing = json.loads(trowel_session.settings_json.read_text())
existing["editor.rainbowBrackets"] = False
trowel_session.settings_json.write_text(json.dumps(existing))
```

`read_text()` raises `FileNotFoundError`. The `settings_json` property
(`tests/smoke/conftest.py:292`) creates the parent directory but not the
file itself:

```python
@property
def settings_json(self) -> Path:
    p = self._tmp_path / "home" / "config" / "settings.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    return p
```

The file is expected to exist because the app launched via the `trowel`
fixture. But the app does not create `settings.json` during normal startup.

## Why it happens

`Settings::ensureFileExists()` (`src/app/settings.cpp:100`) is the only code
path that creates `settings.json` when it does not exist. It is called from
exactly one place: `MainWindow::openSettings()` (`src/app/main_window.cpp:3186`),
which runs when the user opens the settings file in the editor — not during
startup.

The `Settings` constructor (`src/app/settings.cpp:60`) calls `load()` (which
silently sets empty values if the file is absent) and `migrateFromQSettings()`
(which writes `settings.json` only if QSettings has values to migrate). If
QSettings is empty — which it is in the test environment when
`TROWEL_SETTINGS_DIR` points at a fresh directory and no migration values
exist — neither path creates the file.

The test harness pre-writes `settings.json` only when `TROWEL_TEST_TUR` is
set (`tests/smoke/conftest.py:152`), seeding `turmeric.path`. Without that
override, the file is never created, and the test's `read_text()` fails.

## Suggested fix

Call `Settings::ensureFileExists()` during startup, after the constructor
finishes — for example at the end of `MainWindow` initialization. The file
is the app's source of truth for settings; it should exist after launch
regardless of whether the user has opened it. This also makes the file
watcher meaningful from the first run, since the watcher is only added to
a file that exists.

Alternatively, the test harness could pre-write `settings.json` with `{}`
in `_launch_trowel`, the same way it pre-writes `turmeric.path` when
`TROWEL_TEST_TUR` is set. But that papers over the product gap — a user who
launches Trowel fresh and then edits `settings.json` externally will find
no file to edit.
