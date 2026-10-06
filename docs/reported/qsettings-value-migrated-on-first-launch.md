# `test_qsettings_values_not_read` fails: QSettings value reaches the app on first launch

**Found:** 2026-10-05, against `main` (8a3546b). Pre-existing — reproduces on
the clean tree with the minimap changes stashed.
**Status:** reproduced 2026-10-05 against `main` (9616c3a). Fails in
isolation with `AssertionError: QSettings value leaked: [23, 23, 23, 23]`
at `test_settings.py:130` — all styles below 40, rainbow off. The QSettings
`editor/rainbowBrackets=false` value reached the app through the one-shot
`migrateFromQSettings()` path, exactly as described.
**Fixed:** 2026-10-05. The test now pre-writes `settings.json` with `{}`
before launch, so `migrateFromQSettings()` short-circuits on its
`QFile::exists` check and never reads QSettings — testing the steady-state
invariant the name promises. Verified: `test_settings.py` 7/7 pass.
**Impact:** the test asserts that a QSettings `editor/rainbowBrackets=false`
value is ignored, but rainbow is off after launch. The QSettings value
reaches the app through the migration path.

## What happens

The test writes a QSettings INI with the old key, launches Trowel, and
expects rainbow to be on (default), because the app reads from
`settings.json`, not QSettings:

```python
ini = trowel_session.settings_ini
ini.parent.mkdir(parents=True, exist_ok=True)
ini.write_text("[editor]\nrainbowBrackets=false\n")

t = trowel_session.launch()
t.call("editor.set_text", {"text": "(a (b) c)"})
styles = [t.call("editor.get_style_at", {"pos": p})["style"] for p in [0, 3, 5, 8]]
assert any(s >= 40 for s in styles), f"QSettings value leaked: {styles}"
```

The assertion fails: `QSettings value leaked: [23, 23, 23, 23]`. All styles
are below 40, meaning rainbow is off. The QSettings value reached the app.

## Why it happens

`Settings::migrateFromQSettings()` (`src/app/settings.cpp:134`) runs in the
`Settings` constructor on every launch where `settings.json` does not yet
exist:

```cpp
void Settings::migrateFromQSettings() {
    if (QFile::exists(impl_->filePath)) return;   // only on first launch

    QSettings qs;
    // ...
    if (qs.contains("editor/rainbowBrackets")) {
        migrated["editor.rainbowBrackets"] = qs.value("editor/rainbowBrackets").toBool();
        qs.remove("editor/rainbowBrackets");
    }
    // ...
    if (migrated.isEmpty()) return;
    // Write the migrated settings to settings.json.
    QSaveFile f(impl_->filePath);
    // ...
}
```

The test does not pre-write `settings.json`. So on launch, the file does
not exist, and migration runs. `QSettings qs` reads the INI the test wrote
(at `$TROWEL_SETTINGS_DIR/turmeric/Trowel.ini`, redirected by
`QSettings::setPath` in `src/main.cpp:51`), finds `editor/rainbowBrackets`,
and writes `editor.rainbowBrackets=false` into `settings.json`. The app then
reads that file and rainbow is off.

The test's premise — "QSettings is not read anymore" — is true for the
steady-state read path (`Settings::rainbowBrackets()` reads only
`settings.json`). But it is false for the one-shot migration path, which
reads QSettings on first launch and writes the values into `settings.json`.
The test writes the QSettings value, the migration faithfully migrates it,
and the app honours it. The "leak" is the migration working as designed.

### macOS complication

On macOS, `QSettings` resolves through cfprefsd (keyed by real uid, not
`$HOME`), so `TROWEL_SETTINGS_DIR` isolation via `QSettings::setPath` may
not fully redirect QSettings away from the developer's real preferences.
If the developer's real QSettings contain `editor/rainbowBrackets`, that
value is migrated regardless of what the test INI says. The conftest
comment at `tests/smoke/conftest.py:130` documents this concern. So on
macOS there are two paths by which the QSettings value reaches the app:
the test INI (through migration) and the developer's real cfprefsd store
(through migration, bypassing the INI redirect).

## Suggested fix

The test should pre-write `settings.json` (even just `{}`) before launch,
so `migrateFromQSettings()` short-circuits on the `QFile::exists` check and
never reads QSettings. That tests the intended invariant — the steady-state
read path ignores QSettings — without being defeated by the one-shot
migration that runs on a fresh directory.

The product question is separate: should `migrateFromQSettings()` migrate
`editor/rainbowBrackets` at all? The migration exists to carry users from
the old QSettings world to `settings.json`. If that migration has already
happened for real users (the QSettings keys are removed after migration,
line 168), then re-migrating in a test environment is the only place this
path still fires. It is correct behaviour for a real upgrade; the test
just needs to not trigger it.
