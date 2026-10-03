"""The `#lang` dialect axis: ten bases, and the REPL session's language.

The load-bearing fact these tests pin is that a dialect is not just a
highlighting mode. `#lang r7rs` selects a PRELUDE, so a Scheme buffer cannot
run against a Turmeric REPL session however the file is spelled -- the session
has to be switched first, which resets it. See DialectNeedsSessionSwitch in
src/editor/dialect.h for the measurements behind that.
"""

import json
import subprocess
from pathlib import Path

RESTART_REPL = ["Run", "Restart REPL"]


def test_lang_bases_matches_the_toolchains_own_registry(trowel, tur_binary):
    """The ten bases are read back from the toolchain, not from this test's
    idea of them. A hardcoded list in the client is exactly what drifted on the
    Try Turmeric side when Saffron landed -- the picker went on offering four
    bases after there were eight, so `#lang saffron` worked when typed but
    could not be selected.
    """
    p = subprocess.run([tur_binary, "dialects", "--json"],
                       capture_output=True, text=True, timeout=30)
    assert p.returncode == 0, p.stderr
    upstream = json.loads(p.stdout)["dialects"]

    ours = trowel.call("lang.bases")["bases"]
    assert [b["base"] for b in ours] == [d["base"] for d in upstream]
    assert [b["language"] for b in ours] == [d["language"] for d in upstream]
    assert [b["reader"] for b in ours] == [d["reader"] for d in upstream]


def test_scm_extension_is_r7rs(trowel, fixture_files: Path):
    trowel.call("editor.open", {"path": str(fixture_files / "hello.scm")})
    got = trowel.call("lang.get")
    assert got["base"] == "r7rs"
    assert got["language"] == "r7rs"


def test_a_lang_line_names_the_dialect(trowel, tmp_path: Path):
    for base in ("saffron", "saffron/sweet", "r7rs/sweet",
                 "turmeric/neoteric", "turmeric/sweet"):
        f = tmp_path / (base.replace("/", "_") + ".tur")
        f.write_text("#lang %s\n" % base)
        trowel.call("editor.open", {"path": str(f)})
        assert trowel.call("lang.get")["base"] == base


def test_the_legacy_sweet_exp_alias_is_accepted_on_input(trowel, tmp_path: Path):
    # Accepted by the toolchain, never generated -- so it resolves to the
    # canonical base rather than to a row of its own.
    f = tmp_path / "legacy.tur"
    f.write_text("#lang sweet-exp\ndef x 1\n")
    trowel.call("editor.open", {"path": str(f)})
    assert trowel.call("lang.get")["base"] == "turmeric/sweet"


def test_a_scheme_buffer_needs_a_session_switch_and_a_turmeric_one_does_not(
        trowel, fixture_files: Path, tmp_path: Path):
    trowel.wait_output("turmeric>", timeout_ms=5000)

    trowel.call("editor.open", {"path": str(fixture_files / "hello.tur")})
    assert trowel.call("lang.get")["needs_switch"] is False

    trowel.call("editor.open", {"path": str(fixture_files / "hello.scm")})
    assert trowel.call("lang.get")["needs_switch"] is True

    # A READER difference is not a language difference: the reader travels with
    # the file, so no switch.
    sweet = tmp_path / "s.tur.sweet"
    sweet.write_text("def x 1\n")
    trowel.call("editor.open", {"path": str(sweet)})
    assert trowel.call("lang.get")["needs_switch"] is False


def test_running_a_scheme_buffer_switches_the_session_and_runs_it(
        trowel, fixture_files: Path):
    """The end-to-end case. Against an unswitched Turmeric session this fails
    with `unknown function or operator 'r7rs-display'` -- the forms parse and
    the names get their `r7rs-` rename, and nothing answers to them.
    """
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(fixture_files / "hello.scm")})
    trowel.call("run.buffer")

    hit = trowel.wait_output("scheme ran: 42", timeout_ms=15000)
    assert "scheme ran: 42" in hit["matched"]
    # And the session is now Scheme, by its own account.
    assert trowel.call("lang.get")["session_base"] == "r7rs"


def test_the_session_dialect_follows_a_lang_line_typed_at_the_prompt(trowel):
    """The session's dialect is read from the REPL's acknowledgement, not from
    what Trowel sent -- so a `#lang` the user types into the pane directly is
    picked up. A session we only ever wrote to would be wrong about itself.
    """
    trowel.wait_output("turmeric>", timeout_ms=5000)
    assert trowel.call("lang.get")["session_base"] == "turmeric"

    trowel.send("#lang r7rs")
    trowel.wait_output("language set to r7rs", timeout_ms=10000)
    trowel.wait_idle(quiet_ms=400, timeout_ms=5000)
    assert trowel.call("lang.get")["session_base"] == "r7rs"


def test_restart_repl_starts_in_the_active_buffers_dialect(
        trowel, fixture_files: Path):
    # Otherwise the first Run Buffer in a Scheme project immediately switches
    # and resets the session the user just started.
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(fixture_files / "hello.scm")})
    trowel.call("menu.invoke", {"path": RESTART_REPL})
    trowel.wait_idle(quiet_ms=600, timeout_ms=10000)
    assert trowel.call("lang.get")["session_base"] == "r7rs"


def test_a_selection_out_of_a_headerless_scheme_file_carries_a_synthesized_lang(
        trowel, tmp_path: Path):
    """A `.scm` file is Scheme by extension with no header to copy, and that is
    the idiomatic way to write one. A selection out of it goes to a scratch
    file, so without a synthesized `#lang r7rs` the region would be read as
    Turmeric.
    """
    src = tmp_path / "sel.scm"
    body = '(define ignored 1)\n(define (shout) (display "selected ok") (newline))\n'
    src.write_text(body)
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(src)})
    start = body.index("(define (shout)")
    trowel.call("editor.set_selection", {"start": start, "end": len(body)})
    trowel.call("run.selection")
    trowel.wait_idle(quiet_ms=400, timeout_ms=10000)
    trowel.send("(shout)")
    hit = trowel.wait_output("selected ok", timeout_ms=10000)
    assert "selected ok" in hit["matched"]
