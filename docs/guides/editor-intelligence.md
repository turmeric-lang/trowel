# Editor intelligence

Trowel runs `tur lsp` — the Turmeric language server — in the background to
provide diagnostics, completions, hover documentation, go-to-definition, find
references, rename, and an outline view. These features are available in
Turmeric buffers that have been saved to disk; unsaved buffers get no language
support (the server works by file URI).

## Enabling and configuring

The language server is on by default. Two settings control it (see
[Settings](settings.md)):

| Key | Default | Effect |
|---|---|---|
| `lsp.enabled` | `true` | Whether the server runs. Turning it off stops it; turning it on restarts it. |
| `lsp.serverPath` | `""` | Override the server binary. Empty means use the same `tur` the REPL uses. |

You can also restart the server at any time: **Run → Restart Language Server**.

## Diagnostics

Errors and warnings from the language server appear as squiggles in the editor
and markers in the margin. The server publishes a batch per file; Trowel reads
the decorations back out of Scintilla (not just the model) so what you see
painted is what the server sent.

Diagnostics require a saved file. An unsaved buffer has no URI, so the server
has never seen it — the status bar says "unsaved buffers get no language
support — save the file to enable it."

## Completions, hover, and signature help

| Feature | How to invoke | Shortcut |
|---|---|---|
| **Completions** | Edit → Complete Symbol | `Ctrl+Space` |
| **Hover / documentation** | Edit → Show Documentation | `Ctrl+Shift+D` |
| **Signature help** | Edit → Show Signature Help | — |

Completions suggest symbols at the caret. Hover shows the type and docstring of
the symbol at the caret. Signature help shows the parameter list for the call
being typed, highlighting the active parameter.

All three require a saved file, for the same reason as diagnostics.

## Navigation

| Feature | How to invoke | Shortcut |
|---|---|---|
| **Go to Definition** | Go → Go to Definition | `F12` |
| **Find References** | Go → Find References | `Shift+F12` |
| **Show Symbols (outline)** | Go → Show Symbols | `Ctrl+Shift+M` |
| **Find Symbol in Project** | Go → Find Symbol in Project… | — |
| **Go to Line** | Go → Go to Line… | — |
| **Go to Diagnostic Source** | Go → Go to Diagnostic Source | — |
| **Back** | Go → Back | `Ctrl+Alt+-` |
| **Forward** | Go → Forward | `Ctrl+Alt+Shift+-` |

- **Go to Definition** jumps to where the symbol at the caret is defined. The
  jump goes on a navigation history stack, so Back returns you to where you
  were.
- **Find References** lists every use of the symbol across the workspace.
- **Show Symbols** opens this file's outline — a list of its definitions.
  Picking one jumps to it; that jump goes on the same history stack as Go to
  Definition, so Back works after an outline pick.
- **Find Symbol in Project** searches for a definition across the project.
- **Back / Forward** navigate the jump history. They deliberately avoid
  `Ctrl+Alt+Left`/`Right`, which several Linux desktops claim as a workspace
  switcher.

## Rename

**Edit → Rename Symbol** (`F2`) renames the symbol at the caret across the
workspace.

Rename is scope-aware: renaming a `let` binding or a parameter changes only
its own scope, and renaming a top-level name reaches every workspace file that
imports it. Those files are **opened as dirty tabs, never written to disk** —
so `Ctrl+Z` undoes the whole thing and nothing changes underneath you. Trowel
asks first when a rename would touch more than 20 files.

If the server refuses (a stdlib symbol, a name defined in another file, an
exported name), it says why in the status bar before the input appears.

## See also

- [Keyboard shortcuts](keyboard-shortcuts.md) — the editor-intelligence
  shortcuts.
- [Settings](settings.md) — `lsp.enabled` and `lsp.serverPath`.
- [Scripting Trowel](scripting.md) — `lsp.*` commands over the control socket.
