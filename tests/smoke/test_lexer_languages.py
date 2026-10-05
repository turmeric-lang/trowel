"""Per-file-type highlighting: each language gets its own style band, and
Markdown delegates fenced-code bodies to the right guest scanner.

Style IDs mirror the enums in src/editor/lexers.h. Each language owns a
contiguous block, so most assertions can check "is this in the right band"
rather than pinning an exact slot.
"""

from pathlib import Path

import pytest

# --- style bands (src/editor/lexers.h) --------------------------------------

TUR_DEFINE = 14
TUR_CBLOCK = 21
TUR_NEOTERIC = 25
TUR_IDENT = 26
TUR_SWEET_MARKER = 28
TUR_SCHEME_VECTOR = 29
TUR_BAR_SYMBOL = 30
TUR_LINE_COMMENT = 1
TUR_BLOCK_COMMENT = 3
TUR_STRING = 4
TUR_NUMBER = 6
TUR_BOOLEAN = 7
TUR_KEYWORD_LIT = 9
TUR_CHAR_LIT = 10
TUR_METADATA = 11
TUR_QUOTE = 12
TUR_OPERATOR = 13
TUR_SPECIAL = 19
TUR_BUILTIN = 20
TUR_BAND = range(0, 31)
RAINBOW_BAND = range(40, 48)

C_DEFAULT, C_COMMENT, C_DOC, C_PREPROC, C_KEYWORD, C_TYPE = 48, 49, 50, 51, 52, 53
C_STRING, C_ESCAPE, C_CHAR, C_NUMBER, C_OPERATOR, C_IDENT = 54, 55, 56, 57, 58, 59
C_BAND = range(48, 60)

MD_DEFAULT, MD_HEADING, MD_EMPHASIS, MD_STRONG, MD_CODESPAN = 64, 65, 66, 67, 68
MD_FENCE, MD_CODEBLOCK, MD_LINKTEXT, MD_LINKURL = 69, 70, 71, 72
MD_BLOCKQUOTE, MD_LISTMARKER, MD_RULE, MD_HTML, MD_ESCAPE = 73, 74, 75, 76, 77
MD_BAND = range(64, 78)

JSON_DEFAULT, JSON_KEY, JSON_STRING, JSON_ESCAPE = 80, 81, 82, 83
JSON_NUMBER, JSON_LITERAL, JSON_OPERATOR, JSON_ERROR = 84, 85, 86, 87

JUST_DEFAULT, JUST_COMMENT, JUST_RECIPE, JUST_DEP, JUST_PARAM = 91, 92, 93, 94, 95
JUST_ASSIGN, JUST_INTERP, JUST_BACKTICK, JUST_KEYWORD = 96, 97, 98, 99
JUST_STRING, JUST_NUMBER, JUST_BODY, JUST_ATTR, JUST_OP = 100, 101, 102, 103, 104

CM_DEFAULT, CM_COMMENT, CM_COMMAND, CM_KEYWORD, CM_VARIABLE = 112, 113, 114, 115, 116
CM_STRING, CM_ESCAPE, CM_NUMBER, CM_OPERATOR, CM_IDENT = 117, 118, 119, 120, 121
CM_BAND = range(112, 122)

TOML_DEFAULT, TOML_COMMENT, TOML_TABLE, TOML_KEY = 128, 129, 130, 131
TOML_STRING, TOML_ESCAPE, TOML_NUMBER = 132, 133, 134
TOML_BOOLEAN, TOML_DATETIME, TOML_OPERATOR, TOML_ERROR = 135, 136, 137, 138
TOML_BAND = range(128, 139)

SH_DEFAULT, SH_COMMENT, SH_KEYWORD, SH_BUILTIN, SH_FUNCTION = 144, 145, 146, 147, 148
SH_STRING, SH_ESCAPE, SH_VARIABLE, SH_NUMBER = 149, 150, 151, 152
SH_OPERATOR, SH_BACKTICK, SH_IDENT = 153, 154, 155
SH_BAND = range(144, 156)

PY_DEFAULT, PY_COMMENT, PY_KEYWORD, PY_BUILTIN, PY_DECORATOR = 160, 161, 162, 163, 164
PY_CLASSNAME, PY_FUNCNAME, PY_STRING, PY_ESCAPE = 165, 166, 167, 168
PY_TRIPLE, PY_NUMBER, PY_OPERATOR, PY_IDENT = 169, 170, 171, 172
PY_BAND = range(160, 173)


# --- helpers ----------------------------------------------------------------


def style_at(trowel, pos):
    return trowel.call("editor.get_style_at", {"pos": pos})["style"]


class Doc:
    """A fixture opened in Trowel, with offset lookup by substring."""

    def __init__(self, trowel, path: Path):
        self.trowel = trowel
        self.text = path.read_text()
        trowel.call("editor.open", {"path": str(path)})

    def offset(self, needle: str, after: str | None = None) -> int:
        start = self.text.index(after) if after else 0
        idx = self.text.index(needle, start)
        assert idx >= 0, f"{needle!r} not in fixture"
        return idx

    def style_of(self, needle: str, after: str | None = None) -> int:
        return style_at(self.trowel, self.offset(needle, after))


@pytest.fixture
def md(trowel, fixture_files):
    return Doc(trowel, fixture_files / "sample.md")


# --- Markdown ---------------------------------------------------------------


def test_markdown_block_constructs(md):
    assert md.style_of("# Heading one") == MD_HEADING
    assert md.style_of("- list item") == MD_LISTMARKER
    assert md.style_of("`inline code span`") == MD_CODESPAN
    assert md.style_of("*emphasis*") == MD_EMPHASIS
    assert md.style_of("**strong**") == MD_STRONG
    assert md.style_of("[link]") == MD_LINKTEXT
    assert md.style_of("(https://example.com)") == MD_LINKURL


def test_markdown_prose_is_not_lexed_as_turmeric(md):
    # The whole point of the change: prose in a .md file must land in the
    # Markdown band, not be styled by the Turmeric lexer.
    assert md.style_of("After the fenced block.") == MD_DEFAULT
    assert md.style_of("Some *emphasis*") == MD_DEFAULT


def test_markdown_fence_delimiters(md):
    assert md.style_of("```turmeric") == MD_FENCE


# --- the nested case: markdown -> turmeric -> C -----------------------------


def test_turmeric_fence_body_is_turmeric(md):
    assert md.style_of("def hi") == TUR_DEFINE


def test_inner_c_block_inside_turmeric_fence_is_c(md):
    # ```c opened *inside* a ```turmeric fence: the body is C, not Turmeric
    # and not flat code.
    assert md.style_of("int answer") == C_TYPE
    assert md.style_of("42;") == C_NUMBER


def test_equal_length_inner_fence_does_not_close_outer_block(md):
    # The inner ``` closes the C block only. Turmeric that follows it is still
    # inside the outer fence and must still highlight as Turmeric.
    assert md.style_of("def bye") == TUR_DEFINE


def test_outer_fence_closes_and_prose_resumes(md):
    # ...and once the real closing fence lands, we're back to markdown.
    assert md.style_of("After the fenced block.") == MD_DEFAULT


def test_four_backtick_fence_survives_inner_three_backticks(md):
    assert md.style_of("def outer") == TUR_DEFINE
    assert md.style_of("long wide") == C_TYPE
    assert md.style_of("def still-turmeric") == TUR_DEFINE
    assert md.style_of("Done.") == MD_DEFAULT


# --- C ----------------------------------------------------------------------


def test_c_file_highlights_as_c(trowel, fixture_files):
    doc = Doc(trowel, fixture_files / "sample.c")
    assert doc.style_of("#include") == C_PREPROC
    assert doc.style_of("/* A block comment. */") == C_COMMENT
    assert doc.style_of("int main") == C_TYPE
    assert doc.style_of("return") == C_KEYWORD
    assert doc.style_of('"hello') == C_STRING
    assert doc.style_of("\\n") == C_ESCAPE
    assert doc.style_of("0;") == C_NUMBER


# --- inline C in a plain .tur file ------------------------------------------


def test_turmeric_inline_c_block_delegates_to_c(trowel):
    # Multi-line ``` blocks in a .tur buffer: the fence stays CBlock-colored,
    # the body is scanned as C. (This input also used to hang the lexer.)
    trowel.call("editor.set_text",
                {"text": "(def x 1)\n```c\nint y = 2;\n```\n(def z 3)\n"})
    text = "(def x 1)\n```c\nint y = 2;\n```\n(def z 3)\n"
    assert style_at(trowel, text.index("```c")) == TUR_CBLOCK
    assert style_at(trowel, text.index("int y")) == C_TYPE
    assert style_at(trowel, text.index("2;")) == C_NUMBER
    assert style_at(trowel, text.index("def z")) == TUR_DEFINE


# --- JSON -------------------------------------------------------------------


def test_json_file_highlights_as_json(trowel, fixture_files):
    doc = Doc(trowel, fixture_files / "sample.json")
    assert doc.style_of('"name"') == JSON_KEY
    assert doc.style_of('"trowel"') == JSON_STRING
    assert doc.style_of("10,") == JSON_NUMBER
    assert doc.style_of("true") == JSON_LITERAL
    assert doc.style_of("null") == JSON_LITERAL
    # Braces take the shared rainbow palette when rainbow brackets are on.
    assert style_at(trowel, 0) in RAINBOW_BAND


# --- Justfile ---------------------------------------------------------------


def test_justfile_highlights_as_just(trowel, fixture_files):
    doc = Doc(trowel, fixture_files / "Justfile")
    assert doc.style_of("# Build everything") == JUST_COMMENT
    assert doc.style_of("preset :=") == JUST_ASSIGN
    assert doc.style_of('"macos-debug"') == JUST_STRING
    assert doc.style_of("build:") == JUST_RECIPE
    assert doc.style_of("configure\n") == JUST_DEP
    assert doc.style_of("{{preset}}") == JUST_INTERP


# --- sweet expressions ------------------------------------------------------


@pytest.fixture
def sweet(trowel, fixture_files):
    return Doc(trowel, fixture_files / "sweet_syntax.tur.sweet")


def test_sweet_markers_are_styled(sweet):
    # The two markers the reader actually accepts: GROUP/SPLIT `$`, and a lone
    # `\` SPLIT on its own line.
    assert sweet.style_of("$") == TUR_SWEET_MARKER
    assert sweet.style_of("\\") == TUR_SWEET_MARKER


def test_sweet_keywords_highlight_without_parens(sweet):
    # Keyword styling is position-independent, so a paren-less `def` line reads
    # the same as `(def ...)`.
    assert sweet.style_of("def sweet-plain") == TUR_DEFINE
    assert sweet.style_of("defn sweet-add") == TUR_DEFINE


def test_sweet_neoteric_and_curly_infix(sweet):
    assert sweet.style_of("sweet-add(1 2)") == TUR_NEOTERIC
    assert sweet.style_of("{3 + 4}") in RAINBOW_BAND


def test_dollar_is_not_a_marker_in_plain_turmeric(trowel):
    # `$` is a legal symbol character, so outside sweet mode it must stay an
    # identifier rather than pick up the marker color.
    text = "(def x $)\n"
    trowel.call("editor.set_text", {"text": text})
    assert style_at(trowel, text.index("$")) != TUR_SWEET_MARKER


def test_dollar_prefixed_symbol_is_not_a_marker(trowel, tmp_path):
    # `$foo` is one symbol; only a standalone `$` is the GROUP marker.
    f = tmp_path / "dollar.tur.sweet"
    f.write_text("def $named 1\n")
    trowel.call("editor.open", {"path": str(f)})
    doc = Doc(trowel, f)
    assert doc.style_of("$named") == TUR_IDENT


def test_markdown_sweet_fence_delegates_to_sweet(md):
    assert md.style_of("def fenced-sweet") == TUR_DEFINE
    assert md.style_of("$", after="def fenced-sweet") == TUR_SWEET_MARKER


# --- #lang directive --------------------------------------------------------
#
# Trowel mirrors the toolchain's own precedence (elab_toplevel.c): an extension
# that names a non-default reader wins, otherwise the `#lang` line decides.


def lang_doc(trowel, tmp_path, name, body):
    f = tmp_path / name
    f.write_text(body)
    trowel.call("editor.open", {"path": str(f)})
    return Doc(trowel, f)


def test_lang_sweet_in_tur_file_selects_sweet(trowel, tmp_path):
    # `.tur` is the default reader, so the directive decides.
    doc = lang_doc(trowel, tmp_path, "a.tur", "#lang turmeric/sweet\ndef x $ + 1 2\n")
    assert doc.style_of("$") == TUR_SWEET_MARKER


def test_legacy_sweet_exp_alias_still_selects_sweet(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "b.tur", "#lang sweet-exp\ndef x $ + 1 2\n")
    assert doc.style_of("$") == TUR_SWEET_MARKER


def test_lang_turmeric_does_not_make_a_tur_file_sweet(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "c.tur", "#lang turmeric\n(def x $)\n")
    assert doc.style_of("$") != TUR_SWEET_MARKER


def test_sweet_extension_beats_a_plain_lang_directive(trowel, tmp_path):
    # `.tur.sweet` already names a non-default reader, so it wins and the
    # directive is only a redundant hint — matching how the file would run.
    doc = lang_doc(trowel, tmp_path, "d.tur.sweet",
                   "#lang turmeric\ndef x $ + 1 2\n")
    assert doc.style_of("$") == TUR_SWEET_MARKER


def test_bare_sweet_extension_is_plain_turmeric(trowel, tmp_path):
    # Turmeric's reader_type_from_extension only knows `.tur.sweet`; a bare
    # `.sweet` runs as ordinary Turmeric, so it must highlight that way.
    doc = lang_doc(trowel, tmp_path, "e.sweet", "def x $ + 1 2\n")
    assert doc.style_of("$") != TUR_SWEET_MARKER


def test_bare_sweet_extension_honors_lang_directive(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "f.sweet",
                   "#lang turmeric/sweet\ndef x $ + 1 2\n")
    assert doc.style_of("$") == TUR_SWEET_MARKER


def test_lang_line_after_shebang_is_honored(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "g.tur",
                   "#!/usr/bin/env tur\n#lang turmeric/sweet\ndef x $ + 1 2\n")
    assert doc.style_of("$") == TUR_SWEET_MARKER


def test_lang_layers_do_not_disturb_the_base(trowel, tmp_path):
    # Layer tokens follow the base name and don't change the reader.
    doc = lang_doc(trowel, tmp_path, "h.tur",
                   "#lang turmeric/sweet stringed\ndef x $ + 1 2\n")
    assert doc.style_of("$") == TUR_SWEET_MARKER


def test_lang_directive_is_ignored_below_line_one(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "i.tur",
                   "(def a 1)\n#lang turmeric/sweet\ndef x $ + 1 2\n")
    assert doc.style_of("$") != TUR_SWEET_MARKER


def test_typing_a_lang_line_switches_language_live(trowel):
    # Untitled buffer: no extension to go on, so the directive is the only
    # signal — and it has to take effect without a save.
    plain = "def x $ + 1 2\n"
    trowel.call("editor.set_text", {"text": plain})
    assert style_at(trowel, plain.index("$")) != TUR_SWEET_MARKER

    switched = "#lang turmeric/sweet\ndef x $ + 1 2\n"
    trowel.call("editor.set_text", {"text": switched})
    assert style_at(trowel, switched.index("$")) == TUR_SWEET_MARKER


def test_lang_line_in_a_markdown_file_is_not_a_directive(trowel, tmp_path):
    # Only Turmeric-family extensions consult the directive; elsewhere it is
    # just text.
    # (`#lang` is not a heading either — ATX headings need a space after the
    # hash — so this asserts the Markdown band rather than a specific style.)
    doc = lang_doc(trowel, tmp_path, "j.md", "#lang turmeric/sweet\n")
    assert doc.style_of("#lang turmeric/sweet") in MD_BAND


# --- shebang / #lang interaction --------------------------------------------
#
# reader.c accepts a Racket-style shebang only at byte 0, and only when the
# `#!` is followed by `/`, a blank, or end of line. It runs to end of line and
# nothing more, so `#lang` on the next line is still in the leading position.
# Trowel's styling has to agree with that, or the colors would contradict the
# reader Trowel itself picked.


def test_shebang_is_styled_as_a_comment(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "sb.tur", "#!/usr/bin/env tur\n(def x 1)\n")
    assert doc.style_of("#!/usr/bin/env tur") == 1  # TurStyle::LineComment


def test_shebang_does_not_swallow_the_next_line(trowel, tmp_path):
    # The shebang stops at its own newline: the code below it is still code.
    doc = lang_doc(trowel, tmp_path, "sb2.tur", "#!/usr/bin/env tur\n(def x 1)\n")
    assert doc.style_of("def x") == TUR_DEFINE


def test_lang_after_shebang_is_styled_as_a_directive(trowel, tmp_path):
    body = "#!/usr/bin/env tur\n#lang turmeric/sweet\ndef x $ + 1 2\n"
    doc = lang_doc(trowel, tmp_path, "sb3.tur", body)
    assert doc.style_of("#lang") == 22  # TurStyle::LangDir
    # ...and it still selects the sweet reader.
    assert doc.style_of("$") == TUR_SWEET_MARKER


def test_lang_below_the_leading_position_is_not_a_directive(trowel, tmp_path):
    # A `#lang` on line 3 is ordinary source, so it must not be painted like
    # the directive it is not.
    doc = lang_doc(trowel, tmp_path, "sb4.tur",
                   "(def a 1)\n(def b 2)\n#lang turmeric/sweet\n")
    assert doc.style_of("#lang", after="(def b 2)") != 22


def test_shebang_only_counts_on_line_one(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "sb5.tur", "(def a 1)\n#!/bin/sh\n")
    assert doc.style_of("#!/bin/sh") != 1


def test_hash_bang_dispatch_form_is_not_a_shebang(trowel, tmp_path):
    # `#!fold-case`-style reader directives are exactly what reader.c's
    # "`/`, blank, or EOL" rule protects.
    doc = lang_doc(trowel, tmp_path, "sb6.tur", "#!fold-case\n(def x 1)\n")
    assert doc.style_of("#!fold-case") != 1


def test_shebang_picks_the_language_of_an_extensionless_file(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "runner",
                   "#!/usr/bin/env bash\nexport FOO=1\n")
    assert doc.style_of("export") == SH_BUILTIN


def test_env_dash_s_shebang_still_resolves_the_interpreter(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "runner2",
                   "#!/usr/bin/env -S python3 -u\nimport os\n")
    assert doc.style_of("import") == PY_KEYWORD


def test_extension_beats_the_shebang(trowel, tmp_path):
    # A name that already says what the file is wins; the shebang is only a
    # fallback for names that say nothing.
    doc = lang_doc(trowel, tmp_path, "weird.py", "#!/bin/sh\nimport os\n")
    assert doc.style_of("import") == PY_KEYWORD


# --- CMake ------------------------------------------------------------------


def test_cmake_file_highlights_as_cmake(trowel, fixture_files):
    doc = Doc(trowel, fixture_files / "sample.cmake")
    assert doc.style_of("# A CMake module.") == CM_COMMENT
    assert doc.style_of("cmake_minimum_required") == CM_COMMAND
    assert doc.style_of("${MY_SOURCES}") == CM_VARIABLE
    assert doc.style_of('"on apple') == CM_STRING
    assert doc.style_of("3.24") == CM_NUMBER
    assert doc.style_of("ON)") == CM_KEYWORD


def test_cmake_control_flow_is_a_keyword(trowel, fixture_files):
    doc = Doc(trowel, fixture_files / "sample.cmake")
    assert doc.style_of("if(APPLE") == CM_KEYWORD
    assert doc.style_of("endif") == CM_KEYWORD
    assert doc.style_of("AND") == CM_KEYWORD


def test_cmakelists_is_recognized_by_name(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "CMakeLists.txt",
                   "project(demo)\n")
    assert doc.style_of("project") == CM_COMMAND


# --- TOML -------------------------------------------------------------------


def test_toml_file_highlights_as_toml(trowel, fixture_files):
    doc = Doc(trowel, fixture_files / "sample.toml")
    assert doc.style_of("# A TOML document.") == TOML_COMMENT
    assert doc.style_of("title ") == TOML_KEY
    assert doc.style_of('"trowel"') == TOML_STRING
    assert doc.style_of("42") == TOML_NUMBER
    assert doc.style_of("true") == TOML_BOOLEAN
    assert doc.style_of("2026-07-31") == TOML_DATETIME
    assert doc.style_of("[package]") == TOML_TABLE
    assert doc.style_of("[[bin]]") == TOML_TABLE


def test_toml_multiline_string_spans_lines(trowel, tmp_path):
    body = 'desc = """\nstill inside\n"""\nafter = 1\n'
    doc = lang_doc(trowel, tmp_path, "ml.toml", body)
    assert doc.style_of("still inside") == TOML_STRING
    assert doc.style_of("after") == TOML_KEY


# --- sh ---------------------------------------------------------------------


def test_sh_file_highlights_as_sh(trowel, fixture_files):
    doc = Doc(trowel, fixture_files / "sample.sh")
    assert doc.style_of("#!/usr/bin/env bash") == SH_COMMENT
    assert doc.style_of("# A shell script.") == SH_COMMENT
    assert doc.style_of("set -euo") == SH_BUILTIN
    assert doc.style_of("greeting=") == SH_VARIABLE
    assert doc.style_of('"hello"') == SH_STRING
    assert doc.style_of("say_hi()") == SH_FUNCTION
    assert doc.style_of("${greeting}") == SH_VARIABLE
    assert doc.style_of("if [") == SH_KEYWORD


def test_bashrc_is_recognized_by_name(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, ".bashrc", "export PATH=/bin\n")
    assert doc.style_of("export") == SH_BUILTIN


# --- Python -----------------------------------------------------------------


def test_python_file_highlights_as_python(trowel, fixture_files):
    doc = Doc(trowel, fixture_files / "sample.py")
    assert doc.style_of("#!/usr/bin/env python3") == PY_COMMENT
    assert doc.style_of('"""Module docstring."""') == PY_TRIPLE
    assert doc.style_of("import os") == PY_KEYWORD
    assert doc.style_of("@staticmethod") == PY_DECORATOR
    assert doc.style_of("def greet") == PY_KEYWORD
    assert doc.style_of("greet(name)") == PY_FUNCNAME
    assert doc.style_of("42") == PY_NUMBER
    assert doc.style_of("print") == PY_BUILTIN
    assert doc.style_of('f"hello {name}"') == PY_STRING
    assert doc.style_of("Greeter") == PY_CLASSNAME


def test_python_triple_quoted_string_spans_lines(trowel, tmp_path):
    body = 'x = """\nstill inside\n"""\ny = 1\n'
    doc = lang_doc(trowel, tmp_path, "ml.py", body)
    assert doc.style_of("still inside") == PY_TRIPLE
    assert doc.style_of("y =") == PY_IDENT


# --- markdown fences for the new guests -------------------------------------


def test_markdown_delegates_new_language_fences(trowel, tmp_path):
    body = (
        "text\n\n"
        "```python\n"
        "import os\n"
        "```\n\n"
        "```toml\n"
        "key = 1\n"
        "```\n\n"
        "```bash\n"
        "export FOO=1\n"
        "```\n\n"
        "```cmake\n"
        "project(demo)\n"
        "```\n"
    )
    doc = lang_doc(trowel, tmp_path, "guests.md", body)
    assert doc.style_of("import os") == PY_KEYWORD
    assert doc.style_of("key = 1") == TOML_KEY
    assert doc.style_of("export FOO=1") == SH_BUILTIN
    assert doc.style_of("project(demo)") == CM_COMMAND
    # ...and the markdown around them is still markdown.
    assert doc.style_of("text") == MD_DEFAULT


# --- dispatch ---------------------------------------------------------------


def test_unknown_extension_falls_back_to_turmeric(trowel, tmp_path):
    f = tmp_path / "notes.xyz"
    f.write_text("(def hi 42)\n")
    trowel.call("editor.open", {"path": str(f)})
    assert style_at(trowel, 1) == TUR_DEFINE


def test_plain_text_has_no_highlighting(trowel, tmp_path):
    f = tmp_path / "notes.txt"
    f.write_text("(def hi 42)\n")
    trowel.call("editor.open", {"path": str(f)})
    # PlainText: everything is style 0 (default), no Turmeric highlighting.
    assert style_at(trowel, 1) == 0


def test_language_switches_when_saved_under_a_new_extension(trowel, tmp_path):
    src = tmp_path / "notes.tur"
    src.write_text("# Heading one\n")
    trowel.call("editor.open", {"path": str(src)})
    # As Turmeric, a leading '#' is not a heading.
    assert style_at(trowel, 0) != MD_HEADING

    trowel.call("editor.save_as", {"path": str(tmp_path / "notes.md")})
    assert style_at(trowel, 0) == MD_HEADING


# --- R7RS Scheme ------------------------------------------------------------
#
# `.scm` is Scheme by extension upstream, so these fixtures carry no `#lang`
# line -- the headerless case, which is the idiomatic one for a Scheme file.


@pytest.fixture
def scm(trowel, fixture_files):
    return Doc(trowel, fixture_files / "scheme_syntax.scm")


def test_scm_extension_selects_the_scheme_scanner(scm):
    # `define` is a Scheme definition form. It is also in Turmeric's own define
    # set, so this alone would not prove the dialect -- the tests below do.
    assert scm.style_of("define scm-bool") == TUR_DEFINE


def test_scheme_booleans_both_spellings(scm):
    assert scm.style_of("#true") == TUR_BOOLEAN
    assert scm.style_of("#f", after="scm-bool-short") == TUR_BOOLEAN


def test_scheme_character_literals(scm):
    # The whole `#\x41`, not `#\x` with `41` left over as a number.
    assert scm.style_of("#\\x41") == TUR_CHAR_LIT
    assert scm.style_of("41", after="#\\x") == TUR_CHAR_LIT
    assert scm.style_of("#\\space") == TUR_CHAR_LIT


def test_scheme_vector_and_bytevector_prefixes(scm):
    assert scm.style_of("#(") == TUR_SCHEME_VECTOR
    assert scm.style_of("#u8(") == TUR_SCHEME_VECTOR
    # Only the prefix is painted: the paren still counts toward nesting depth,
    # so it keeps its rainbow colour and the unmatched-closer check stays honest.
    assert scm.style_of("(", after="scm-vec #") in RAINBOW_BAND


def test_scheme_numeric_prefixes_and_rationals(scm):
    assert scm.style_of("#xff") == TUR_NUMBER
    assert scm.style_of("#e1.5") == TUR_NUMBER
    # `1/2` is one number, not `1` followed by the symbol `/2`.
    assert scm.style_of("1/2") == TUR_NUMBER
    assert scm.style_of("/2", after="scm-ratio ") == TUR_NUMBER


def test_scheme_bar_symbols(scm):
    assert scm.style_of("|a bar symbol|") == TUR_BAR_SYMBOL


def test_scheme_unquote_forms(scm):
    assert scm.style_of(",scm-hex") == TUR_QUOTE
    assert scm.style_of(",@") == TUR_QUOTE


def test_scheme_special_forms(scm):
    assert scm.style_of("define-record-type") == TUR_DEFINE
    assert scm.style_of("display") == TUR_BUILTIN
    assert scm.style_of("newline") == TUR_BUILTIN


def test_scheme_shares_turmerics_comment_forms(scm):
    assert scm.style_of("#| a block comment") == TUR_BLOCK_COMMENT
    assert scm.style_of("#;(a datum comment") == TUR_LINE_COMMENT


def test_turmeric_only_syntax_is_not_applied_in_a_scheme_buffer(scm):
    # A leading-colon identifier is a legal Scheme symbol, not a keyword
    # literal. Painting it as one would be a claim about the wrong language --
    # there is an upstream report on exactly this spelling.
    assert scm.style_of(":not-a-keyword-literal") == TUR_IDENT


def test_turmeric_buffer_keeps_its_own_syntax(trowel, tmp_path):
    # The other side of the gate: the Scheme rules must not leak into a `.tur`
    # buffer. `|>` stays an operator and `|a b|` is not a bar symbol.
    doc = lang_doc(trowel, tmp_path, "gate.tur",
                   "(def piped (|> 1 2))\n(def kw :a-keyword)\n(def meta ^mut)\n")
    assert doc.style_of("|>") == TUR_OPERATOR
    assert doc.style_of(":a-keyword") == TUR_KEYWORD_LIT
    assert doc.style_of("^mut") == TUR_METADATA


def test_lang_r7rs_in_a_tur_file_selects_scheme(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "b.tur",
                   "#lang r7rs\n(define v #(1 2))\n(define n 1/2)\n")
    assert doc.style_of("#(") == TUR_SCHEME_VECTOR
    assert doc.style_of("1/2") == TUR_NUMBER


def test_lang_r7rs_sweet_is_scheme_and_sweet_at_once(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "c.tur",
                   "#lang r7rs/sweet\ndefine v #(1 2)\ndisplay $ + 1 2\n")
    assert doc.style_of("#(") == TUR_SCHEME_VECTOR
    assert doc.style_of("$") == TUR_SWEET_MARKER


def test_lang_saffron_sweet_is_sweet(trowel, tmp_path):
    # The regression this part exists for: `#lang saffron/sweet` used to fall
    # through to plain, UNSWEET Turmeric, so the reader's markers went unpainted.
    doc = lang_doc(trowel, tmp_path, "d.tur",
                   "#lang saffron/sweet\ndef x $ + 1 2\n")
    assert doc.style_of("$") == TUR_SWEET_MARKER
    assert doc.style_of("def") == TUR_DEFINE


def test_lang_saffron_is_turmeric_to_the_scanner(trowel, tmp_path):
    doc = lang_doc(trowel, tmp_path, "e.tur",
                   "#lang saffron\n(defn double [x] (* x 2))\n")
    assert doc.style_of("defn") == TUR_DEFINE


# --- Markdown fences for the dialects ---------------------------------------


def test_markdown_sweet_exp_fence_is_recognized(trowel, tmp_path):
    # ```sweet-exp is the tag Turmeric's own guides use for every
    # sweet-expression example -- 996 of them. It was not in the accept list, so
    # all of them rendered as undifferentiated code.
    body = "text\n\n```sweet-exp\ndef fenced-x 7\ndef g $ + 1 2\n```\n"
    doc = lang_doc(trowel, tmp_path, "sweetfence.md", body)
    assert doc.style_of("def fenced-x") == TUR_DEFINE
    assert doc.style_of("$") == TUR_SWEET_MARKER


def test_markdown_scheme_fence_delegates_to_scheme(trowel, tmp_path):
    body = "text\n\n```scheme\n(define v #(1 2))\n(display v)\n```\n"
    doc = lang_doc(trowel, tmp_path, "schemefence.md", body)
    assert doc.style_of("#(") == TUR_SCHEME_VECTOR
    assert doc.style_of("display") == TUR_BUILTIN


def test_markdown_guest_applies_to_every_line_of_a_fence(trowel, tmp_path):
    """Every pre-existing fence assertion sits on the fence's FIRST line, which
    is the line that sets the per-line guest field rather than reading it back.
    This covers the later lines.

    It does not, however, catch the pack/unpack mask asymmetry described in
    lexer_adapter.cpp -- that was checked by reintroducing the narrow mask, and
    this test still passed, because `editor.get_style_at` has Scintilla
    colourise from position 0 and the packed seed is never consulted. Kept for
    what it does cover.
    """
    body = (
        "text\n\n"
        "```python\n"
        "import os\n"
        "import sys\n"
        "```\n\n"
        "```bash\n"
        "export FOO=1\n"
        "export BAR=2\n"
        "```\n"
    )
    doc = lang_doc(trowel, tmp_path, "secondline.md", body)
    assert doc.style_of("import os") == PY_KEYWORD
    assert doc.style_of("import sys") == PY_KEYWORD      # the line that regressed
    assert doc.style_of("export FOO=1") == SH_BUILTIN
    assert doc.style_of("export BAR=2") == SH_BUILTIN    # ditto
