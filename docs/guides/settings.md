# Settings

Trowel reads its settings from a hand-editable JSON file called
`settings.json`. The **Settings…** menu command (`⌘,` on macOS, `Ctrl+,`
on Linux and Windows) opens it in a normal editor tab, creating the file
first if it does not exist.

Changes apply on save — there is no restart, no Apply button, and no
in-app preferences panel. Deleting the file restores the defaults.

## Where the file lives

| Platform | Path |
|---|---|
| macOS | `~/.config/trowel/settings.json` |
| Linux | `$XDG_CONFIG_HOME/trowel/settings.json` (default `~/.config/trowel/`) |
| Windows | `%APPDATA%\trowel\settings.json` |

If `TROWEL_CONFIG_DIR` is set, it overrides the location on all platforms.

This sits next to `~/.config/turmeric/`, which holds Turmeric's own
configuration. The two tools' config directories are separate.

## Keys

All keys are flat and dotted, as in VS Code. A key in this document is
exactly what you type in the file.

| Key | Type | Default | Effect |
|---|---|---|---|
| `turmeric.path` | string | `""` (auto-detect) | Path to the `tur` binary. Empty means auto-detect: bundled copy first, then `tur` on `PATH`. Applies at the next REPL restart. |
| `lsp.enabled` | bool | `true` | Whether the language server runs. Turning it off stops the server; turning it on restarts it. |
| `lsp.serverPath` | string | `""` | Override the language server binary. Empty means use the same `tur` the REPL uses. |
| `editor.font.family` | string | platform default monospace | The editor font family. |
| `editor.font.size` | number | `12` | The editor font size in points. |
| `editor.rainbowBrackets` | bool | `true` | Colourise brackets by nesting depth. |
| `editor.bracketPairGuides` | bool | `true` | Draw a guide line connecting bracket pairs. |
| `editor.wrap.prose` | bool | `true` | Word-wrap Markdown and plain text by default. (Phase 4) |
| `editor.wrap.code` | bool | `false` | Word-wrap code files by default. (Phase 4) |
| `editor.rememberDocumentState` | bool | `true` | Remember caret, scroll, and folds per file. (Phase 6) |
| `plugins.disabled` | array of strings | `[]` | Plugin names to skip at load time. Each entry is a plugin's directory name (e.g. `"bookmarks"`, `"snippets"`). All plugins are enabled by default; listing a name here disables it. Changes apply on next startup. |

When Trowel creates the file, it writes `{}` and nothing else. It does
not write every key with its default, because that would pin the
defaults and prevent future changes from reaching existing users.

## What is *not* in this file

Window geometry, the splitter, the session (open files, breakpoints),
recent files, the last open directory, and the zoom level are all
*state*, not *settings*. They stay in the platform's native settings
backend (a plist on macOS, an INI file on Linux) and are not meant to
be edited by hand.

Per-file state (caret, scroll, folds, a buffer's wrap toggle) is also
not a setting. It is recorded automatically as you work and stored
separately (phase 6).

## Errors

On a parse error or a wrong-typed value, Trowel keeps the last good
values and shows a status-bar message naming the line or the key.
Unknown keys produce a warning, which catches typos such as
`editor.rainbowBracket`.

## Migration

On the first launch after upgrading, if `settings.json` does not exist
and the old QSettings store holds any of the moved keys, Trowel writes
those values into a new `settings.json` and removes them from QSettings.
A status-bar message says where the settings moved to.
