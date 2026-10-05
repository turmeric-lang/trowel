"""P5 — syntax plugins: data-driven Scintilla lexer."""

import pytest
import json

JSON_SYNTAX = r''';; syntax/json/syntax.tur — a data-driven JSON syntax descriptor.
(trowel-define-syntax
  "json"
  "json,json5,jsonc"
  "keywords" ""
  "line-comment" ""
  "block-comment-open" ""
  "block-comment-close" ""
  "string-delim" "\""
  "string-escape" "\\"
  "operators" "{}[]:,"
  "fold-open" "{["
  "fold-close" "}]"
  "numbers" true)
'''


@pytest.fixture
def json_syntax(tmp_path):
    """Create the JSON syntax plugin before Trowel launches."""
    syntax_dir = tmp_path / "home" / ".trowel" / "syntax" / "json"
    syntax_dir.mkdir(parents=True, exist_ok=True)
    (syntax_dir / "syntax.tur").write_text(JSON_SYNTAX)


def test_json_syntax_loads(json_syntax, trowel, tmp_path):
    """Opening a .json file uses the plugin syntax lexer, not the built-in."""
    path = tmp_path / "test.json"
    path.write_text('{}')
    trowel.call("editor.open", {"path": str(path)})
    # Type some JSON content
    trowel.call("editor.set_text", {"text": '{"key": "value"}'})
    # Get the style at position 0 (the opening brace) — should be an operator
    # style, not the default (0).
    style = trowel.call("editor.get_style_at", {"pos": 0})
    assert style["style"] != 0, f"Expected non-default style at pos 0, got {style}"


def test_json_string_highlighted(json_syntax, trowel, tmp_path):
    """String values in JSON get the string style."""
    path = tmp_path / "test.json"
    path.write_text('{"name": "test"}')
    trowel.call("editor.open", {"path": str(path)})
    # Position 0 is '{', position 1 is '"'. The string should have a
    # non-default style.
    style = trowel.call("editor.get_style_at", {"pos": 1})
    assert style["style"] != 0, f"Expected string style at pos 1, got {style}"
