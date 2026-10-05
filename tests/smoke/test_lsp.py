"""Language server integration — diagnostics, completion, hover.

Every wait goes through `wait.diagnostics` or a handler-side timeout; no test
sleeps. See docs/plans/smoke-tests.md.

Timeouts stay under the control socket's own 10s read timeout (see
tests/support/trowel_ctl.py) — a longer `timeout_ms` would surface as a socket
timeout rather than the handler's `timeout` error.
"""

from pathlib import Path

import pytest

from trowel_ctl import ControlError

# The first request pays for process spawn + initialize + a full compile.
WAIT_MS = 8000


def _require_server(trowel) -> dict:
    status = trowel.call("lsp.status")
    if not status["enabled"]:
        pytest.skip("language server disabled in settings")
    if not status["server_path"]:
        pytest.skip("no `tur` binary available to run `tur lsp`")
    return status


def _open_and_analyze(trowel, path: Path, min_count: int = 0) -> dict:
    """Open `path` and block until the server publishes a batch for it."""
    trowel.call("editor.open", {"path": str(path)})
    return trowel.call("wait.diagnostics", {"min_count": min_count, "timeout_ms": WAIT_MS})


def test_status_reports_a_server_binary(trowel):
    status = _require_server(trowel)
    assert status["server_path"].endswith("tur")
    assert status["state"] in {"stopped", "starting", "ready"}


def test_syntax_error_produces_a_diagnostic(trowel, fixture_files: Path):
    _require_server(trowel)
    r = _open_and_analyze(trowel, fixture_files / "syntax_error.tur", min_count=1)

    assert r["count"] >= 1
    first = r["diagnostics"][0]
    assert first["message"]
    assert first["severity"] in (1, 2, 3, 4)
    # The fixture is a single unterminated line, so the range must land on it.
    assert first["start_line"] == 0


def test_diagnostic_is_painted_in_the_editor(trowel, fixture_files: Path):
    """The squiggle and gutter marker actually reach Scintilla.

    lsp.decorations reads back out of the widget, so this fails if
    setDiagnostics stops painting even while the manager still holds the data.
    """
    _require_server(trowel)
    r = _open_and_analyze(trowel, fixture_files / "syntax_error.tur", min_count=1)
    first = r["diagnostics"][0]

    d = trowel.call("lsp.decorations")
    assert d["error_ranges"], d
    assert d["error_marker_lines"] == [first["start_line"]]

    # The squiggle must be non-empty and sit inside the document.
    start, end = d["error_ranges"][0]["start"], d["error_ranges"][0]["end"]
    assert end > start
    assert end <= len(trowel.call("editor.get_text")["text"].encode("utf-8"))


def test_decorations_clear_when_the_error_is_fixed(trowel, fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "syntax_error.tur", min_count=1)
    assert trowel.call("lsp.decorations")["error_ranges"]

    trowel.call("editor.set_text", {"text": '(def fixed "ok")\n'})
    trowel.call("wait.diagnostics", {"min_count": 0, "max_count": 0, "timeout_ms": WAIT_MS})

    d = trowel.call("lsp.decorations")
    assert d["error_ranges"] == []
    assert d["error_marker_lines"] == []


def test_clean_file_has_no_diagnostics(trowel, fixture_files: Path):
    _require_server(trowel)
    # min_count 0 still waits for a real publish — wait.diagnostics checks
    # hasPublishedFor, so this cannot pass before the server has analyzed.
    r = _open_and_analyze(trowel, fixture_files / "hello.tur")
    assert r["count"] == 0


def test_diagnostics_clear_when_the_error_is_fixed(trowel, fixture_files: Path):
    _require_server(trowel)
    assert _open_and_analyze(trowel, fixture_files / "syntax_error.tur", min_count=1)["count"] >= 1

    trowel.call("editor.set_text", {"text": '(def fixed "ok")\n'})
    # max_count 0 blocks until the repaired buffer publishes an empty batch —
    # without it the already-published error batch would satisfy min_count 0
    # immediately and the assert would race.
    r = trowel.call("wait.diagnostics",
                    {"min_count": 0, "max_count": 0, "timeout_ms": WAIT_MS})
    assert r["count"] == 0


def _completions_at_end(trowel, extra_text: str = "") -> dict:
    """Append `extra_text`, park the caret at the end, and complete there."""
    text = trowel.call("editor.get_text")["text"] + extra_text
    trowel.call("editor.set_text", {"text": text})
    trowel.call("editor.set_cursor", {"pos": len(text.encode("utf-8"))})
    return trowel.call("lsp.completions", {"timeout_ms": WAIT_MS})


def test_completion_returns_symbols(trowel, fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "defs.tur")

    # Offset 0 is the obvious thing a client does — ask for completions right
    # after opening a file. It used to return nothing (the server derived its
    # prefix with a helper built for hover, which stepped right and filtered
    # every candidate away), which was indistinguishable from "no matches".
    # Fixed in Turmeric v0.32.2; pinned here so it stays fixed.
    trowel.call("editor.set_cursor", {"pos": 0})
    r = trowel.call("lsp.completions", {"timeout_ms": WAIT_MS})
    assert r["count"] > 0
    assert all(isinstance(label, str) and label for label in r["labels"])


def test_completion_survives_an_unbalanced_buffer(trowel, fixture_files: Path):
    """Typing `(` unbalances the buffer — completion must still work.

    Not parsing is the *normal* state mid-keystroke, and the server builds its
    symbol index from a successful compile. It used to return nothing here,
    which meant completion went silent exactly when it was wanted. v0.32.2
    retains the last good index (and falls back to stdlib for a file that has
    never parsed), so the buffer's own defs still come back.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "defs.tur")

    r = _completions_at_end(trowel, "\n(smoke")
    assert any("smoke-" in label for label in r["labels"]), r["labels"][:20]


def test_completion_includes_document_symbols(trowel, fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "defs.tur")

    # Caret inside `smoke-x` in `(def smoke-x 42)`. Completing the symbol under
    # the caret surfaces the buffer's own defs; the unfiltered list is capped
    # at 200 server-side and would bury them.
    trowel.call("editor.set_cursor", {"pos": 5})
    r = trowel.call("lsp.completions", {"timeout_ms": WAIT_MS})
    assert any("smoke-" in label for label in r["labels"]), r["labels"][:20]


def test_hover_reports_the_symbol(trowel, fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "hello.tur")

    # Caret inside `greeting` in `(def greeting "hello, world")`.
    trowel.call("editor.set_cursor", {"pos": 7})
    r = trowel.call("lsp.hover", {"timeout_ms": WAIT_MS})
    assert "greeting" in r["text"]


def test_unsaved_buffer_is_skipped(trowel):
    _require_server(trowel)
    trowel.type("(def x 1)")
    with pytest.raises(ControlError) as excinfo:
        trowel.call("wait.diagnostics", {"min_count": 0, "timeout_ms": 2000})
    assert excinfo.value.code == "no_uri"


def test_non_turmeric_buffer_is_skipped(trowel, fixture_files: Path):
    _require_server(trowel)
    trowel.call("editor.open", {"path": str(fixture_files / "sample.json")})
    # A JSON buffer is never registered, so no batch ever arrives for it.
    with pytest.raises(ControlError) as excinfo:
        trowel.call("wait.diagnostics", {"min_count": 0, "timeout_ms": 1500})
    assert excinfo.value.code == "timeout"


def test_restart_recovers(trowel, fixture_files: Path):
    _require_server(trowel)
    assert _open_and_analyze(trowel, fixture_files / "syntax_error.tur", min_count=1)["count"] >= 1

    trowel.call("lsp.restart")
    trowel.call("editor.set_text", {"text": "(def still broken (\n"})
    r = trowel.call("wait.diagnostics", {"min_count": 1, "timeout_ms": WAIT_MS})
    assert r["count"] >= 1


def test_completion_list_with_a_question_mark_name_does_not_abort(
        trowel, fixture_files: Path, tmp_path):
    """A `?`-suffixed predicate in the completion list must not kill the app.

    Scintilla's default autocomplete *type* separator is '?', and Turmeric
    spells predicates `empty?` / `nil?` / `zero?` by convention.
    `ListBoxImpl::SetList` splits each item at that separator and calls
    `Append(word, atoi(rest))`; a bare trailing '?' makes that `atoi("")` == 0,
    and `Append` then asserts `images.contains(0)` against a map Trowel never
    registers into. Typing `(` in such a buffer aborted the debug build
    outright (SIGABRT), and silently reserved a phantom icon in release.

    The assertion here is simply that the process is still answering: a crash
    takes the control socket with it, so any later call fails.
    """
    _require_server(trowel)
    prog = tmp_path / "predicates.tur"
    prog.write_text(
        "(defn empty? [n : int] : int\n"
        "  (if (< n 1) 1 0))\n"
        "\n"
        "(defn main [] : int\n"
        "  (empty? 0))\n")
    _open_and_analyze(trowel, prog)

    text = trowel.call("editor.get_text")["text"]
    trowel.call("editor.set_cursor", {"pos": len(text.encode("utf-8"))})
    trowel.type("\n(empt")

    # `lsp.completions` deliberately does NOT reproduce this: it calls the
    # manager and returns labels, never building Scintilla's list box. Only a
    # path that reaches `showCompletions` -> `autoCShow` can. Complete Symbol
    # is that path, and invoking it is the whole test.
    trowel.call("menu.invoke", {"path": ["Edit", "Complete Symbol"]})

    # Still answering. A crash takes the control socket with it, so this call
    # raises BrokenPipeError rather than failing an assert — which is exactly
    # what it did before the fix.
    import time
    time.sleep(0.5)
    assert trowel.call("lsp.status")["enabled"] is True
    assert trowel.call("editor.get_text")["text"].endswith("(empt")


# --- headerless dialect files ----------------------------------------------
#
# `tur lsp` used to pick its reader from the `#lang` line only and ignore the
# file extension, where the COMPILER honours the extension -- so a headerless
# `.scm` or `.tur.sweet`, the idiomatic way to write either, was analysed as
# Turmeric: bogus errors on every line and no symbols at all. Filed as
# `docs/upstream/lsp-ignores-the-file-extension.md` and fixed in v0.61.0 (the
# scratch file the server analyses now takes the document's suffix, which is
# where the `.tur` was coming from).
#
# Trowel's own workaround -- prepending the implied header and shifting every
# line number back -- is gone with it. These tests are unchanged by that, which
# is the point: what they pin is that the editor shows no errors in a file that
# compiles, whoever is responsible for getting the reader right.


def test_a_headerless_scheme_file_has_no_diagnostics(trowel, fixture_files: Path):
    _require_server(trowel)
    r = _open_and_analyze(trowel, fixture_files / "hello.scm")
    assert r["count"] == 0, trowel.call("lsp.diagnostics")


def test_a_headerless_scheme_file_has_symbols_on_the_right_lines(
        trowel, fixture_files: Path):
    """The symbols must also be where the BUFFER has them, not where the text
    sent to the server has them -- that one-line shift is the whole cost of the
    workaround, and an off-by-one here is a wrong jump on every Go to
    Definition.
    """
    _require_server(trowel)
    path = fixture_files / "hello.scm"
    _open_and_analyze(trowel, path)
    syms = trowel.call("lsp.symbols")["symbols"]
    names = {s["name"]: s for s in syms}
    assert "double" in names, syms

    lines = path.read_text().splitlines()
    got = names["double"]["line"]
    assert 0 <= got < len(lines), f"line {got} is outside a {len(lines)}-line file"
    assert "double" in lines[got], \
        f"symbol 'double' reported at line {got}, which holds {lines[got]!r}"


def test_a_headerless_sweet_file_has_no_diagnostics(trowel, fixture_files: Path):
    # Trowel's own sweet fixtures have never carried a header, so this case was
    # showing false `TUR-E0003 unbound symbol 'defn'` errors in the gutter.
    _require_server(trowel)
    r = _open_and_analyze(trowel, fixture_files / "sweet_main.tur.sweet")
    assert r["count"] == 0, trowel.call("lsp.diagnostics")


def test_a_file_that_already_has_a_lang_line_is_sent_unchanged(
        trowel, tmp_path: Path):
    """A file that carries its own `#lang` line is analysed by it, and its
    symbols land on the buffer's own lines.

    This was the no-double-header case while Trowel synthesized one; it stays
    because the line numbers are the part worth pinning either way.
    """
    _require_server(trowel)
    src = tmp_path / "hdr.scm"
    src.write_text("#lang r7rs\n(define (g y) (+ y 1))\n")
    _open_and_analyze(trowel, src)
    syms = trowel.call("lsp.symbols")["symbols"]
    names = {s["name"]: s for s in syms}
    assert "g" in names, syms
    # `g` is on line 1 of the buffer (0-based), after the header it really has.
    assert names["g"]["line"] == 1, syms


# --- signature help and workspace symbols ----------------------------------
#
# Two capabilities `tur lsp` has advertised all along that nothing asked for.
# Signature help lands on Scintilla's call tip -- the same surface as hover --
# so it is asserted through the tip's text rather than through the manager's
# reply: the manager having an answer and the editor showing it are different
# claims, and only the second is the feature.


def test_signature_help_shows_the_callees_parameter_list(trowel, fixture_files: Path):
    _require_server(trowel)
    # symbols.tur defines `nav-double [n : int] : int` and calls it, so there is
    # a real callee with a real parameter to describe.
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    text = trowel.call("editor.get_text")["text"]
    # In ARGUMENT position, not at the `(`: the server answers null at its own
    # advertised trigger character and only returns a signature once the cursor
    # is past the callee's name and a space. Measured; see the comment in
    # EditorView's charAdded hook.
    #
    # The offset is in BYTES. Scintilla positions are byte offsets, and
    # symbols.tur contains a `§`, so a Python character index is one short of
    # the right spot by the time it reaches line 16 — which reads exactly like
    # the server declining the position.
    chars = text.index("(nav-double nav-total") + len("(nav-double ")
    inside = len(text[:chars].encode("utf-8"))

    r = trowel.call("lsp.signature_help", {"pos": inside, "timeout_ms": WAIT_MS})
    assert "nav-double" in r["text"], r
    # The parameter list, which is the thing you cannot see once the name is
    # behind the cursor.
    assert "int" in r["text"], r
    assert r["active_parameter"] == 0, r


def test_signature_help_is_silent_where_there_is_no_call(trowel, fixture_files: Path):
    # No tip rather than an empty one: a call tip with nothing in it is worse
    # than none, because it covers the line under the caret.
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")
    # The server answers `null` away from a call, and that is REPORTED as an
    # empty signature rather than dropped -- a dropped reply is
    # indistinguishable from one still in flight, which is why this used to
    # surface as a control-socket timeout instead of an answer.
    r = trowel.call("lsp.signature_help", {"pos": 0, "timeout_ms": 5000})
    assert r["text"] == "", r
    assert r["tip_active"] is False, r


def test_workspace_symbols_finds_a_definition_in_another_file(
        trowel, fixture_files: Path):
    """The point of `workspace/symbol` over the document outline: the answer
    comes from files other than the open one.
    """
    _require_server(trowel)
    # Open one fixture, search for a symbol defined in a different one. The
    # server has to have seen symbols.tur for this to be findable, so it is
    # opened first and then navigated away from.
    _open_and_analyze(trowel, fixture_files / "symbols.tur")
    _open_and_analyze(trowel, fixture_files / "hello.tur")

    r = trowel.call("lsp.workspace_symbols",
                    {"query": "nav-double", "timeout_ms": WAIT_MS})
    assert r["count"] >= 1, r
    paths = [s["path"] for s in r["symbols"]]
    assert any(p.endswith("symbols.tur") for p in paths), r


def test_workspace_symbols_reports_no_matches_rather_than_hanging(trowel,
                                                                 fixture_files: Path):
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "hello.tur")
    r = trowel.call("lsp.workspace_symbols",
                    {"query": "zzz-no-such-symbol-anywhere", "timeout_ms": WAIT_MS})
    assert r["count"] == 0, r
    assert r["reason"], r


def test_signature_help_auto_triggers_on_space(trowel, fixture_files: Path):
    """Space is the server's trigger character as of v0.61.0, and Trowel now
    fires on it from `charAdded`.

    It used to advertise `(` and answer null there -- in a lisp the callee is
    typed AFTER the paren, so there is nothing to describe yet. Asserted through
    the call TIP rather than a control request, because the auto-trigger path
    goes through the editor and the tip is what the user sees.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "symbols.tur")

    # Type a fresh call. The space after the callee is the trigger.
    text = trowel.call("editor.get_text")["text"]
    trowel.call("editor.set_cursor", {"pos": len(text.encode("utf-8"))})
    trowel.type("\n(nav-double ")
    trowel.wait_idle(quiet_ms=600, timeout_ms=8000)

    tip = trowel.call("editor.call_tip")["text"]
    assert "nav-double" in tip, f"no signature tip after the trigger space: {tip!r}"


# --- dependency errors (relatedInformation) --------------------------------
#
# `tur lsp` analyses one document at a time and `load` is textual inclusion, so
# an error inside a loaded file is reported against the OPEN file, on its
# `(load "...")` form -- clangd's model for an error inside an `#include`. The
# range points at the load; the real site arrives in `relatedInformation`.
#
# Trowel used to declare `relatedInformation: false` and drop it, so the one
# actionable part of such a diagnostic was lost.
#
# The loads below are ABSOLUTE on purpose. A relative `(load "b.tur")` under
# `tur lsp` resolves against the server's scratch directory rather than the
# document's, so it fails with `load: cannot open ...` and never reaches the
# dependency case at all -- an open upstream report
# (`lsp-relative-load-resolves-against-scratch-dir`). A first version of these
# tests used a relative load and one of them PASSED anyway, asserting a
# "cannot open" diagnostic while claiming to test a dependency error.


def _dependency_pair(tmp_path: Path) -> Path:
    """A broken file and an importer that loads it by absolute path."""
    broken = tmp_path / "dep_broken.tur"
    broken.write_text("(def dep-broken-thing (this-name-does-not-exist 1))\n")
    importer = tmp_path / "dep_importer.tur"
    importer.write_text('(load "%s")\n(def importer-ok 1)\n' % broken)
    return importer


def test_a_dependency_error_lands_on_the_load_form(trowel, tmp_path: Path):
    _require_server(trowel)
    importer = _dependency_pair(tmp_path)
    r = _open_and_analyze(trowel, importer, min_count=1)
    assert r["count"] >= 1, r

    d = trowel.call("lsp.diagnostics")["diagnostics"][0]
    # It is the DEPENDENCY error, not a failure to open the file: the server
    # prefixes the real location. Asserted first, so this test cannot pass on a
    # "cannot open" diagnostic that happens to sit on the same line.
    assert d["message"].startswith("in "), d
    assert "this-name-does-not-exist" in d["message"], d

    # Drawn on line 0 -- the `(load ...)` -- and on the path STRING, not at
    # column 0 and not at the dependency's own column.
    assert d["start_line"] == 0, d
    line0 = trowel.call("editor.get_text")["text"].split("\n")[0]
    assert line0[d["start_char"]] == '"', (line0, d)


def test_a_dependency_error_carries_a_jumpable_related_location(
        trowel, tmp_path: Path):
    """The part Trowel used to throw away. Without `relatedInformation: true`
    in `initialize` the server sends nothing here, so this also pins the
    capability declaration.
    """
    _require_server(trowel)
    importer = _dependency_pair(tmp_path)
    _open_and_analyze(trowel, importer, min_count=1)

    d = trowel.call("lsp.diagnostics")["diagnostics"][0]
    assert d["from_dependency"] is True, d
    assert d["related"], d
    rel = d["related"][0]
    assert rel["path"].endswith("dep_broken.tur"), rel
    assert rel["line"] == 0, rel
    assert "this-name-does-not-exist" in rel["message"], rel


def test_go_to_diagnostic_source_opens_the_dependency(trowel, tmp_path: Path):
    _require_server(trowel)
    importer = _dependency_pair(tmp_path)
    _open_and_analyze(trowel, importer, min_count=1)

    d = trowel.call("lsp.diagnostics")["diagnostics"][0]
    trowel.call("editor.set_cursor",
                {"line": d["start_line"], "col": d["start_char"]})
    trowel.call("menu.invoke", {"path": ["Go", "Go to Diagnostic Source"]})

    opened = trowel.call("editor.get_text")["text"]
    assert "this-name-does-not-exist" in opened, \
        f"did not open the dependency; buffer is {opened!r}"


def test_go_to_diagnostic_source_declines_a_local_diagnostic(
        trowel, fixture_files: Path):
    """A diagnostic reported where it happened has no related location, and the
    action must say so rather than silently doing nothing or jumping somewhere.
    """
    _require_server(trowel)
    _open_and_analyze(trowel, fixture_files / "syntax_error.tur", min_count=1)
    before = trowel.call("editor.get_text")["text"]

    d = trowel.call("lsp.diagnostics")["diagnostics"][0]
    assert d["from_dependency"] is False, d
    trowel.call("editor.set_cursor",
                {"line": d["start_line"], "col": d["start_char"]})
    trowel.call("menu.invoke", {"path": ["Go", "Go to Diagnostic Source"]})

    # Same buffer, no jump.
    assert trowel.call("editor.get_text")["text"] == before


def test_a_dependency_error_through_a_load_chain_points_at_the_innermost_file(
        trowel, tmp_path: Path):
    """`top.tur` loads `mid.tur` loads `deep.tur`, and the error is in `deep`.

    Turmeric v0.62.0 places such a diagnostic on the OPEN file's own load form
    and names the intermediate hop in the message (`(via mid.tur)`), with the
    related location pointing at the innermost file. Trowel needed no change for
    that -- it renders whatever range and related location arrive -- and this
    pins that, because the tempting bug is to jump to the file named in the
    `(load ...)` the squiggle sits on rather than the one that is wrong.
    """
    _require_server(trowel)
    deep = tmp_path / "deep.tur"
    deep.write_text("(def deep-broken (this-name-does-not-exist 1))\n")
    mid = tmp_path / "mid.tur"
    mid.write_text('(load "%s")\n' % deep)
    top = tmp_path / "top.tur"
    top.write_text('(load "%s")\n(def top-ok 1)\n' % mid)

    _open_and_analyze(trowel, top, min_count=1)
    d = trowel.call("lsp.diagnostics")["diagnostics"][0]

    assert "deep.tur" in d["message"], d
    assert "via mid.tur" in d["message"], d
    assert d["from_dependency"] is True, d
    # The related location is the INNERMOST file, not the hop.
    assert d["related"][0]["path"].endswith("deep.tur"), d["related"]

    trowel.call("editor.set_cursor",
                {"line": d["start_line"], "col": d["start_char"]})
    trowel.call("menu.invoke", {"path": ["Go", "Go to Diagnostic Source"]})
    assert "this-name-does-not-exist" in trowel.call("editor.get_text")["text"]
