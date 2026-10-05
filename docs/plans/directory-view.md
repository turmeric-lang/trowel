# Directory View

> **Status:** Complete. `DirectoryView` inherits `TabContent`
> (`src/app/tab_content.h`, `src/app/directory_view.{h,cpp}`), wired into
> `MainWindow` with `File → Open Directory…`, in-place file activation,
> and session persistence (`dir://` prefix encoding).

A netrw-inspired directory browser that lives inside a tab, alongside editor
tabs. Opening a directory (via `File → Open Directory…` or the existing
`File → Open…` dialog if a directory is selected) creates a directory tab.
Activating a file from that tab replaces the tab with an editor tab for the
chosen file. Activating a subdirectory re-roots the same directory tab.

## Goals

- Allow opening directories with the Open dialog or a dedicated menu item.
- Show a scrollable list of files/subdirectories underneath a header row.
- Instead of `.` and `..`, provide a **Back** button in the header.
- Clicking a directory re-focuses the view on that directory.
- Double-clicking a file (or pressing Enter/Return with it selected) loads the
  file in the **current tab**, replacing the directory view.
- Up/Down arrows move selection; Enter/Return activates.
- Show hidden files by default (users need to edit `.gitignore`, etc.).

Explicit non-goals for this iteration: multi-select, drag-and-drop, file
operations (rename/delete/create), file previews, git status decorations, and
persistent per-directory scroll position.

## Architecture

### `TabContent` — new base class

The current `Buffer` in `src/app/main_window.h` hard-codes `EditorView* view`,
and every tab is assumed to be an editor. To let a tab hold either an editor
or a directory view, introduce a thin abstract base:

- `src/app/tab_content.h` — `class TabContent : public QWidget` with:
  - `enum class Kind { Editor, Directory };`
  - `virtual Kind kind() const = 0;`
  - `virtual QString displayName() const = 0;`
  - `virtual QString filePath() const { return {}; }`
  - `virtual bool isModified() const { return false; }`
  - `virtual bool isEmpty() const { return false; }`
  - Signals: `displayNameChanged()`, `modifiedChanged(bool)`.

`EditorView` is refactored to inherit `TabContent` and override those methods
with its existing behavior (no functional change). `MainWindow::Buffer::view`
becomes `TabContent*`. Call sites that need the editor specifically
(`runBuffer`, `save`, `pickFont`, `focusEditor`) go through a helper:

```cpp
EditorView* MainWindow::editorView() const {
    if (activeIndex_ < 0) return nullptr;
    auto* v = buffers_[activeIndex_]->view;
    return v && v->kind() == TabContent::Kind::Editor
        ? static_cast<EditorView*>(v) : nullptr;
}
```

Actions that only make sense for editor tabs (Save, Save As, Run Buffer, Run
Selection, Pick Font) are disabled when the active tab is a directory. This
happens in `refreshTabBar()` / `activateBuffer()`.

### `DirectoryView` — new widget

- `src/app/directory_view.{h,cpp}` — `class DirectoryView : public TabContent`.
- Layout:
  - Header row: a **Back** `QToolButton` (enabled iff `QDir(root_).cdUp()`
    would succeed), followed by the current absolute path as a label.
  - `QListView` populated by a `QFileSystemModel` rooted at `root_`.
- Model config:
  - `setFilter(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)`.
  - `setNameFilterDisables(false)` (no filters yet).
  - Sort: directories first, then case-insensitive name (custom
    `QSortFilterProxyModel::lessThan`).
- Interaction:
  - Up/Down: default `QListView` keyboard nav.
  - Enter/Return: activate selected row (`QAbstractItemView::activated`
    handles both Enter and double-click on most platforms; we'll also
    connect `doubleClicked` explicitly to be safe).
  - On activation:
    - If directory → `setRoot(newPath)` (updates model root, header label,
      Back button enabled state, resets selection to row 0). Emits
      `displayNameChanged()`.
    - If file → emits `fileActivated(QString absolutePath)`.
  - Back button → `setRoot(QDir(root_).cdUp() ? parent : root_)`.
- `displayName()` returns `QDir(root_).dirName()` (or `"/"` at filesystem
  root), so the tab label shows the directory basename.
- Styling: pull the same background/foreground/accent colors used by
  `EditorView` (via `theme_loader`) so the view matches the editor theme.

### `MainWindow` wiring

- **`openDirectory(const QString& path)`** — mirrors `openPath`:
  - If the current buffer is a fresh empty untitled editor, replace it with a
    `DirectoryView` in place (same tab index).
  - Otherwise `addBuffer` a new directory buffer and activate it.
  - Connect `DirectoryView::fileActivated` → `replaceBufferWithFile(index,
    path)`, which:
    1. Creates a new `EditorView`, loads `path`.
    2. Swaps it into the `QStackedWidget` at the same index the directory
       view occupies.
    3. Deletes the old `DirectoryView`.
    4. Refreshes tab label, remembers recent file, updates window title.
  - Connect `DirectoryView::displayNameChanged` → `updateBufferDisplayName(i)`
    so navigating subdirectories updates the tab label.

- **`openFile()`** — two changes:
  - Broaden the file dialog filter so any file can be opened. Change default
    filter to `"All files (*)"` with `"Turmeric (*.tur *.tur.sweet)"` as an
    additional option (order matters: default is first). This lets users edit
    `.gitignore`, `README`, `Justfile`, etc. without changing the filter.
  - After the user picks paths, for each path: if `QFileInfo(path).isDir()`
    call `openDirectory(path)`, else `openPath(path)`. This means the single
    Open dialog can open either kind (with `QFileDialog::ExistingFiles`; on
    macOS this natively supports selecting directories when
    `QFileDialog::Directory` mode is combined — but mixing modes is finicky,
    so we'll keep Open as files-only and add a separate action below).

- **`openDirectoryAction`** — new `File → Open Directory…` menu item using
  `QFileDialog::getExistingDirectory`, wired to `openDirectory()`. Shortcut:
  `Ctrl+Shift+O`.

- **Guards on editor-only actions** — `activateBuffer(index)` sets the enabled
  state of `runBufferAction_`, `runSelectionAction_`, and the Save actions
  based on `buf->view->kind()`. `maybeSaveBuffer` and `maybeSaveAll` skip
  directory buffers entirely (nothing to save).

### Session persistence

`persistState` / `restoreState` currently store an `openBuffers` `QStringList`
of file paths. Extend this to encode kind by prefixing directory entries with
`dir://`:

- File: `/abs/path/to/foo.tur`
- Directory: `dir:///abs/path/to/some-dir`
- Untitled: empty string (unchanged)

On restore, split on the prefix and dispatch to `openPath` or `openDirectory`.
Old settings without the prefix continue to load as files.

## Files to add / change

- **Add:** `src/app/tab_content.h`
- **Add:** `src/app/directory_view.h`, `src/app/directory_view.cpp`
- **Change:** `src/editor/editor_view.h` — inherit `TabContent`, override
  virtuals; add `displayNameChanged`/`modifiedChanged` signals if not already
  present (or route existing `modificationChanged` through).
- **Change:** `src/app/main_window.h` — `Buffer::view` becomes `TabContent*`;
  add `openDirectory`, `replaceBufferWithFile`, `openDirectoryAction_`.
- **Change:** `src/app/main_window.cpp` — implement above; loosen Open dialog
  filter; enable/disable editor-only actions on tab switch; extend
  persist/restore encoding.
- **Change:** `CMakeLists.txt` (or the `src/app` CMake fragment) — add the new
  sources.
- **Add:** an entry in `docs/smoke-tests.md` covering: open a directory, walk
  into a subdir, Back button, double-click a file to open in place, Up/Down
  navigation, hidden files visible.

## Risks / edge cases

- **Root directory** — `cdUp()` returns false at `/`; Back button disabled.
- **Symlinks** — followed by `QFileSystemModel` by default; fine.
- **Very large directories** — `QFileSystemModel` is lazy and handles this;
  no special work needed.
- **Directory deleted while open** — `QFileSystemModel` emits
  `rootPathChanged`; on error, fall back to parent or `QDir::homePath()`.
- **Tab replacement mid-session** — swapping widgets inside `QStackedWidget`
  must preserve `activeIndex_` and re-run `connectBufferSignals` for the new
  editor.
- **`editorView()` returning null** — audit every existing call site to ensure
  it handles null (most already do because of untitled/no-buffer cases).

## Rollout

1. Land `TabContent` refactor with `EditorView` inheriting it, no behavior
   change. Verify all existing flows still work.
2. Add `DirectoryView` + `openDirectory` + `File → Open Directory…` menu item.
3. Wire `fileActivated` → in-place replacement.
4. Loosen Open dialog filter.
5. Extend persistence encoding.
6. Update `docs/smoke-tests.md`.
