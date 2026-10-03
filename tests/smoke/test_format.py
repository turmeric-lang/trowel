"""Format File — `tur fmt --stdin --lang <reader>`.

Two things are being pinned here. First, that the dialect reaches the
formatter: without `--lang` a Scheme buffer formats to different bytes, so a
test that only checked "something changed" would have passed on the old
`tur format` call too. Second, that the sweet dialects are DECLINED rather
than silently rewritten into s-expressions.
"""

import subprocess
from pathlib import Path

FORMAT = ["Run", "Format File"]


def tur_fmt(tur: str, text: str, lang: str) -> str:
    """What the toolchain itself answers, as the oracle for the editor."""
    p = subprocess.run([tur, "fmt", "--stdin", "--lang", lang],
                       input=text, capture_output=True, text=True, timeout=30)
    assert p.returncode == 0, p.stderr
    return p.stdout


def test_a_scheme_buffer_formats_as_scheme(trowel, tur_binary, tmp_path: Path):
    src = tmp_path / "fmt.scm"
    body = "(define (f x)\n(* x 2))\n(display (f 21))\n"
    src.write_text(body)
    trowel.call("editor.open", {"path": str(src)})
    trowel.call("menu.invoke", {"path": FORMAT})

    got = trowel.call("editor.get_text")["text"]
    assert got == tur_fmt(tur_binary, body, "r7rs"), got
    # And specifically NOT what the dialect-less call produces, which is the
    # bug this replaced: that one collapses the body onto one line.
    assert got != tur_fmt(tur_binary, body, "turmeric"), \
        "formatting ignored the buffer's dialect"


def test_a_turmeric_buffer_still_formats(trowel, tur_binary, tmp_path: Path):
    src = tmp_path / "fmt.tur"
    body = "(defn   double [x]\n   (* x 2))\n"
    src.write_text(body)
    trowel.call("editor.open", {"path": str(src)})
    trowel.call("menu.invoke", {"path": FORMAT})
    got = trowel.call("editor.get_text")["text"]
    assert got == tur_fmt(tur_binary, body, "turmeric"), got


def test_a_sweet_buffer_is_left_exactly_as_written(trowel, tmp_path: Path):
    """`tur fmt --lang sweet` reprints sweet-expressions as s-expressions and
    keeps the `#lang turmeric/sweet` header, which would leave a file whose
    header contradicts its body. Asserted on the BYTES, not on the message --
    the bytes are the bug.
    """
    src = tmp_path / "fmt.tur.sweet"
    body = "#lang turmeric/sweet\n\ndefn double [x]\n  {x * 2}\n"
    src.write_text(body)
    trowel.call("editor.open", {"path": str(src)})
    trowel.call("menu.invoke", {"path": FORMAT})
    assert trowel.call("editor.get_text")["text"] == body


def test_r7rs_sweet_is_allowed_through_and_keeps_its_body(
        trowel, tur_binary, tmp_path: Path):
    """`r7rs/sweet` is the one sweet reader `tur fmt` does not rewrite: it checks
    the buffer and returns the body as written, so it is allowed through.

    Asserted against the toolchain's own answer rather than against the input.
    It is NOT byte-identical to the input: a `#lang` header is preserved
    verbatim but the blank line after it is dropped, so the sweet BODY survives
    while the header's spacing does not. Measured, not assumed -- an earlier
    version of this test asserted equality with the input and failed on exactly
    that blank line.
    """
    src = tmp_path / "sweetscheme.tur"
    body = "#lang r7rs/sweet\n\ndefine (f x)\n  * x 2\n"
    src.write_text(body)
    trowel.call("editor.open", {"path": str(src)})
    trowel.call("menu.invoke", {"path": FORMAT})

    got = trowel.call("editor.get_text")["text"]
    assert got == tur_fmt(tur_binary, body, "r7rs/sweet"), got
    # The point of letting it through: the sweet body is still sweet afterwards.
    assert "define (f x)" in got and "  * x 2" in got, got
