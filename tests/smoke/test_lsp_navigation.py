"""Navigation — go-to-definition, read-only stdlib tabs, Back/Forward.

Covers T1 of docs/plans/lsp-navigation.md. Every wait goes through a `wait.*`
command or a handler-side timeout; no test sleeps. See docs/plans/smoke-tests.md.

The fixture is `symbols.tur`, which must analyze cleanly: one rejected form
empties documentSymbol and nulls every definition reply for the whole file
(navigation plan §4.2.1), so a fixture regression would look like a navigation
regression.
"""

from pathlib import Path

import pytest

from trowel_ctl import ControlError

# The first request pays for process spawn + initialize + a full compile.
WAIT_MS = 8000

GOTO = ["Go", "Go to Definition"]
BACK = ["Go", "Go Back"]
FORWARD = ["Go", "Go Forward"]


def _require_server(trowel) -> dict:
    status = trowel.call("lsp.status")
    if not status["enabled"]:
        pytest.skip("language server disabled in settings")
    if not status["server_path"]:
        pytest.skip("no `tur` binary available to run `tur lsp`")
    return status


def _open_and_analyze(trowel, path: Path):
    """Open `path` and block until the server publishes a batch for it."""
    trowel.call("editor.open", {"path": str(path)})
    trowel.call("wait.diagnostics", {"min_count": 0, "timeout_ms": WAIT_MS})


def _offset_of(trowel, needle: str, occurrence: int = 0) -> int:
    """Byte offset of the nth `needle` in the active buffer.

    Byte, not character: editor.set_cursor speaks Scintilla positions, and the
    fixture is ASCII only so the two coincide — but slicing before encoding is
    what keeps that true if the fixture ever grows a non-ASCII comment.
    """
    text = trowel.call("editor.get_text")["text"]
    idx = -1
    for _ in range(occurrence + 1):
        idx = text.index(needle, idx + 1)
    return len(text[:idx].encode("utf-8"))


def _caret_at(trowel, needle: str, occurrence: int = 0) -> int:
    pos = _offset_of(trowel, needle, occurrence)
    trowel.call("editor.set_cursor", {"pos": pos})
    return pos


def _active_path(trowel) -> str:
    return trowel.call("editor.get_text")["path"]


def _tabs(trowel) -> list:
    return trowel.call("window.list")["windows"][0]["tabs"]


# --- the request itself ----------------------------------------------------


def test_definition_resolves_a_local_symbol(trowel, fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    # Caret on the *use* of nav-total inside nav-use.
    _caret_at(trowel, "nav-total", 3)
    r = trowel.call("lsp.definition", {"timeout_ms": WAIT_MS})

    loc = r["location"]
    assert loc is not None, "definition came back null for a symbol defined in this file"
    assert loc["path"] == str(fixture_files / "symbols.tur")
    # `(def nav-total 10)` — the name starts at character 5.
    assert loc["character"] == 5


def test_definition_reports_no_answer_rather_than_going_silent(trowel, fixture_files: Path):
    """A name the server has never seen must come back as an explicit null.

    The manager reports this rather than dropping the callback, so the window
    can say "no definition found" instead of leaving the jump looking pending.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    _caret_at(trowel, "in this comment")
    r = trowel.call("lsp.definition", {"timeout_ms": WAIT_MS})
    assert r["location"] is None


# --- where the jump lands --------------------------------------------------


def test_definition_within_one_file_moves_the_caret(trowel, fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")
    before = _tabs(trowel)

    origin = _caret_at(trowel, "nav-total", 3)
    trowel.call("nav.goto_definition", {"timeout_ms": WAIT_MS})

    caret = trowel.call("editor.get_cursor")["pos"]
    assert caret == _offset_of(trowel, "nav-total", 0)
    assert caret != origin
    # A same-file jump must not churn the tab set.
    assert _tabs(trowel) == before


def test_definition_into_the_stdlib_opens_a_read_only_tab(trowel, fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    _caret_at(trowel, "list-head")
    trowel.call("nav.goto_definition", {"timeout_ms": WAIT_MS})

    landed = _active_path(trowel)
    assert landed.endswith("list.tur"), landed
    assert "stdlib" in landed
    assert trowel.call("editor.is_read_only")["read_only"] is True


def test_a_read_only_buffer_rejects_edits(trowel, fixture_files: Path):
    """The lock is real, not just a label: typing into the tab does nothing."""
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    _caret_at(trowel, "list-head")
    trowel.call("nav.goto_definition", {"timeout_ms": WAIT_MS})

    before = trowel.call("editor.get_text")["text"]
    trowel.type("zzz")
    assert trowel.call("editor.get_text")["text"] == before


def test_stdlib_tab_is_absent_from_recent_files(trowel, fixture_files: Path):
    """"Open Recent > list.tur" landing in an unwritable bundle file is a puzzle."""
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    _caret_at(trowel, "list-head")
    trowel.call("nav.goto_definition", {"timeout_ms": WAIT_MS})
    assert _active_path(trowel).endswith("list.tur")

    with pytest.raises(ControlError) as ei:
        trowel.call("menu.invoke", {"path": ["File", "Open Recent", "list.tur"]})
    assert ei.value.code == "no_action"
    # The workspace file that *was* opened normally is still there, so this is
    # not passing merely because the recent menu is empty.
    trowel.call("menu.invoke", {"path": ["File", "Open Recent", "symbols.tur"]})


def test_non_turmeric_buffer_disables_the_action(trowel, fixture_files: Path):
    """Nine languages are highlighted; one has a language server.

    F12 doing nothing in a .py file is a bug report, so the action is greyed.
    """
    _require_server(trowel)
    trowel.call("editor.open", {"path": str(fixture_files / "sample.py")})

    with pytest.raises(ControlError) as ei:
        trowel.call("menu.invoke", {"path": GOTO})
    assert ei.value.code == "action_disabled"


# --- navigation history ----------------------------------------------------


def test_back_returns_to_the_origin_and_forward_returns_to_the_target(
    trowel, fixture_files: Path
):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    origin = _caret_at(trowel, "list-head")
    trowel.call("nav.goto_definition", {"timeout_ms": WAIT_MS})
    target_path = _active_path(trowel)
    target_pos = trowel.call("editor.get_cursor")["pos"]

    hist = trowel.call("nav.history")
    assert len(hist["back"]) == 1
    assert hist["back"][0]["path"] == str(fixture_files / "symbols.tur")
    assert hist["back"][0]["pos"] == origin

    trowel.call("menu.invoke", {"path": BACK})
    assert _active_path(trowel) == str(fixture_files / "symbols.tur")
    assert trowel.call("editor.get_cursor")["pos"] == origin
    assert trowel.call("nav.history")["back"] == []

    trowel.call("menu.invoke", {"path": FORWARD})
    assert _active_path(trowel) == target_path
    assert trowel.call("editor.get_cursor")["pos"] == target_pos


def test_a_new_jump_drops_the_forward_stack(trowel, fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    _caret_at(trowel, "nav-total", 3)
    trowel.call("nav.goto_definition", {"timeout_ms": WAIT_MS})
    trowel.call("menu.invoke", {"path": BACK})
    assert trowel.call("nav.history")["forward"], "Back should have filled Forward"

    _caret_at(trowel, "nav-double", 1)
    trowel.call("nav.goto_definition", {"timeout_ms": WAIT_MS})
    assert trowel.call("nav.history")["forward"] == []


def test_back_with_an_empty_stack_is_a_no_op(trowel, fixture_files: Path):
    """Nothing to go back to is a greyed menu item, not an error dialog."""
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    assert trowel.call("nav.history") == {"back": [], "forward": []}
    # A disabled action *is* the no-op: menu.invoke refuses to trigger it, and
    # the editor is untouched either way.
    before = trowel.call("editor.get_cursor")["pos"]
    with pytest.raises(ControlError) as ei:
        trowel.call("menu.invoke", {"path": BACK})
    assert ei.value.code == "action_disabled"
    assert trowel.call("editor.get_cursor")["pos"] == before


def test_history_reopens_a_file_that_was_closed(trowel, fixture_files: Path):
    """Entries are paths and offsets, never EditorView pointers (§5.4).

    Closing the tab an entry names destroys the EditorView a pointer-based
    stack would have held. The entry must survive that and reopen the file —
    still read-only, since the reopen goes through the same load path.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    _caret_at(trowel, "list-head")
    trowel.call("nav.goto_definition", {"timeout_ms": WAIT_MS})
    target_pos = trowel.call("editor.get_cursor")["pos"]

    # Back leaves the stdlib tab open but no longer active, with Forward
    # pointing at it.
    trowel.call("menu.invoke", {"path": BACK})
    assert _active_path(trowel) == str(fixture_files / "symbols.tur")

    # Close it out from under the Forward entry.
    trowel.call("menu.invoke", {"path": ["View", "Next Tab"]})
    assert _active_path(trowel).endswith("list.tur")
    trowel.call("menu.invoke", {"path": ["File", "Close Tab"]})
    assert not any(t.endswith("list.tur") for t in _tabs(trowel))

    trowel.call("menu.invoke", {"path": FORWARD})
    assert _active_path(trowel).endswith("list.tur")
    assert trowel.call("editor.get_cursor")["pos"] == target_pos
    assert trowel.call("editor.is_read_only")["read_only"] is True


# --- persistence -----------------------------------------------------------


def test_stdlib_tab_is_absent_from_the_restored_session(
    trowel_session, fixture_files: Path
):
    """A stdlib path is stable across upgrades, so restoring one would present
    last version's stdlib as current after a TROWEL_TURMERIC_VERSION bump."""
    symbols = fixture_files / "symbols.tur"

    t = trowel_session.launch([str(symbols)])
    if not t.call("lsp.status")["server_path"]:
        pytest.skip("no `tur` binary available to run `tur lsp`")
    t.call("wait.diagnostics", {"min_count": 0, "timeout_ms": WAIT_MS})

    _caret_at(t, "list-head")
    t.call("nav.goto_definition", {"timeout_ms": WAIT_MS})
    assert _active_path(t).endswith("list.tur")
    trowel_session.quit(t)

    t2 = trowel_session.launch()
    tabs = [tab for w in t2.call("window.list")["windows"] for tab in w["tabs"]]
    assert str(symbols) in tabs
    assert not any(tab.endswith("list.tur") for tab in tabs), tabs


# --- the outline (T2) ------------------------------------------------------

OUTLINE = ["Go", "Show Symbols"]


def _names(symbols) -> list:
    return [s["name"] for s in symbols]


def test_outline_lists_definitions_in_document_order(trowel, fixture_files: Path):
    """Document order is the one thing an outline is for.

    Alphabetising would sort nav-total after nav-stdlib and NavPoint first;
    asserting the exact sequence is what catches a stray sort().
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    r = trowel.call("lsp.symbols", {"timeout_ms": WAIT_MS})
    assert r["reason"] == ""
    assert _names(r["symbols"]) == [
        "nav-total",
        "nav-double",
        "NavPoint",
        "nav-note",
        "nav-use",
        "nav-use-again",
        "nav-stdlib",
    ]
    # Strictly increasing lines — the same claim from the other direction.
    lines = [s["line"] for s in r["symbols"]]
    assert lines == sorted(lines)


def test_outline_kinds_are_meaningful(trowel, fixture_files: Path):
    """Before the v0.42.0 bump every non-function came back Variable.

    A kind column that says "value" for a struct is worse than no column, so
    this pins that the distinction actually survives to the UI.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    by_name = {s["name"]: s for s in trowel.call("lsp.symbols", {"timeout_ms": WAIT_MS})["symbols"]}
    assert by_name["nav-double"]["kind_label"] == "function"
    assert by_name["nav-total"]["kind_label"] == "value"
    assert by_name["NavPoint"]["kind_label"] == "type"


def test_outline_omits_macros(trowel, fixture_files: Path):
    """A measured upstream gap, pinned so it is noticed when it closes.

    `defmacro` produces no documentSymbol entry at all on v0.42.0 (navigation
    plan §4.2.1) — not a wrong kind, an absent row. When this starts failing,
    the server grew macro symbols and §6.1's `macro` kind label becomes
    reachable.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    names = _names(trowel.call("lsp.symbols", {"timeout_ms": WAIT_MS})["symbols"])
    assert "nav-twice" not in names
    assert "nav-twice" in trowel.call("editor.get_text")["text"]


def test_current_marks_the_symbol_the_caret_is_on(trowel, fixture_files: Path):
    """Caret literally on a name: that name wins outright."""
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    _caret_at(trowel, "nav-double", 0)
    symbols = trowel.call("lsp.symbols", {"timeout_ms": WAIT_MS})["symbols"]
    current = [s["name"] for s in symbols if s["current"]]
    assert current == ["nav-double"]


def test_current_marks_the_enclosing_symbol_from_inside_a_body(
    trowel, fixture_files: Path
):
    """Caret in a body, not on a name: the smallest containing range wins.

    Someone editing the middle of a function is not sitting on its name, and
    an outline that highlights nothing there answers "where am I" with silence.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    # Inside nav-use's body, on the call to nav-double.
    _caret_at(trowel, "(nav-double nav-total)")
    symbols = trowel.call("lsp.symbols", {"timeout_ms": WAIT_MS})["symbols"]
    current = [s["name"] for s in symbols if s["current"]]
    assert current == ["nav-use"]


def test_empty_file_and_broken_file_are_different_states(trowel, fixture_files: Path):
    """The distinction §4.2.1 forced into the design.

    One bad form empties documentSymbol for the whole file, so "no symbols" is
    usually "did not compile" rather than "defines nothing". Saying the latter
    when the former is true sends the user looking in the wrong place.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "hello.tur")

    trowel.call("editor.set_text", {"text": ";; a comment and nothing else\n"})
    trowel.call("wait.diagnostics", {"min_count": 0, "max_count": 0, "timeout_ms": WAIT_MS})
    empty = trowel.call("lsp.symbols", {"timeout_ms": WAIT_MS})
    assert empty["count"] == 0
    assert empty["reason"] == "Nothing defined yet"

    trowel.call("editor.set_text", {"text": "(def broken (\n"})
    trowel.call("wait.diagnostics", {"min_count": 1, "timeout_ms": WAIT_MS})
    broken = trowel.call("lsp.symbols", {"timeout_ms": WAIT_MS})
    assert broken["count"] == 0
    assert broken["reason"] == "Not analyzed — fix errors first"


def test_outline_on_an_unsaved_buffer_reports_why(trowel):
    """No path means no URI means the server has never seen the buffer."""
    _require_server(trowel)
    trowel.type("(def unsaved 1)")

    r = trowel.call("lsp.symbols", {"timeout_ms": WAIT_MS})
    assert r["count"] == 0
    assert r["reason"]
    # Same wording completion and documentation already use for this case.
    assert "save" in r["reason"].lower() or "unsaved" in r["reason"].lower()


def test_outline_action_is_disabled_in_a_non_turmeric_buffer(trowel, fixture_files: Path):
    _require_server(trowel)
    trowel.call("editor.open", {"path": str(fixture_files / "sample.py")})

    with pytest.raises(ControlError) as ei:
        trowel.call("menu.invoke", {"path": OUTLINE})
    assert ei.value.code == "action_disabled"


# --- occurrence highlight (T3) ---------------------------------------------


def test_highlights_cover_every_real_use(trowel, fixture_files: Path):
    """nav-total is defined once and used twice: three ranges, no more."""
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    _caret_at(trowel, "nav-total", 0)
    r = trowel.call("lsp.highlights", {"timeout_ms": WAIT_MS})
    assert r["count"] == 3, r["ranges"]

    text = trowel.call("editor.get_text")["text"]
    for rng in r["ranges"]:
        assert text.encode("utf-8")[rng["start"]:rng["end"]] == b"nav-total"


def test_highlights_skip_comments_and_strings(trowel, fixture_files: Path):
    """The whole point of the phase, and the one assertion that would catch a
    regression back to textual matching.

    `nav-total` also appears in a comment and inside a string literal. Word
    matching would return five ranges and paint two of them wrong; the server's
    token-based answer returns three.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    text = trowel.call("editor.get_text")["text"]
    comment_at = _offset_of(trowel, "nav-total appears in this comment")
    string_at = _offset_of(trowel, "nav-total in a string")
    # The fixture really does contain the decoys this test is about.
    assert text.count("nav-total") == 5

    _caret_at(trowel, "nav-total", 0)
    starts = {r["start"] for r in trowel.call("lsp.highlights", {"timeout_ms": WAIT_MS})["ranges"]}
    assert comment_at not in starts
    assert string_at not in starts


def test_highlights_are_painted_with_indicator_ten(trowel, fixture_files: Path):
    """Asserting on the model does not prove a decoration was painted.

    lsp.decorations reads indicator 10 back out of Scintilla, so this fails if
    setOccurrences stops painting even while the ranges are still correct.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    _caret_at(trowel, "nav-total", 0)
    r = trowel.call("lsp.highlights", {"timeout_ms": WAIT_MS})
    painted = trowel.call("lsp.decorations")["occurrence_ranges"]

    assert painted
    assert [(p["start"], p["end"]) for p in painted] == \
           sorted((x["start"], x["end"]) for x in r["ranges"])


def test_highlights_do_not_read_as_diagnostics(trowel, fixture_files: Path):
    """An occurrence is not a problem; it must not land in a squiggle slot."""
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    _caret_at(trowel, "nav-total", 0)
    trowel.call("lsp.highlights", {"timeout_ms": WAIT_MS})

    d = trowel.call("lsp.decorations")
    assert d["occurrence_ranges"]
    assert d["error_ranges"] == []
    assert d["warning_ranges"] == []


def test_highlights_clear_when_the_caret_leaves(trowel, fixture_files: Path):
    """The previous set is dropped on caret movement, not on the next reply.

    Otherwise the highlight lags the caret by a whole round trip and briefly
    marks a symbol the user has already left.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    _caret_at(trowel, "nav-total", 0)
    trowel.call("lsp.highlights", {"timeout_ms": WAIT_MS})
    assert trowel.call("lsp.decorations")["occurrence_ranges"]

    # Park the caret in the leading comment, where nothing is defined.
    _caret_at(trowel, "Fixture for the navigation")
    assert trowel.call("lsp.decorations")["occurrence_ranges"] == []
