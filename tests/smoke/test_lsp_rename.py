"""Scope-aware highlight, references and rename — track R of editor-intelligence.md.

The shadowing tests are the ones that matter. They are cheap to write and they
are the only thing standing between "rename works" and "rename quietly
corrupted a file".

Nothing here saves. Renames are applied to *buffers* and left dirty, which is
both the designed behaviour (§3.4) and what keeps the fixtures on disk pristine
across a run.
"""

from pathlib import Path

import pytest

from trowel_ctl import ControlError

WAIT_MS = 8000
# prepareRename and rename compile every importing file, so they get the
# generous budget the handler defaults to rather than the interactive one.
RENAME_MS = 12000

REFERENCES = ["Go", "Find References"]
RENAME = ["Edit", "Rename Symbol…"]


def _require_server(trowel):
    status = trowel.call("lsp.status")
    if not status["enabled"]:
        pytest.skip("language server disabled in settings")
    if not status["server_path"]:
        pytest.skip("no `tur` binary available to run `tur lsp`")
    return status


def _open_and_analyze(trowel, path: Path):
    trowel.call("editor.open", {"path": str(path)})
    trowel.call("wait.diagnostics", {"min_count": 0, "timeout_ms": WAIT_MS})


def _offset_of(trowel, needle: str, occurrence: int = 0) -> int:
    text = trowel.call("editor.get_text")["text"]
    idx = -1
    for _ in range(occurrence + 1):
        idx = text.index(needle, idx + 1)
    return len(text[:idx].encode("utf-8"))


def _caret_at(trowel, needle: str, occurrence: int = 0) -> int:
    pos = _offset_of(trowel, needle, occurrence)
    trowel.call("editor.set_cursor", {"pos": pos})
    return pos


def _lines(trowel) -> list:
    return trowel.call("editor.get_text")["text"].split("\n")


# `scopes.tur` binds `total` four ways; these name the lines each one owns.
GLOBAL_DEF_LINE = 9
GLOBAL_USE_LINE = 15
LET_BIND_LINE = 12
LET_USE_LINE = 13
PARAM_LINE = 17
COMMENT_LINE = 19
STRING_LINE = 20


# --- scope-aware highlight -------------------------------------------------


def test_highlight_of_a_parameter_stops_at_its_function(trowel, fixture_files: Path):
    """A parameter's marks are its function's, not the file's.

    Before the scope pass this was textual and painted every `total` in the
    file — the failure the whole track exists to prevent.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")

    # Caret on the `total` parameter of param-fn.
    _caret_at(trowel, "(defn param-fn [total")
    _caret_at(trowel, "total", 0)  # re-anchor: find the param occurrence below
    pos = _offset_of(trowel, "[total : int]") + 1
    trowel.call("editor.set_cursor", {"pos": pos})

    r = trowel.call("lsp.highlights", {"timeout_ms": WAIT_MS})
    lines = sorted({h["start_line"] for h in r["ranges"]})
    assert lines == [PARAM_LINE], r["ranges"]


def test_highlight_of_the_global_skips_shadowed_regions(trowel, fixture_files: Path):
    """Both halves of the scope rule, in one assertion.

    The global's marks must reach its real use and must *not* reach the `let`
    binding, the parameter, the comment or the string — five candidates, two
    correct answers.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")

    _caret_at(trowel, "(def total 100)")
    trowel.call("editor.set_cursor", {"pos": _offset_of(trowel, "(def total 100)") + 6})

    r = trowel.call("lsp.highlights", {"timeout_ms": WAIT_MS})
    lines = sorted({h["start_line"] for h in r["ranges"]})
    assert lines == [GLOBAL_DEF_LINE, GLOBAL_USE_LINE], r["ranges"]
    for shadowed in (LET_BIND_LINE, LET_USE_LINE, PARAM_LINE, COMMENT_LINE, STRING_LINE):
        assert shadowed not in lines


# --- references ------------------------------------------------------------


def test_references_include_the_declaration(trowel, fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")
    trowel.call("editor.set_cursor", {"pos": _offset_of(trowel, "(def total 100)") + 6})

    r = trowel.call("lsp.references", {"timeout_ms": WAIT_MS})
    assert r["reason"] == ""
    lines = sorted(ref["line"] for ref in r["references"])
    assert GLOBAL_DEF_LINE in lines, "includeDeclaration should put the def in the list"
    assert lines == [GLOBAL_DEF_LINE, GLOBAL_USE_LINE]


def test_references_of_a_let_binding_are_its_scope(trowel, fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")
    trowel.call("editor.set_cursor", {"pos": _offset_of(trowel, "[total (* n 2)]") + 1})

    r = trowel.call("lsp.references", {"timeout_ms": WAIT_MS})
    lines = sorted(ref["line"] for ref in r["references"])
    assert lines == [LET_BIND_LINE, LET_USE_LINE]


def test_references_action_is_disabled_in_a_non_turmeric_buffer(trowel, fixture_files: Path):
    _require_server(trowel)
    trowel.call("editor.open", {"path": str(fixture_files / "sample.py")})
    with pytest.raises(ControlError) as ei:
        trowel.call("menu.invoke", {"path": REFERENCES})
    assert ei.value.code == "action_disabled"


# --- prepareRename ---------------------------------------------------------


def test_prepare_rename_offers_the_current_name(trowel, fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")
    trowel.call("editor.set_cursor", {"pos": _offset_of(trowel, "(def total 100)") + 6})

    r = trowel.call("lsp.prepare_rename", {"timeout_ms": RENAME_MS})
    assert r["renameable"] is True
    assert r["refusal"] == ""
    # The placeholder is what pre-fills the input, so it has to be the name.
    assert r["placeholder"] == "total"
    assert r["line"] == GLOBAL_DEF_LINE


def test_prepare_rename_refuses_a_stdlib_symbol_in_the_servers_own_words(
    trowel, fixture_files: Path
):
    """The refusal arrives as a JSON-RPC error and must survive intact.

    "cannot rename stdlib symbol" tells the user why and implies what to do
    instead; a paraphrase like "rename failed" throws that away.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")
    trowel.call("editor.set_cursor", {"pos": _offset_of(trowel, "list-head") + 2})

    r = trowel.call("lsp.prepare_rename", {"timeout_ms": RENAME_MS})
    assert r["renameable"] is False
    assert r["refusal"] == "cannot rename stdlib symbol"


def test_prepare_rename_ignores_comment_and_string_context(trowel, fixture_files: Path):
    """Pins a measured upstream quirk (§3.1.1).

    prepareRename resolves the *word* at the position against the symbol table
    without consulting lexical context, so it says "yes, renameable" for
    `total` sitting in a comment or a string. Only the preview range is wrong —
    the rename that follows correctly targets the global and leaves both decoys
    alone, which the two tests below assert. If upstream teaches prepareRename
    about context, this test is where that shows up.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")

    for needle in ("total in a comment", "total in a string"):
        trowel.call("editor.set_cursor", {"pos": _offset_of(trowel, needle) + 2})
        r = trowel.call("lsp.prepare_rename", {"timeout_ms": RENAME_MS})
        assert r["renameable"] is True, needle
        assert r["placeholder"] == "total"


def test_prepare_rename_on_empty_space_offers_nothing(trowel, fixture_files: Path):
    """The one case that answers `null` rather than an error."""
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")
    text = trowel.call("editor.get_text")["text"]
    trowel.call("editor.set_cursor", {"pos": len(text.encode("utf-8"))})

    r = trowel.call("lsp.prepare_rename", {"timeout_ms": RENAME_MS})
    assert r["renameable"] is False


def test_rename_from_inside_a_comment_leaves_the_comment_alone(
    trowel, fixture_files: Path
):
    """The companion to the string case: a mention is not a use, and the
    rename engine knows that even though prepareRename does not."""
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")
    before = _lines(trowel)
    trowel.call("editor.set_cursor",
                {"pos": _offset_of(trowel, "total in a comment") + 2})

    r = trowel.call("lsp.rename",
                    {"new_name": "renamed", "apply": True, "timeout_ms": RENAME_MS})
    assert r["error"] == ""
    after = _lines(trowel)
    assert after[COMMENT_LINE] == before[COMMENT_LINE]
    assert "renamed" in after[GLOBAL_DEF_LINE]


# --- rename ----------------------------------------------------------------


def test_rename_a_let_binding_changes_only_its_scope(trowel, fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")
    before = _lines(trowel)
    trowel.call("editor.set_cursor", {"pos": _offset_of(trowel, "[total (* n 2)]") + 1})

    r = trowel.call("lsp.rename",
                    {"new_name": "subtotal", "apply": True, "timeout_ms": RENAME_MS})
    assert r["error"] == ""
    assert r["applied"] == 1

    after = _lines(trowel)
    assert "subtotal" in after[LET_BIND_LINE]
    assert "subtotal" in after[LET_USE_LINE]
    # Everything else is byte-identical — including the global, the parameter,
    # the comment and the string.
    for i, (old, new) in enumerate(zip(before, after)):
        if i in (LET_BIND_LINE, LET_USE_LINE):
            continue
        assert old == new, f"line {i} changed and should not have"


def test_rename_the_global_leaves_the_shadowing_bindings_alone(trowel, fixture_files: Path):
    """The destructive case. A textual rename would rewrite all five sites."""
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")
    before = _lines(trowel)
    trowel.call("editor.set_cursor", {"pos": _offset_of(trowel, "(def total 100)") + 6})

    r = trowel.call("lsp.rename",
                    {"new_name": "grand-total", "apply": True, "timeout_ms": RENAME_MS})
    assert r["error"] == ""
    assert r["applied"] == 1

    after = _lines(trowel)
    assert "grand-total" in after[GLOBAL_DEF_LINE]
    assert "grand-total" in after[GLOBAL_USE_LINE]
    # The shadowing binder, its use, the parameter, the comment and the string
    # are all untouched.
    assert after[LET_BIND_LINE] == before[LET_BIND_LINE]
    assert after[LET_USE_LINE] == before[LET_USE_LINE]
    assert after[PARAM_LINE] == before[PARAM_LINE]
    assert after[COMMENT_LINE] == before[COMMENT_LINE]
    assert after[STRING_LINE] == before[STRING_LINE]


def test_rename_from_inside_a_string_targets_the_symbol_not_the_string(
    trowel, fixture_files: Path
):
    """Pins a measured upstream quirk (§3.1.1).

    From a caret inside `"total in a string"`, prepareRename returns a range
    *inside the string* — but references, highlight and rename all correctly
    resolve to the global. The edit is right and only the preview range is
    wrong. If upstream tightens prepareRename, this test notices.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")
    before = _lines(trowel)
    trowel.call("editor.set_cursor",
                {"pos": _offset_of(trowel, "total in a string") + 2})

    r = trowel.call("lsp.rename",
                    {"new_name": "renamed", "apply": True, "timeout_ms": RENAME_MS})
    assert r["error"] == ""
    after = _lines(trowel)
    # The string literal is byte-identical; the global moved.
    assert after[STRING_LINE] == before[STRING_LINE]
    assert "renamed" in after[GLOBAL_DEF_LINE]


def test_rename_is_disabled_in_a_non_turmeric_buffer(trowel, fixture_files: Path):
    _require_server(trowel)
    trowel.call("editor.open", {"path": str(fixture_files / "sample.py")})
    with pytest.raises(ControlError) as ei:
        trowel.call("menu.invoke", {"path": RENAME})
    assert ei.value.code == "action_disabled"


def test_rename_is_disabled_in_a_read_only_stdlib_buffer(trowel, fixture_files: Path):
    """The server would refuse anyway; greying it out says so without a round trip."""
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")
    trowel.call("editor.set_cursor", {"pos": _offset_of(trowel, "list-head") + 2})
    trowel.call("nav.goto_definition", {"timeout_ms": WAIT_MS})
    assert trowel.call("editor.is_read_only")["read_only"] is True

    with pytest.raises(ControlError) as ei:
        trowel.call("menu.invoke", {"path": RENAME})
    assert ei.value.code == "action_disabled"


# --- the inline input ------------------------------------------------------


def test_rename_input_opens_pre_filled_and_escape_dismisses(trowel, fixture_files: Path):
    """The input is non-modal, so the control socket can still be served.

    A modal dialog here would hang every test that reached it rather than
    failing it.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")
    trowel.call("editor.set_cursor", {"pos": _offset_of(trowel, "(def total 100)") + 6})

    assert trowel.call("editor.rename_input")["visible"] is False
    # begin_rename drives the same F2 path but connects before triggering, so
    # the reply cannot be missed by a wait that registered a moment too late.
    r = trowel.call("editor.begin_rename", {"timeout_ms": RENAME_MS})
    assert r["opened"] is True
    assert r["refusal"] == ""

    state = trowel.call("editor.rename_input")
    assert state["visible"] is True
    assert state["text"] == "total"


def test_rename_input_does_not_open_when_the_server_refuses(trowel, fixture_files: Path):
    """prepareRename runs first, always — the refusal reaches the user before
    they have typed anything."""
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "scopes.tur")
    trowel.call("editor.set_cursor", {"pos": _offset_of(trowel, "list-head") + 2})

    r = trowel.call("editor.begin_rename", {"timeout_ms": RENAME_MS})
    assert r["opened"] is False
    assert r["refusal"] == "cannot rename stdlib symbol"
    assert trowel.call("editor.rename_input")["visible"] is False


# --- cross-file ------------------------------------------------------------


def test_cross_file_rename_edits_both_files_and_leaves_them_dirty(
    trowel, fixture_files: Path
):
    """Renaming an exported name reaches a file that has no tab.

    The importing file is opened rather than written: a rename that silently
    modified files on disk is not undoable by Ctrl+Z, and Trowel has no VCS
    integration to fall back on.
    """
    _require_server(trowel)
    ws = fixture_files / "renamews"
    helper, main = ws / "src" / "lib" / "helper.tur", ws / "src" / "main.tur"

    _open_and_analyze(trowel, helper)
    assert trowel.call("window.list")["windows"][0]["tabs"] == [str(helper)]
    trowel.call("editor.set_cursor",
                {"pos": _offset_of(trowel, "(def shared-value 42)") + 6})

    r = trowel.call("lsp.rename",
                    {"new_name": "shared-total", "apply": True, "timeout_ms": RENAME_MS})
    assert r["error"] == ""
    assert r["apply_error"] == ""
    assert r["document_count"] == 2, r["documents"]
    assert r["applied"] == 2

    # Both files now have tabs, both are dirty, neither was written.
    tabs = trowel.call("window.list")["windows"][0]["tabs"]
    assert str(main) in tabs

    for path in (helper, main):
        trowel.call("editor.open", {"path": str(path)})
        state = trowel.call("editor.get_text")
        assert "shared-total" in state["text"], path
        assert state["modified"] is True, f"{path} should be left dirty, not saved"
        assert "shared-total" not in path.read_text(), f"{path} was written to disk"


def test_cross_file_rename_returns_focus_to_the_starting_tab(trowel, fixture_files: Path):
    """Opening tabs for the other documents must not strand the user in one."""
    _require_server(trowel)
    ws = fixture_files / "renamews"
    helper = ws / "src" / "lib" / "helper.tur"

    _open_and_analyze(trowel, helper)
    trowel.call("editor.set_cursor",
                {"pos": _offset_of(trowel, "(def shared-value 42)") + 6})
    trowel.call("lsp.rename",
                {"new_name": "shared-total", "apply": True, "timeout_ms": RENAME_MS})

    assert trowel.call("editor.get_text")["path"] == str(helper)
