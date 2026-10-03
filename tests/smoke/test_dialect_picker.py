"""The dialect picker: the `#lang` line is the source of truth.

The picker is a TEXT EDIT, not a hidden mode. Flipping it writes the header;
typing the header reconciles the picker. Nothing about the dialect lives in UI
state — which is what these tests check, by reading the buffer back rather than
by inspecting the menu.
"""

from pathlib import Path


def set_dialect(trowel, base: str):
    return trowel.call("lang.set", {"base": base})


def text(trowel) -> str:
    return trowel.call("editor.get_text")["text"]


def test_picking_a_non_default_dialect_inserts_a_header(trowel, tmp_path: Path):
    f = tmp_path / "a.tur"
    f.write_text("(def x 1)\n")
    trowel.call("editor.open", {"path": str(f)})

    set_dialect(trowel, "saffron")
    # Header on line 1, then a blank line, then the original body untouched.
    assert text(trowel) == "#lang saffron\n\n(def x 1)\n", text(trowel)
    assert trowel.call("lang.get")["base"] == "saffron"


def test_picking_the_default_removes_the_header(trowel, tmp_path: Path):
    f = tmp_path / "b.tur"
    f.write_text("#lang saffron\n\n(def x 1)\n")
    trowel.call("editor.open", {"path": str(f)})

    set_dialect(trowel, "turmeric")
    # The blank line the header was separated by goes with it: `#lang turmeric`
    # is what a file with no header already means, so leaving either behind
    # would make the picker's "off" state visible in the source forever.
    assert text(trowel) == "(def x 1)\n", text(trowel)
    assert trowel.call("lang.get")["base"] == "turmeric"


def test_a_plain_file_is_not_decorated_with_a_redundant_header(trowel, tmp_path: Path):
    f = tmp_path / "c.tur"
    f.write_text("(def x 1)\n")
    trowel.call("editor.open", {"path": str(f)})
    set_dialect(trowel, "turmeric")
    assert text(trowel) == "(def x 1)\n", text(trowel)


def test_switching_between_two_non_default_dialects_replaces_only_that_line(
        trowel, tmp_path: Path):
    f = tmp_path / "d.tur"
    f.write_text("#lang saffron\n\n(def x 1)\n(def y 2)\n")
    trowel.call("editor.open", {"path": str(f)})

    set_dialect(trowel, "r7rs/sweet")
    assert text(trowel) == "#lang r7rs/sweet\n\n(def x 1)\n(def y 2)\n", text(trowel)


def test_a_language_switch_is_one_undo_step(trowel, tmp_path: Path):
    """A single Ctrl+Z restores the previous text exactly, rather than unpicking
    the edit a line at a time."""
    f = tmp_path / "e.tur"
    before = "(def x 1)\n"
    f.write_text(before)
    trowel.call("editor.open", {"path": str(f)})

    set_dialect(trowel, "saffron/sweet")
    assert text(trowel) != before

    trowel.call("editor.press", {"key": "Z", "mods": ["ctrl"]})
    assert text(trowel) == before, text(trowel)


def test_typing_a_header_by_hand_reconciles_the_picker(trowel, tmp_path: Path):
    # The round trip the other way: the picker reads dialect() off the buffer,
    # so there is no second source of truth to drift.
    f = tmp_path / "g.tur"
    f.write_text("(def x 1)\n")
    trowel.call("editor.open", {"path": str(f)})
    trowel.call("editor.set_text", {"text": "#lang r7rs\n(define x 1)\n"})
    assert trowel.call("lang.get")["base"] == "r7rs"


def test_the_menu_offers_six_rows_and_hides_the_bare_readers(trowel):
    """Curly-infix and neoteric are not offered: `{a + b}` is enabled in every
    dialect and neoteric is one of sweet-exp's three tools, so presenting them
    as dialects of their own misrepresents them. Both stay spellable, and a
    buffer that names one gets its row back (the next test).
    """
    rows = trowel.call("lang.menu")["rows"]
    bases = [r["base"] for r in rows if r.get("base")]
    assert bases == ["turmeric", "turmeric/sweet",
                     "saffron", "saffron/sweet",
                     "r7rs", "r7rs/sweet"], bases
    headings = [r["heading"] for r in rows if r.get("heading")]
    assert headings == ["turmeric", "saffron", "r7rs"], headings


def test_a_buffer_naming_a_hidden_reader_gets_its_row_back(trowel, tmp_path: Path):
    f = tmp_path / "h.tur"
    f.write_text("#lang turmeric/neoteric\n(def x 1)\n")
    trowel.call("editor.open", {"path": str(f)})
    rows = trowel.call("lang.menu")["rows"]
    bases = [r["base"] for r in rows if r.get("base")]
    assert "turmeric/neoteric" in bases, bases
    # ...and it is the checked one, so the menu never disagrees with the source.
    checked = [r["base"] for r in rows if r.get("checked")]
    assert checked == ["turmeric/neoteric"], checked
