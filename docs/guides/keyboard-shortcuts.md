# Keyboard Shortcuts

All Trowel shortcuts are registered in `src/app/main_window.cpp` (`setupMenus()`).
On macOS, `Ctrl` maps to `Cmd` for the platform-standard sequences
(`QKeySequence::New`, `Save`, `Quit`, etc.).

## File

| Shortcut | Action |
| --- | --- |
| `Ctrl+N` | New buffer |
| `Ctrl+Shift+N` | New window |
| `Ctrl+O` | Open file… |
| `Ctrl+Shift+O` | Open directory… |
| `Ctrl+S` | Save |
| `Ctrl+Shift+S` | Save As… |
| `Ctrl+W` | Close current tab (the last tab leaves an empty one) |
| `Ctrl+Shift+W` | Close window |
| `Ctrl+Q` / `Alt+F4` | Quit (all windows) |

## Window

The **Window** menu lists every open window; selecting one brings it to the
front. See [Windows, tabs & the REPL](windows-tabs-and-repl.md) for how
Trowel decides where a file opens.

## View

| Shortcut | Action |
| --- | --- |
| `Ctrl+,` | Pick font… |
| `Alt+M` | Toggle minimap |
| `Ctrl+Tab` | Next tab |
| `Ctrl+Shift+Tab` | Previous tab |

## Run

| Shortcut | Action |
| --- | --- |
| `Ctrl+R` | Run buffer (evaluate file) |
| `Ctrl+Shift+E` | Run selection |
| `Ctrl+Shift+R` | Restart REPL (in the current file's directory) |
| `Ctrl+Shift+K` | Clear REPL output |
| `Ctrl+Shift+F` | Format file (`tur format`) |
| `Ctrl+Shift+T` | Trace buffer (record an execution trace with `tur trace`) |
| `F5` | Debug buffer (run the file under the interpreter debugger, `tur dap`) |
| `Ctrl+F5` | Time-travel debug (record the run, then step it in both directions) |
| `Ctrl+Shift+F5` | Restart the debug session (a respawn — one program per session) |
| `F9` | Toggle a breakpoint on the caret's line (or click the gutter) |
| `Ctrl+E` | Focus editor |
| `Ctrl+T` | Focus REPL |
| ``Ctrl+` `` | Toggle focus between editor and REPL |

## Language

Turmeric buffers only — these are greyed out for the other languages Trowel
highlights, since the language server speaks only Turmeric.

| Shortcut | Action |
| --- | --- |
| `Ctrl+Space` | Complete symbol at the caret |
| `Ctrl+Shift+D` | Show documentation for the symbol at the caret |
| `F12` | Go to definition |
| `Shift+F12` | Find references (every use, across the workspace) |
| `F2` | Rename symbol |
| `Ctrl+Shift+M` | Show symbols (this file's outline) |
| `Ctrl+Alt+-` | Go back (return to the position before the last jump) |
| `Ctrl+Alt+Shift+-` | Go forward |

Back and Forward deliberately avoid `Ctrl+Alt+Left`/`Right`, which several
Linux desktops claim as a workspace switcher; the pair above is VS Code's
alternate and is unclaimed on all three platforms.

Rename is scope-aware: renaming a `let` binding or a parameter changes only its
own scope, and renaming a top-level name reaches every workspace file that
imports it. Those files are **opened as dirty tabs, never written to disk** —
so Ctrl+Z undoes the whole thing and nothing changes underneath you. Trowel
asks first when a rename would touch more than 20 files. If the server refuses
(a stdlib symbol, a name defined in another file, an exported name), it says
why in the status bar before the input appears.

A definition inside the bundled Turmeric stdlib opens a read-only tab, marked
`(ro)` in the tab bar. It stays out of *Open Recent* and out of the restored
session — the file lives in the app bundle and is replaced wholesale on
upgrade, so a remembered one would quietly show the previous version's stdlib.

## Directory view

Active when the directory list has focus.

| Shortcut | Action |
| --- | --- |
| `Left` | Navigate to parent directory |
| `Right` | Activate current item |
| `Esc` | Exit path-edit mode |

## Terminal / REPL

The REPL pane forwards keystrokes to the underlying PTY, so shortcuts follow
standard terminal conventions:

- `Ctrl+A`–`Ctrl+Z` are sent as control characters (e.g. `Ctrl+C` = `\x03`).
- `Ctrl+C` / `Cmd+C` copies when a selection exists.
- `Ctrl+V` / `Cmd+V` pastes from the clipboard.
- Arrow keys, `Home`, `End`, `Delete`, `Tab`, and `Enter` emit VT100 escape
  sequences.

## Editor (Scintilla)

The editor pane is a [Scintilla](https://www.scintilla.org/) widget and inherits
its full built-in keymap (cursor movement, selection, indentation, undo/redo,
etc.). For the complete list see the upstream reference:

- **SciTE / Scintilla key bindings:** <https://www.scintilla.org/SciTEDoc.html#KeyBindings>
- **Scintilla default key definitions:** <https://www.scintilla.org/ScintillaDoc.html#KeyDefinitions>
