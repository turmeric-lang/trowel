# Directory view & project navigation

Trowel has a built-in directory browser that opens as a tab in the editor
stack, alongside your file tabs. It lets you navigate a project tree, open
files, and move between directories without leaving the editor.

## Opening a directory

- **File → Open Directory…** (`Ctrl+Shift+O`) opens a directory picker. The
  selected directory opens as a new tab showing its contents.
- Dragging a directory onto the window does the same thing.

If the active tab is a fresh, empty, untitled buffer, the directory view
replaces it in place rather than opening a new tab — so you do not accumulate
blank tabs.

## The directory tab

```
┌─────────────────────────────────────┐
│  ◀  /path/to/project                │  ← back button + editable path
├─────────────────────────────────────┤
│  src/                                │
│  tests/                              │
│  CMakeLists.txt                      │
│  README.md                           │  ← dirs first, then files
└─────────────────────────────────────┘
```

- **Back button (◀)** — navigate up to the parent directory. Disabled at the
  filesystem root.
- **Path bar** — shows the current directory. Type a path and press Enter to
  jump there; press `Esc` to revert to the current directory and return focus
  to the list.
- **File list** — directories are sorted first, then files. Double-click (or
  press Enter) on a directory to descend into it; double-click a file to open
  it in a new editor tab.

The tab's title is the directory's basename (e.g. `project` for
`/path/to/project`).

## Opening files

Activating a file in the directory view opens it as a normal editor tab,
following the same window/tab placement rules as any other file open — see
[Windows, tabs & the REPL](windows-tabs-and-repl.md).

## Keyboard navigation

The directory list takes keyboard focus when the tab is active:

- **Up / Down** — move the selection.
- **Enter** — open the selected directory or file.
- **Backspace** — not bound; use the back button to go up.

The first row is selected automatically when you enter a directory, so you
can navigate immediately without reaching for the mouse.

## See also

- [Windows, tabs & the REPL](windows-tabs-and-repl.md) — where a file opens
  when you activate it from the directory view.
- [Editor intelligence](editor-intelligence.md) — **Find Symbol in Project**
  searches across the project without a directory view open.
- [Keyboard shortcuts](keyboard-shortcuts.md) — `Ctrl+Shift+O` to open a
  directory.
