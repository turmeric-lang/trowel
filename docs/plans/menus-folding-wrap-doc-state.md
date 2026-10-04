# Menu audit, settings file, find/replace, word wrap, folding, per-document state — plan

> **Status:** **Not started** (planned 2026-10-04). Phases 1–6 below are
> ordered. Each phase ships on its own.
> **Related:** [`minimap.md`](minimap.md) (requires that nothing here
> breaks its display-line mapping), [`lsp-navigation.md`](lsp-navigation.md)
> (owns the jump commands that folding has to unfold for),
> [`multi-window.md`](multi-window.md) (session restore, which phase 6 builds on),
> [`experiment-flags.md`](experiment-flags.md) (the hand-edited
> `~/.config/turmeric/` file model that phase 2 copies),
> [`PLAN.md`](PLAN.md) §3 (platform code lives in `src/platform/`).

This plan covers six features. They are planned together because each one
depends on another:

- **The menu audit** gives everything else somewhere to live. Today the Edit
  menu is empty, and the View menu holds only Font… and two tab commands.
- **The settings file** replaces the in-app Preferences tab with a
  hand-editable JSON file that the Settings… command opens. New options from
  the later phases (wrap defaults, remembering per-file state) become keys in
  that file, not checkboxes.
- **Find / Replace** is the most obvious gap in the new Edit menu.
- **Word wrap and folding** both separate *display lines* from *document lines*.
  Any code that assumes one display line per document line breaks under either
  one. Doing wrap first finds those assumptions, and folding then reuses the
  fixes. Folding also has to unfold for find's jumps.
- **Per-document state** saves the wrap override and the folded regions, so it
  has to come last.

**Settings versus per-file state.** These are different things and are stored
differently:

- *Settings* are choices the user makes on purpose and edits by hand. They go
  in `settings.json` (phase 2).
- *Per-file state* (caret, scroll, folds, a buffer's wrap toggle) is recorded
  automatically as you work. It goes in a separate `document-state.json` that
  Trowel owns and the user never edits (phase 6).

A per-file *setting* file, such as per-path overrides inside `settings.json`,
is not part of this plan.

---

## 0. What the code looks like today (checked 2026-10-04 against `85ebee5`)

- **Menus** are built by hand in `MainWindow::setupMenus()`
  (`src/app/main_window.cpp:238`). There is no `#ifdef` anywhere in menu code,
  so every OS gets the same tree, and Qt's `Ctrl`→`⌘` mapping is the only
  thing that changes between platforms.
- **The Edit menu is empty.** `menuBar()->addMenu("&Edit")` at `:286` has no
  actions. Undo and the clipboard work only because Scintilla's own keymap
  handles them.
- **There is no Find or Replace of any kind.** No find bar, no dialog, and no
  `SCI_SEARCHINTARGET` calls anywhere in `src/`.
- **The Run menu has 28 items.** It mixes run, debug, REPL, LSP completion and
  docs, navigation, the Dialect submenu, and focus commands.
- **Three commands exist only on the side bar** (`setupToolBar()`, `:572`):
  Toggle Split Orientation, Show/Hide REPL, and the Settings gear (Trowel
  Settings / Turmeric Settings). They have no menu item and no shortcut, so
  `menu.invoke` cannot reach them.
- **Settings today:**
  - *Trowel Settings* opens an in-app tab, `PreferencesView`
    (`src/app/preferences_view.cpp`), with a tur path field, rainbow brackets,
    bracket pair guide, LSP enabled, and *Restore defaults*.
  - Those values are stored in **QSettings** under `repl/turBinary`,
    `editor/rainbowBrackets`, `editor/bracketPairGuides`, and `lsp/enabled`.
    `lsp/serverPath` is read in `LspManager::serverPath()` but has no UI.
    `editorFont` is written by View > Font….
  - QSettings is a plist on macOS (`~/Library/Preferences/com.turmeric.Trowel.plist`)
    and an INI file on Linux (`~/.config/turmeric/Trowel.conf`).
  - *Turmeric Settings* opens the `~/.config/turmeric/` directory in a
    directory tab (`openSettingsDirectory`, `:2737`). On this machine that
    directory exists and is empty.
  - Tests isolate QSettings with `TROWEL_SETTINGS_DIR`, which switches it to an
    INI file (`src/main.cpp:45`). `conftest.py:144` writes `repl/turBinary`
    into that INI to choose which `tur` the tests run.
- **There is no About item and no Help menu.**
- **Smoke tests drive menus by path.** `menu.invoke` matches title text after
  stripping `&` (`FindMenuAction`, `src/control/control_handlers.cpp:122`).
  Tests hard-code these paths: `["Run","Go to Diagnostic Source"]`,
  `["Run","Complete Symbol"]`, `["Run","Toggle Breakpoint"]`,
  `["Run","Restart Debug Session"]`, `["View","Next Tab"]`,
  `["File","Close Window"]`, and more (`grep -rn '"path": \[' tests`). Any item
  that moves breaks its test in the same commit.
- **Folding is off.** `ScannerLexer::Fold()` is an empty override
  (`src/editor/lexer_adapter.cpp:429`). Margin 2 is set to width 0 and kept
  free for folding on purpose (`editor_view.cpp:371-374`).
- **Word wrap is off.** `setWrapMode` is never called.
- **`tur lsp` has no `foldingRangeProvider`.** I checked by sending
  `initialize` to the installed `tur`: the capabilities list hover,
  definition, documentSymbol, documentHighlight, rename, references,
  workspaceSymbol, formatting, signatureHelp, and completion. Fold ranges
  therefore have to be computed locally.
- **What the bundled Scintilla (5.5.5) provides:**
  - `SC_FOLDACTION_CONTRACT` passed to `FoldAll` folds **only top-level**
    headers. Adding `SC_FOLDACTION_CONTRACT_EVERY_LEVEL` (4) folds every level
    (`Editor::FoldAll`, `Editor.cxx:5769`).
  - `NO_CXX11_REGEX` is not defined, so `SCFIND_CXX11REGEX` search is
    available.
- **The Turmeric scanner already tracks bracket depth** in the packed line
  state (`LexState::turBracketDepth`, capped at 127). **It has a bug:** a
  closer only decrements the depth inside `if (in.rainbow)`
  (`scanner_turmeric.cpp:616`). With rainbow brackets turned off, the depth only
  goes up. Folding needs the depth to be correct whether rainbow is on or not,
  so phase 5 fixes this first.
- **Session state** (`MainWindow::sessionState()`, `:3066`) saves geometry, the
  splitter, open paths, the active tab, and breakpoints. **It does not save
  caret, selection, or scroll position.** A restored session opens every file
  at line 1.
- **No `PlainText` language exists.** `LanguageForPath()` treats any
  unrecognised extension as **Turmeric**, so `notes.txt` gets Turmeric
  highlighting today.
- **Qt supports chorded shortcuts.** `QKeySequence("Ctrl+K, Ctrl+0")` binds a
  two-key chord to a `QAction` with no extra code.
- **Platform reach:** CI builds macOS, Linux (AppImage), and Windows (ConPTY
  backend exists). PLAN.md calls Windows "deferred". This plan still gives
  Windows correct conventions, because the cost is a few table rows.

---

## Phase 1 — Menu bar audit

### 1.1 Findings

The "verify" column marks claims that come from how Qt and macOS are documented
to behave, not from pressing the keys. **Press each key in a real build before
fixing it** (see the *Probe a real tur* memory; the same rule applies to the OS).

| # | Finding | Severity | Verify |
|---|---|---|---|
| F1 | `Ctrl+Shift+T` is bound to both **Trace Buffer** (`:322`) and **Find Symbol in Project** (`:420`). Qt treats the key as ambiguous, so neither command fires from the keyboard. `keyboard-shortcuts.md` lists only Trace. | **Bug** | yes |
| F2 | On macOS, `Ctrl+Tab` / `Ctrl+Shift+Tab` (Next/Previous Tab) become `⌘Tab` / `⌘⇧Tab`. The OS app switcher takes those keys first. | **Bug (mac)** | yes |
| F3 | On macOS, `Ctrl+Space` (Complete Symbol) becomes `⌘Space`, which is Spotlight. | **Bug (mac)** | yes |
| F4 | On macOS, ``Ctrl+` `` (Toggle REPL/Editor Focus) becomes ``⌘` ``. The OS uses that to cycle an app's windows, and Trowel has multiple windows. | **Bug (mac)** | yes |
| F5 | `Ctrl+,` opens **Font…**. On every platform `⌘,` / `Ctrl+,` conventionally opens Settings. | Convention | — |
| F6 | The Edit menu is empty. Undo and clipboard actions are invisible, cannot be driven by `menu.invoke`, and macOS fills the empty menu with only its own Dictation / Emoji items. | Convention | — |
| F7 | Settings, Show/Hide REPL, and Toggle Split have no menu item or shortcut. | Accessibility | — |
| F8 | There is no About item or Help menu. | Convention | — |
| F9 | The macOS Window menu has no Minimize (`⌘M`), Zoom, or Bring All to Front. Qt does not add these itself. | Convention (mac) | yes (does `⌘M` do anything?) |
| F10 | On Windows, `QKeySequence::Quit` has **no binding**, but `keyboard-shortcuts.md` says `Ctrl+Q` works. On Windows the item should be labelled *Exit*. | Doc bug (win) | yes |
| F11 | The Run menu is a catch-all (see §0). LSP and navigation commands are not "Run" commands. | Organisation | — |
| F12 | On Linux and Windows, every `Ctrl+<letter>` menu shortcut overrides the matching control character in the REPL terminal (`Ctrl+R`, `Ctrl+E`, `Ctrl+T`, `Ctrl+W`, `Ctrl+N`, `Ctrl+O`…). The window-level `QAction` shortcut fires before `TerminalView::keyPressEvent` sees the key. macOS is unaffected because `⌘` and `⌃` are different keys. | Behaviour (linux/win) | yes |
| F13 | On macOS, `⌘E` (Focus Editor) takes the key every Mac app uses for *Use Selection for Find*. `⌘T` (Focus REPL) takes the usual *New Tab* key. Phase 3 needs `⌘E`, so Focus Editor moves (see the shortcut table). | Conflict (mac) | — |
| F14 | Qt moves actions into the macOS app menu by guessing from their text (`TextHeuristicRole`). That is how *Quit* ends up there today, and it would also move any future item whose text starts with "Settings", "Preferences", "Options", or "Config". Roles should be set explicitly. | Robustness | — |

### 1.2 Target menu trees

The command names stay the same unless a row says otherwise. `⌃` means the
physical Control key, which Qt spells `Meta` on macOS. `Ctrl` in the Linux and
Windows columns means Control. Commands added by later phases are shown in
place, tagged with their phase.

#### macOS

```
Trowel        About Trowel
              ─
              Settings…                    ⌘,      (opens settings.json; PreferencesRole)  [P2]
              Turmeric Settings…                   (opens ~/.config/turmeric/)
              ─
              Services ▸ / Hide Trowel ⌘H / Hide Others ⌥⌘H / Show All   (Qt supplies these)
              ─
              Quit Trowel                  ⌘Q      (QuitRole)
File          New ⌘N · New Window ⇧⌘N · Open… ⌘O · Open Directory… ⇧⌘O · Open Recent ▸
              ─ Close Tab ⌘W · Close Window ⇧⌘W
              ─ Save ⌘S · Save As… ⇧⌘S
Edit          Undo ⌘Z · Redo ⇧⌘Z
              ─ Cut ⌘X · Copy ⌘C · Paste ⌘V · Select All ⌘A
              ─ Find ▸                                                               [P3]
                  Find… ⌘F · Find and Replace… ⌥⌘F
                  ─ Find Next ⌘G · Find Previous ⇧⌘G · Use Selection for Find ⌘E
                  ─ Select All Occurrences ⌃⌘G
              ─ Toggle Comment ⌘/ · Indent ⌘] · Outdent ⌘[ · Format File ⇧⌥F
              ─ Complete Symbol ⌃Space · Show Documentation ⇧⌘D · Show Signature Help ⇧⌘P
                · Rename Symbol… F2
              (macOS appends AutoFill / Dictation / Emoji itself)
View          Show REPL (✓) ⌥⌘R · Toggle Split Orientation
              ─ Word Wrap (✓) ⌥Z                                                     [P4]
              · Folding ▸                                                            [P5]
              ─ Zoom In ⌘= · Zoom Out ⌘- · Actual Size ⌘0
              ─ Next Tab ⇧⌘] (also ⌃Tab) · Previous Tab ⇧⌘[ (also ⌃⇧Tab)
              ─ Enter Full Screen ⌃⌘F     (only if macOS has not already added one)
Go            Go to Definition F12 · Find References ⇧F12
              ─ Show Symbols ⇧⌘M · Find Symbol in Project… ⇧⌘J · Go to Line… ⌘L
              · Go to Diagnostic Source
              ─ Back ⌃- · Forward ⌃⇧-
Run           Run Buffer ⌘R · Run Selection ⇧⌘E · Trace Buffer ⇧⌘T
              ─ Debug Buffer F5 · Time-Travel Debug ⌘F5 · Restart Debug Session ⇧⌘F5
                · Toggle Breakpoint F9
              ─ Restart REPL ⇧⌘R · Restart REPL In… · Clear REPL ⇧⌘K
              ─ Dialect ▸ · Restart Language Server
Window        Minimize ⌘M · Zoom
              ─ Focus Editor ⌥⌘E · Focus REPL ⌘T · Toggle REPL/Editor Focus ⌃`
              ─ Bring All to Front
              ─ <window list>
Help          Trowel Help · Keyboard Shortcuts · Turmeric Documentation · Report an Issue…
              (macOS adds the Help search field itself)
```

**Font… is removed from the menu.** The font becomes the `editor.font.family`
and `editor.font.size` keys in `settings.json` (phase 2). Zoom In / Zoom Out /
Actual Size cover quick size changes without editing the file.

#### Linux and Windows

These match macOS except for the following:

- **No app menu.** *About Trowel* goes at the bottom of **Help**. *Settings…*
  (`Ctrl+,`) and *Turmeric Settings…* go at the bottom of **Edit** on both
  Linux and Windows. That is the GNOME convention, and it means only one
  non-mac tree has to be maintained.
- **File** ends with `─ Quit Ctrl+Q` on Linux, and with `─ E&xit` on Windows.
  On Windows, Alt+F4 comes from the system, and `Ctrl+Q` is added as an
  explicit second shortcut so the documented key actually works (fixes F10).
- **Redo** is `Ctrl+Shift+Z` everywhere, plus `Ctrl+Y` on Windows.
  `QKeySequence::Redo` already provides both.
- **Find keys:** Find `Ctrl+F`, Find and Replace `Ctrl+H`, Find Next `F3`
  (plus `Ctrl+G`), Find Previous `Shift+F3`, Select All Occurrences
  `Ctrl+Shift+L`. There is no *Use Selection for Find*. Instead, opening Find
  with a single-line selection fills the search field with it (§3.1).
- **Window** has no Minimize, Zoom, or Bring All to Front, because the window
  manager provides them.
- **Full screen** is `F11`.
- **Shortcuts that differ from macOS:** Complete Symbol `Ctrl+Space`, Focus
  Editor `Ctrl+E` (unchanged), Toggle Focus ``Ctrl+` ``, Back/Forward
  `Ctrl+Alt+-` / `Ctrl+Alt+Shift+-` (unchanged; see `keyboard-shortcuts.md`
  for why), Go to Line `Ctrl+L`, Next/Previous Tab `Ctrl+Tab` /
  `Ctrl+Shift+Tab`, plus `Ctrl+PgDown` / `Ctrl+PgUp` as alternates.

#### Shortcut changes, consolidated

| Command | Today | macOS after | Linux/Win after | Fixes |
|---|---|---|---|---|
| Find Symbol in Project | `Ctrl+Shift+T` (collides) | `⇧⌘J` | `Ctrl+Shift+J` | F1 |
| Next / Previous Tab | `Ctrl+Tab` | `⇧⌘]` / `⇧⌘[`, alt `⌃Tab` | unchanged + `Ctrl+PgDn/PgUp` | F2 |
| Complete Symbol | `Ctrl+Space` | `⌃Space` | unchanged | F3 |
| Toggle REPL/Editor Focus | ``Ctrl+` `` | ``⌃` `` (VS Code's terminal toggle) | unchanged | F4 |
| Settings… | (side bar only) | `⌘,` | `Ctrl+,` | F5, F7 |
| Font… | `Ctrl+,` | removed (settings key) | removed | F5 |
| Focus Editor | `Ctrl+E` | `⌥⌘E` | unchanged | F13 |
| Back / Forward | `Ctrl+Alt+-` | `⌃-` / `⌃⇧-` (VS Code / Xcode) | unchanged | — |
| Show REPL | (side bar only) | `⌥⌘R` | `Ctrl+Alt+R` | F7 |

`⇧⌘J` was chosen because it is free in Trowel. In Xcode it means *Reveal in
Project Navigator*, which is close enough in spirit. **Before committing to any
new key, check it against every existing binding.** The §1.4 test does this
automatically.

### 1.3 Implementation

1. **Write one shortcut table** in `src/platform/shortcuts.{h,cpp}`, which
   creates the `src/platform/` directory PLAN.md §3 asks for.
   - Define an `enum class Command` with one entry per menu command, including
     the Find and Folding commands from later phases, so every key is chosen in
     one place.
   - `QList<QKeySequence> ShortcutsFor(Command)` is the only `#ifdef Q_OS_MACOS`
     / `Q_OS_WIN` in the change.
   - `setupMenus()` calls `a->setShortcuts(ShortcutsFor(Command::X))` instead of
     writing key strings inline.
   - The table also serves the control API and the docs check (§1.4).
2. **Set menu roles explicitly** (fixes F14):
   - Use a helper `MakeAction(menu, text, Command)` that sets
     `QAction::NoRole` by default.
   - Set `AboutRole`, `PreferencesRole`, and `QuitRole` only on the three
     actions that should move to the macOS app menu.
   - With roles set this way, the File > Quit and Edit > Settings… items *are*
     the app-menu items on macOS, so the tree in code can stay the same on
     every platform for those entries.
3. **Route Edit actions to whichever widget has focus** using
   `QApplication::focusWidget()`:
   - `ScintillaEdit` → `undo()`, `redo()`, `cut()`, `copy()`, `paste()`,
     `selectAll()`.
   - `TerminalView` (a `QPlainTextEdit`) → copy, paste, select all. Undo and
     Redo are disabled there.
   - Any other widget with matching slots (`QLineEdit` in the rename and
     completion popups and the phase 3 find bar) → call them with
     `QMetaObject::invokeMethod`.
   - Update the enabled state on `QApplication::focusChanged` and in the Edit
     menu's `aboutToShow`. That means `canUndo`/`canRedo`, and whether a
     selection exists for Cut/Copy.
   - A disabled `QAction` does not take the key, so Scintilla's own keymap
     still handles the keystroke and nothing runs twice.
   - **Toggle Comment / Indent / Outdent are new editor commands**, not just
     menu items. Toggle Comment needs a per-language line-comment token
     (`;` for Turmeric/R7RS, `#` for Python, Sh, TOML, CMake, and Just, `//`
     for C; JSON, Markdown, and PlainText have no line comment, so the item is
     greyed out). The token belongs in `lexers.h` next to `LanguageForPath`. If
     this grows beyond the menu work, split it into a separate change.
4. **Linux/Windows terminal keys (F12).** `TerminalView` should accept
   `QEvent::ShortcutOverride` for unshifted `Ctrl+<letter>` while it has focus,
   so those keys reach the PTY the way they do in every Linux terminal.
   Copy and paste in the terminal then use `Ctrl+Shift+C` / `Ctrl+Shift+V`
   (add these as terminal-only shortcuts). This changes behaviour, so press the
   keys in the AppImage before and after, and record the result in the commit
   message. This also keeps the `Ctrl+K` chord prefix (phase 5) and `Ctrl+L` /
   `Ctrl+H` (Go to Line, Replace) from taking keys away from the REPL.
5. **Move the side-bar-only actions into menus.** `toggleSplitAction_`,
   `toggleReplAction_`, and the two settings actions are currently created
   inside `setupToolBar()`. Create them in `setupMenus()` and have the side bar
   reuse them with `addSideBarAction`, which is how every other side-bar button
   already works. That keeps the checked state shared between the menu and the
   button automatically. The gear's popup menu keeps its two entries and reuses
   the same two actions.
6. **Go to Line…** is a new command: a small single-line input in the same
   style as the rename input. It accepts `line` or `line:column`, and jumps
   through `EditorView::revealLine` once phase 5 adds it.
7. **Help menu.**
   - *Keyboard Shortcuts* opens `docs/guides/keyboard-shortcuts.md` on GitHub
     at the matching release tag.
   - *Turmeric Documentation* opens the docs for the bundled Turmeric version
     (`TROWEL_TURMERIC_VERSION`).
   - *About* uses `QMessageBox::about` with the version, the Turmeric version,
     and the licence.
   - Bundling an offline copy of the docs is out of scope.
8. **Update the tests in the same commit.** Every moved item changes its
   `menu.invoke` path. Add a `CHANGELOG.md` entry, because the control socket
   is a public API (see [`socket-api.md`](socket-api.md)).

Until phase 2 lands, *Settings…* opens the existing `PreferencesView` tab.

### 1.4 Tests

- **`test_menu.py::test_no_duplicate_shortcuts`:** a new control command
  `menu.list` returns every action with its path, shortcuts, and role. The test
  fails if two actions in the same window share a key sequence. **It also fails
  if a single-key shortcut equals the first key of a chord** (for example a
  bare `Ctrl+K` alongside the `Ctrl+K, Ctrl+0` chord), because Qt would never
  fire the bare one. This catches F1 and any future collision.
- **`test_menu.py::test_edit_routes_to_focus`:**
  1. Type into the editor, then call `menu.invoke ["Edit","Undo"]` and check
     the text is gone.
  2. Focus the REPL, then call `menu.invoke ["Edit","Select All"]` and check
     the terminal's selection.
- **`test_menu.py::test_side_bar_actions_are_menu_reachable`:** check that
  `["View","Show REPL"]` toggles `window.list`'s REPL visibility.
- **`test_menu.py::test_go_to_line`:** jump to `12:3` and check the caret.
- **Docs drift check:** `scripts/check_shortcuts_doc.py` compares `menu.list`
  against the tables in `keyboard-shortcuts.md`. Run it from the smoke suite.
  (Smoke tests need a self-made venv, because `just smoke` alone fails with
  "pytest: command not found".)
- **Manual pass on each OS:** press every row of the *Shortcut changes* table,
  and every F-row marked "verify", once before and once after.

---

## Phase 2 — Settings as a config file

### 2.1 Behaviour

- Trowel reads its settings from **`settings.json`** in Trowel's config
  directory:
  - `$TROWEL_CONFIG_DIR` if set (tests use this),
  - else `$XDG_CONFIG_HOME/trowel/`,
  - else `~/.config/trowel/` on macOS and Linux, and `%APPDATA%\trowel\` on
    Windows.

  This mirrors where `~/.config/turmeric/` already lives, so the two tools'
  config directories sit next to each other.
- **Settings…** (`⌘,` / `Ctrl+,`) opens `settings.json` **in a normal editor
  tab**, creating the directory and the file first if they do not exist. If
  the file is already open, that tab is activated.
- **Turmeric Settings…** keeps opening the `~/.config/turmeric/` directory in a
  directory tab, as it does today. That directory holds several files
  (`experiments.tur` per `experiment-flags.md`, and whatever Turmeric adds
  later), so there is no single file to open.
- **Saving the file applies the change immediately.** There is no restart and
  no Apply button.
- **The in-app Preferences tab is removed:** `PreferencesView`,
  `TabContent::Kind::Preferences`, and `openPreferences()`. *Restore defaults*
  becomes "delete the file", which is documented in the file's guide (§2.6).

### 2.2 File format and keys

Use **strict JSON**, read with `QJsonDocument`. Qt has no TOML parser, and a
second format would need a new dependency. JSON has no comments, so §2.6
documents the keys instead. Keys are **flat and dotted**, as in VS Code, so
a key in the docs is exactly what you type.

| Key | Type | Default | Replaces |
|---|---|---|---|
| `turmeric.path` | string | `""` (auto-detect) | QSettings `repl/turBinary` |
| `lsp.enabled` | bool | `true` | `lsp/enabled` |
| `lsp.serverPath` | string | `""` | `lsp/serverPath` |
| `editor.font.family` | string | the platform default monospace font used today | `editorFont` |
| `editor.font.size` | number | `12` | `editorFont` |
| `editor.rainbowBrackets` | bool | current `rainbowBracketsDefault()` | `editor/rainbowBrackets` |
| `editor.bracketPairGuides` | bool | current `bracketPairGuidesDefault()` | `editor/bracketPairGuides` |
| `editor.wrap.prose` | bool | `true` | new (phase 4) |
| `editor.wrap.code` | bool | `false` | new (phase 4) |
| `editor.rememberDocumentState` | bool | `true` | new (phase 6) |

**What stays in QSettings** is everything that is *state*, not a *setting*:
window geometry, the splitter, the session, `recentFiles`, `lastOpenDir`, find
history (phase 3), and the zoom level. None of that is meant to be edited by
hand.

When Trowel creates the file, it writes `{}` plus nothing else. **It does not
write every key with its default.** A file full of defaults pins them, so a
later change to a default would never reach existing users. The guide in §2.6
lists every key, and *Settings…* opens the file next to it.

### 2.3 Implementation

- **`src/app/settings.{h,cpp}`**, a `Settings` singleton:
  - typed getters (`bool rainbowBrackets() const`, `QFont editorFont() const`,
    and so on), each returning the default when the key is missing or has the
    wrong type
  - `signal changed(const QStringList& keys)`, emitted after a reload with only
    the keys whose effective value changed
  - `QString path() const`, and `void ensureFileExists()` for *Settings…*
- **Loading:** read once at startup, before the first window is created, so
  the first window already has the right font and LSP setting.
- **Reloading:**
  - Use a `QFileSystemWatcher` on the file **and its directory**. Editors that
    save atomically, Trowel's own `QSaveFile` included, replace the file, which
    drops a watch that is only on the file. Re-add the file watch after every
    directory change.
  - Debounce by 200ms.
  - Also reload directly from `MainWindow`'s save path whenever the saved path
    equals `Settings::path()`, so a save inside Trowel applies without waiting
    on the watcher.
- **Errors:** on a parse error or a wrong-typed value, **keep the last good
  values**. Show a status-bar message with the line and column (convert
  `QJsonParseError::offset` to line and column) or the key name. If the file
  is open in a tab, also put a diagnostic marker on that line, reusing the
  existing diagnostic indicator. Unknown keys get a warning, which catches
  typos such as `editor.rainbowBracket`.
- **Applying changes:** connect `changed` to the existing hooks:
  - `applyRainbowBrackets` and `applyBracketPairGuides` across all windows
  - the font, for every editor in every window (replaces `pickFont`'s loop)
  - `lsp.enabled` / `lsp.serverPath` → restart or stop the language server,
    the same way the Preferences checkbox does today
  - `turmeric.path` → applies at the next REPL restart, and the status bar
    says so
- **Replace every `QSettings().value(...)` read of a moved key** with the
  `Settings` getter: `preferences_view.cpp` (deleted), `main_window.cpp:84`,
  `:1366`, `:3118`, `lsp_manager.cpp:68`, and `ResolveTurBinary()`. Find any
  others with `grep -rn 'QSettings' src` before merging.
- **One-time migration:** on startup, if `settings.json` does not exist and
  QSettings holds any of the moved keys, write those values (and only those)
  into a new `settings.json`, then remove them from QSettings. Log one status
  message saying where the settings moved to.
- **The file gets JSON highlighting** for free, because `.json` already maps
  to `Language::Json`.

### 2.4 Is a config file sensible for the new options?

Yes, for every option in this plan:

- The wrap defaults and `rememberDocumentState` are set-once choices, which is
  what a config file is for.
- The per-buffer wrap toggle is a View-menu command, and its result is
  per-file *state* (phase 6), not a setting.
- Clearing per-file state is a command (phase 6), not a setting.

None of the six phases needs a checkbox UI.

### 2.5 Tests

- `conftest.py` sets `TROWEL_CONFIG_DIR` to a per-test directory and writes
  `{"turmeric.path": ...}` there, replacing the INI write at `:144`. Keep
  `TROWEL_SETTINGS_DIR` for the QSettings state that remains.
- `test_settings.py`:
  - *Settings…* with no file → the file is created with `{}` and opened in a
    tab
  - write `{"editor.rainbowBrackets": false}` to the file from the test → the
    lexer theme check from `test_lexer_theme.py` sees flat bracket styles,
    with no relaunch
  - edit and save the file *inside* Trowel through `editor.*` commands → the
    change applies
  - write broken JSON → the previous values stay in effect, and the status bar
    names the line
  - an unknown key → a warning, and everything else still applies
  - migration: a QSettings INI with `editor/rainbowBrackets=false` and no
    `settings.json` → after launch, `settings.json` has
    `"editor.rainbowBrackets": false` and the INI no longer has the key
- Delete the tests that drive `PreferencesView`, if any exist
  (`grep -rn -i preferences tests`).

### 2.6 Docs

Add `docs/guides/settings.md`. It covers where the file lives on each OS,
every key with its type, default, and effect, that changes apply on save, that
deleting the file restores the defaults, and what is deliberately *not* in it
(window and session state).

---

## Phase 3 — Find and Replace

### 3.1 Behaviour

- **A find bar, not a dialog.** It sits across the bottom of the editor pane,
  inside `EditorView`'s layout as a sibling below `sci_`, which is the same
  structure the minimap plan uses for its column. Each tab has its own bar
  and keeps its own open/closed state.
- **Two rows:** *Find* is always shown. *Replace* is shown in replace mode
  (Find and Replace…), or when you click the disclosure arrow.
- **Find row:**
  - the query field
  - toggles for **Match Case**, **Whole Word**, **Regex**, and **In Selection**
  - a match count, "3 of 17"
  - Previous and Next buttons, and Close
- **Replace row:** the replacement field, **Replace** (replaces the current
  match and moves to the next), and **Replace All**.
- **Searching as you type:** the first match after the caret is selected and
  revealed after a 100ms debounce. All matches are highlighted.
- **Keys inside the bar:**
  - `Return` → Find Next, `Shift+Return` → Find Previous
  - in the replace field, `Return` → Replace, and `⌘Return` / `Ctrl+Alt+Return`
    → Replace All
  - `Esc` closes the bar, keeps the current match selected, and returns focus
    to the editor. If a completion or signature popup is open, `Esc` goes to
    that first.
- **Find Next / Find Previous** (`⌘G` / `F3`) work with the bar closed. They
  use the last query and do not reopen it.
- **Opening the bar fills the query:**
  - with a selection on one line → the selection becomes the query
  - with no selection → the last query, kept for the window and stored in the
    QSettings find history (20 entries, browsable with Up and Down in the
    field)
- **Use Selection for Find** (`⌘E`, macOS only) sets the query from the
  selection without opening the bar, so `⌘E ⌘G` works the way it does in every
  Mac app. Sharing queries with other apps through the macOS find pasteboard
  (`NSFindPboard`) is out of scope.
- **Select All Occurrences** turns every match into a multiple selection
  (`SCI_ADDSELECTION` for each). Scintilla's multi-selection is already on, so
  typing then edits every match.
- **Wrap-around:** searching past the end continues from the start, and the
  status bar briefly says "Wrapped". No dialog asks first.
- **Replace All** is **one undo step** (`beginEditGroup` / `endEditGroup`,
  which already exist at `editor_view.cpp:1286`). The status bar reports
  "Replaced 17". With **In Selection** on, only matches inside the selection
  range that was set when the bar opened are replaced.
- **The REPL is not searched.** Find commands are disabled while the terminal
  has focus. Searching scrollback is a separate feature.
- **Searching across files** (project-wide grep) is out of scope.
  *Find Symbol in Project* already covers definitions.

### 3.2 Engine

- Use Scintilla's target search: `setSearchFlags`, `setTargetRange`,
  `searchInTarget`.
- **Flags:** `SCFIND_MATCHCASE`, `SCFIND_WHOLEWORD`, and for Regex
  `SCFIND_REGEXP | SCFIND_CXX11REGEX`. ECMAScript syntax via `std::regex`
  is available in this build (§0).
- **Replacement:** use `replaceTarget` for plain text. For Regex use
  `replaceTargetRE`, so `\1`…`\9` refer to capture groups.
- **Regex limits that must be documented:** `std::regex` works on bytes, so
  `.` and character classes match single UTF-8 bytes rather than characters.
  Matches never span lines. An invalid pattern turns the field red, shows the
  error as its tooltip, and leaves the old highlights in place. Write these in
  the guide rather than working around them.
- **Highlighting every match:** use a new indicator, `find::kMatchIndicator`
  (`INDIC_ROUNDBOX`, alpha fill, colour from the theme), plus a stronger
  indicator for the current match.
  - Count across the whole document but **cap at 10 000 matches**, shown as
    "10 000+". Highlight only within the visible range plus one screen above
    and below, and refresh on scroll (`updateUi`), so a one-character query on
    a large file stays fast.
  - Clear both indicators when the bar closes and on every edit that changes
    the text. Re-run the search after edits while the bar is open (debounced).
- **Choosing indicator numbers:** the diagnostics and bracket-guide code
  already use some indicators. Pick free numbers and add them to the same
  constants header so they cannot collide.
- **Revealing a match** goes through `EditorView::revealLine`. Phase 5 adds
  that helper. Until then it is a thin wrapper around `scrollRange`, so find
  does not need changing again when folding lands.

### 3.3 Code shape

- Add `src/editor/find_bar.{h,cpp}` (the widget) and
  `src/editor/find_engine.{h,cpp}`. The engine takes a `ScintillaEdit*` and a
  `FindQuery {text, matchCase, wholeWord, regex, inSelection, range}`, and
  returns `{matches, currentIndex, error}`. The engine has no widgets, so the
  control API can call it directly.
- `EditorView` owns the bar and exposes `showFind(bool replace)`,
  `findNext()`, `findPrevious()`, and `useSelectionForFind()`. `MainWindow`'s
  Find actions call these on the active editor.

### 3.4 Tests

- Control commands:
  - `find.state` → `{open, query, flags, count, current, error}`
  - `find.set {query, flags}`, so tests do not depend on typing into the field
  - `find.replace {replacement, all}`
- `test_find.py`:
  - query `define` on a fixture with 3 matches → count 3, the first match after
    the caret is selected; Find Next three times wraps back to the first
  - Match Case and Whole Word each change the count as expected
  - regex `\(def (\w+)` with replacement `(defn \1` and Replace All → the
    text is correct, and **one** `Edit > Undo` restores the original
  - In Selection → only matches in range are replaced
  - an invalid regex → `error` is set, and the text is unchanged
  - open with a one-line selection → the query equals the selection
  - Select All Occurrences → the selection count equals the match count
  - Find commands are disabled while the REPL has focus
- Once phase 5 lands: a match inside a folded region → the fold opens.

---

## Phase 4 — Word wrap

### 4.1 Behaviour

- **Prose wraps by default; code does not.** "Prose" means Markdown and a new
  `Language::PlainText` (§4.2).
- Wrapping is **at the window edge**, on word boundaries (`SC_WRAP_WORD`).
  Wrapping at a fixed column such as 80 is deferred (§4.5).
- **View > Word Wrap** (checkable, `⌥Z` / `Alt+Z`, which is VS Code's key on all
  three platforms) toggles wrap **for the current buffer only**. Phase 6
  remembers that choice per file. Until phase 6 lands, the toggle lasts only
  as long as the tab is open.
- **Defaults** come from the `editor.wrap.prose` (default `true`) and
  `editor.wrap.code` (default `false`) keys in `settings.json` (phase 2).
- The buffer's wrap setting is resolved in this order: **per-buffer override →
  setting for the buffer's language class**. Changing either setting re-applies
  it to every buffer that has no override.
- **Fenced code blocks inside Markdown wrap along with the prose.** Scintilla
  sets wrap per view, not per range, so they cannot be excluded. This is
  accepted.
- If the language changes mid-buffer (Save As to a new extension, or a typed
  `#lang` line through `refreshLanguage()`), the default is re-resolved, but
  an explicit per-buffer override stays in effect.

### 4.2 `Language::PlainText`

- Add a no-op scanner (one gap-style run per line) and route these names to
  it: `.txt`, `.text`, `.rst`, `.adoc`, `.org`, and the extensionless names
  `LICENSE`, `COPYING`, `AUTHORS`, `NOTICE`, `COMMIT_EDITMSG`.
- **The fallback for unknown files stays Turmeric.** Extensionless Turmeric
  scripts rely on that, and shebang detection already handles the other cases.
- Rule order matters: `CMakeLists.txt` is matched by name first, so it stays
  CMake.
- The LSP and Run gating already key off "is this Turmeric", so PlainText
  buffers grey those commands out without extra work. Confirm with
  `test_eval_gating.py`.
- Add a row to `test_lexer_languages.py`.

### 4.3 Scintilla settings

| Setting | Prose | Code (when wrapped) |
|---|---|---|
| `setWrapMode` | `SC_WRAP_WORD` | `SC_WRAP_WORD` |
| `setWrapIndentMode` | `SC_WRAPINDENT_SAME` | `SC_WRAPINDENT_INDENT` |
| `setWrapVisualFlags` | none | `SC_WRAPVISUALFLAG_MARGIN` (a marker in the gutter on continuation rows) |
| `setLayoutCache` | `SC_CACHE_PAGE` | `SC_CACHE_PAGE` |

Also rebind Home and End to `SCI_VCHOMEWRAP` / `SCI_LINEENDWRAP`, so the first
press goes to the start or end of the display row and the second press goes to
the document line. Include the macOS `⌘←` / `⌘→` mappings. Line numbers appear
only on a line's first row, which is Scintilla's default.

### 4.4 Fixing the one-row-per-line assumptions

Fix these **as part of phase 4**. The minimap plan's rule applies here too:
any code that "only works because wrap is off" is a defect.

- **The bracket guide gutter bar** (`editor_view.cpp:1071-1073`) ends at
  `pointY(closerLine) + textHeight(closerLine)`, which covers only the first
  row of a wrapped closer line. Change it to
  `textHeight * wrapCount(closerLine)`.
- **Every `scrollCaret()` / `gotoLine()` jump** (`main_window.cpp:931, 2540,
  2583, 2623, 2988`; `editor_view.cpp:842`) already works on display lines, so
  wrap needs no change there. Phase 5 changes these same call sites (§5.3), so
  list them now.
- **The `grep` gate:** before merging, `grep -rn "lineFromPosition\|positionFromLine\|textHeight\|firstVisibleLine" src`
  and confirm each hit is either a document-line *buffer* operation or goes
  through a display-line API. Put the result in the PR description.
- **The minimap is not built yet.** Its plan already reserves
  `docLineForY` / `yForDocLine` for exactly this case.

### 4.5 Deferred

- **Wrap at column N.** Scintilla has no wrap-at-column mode. The usual
  workaround sets the right margin (`setMarginRight`) to
  `viewportWidth − N·charWidth` on every resize. That is doable but separate
  work. Its setting would be `editor.wrap.column` (0 = window edge).
- **Hard-wrapping text** (reflowing a paragraph with `gq`-style commands).
- **Treating Markdown links or other syntax as unbreakable** when wrapping.

### 4.6 Tests

- Add `editor.state` (or extend an existing editor-introspection command) with
  `wrap: bool` and `wrapOverride: "default"|"on"|"off"`.
- `test_word_wrap.py`:
  - opening a `.md` file → wrap is on
  - opening a `.tur` file → wrap is off
  - `menu.invoke ["View","Word Wrap"]` on the `.tur` buffer turns it on; other
    tabs do not change
  - writing `"editor.wrap.prose": false` to `settings.json` → open `.md`
    buffers without an override stop wrapping, with no relaunch
  - a long Markdown line in a narrow window → `editor.state` reports more
    display lines than document lines (use `SCI_VISIBLEFROMDOCLINE`)

---

## Phase 5 — Folding

### 5.1 How fold regions are found

`tur lsp` provides no fold ranges (§0), so `ScannerLexer::Fold()` computes
Scintilla fold levels itself. Each language uses one of three strategies.
Languages without a strategy get no fold margin and are otherwise unaffected.

| Strategy | Languages | Header line | Level source |
|---|---|---|---|
| **Bracket depth** | Turmeric, R7RS, JSON | a line whose depth at its end is greater than the lowest depth reached on that line | the bracket depth at line start, which the previous line's packed state already stores |
| **Indentation** | Turmeric-sweet, R7RS-sweet, Python, Just recipes | a non-blank line followed by a more-indented non-blank line | the indent width; blank lines get `SC_FOLDLEVELWHITEFLAG` and inherit from the next non-blank line |
| **Headings** | Markdown (plus code fences), TOML (`[table]`) | `#`…`######` outside a fence; a fence's opening line; a `[table]` line | the heading level, recovered from the previous line's stored fold level (`doc->GetLevel(line-1)`), so **no new line-state bits are needed** |

Notes:

- **Use the lowest depth on the line, not the depth at its start**, to decide
  whether a line opens a fold. A line like `) (define (g y)` closes one form
  and opens another, and should be a header. Have the scanner report
  `minBracketDepth` as a field it fills in during the scan but that is **not
  packed** into line state. The 31 bits are already full (see the comment in
  `scanner.h`).
- **Fix `turBracketDepth` first** (§0): a closer has to decrement the depth
  whether rainbow is on or not, and only the *style* should depend on
  `in.rainbow`. This is a real bug today too, because the line state with
  rainbow off is wrong for every closer. It goes in its own commit with a
  regression test in `test_bracket_guides.py` or `test_lexer_languages.py`.
- **Saturation at 127** limits fold nesting beyond that depth. That is fine.
- **Sweet-expression buffers** use both indentation and brackets. Use
  indentation as the primary signal, and treat a bracketed form spanning lines
  inside it as a bracket fold nested under the current indent level. If that is
  too complex for the first version, sweet buffers can ship with indentation
  only. Record whichever choice is made in this plan.
- **Leave out of the first version:** folding runs of `;` comments, `#| |#`
  block comments, C, CMake (`function`/`endfunction`), and Sh. Each can be added
  later as a single new strategy entry.
- **Where to compute:** Scintilla calls `Fold()` immediately after `Lex()` on
  the same range, so the same invalidation covers both. If `minBracketDepth` is
  only available while scanning, compute levels inside the `Lex()` loop, call
  `doc->SetLevel` there, and leave `Fold()` empty. Note the choice in a comment
  on `Fold()`.

### 5.2 UI and commands

- **Fold margin:** margin 2, `SC_MARGIN_SYMBOL`, mask `SC_MASK_FOLDERS`,
  about 12px, clickable. Use the "arrow" or "box tree" marker set, coloured
  from the theme (`lineNumberFg` for the marks, editor background behind them).
  Show it only for languages that have a strategy, so it does not appear and
  disappear while typing. The only time it changes is when the language itself
  changes.
- **Clicks:**
  `setAutomaticFold(SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE)`
  lets Scintilla handle margin clicks and unfold a region when an edit touches
  it. The existing `marginClicked` handler toggles breakpoints, so it must
  ignore clicks on margin 2. Today it may not check which margin was clicked.
  `⌥`-click / `Alt`-click on a fold marker folds or unfolds that region and
  everything inside it (`SCI_FOLDCHILDREN`).
- **Folded appearance:** `setFoldFlags(SC_FOLDFLAG_LINEAFTER_CONTRACTED)` draws
  a rule under a folded header, and `setDefaultFoldDisplayText(" … ")` with
  `SC_FOLDDISPLAYTEXT_BOXED` puts a boxed ellipsis after it.
- **View > Folding ▸** submenu. Fold and Unfold use VS Code's keys. The bulk
  commands use VS Code's `⌘K` / `Ctrl+K` chords, which Qt supports natively
  (§0).

  | Item | What it does | Scintilla call | macOS | Linux/Win |
  |---|---|---|---|---|
  | Fold | folds the innermost region containing the caret | `foldLine(header, CONTRACT)` | `⌥⌘[` | `Ctrl+Shift+[` |
  | Unfold | unfolds the region at the caret | `foldLine(header, EXPAND)` | `⌥⌘]` | `Ctrl+Shift+]` |
  | Toggle Fold | | `foldLine(header, TOGGLE)` | `⌘K ⌘L` | `Ctrl+K Ctrl+L` |
  | **Fold All** | folds every region at every level, so unfolding a top-level form shows its children still folded | `foldAll(CONTRACT \| CONTRACT_EVERY_LEVEL)` | `⌘K ⌘0` | `Ctrl+K Ctrl+0` |
  | **Unfold All** | unfolds everything | `foldAll(EXPAND)` | `⌘K ⌘J` | `Ctrl+K Ctrl+J` |
  | Fold Top-Level Forms | folds only base-level regions, so every top-level form shows as one line and its insides are left as they were | `foldAll(CONTRACT)` | `⌘K ⌘1` | `Ctrl+K Ctrl+1` |

  - "Header for the caret" means the line itself if it is a header, otherwise
    `SCI_GETFOLDPARENT`. If neither exists, the command does nothing.
  - **After Fold All or Fold Top-Level Forms, move the caret** to the header of
    the region it was in, so it is never left on a hidden line.
  - `⌘[` / `⌘]` are Outdent/Indent and `⇧⌘[` / `⇧⌘]` are previous/next tab,
    so `⌥⌘[` / `⌥⌘]` is the remaining combination.
  - **No bare `⌘K` / `Ctrl+K` binding may exist anywhere**, because it would
    shadow the chords. The §1.4 test enforces this. Clear REPL stays on
    `⇧⌘K` / `Ctrl+Shift+K`, which is a different key sequence and does not
    conflict. On Linux and Windows, step 4 of §1.3 keeps the chord prefix from
    taking `Ctrl+K` (kill-line) away from the REPL.

### 5.3 Where folding meets other features

Each of these needs code, not only a check:

- **Jumps must unfold their target.** Call `sci_->ensureVisibleEnforcePolicy(line)`
  before every `scrollCaret()` / `gotoLine()` listed in §4.4. That covers go to
  definition, references, diagnostic source, back/forward, outline, Go to Line,
  find matches (phase 3), the debugger's execution line
  (`editor_view.cpp:842`), breakpoint lists, and session or document-state
  restore. Put this in one helper, `EditorView::revealLine(int)`, and have
  every jump call it. Phase 3 already routes find through this helper, so this
  step fills it in.
- **Find highlights inside folded regions** are not visible. That is fine,
  because Next unfolds whichever region the match is in. The match count still
  includes hidden matches.
- **Replace All** works on hidden text too, because it edits the document, not
  the view. `SC_AUTOMATICFOLD_CHANGE` unfolds every region it touches. That is
  acceptable, and arguably correct, because it shows what changed.
- **Run Selection with no selection.** If Run Selection falls back to "the form
  at the caret", that is unaffected, because it reads document text. Check it.
- **Bracket guide overlay.** When the closer line is hidden inside a fold,
  `pointYFromPosition` returns the position of the nearest visible line. Clamp
  the bar to the header's row so it does not extend into the next region.
- **Breakpoints and diagnostics on hidden lines** are invisible while folded.
  Scintilla cannot show a child line's marker on the header. As a v1
  compromise, add a "contains" marker on the fold header when any hidden line
  has an error or a breakpoint. If that gets complicated, defer it and note
  the gap.
- **Copy and paste of a folded region** copies the hidden text too. That is
  correct, and it is Scintilla's default.
- **Word wrap and folding together:** Scintilla composes them through display
  lines, so there is nothing extra to do once §4.4 is done.

### 5.4 Tests

- Add `editor.folds` (returns `[{line, level, header, expanded}]`) and
  `editor.fold {line, action: "contract"|"expand"|"toggle"}`. Fold All,
  Unfold All, and Fold Top-Level Forms go through `menu.invoke`.
- `test_folding.py`:
  - a fixture `.tur` file with three top-level `define`s, one containing a
    nested `let` → three base-level headers plus one nested header
  - **Fold All** → every header reports `expanded: false`, including the
    nested one; then unfold the outer one → the nested one is still folded
  - **Unfold All** → every header is expanded
  - **Fold Top-Level Forms** → the three base-level headers are folded; the
    nested header keeps whatever state it had before
  - after Fold All with the caret inside a body → the caret is on that body's
    header
  - Go to Definition into a folded body → it is expanded
  - a find match inside a folded body → Find Next expands it
  - typing inside a folded region → it expands (automatic fold)
  - rainbow off → levels are identical to rainbow on (the regression from §5.1)
  - Markdown: a `##` heading under a `#` heading nests; a `#` inside a code
    fence is not a header
  - Python indentation folding, with blank lines inside a block
- Add a fuzz assertion to `test_editor_fuzz.py`: after random edits, the fold
  levels from an incremental lex equal the levels from a full re-lex. This is
  the property that catches invalidation bugs.

---

## Phase 6 — Per-document state

### 6.1 What is remembered, per file

| Field | Stored as | Why |
|---|---|---|
| Caret and anchor (the main selection) | `{line, column}` pairs | Line and column survive small external edits better than byte offsets, and they are clamped on restore |
| Extra selections (multi-cursor) | not stored | Rarely useful after closing a file |
| Scroll | the document line at the top of the view, plus the horizontal offset | A document line stays stable if wrap or the font changes; a display line does not |
| Folded regions | a list of collapsed header lines | §6.3 says when they are applied |
| Wrap override | `"on"`/`"off"`, or absent for "use the default" | Phase 4's per-buffer toggle |
| Validation | file `size` and `mtime` at the time of saving | Lets restore detect that the file changed outside Trowel |
| `lastSeen` | a timestamp | For pruning old entries |

Breakpoints are already saved per path in the session, and the dialect comes
from the `#lang` line, so neither belongs here. Find bar state is not saved
per file.

### 6.2 Where it is stored

- **Use a JSON file**, `QStandardPaths::AppDataLocation/document-state.json`,
  keyed by the canonical absolute path (`QFileInfo::canonicalFilePath`).
  - **Do not use QSettings.** On macOS QSettings is a plist that is rewritten
    in full on every write, and hundreds of path-keyed entries would bloat the
    file that holds window state.
  - **Do not use the config directory.** This is machine-written state, not
    settings, and it should not sit next to `settings.json`, where users
    (and dotfile repos) expect only hand-edited files.
- **Keep at most 500 entries.** When saving, drop the entries with the oldest
  `lastSeen`.
- **Write atomically** with `QSaveFile`. The file is shared by every window in
  the one running instance (single-instance is enforced), so no locking is
  needed.
- Own it in a small `DocumentStateStore` (`src/app/document_state.{h,cpp}`)
  with these methods:
  - `std::optional<DocState> lookup(path)`
  - `void remember(path, DocState)`
  - `void forget(path)`
  - `void clear()`

  The store loads lazily on first use and saves with a 2-second debounce.

### 6.3 When state is saved and restored

**Saving.** Call `remember()` on:

- tab close
- window close or quit (the same `closeEvent` path that already saves the
  session)
- successful save
- tab deactivation (so a crash loses at most the active tab's state)

Do **not** save on every caret move.

**Restoring** happens in `openPath()` after `loadFile()` succeeds:

1. **If the open request carries an explicit position, skip the restore.** That
   covers Go to Definition, a diagnostic or reference jump, a breakpoint jump,
   and a CLI `file:line` if one exists. Check `openPath` callers. The explicit
   target always wins.
2. Set the wrap override first, because it changes layout and therefore what
   "scroll to line N" means.
3. Clamp the caret and anchor to the document, set them, then
   `setFirstVisibleLine(visibleFromDocLine(topLine))` and the horizontal offset.
4. Restore folds **only if `size` and `mtime` match**. If the file changed
   outside Trowel, the old header lines may now point at unrelated code, so
   drop the folds and keep only the clamped caret and scroll. Before applying
   folds, run `sci_->colourise(0, positionFromLine(maxFoldLine + 1))` so the
   fold levels exist, because lexing is lazy. Skip any line that is not a
   header.
5. Through `revealLine` (§5.3), make sure the caret is not inside a folded
   region.

**Skipped entirely:** untitled buffers, read-only stdlib tabs (for the reason
the session already skips them), and directory tabs. `settings.json` itself is
an ordinary file here and gets its caret remembered like any other.

### 6.4 Settings and privacy

- `editor.rememberDocumentState` in `settings.json` (default `true`). Setting
  it to `false` stops reads and writes but does not delete the file.
- **File > Open Recent > Clear Menu** also clears document state. The user
  expectation behind both is "forget which files I had open". There is no
  separate button for clearing only per-file state. If one is needed later,
  it would be a command, not a setting.
- The paths stored here are the same kind of data `recentFiles` already keeps,
  so there is no new exposure.

### 6.5 Tests

- `test_document_state.py`. Each test quits through File > Quit and relaunches,
  the same way `test_session_restore.py` does:
  - put the caret on line 40 with a selection, quit, relaunch → the caret and
    selection come back; `firstVisibleLine` is at most 40 and 40 is on screen
  - close the tab, reopen it with `editor.open` (no explicit position) → the
    state comes back without a relaunch
  - Go to Definition into a file with saved state → the definition target wins
  - Fold All, quit, relaunch → the same headers are folded
  - fold a region, quit, append a line to the file from the test, relaunch →
    the caret is restored (clamped) and the folds are **not**
  - turn wrap on for a `.tur` buffer, quit, relaunch → it is still wrapped; a
    different `.tur` file is not
  - `"editor.rememberDocumentState": false` → nothing is restored, and
    `document-state.json` does not change
  - Clear Menu → `document-state.json` is empty
- Unit-style: a 501st entry drops the oldest one.

---

## Order of work and commit boundaries

1. `fix(lexer): bracket depth decrements with rainbow off`. Independent; land
   it first.
2. **Phase 1**, as two or three commits: the shortcut table and roles (fixing
   F1–F5, F10, and F13), then the Edit menu, routing, and Go to Line, then the
   new Go/Help menus and moved items along with their test path updates. F12
   (terminal control keys) gets its own commit because it changes behaviour.
3. **Phase 2:** first `Settings` with migration and the reloading watcher, all
   reads switched over, and the conftest change. Then *Settings…* opening the
   file, with `PreferencesView` deleted, Font… removed, and
   `docs/guides/settings.md` added.
4. **Phase 3:** first the engine and control commands with tests, then the
   find bar UI, then Replace, Select All Occurrences, and Use Selection for
   Find.
5. **Phase 4**, starting with `Language::PlainText` in its own commit.
6. **Phase 5:** levels and margin first, then the Folding menu (including Fold
   All / Unfold All), then the `revealLine` sweep.
7. **Phase 6.**

Each step updates the **Status** line at the top of this file in the same
commit, and updates `keyboard-shortcuts.md` whenever a key changes.

## Open questions

None at the moment. The earlier four were settled on 2026-10-04:

- Settings are a config file, opened by *Settings…* (phase 2).
- Settings… sits in Edit on both Linux and Windows.
- Font… is removed; the font is a settings key.
- Fold All / Unfold All and Find / Replace are part of this plan (phases 5
  and 3).
