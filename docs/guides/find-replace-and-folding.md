# Find, replace & folding

## Find and replace

The find bar sits at the bottom of the editor pane. It opens in find-only or
find-and-replace mode:

| Action | Shortcut (Linux) | Shortcut (macOS) |
|---|---|---|
| Find… | `Ctrl+F` | `Ctrl+F` |
| Find and Replace… | `Ctrl+Alt+F` | `Ctrl+H` |
| Find Next | `Ctrl+G` / `F3` | `Ctrl+G` / `F3` |
| Find Previous | `Ctrl+Shift+G` / `Shift+F3` | `Ctrl+Shift+G` / `Shift+F3` |
| Use Selection for Find | `Ctrl+E` | — |
| Select All Occurrences | `Meta+Ctrl+G` | `Ctrl+Shift+L` |

### Search options

Four checkboxes control how the search matches:

| Option | Effect |
|---|---|
| **Match case** | Case-sensitive comparison. |
| **Whole word** | Match only whole words, not substrings. |
| **Regex** | Treat the query as a regular expression. An invalid regex shows an error in the bar. In replace mode, `\1`–`\9` in the replacement refer to capture groups. |
| **In selection** | Restrict the search to the current selection. |

The bar shows a live match count (`current / total`) so you can see how many
matches there are and which one you are on.

### Replace

In replace mode, a second row holds the replacement text. **Replace** replaces
the current match and advances to the next; **Replace All** replaces every
match. With regex on, capture groups (`\1`–`\9`) in the replacement string
refer to the corresponding groups in the pattern.

### Select All Occurrences

Selects every occurrence of the current query (or the current selection) at
once, so you can edit them simultaneously — multi-cursor style.

## Commenting and indentation

| Action | Shortcut |
|---|---|
| Toggle Comment | `Ctrl+/` |
| Indent | `Ctrl+]` |
| Outdent | `Ctrl+[` |
| Format File | `Ctrl+Shift+Alt+F` |

Toggle Comment adds or removes line comments for the selected lines (or the
current line if there is no selection). Indent and Outdent shift the selection
by one indent level. Format File runs `tur fmt` on the whole buffer — see
[Formatting](formatting.md).

## Folding

Code folding collapses a block (a parenthesized form in Turmeric, a function
body, etc.) into a single line. Fold points are detected from the lexer's
structure; a `[...]` marker in the margin indicates a foldable region.

Folding commands live under **View → Folding**:

| Action | Shortcut (Linux) | Shortcut (macOS) |
|---|---|---|
| Fold | `Alt+Ctrl+[` | `Ctrl+Shift+[` |
| Unfold | `Alt+Ctrl+]` | `Ctrl+Shift+]` |
| Toggle Fold | `Ctrl+K, Ctrl+L` | `Ctrl+K, Ctrl+L` |
| Fold All | `Ctrl+K, Ctrl+0` | `Ctrl+K, Ctrl+0` |
| Unfold All | `Ctrl+K, Ctrl+J` | `Ctrl+K, Ctrl+J` |
| Fold Top-Level Forms | `Ctrl+K, Ctrl+1` | `Ctrl+K, Ctrl+1` |

- **Fold / Unfold** act on the fold point at the caret.
- **Toggle Fold** flips the current fold point.
- **Fold All** collapses every fold point in the buffer; **Unfold All**
  expands them.
- **Fold Top-Level Forms** collapses only the top-level forms — useful for
  getting an overview of a file's structure.

You can also click the margin marker to toggle a fold point with the mouse.

## See also

- [Keyboard shortcuts](keyboard-shortcuts.md) — the full shortcut list.
- [Formatting](formatting.md) — `tur fmt` and the Format File command.
- [Editor intelligence](editor-intelligence.md) — the outline view is another
  way to navigate a file's structure.
