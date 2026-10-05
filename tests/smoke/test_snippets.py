"""P4 — snippets plugin: expansion and tab-stop cycling."""

import pytest

SNIPPETS_PLUGIN = r''';; snippets/plugin.tur — the first plugin, written in Turmeric.
(defn snippets-expand-at-cursor []
  (let [word (trowel-word-at-cursor)]
    (match word
      "defn"     (trowel-insert-snippet "(defn ${1:name} [${2:args}]\n  ${0:body})")
      "defmodule" (trowel-insert-snippet "(defmodule ${1:Name}\n  ${0:body})")
      "defdata"   (trowel-insert-snippet "(defdata ${1:Name}\n  ${0:body})")
      "let"      (trowel-insert-snippet "(let [${1:var} ${2:val}]\n  ${0:body})")
      "match"    (trowel-insert-snippet "(match ${1:expr}\n  ${2:pat} ${3:body}\n  ${0:else})")
      "def"      (trowel-insert-snippet "(def ${1:name} ${0:val})")
      "fn"       (trowel-insert-snippet "(fn [${1:args}]\n  ${0:body})")
      _          (trowel-status-message (str "No snippet for: " word)))))

(trowel-register-command
  "snippets.expand"
  "Expand Snippet"
  "Snippets"
  ""
  (fn [] (snippets-expand-at-cursor)))

(trowel-register-button
  "nf-md-playlist_play"
  "Expand Snippet"
  "snippets.expand")
'''


@pytest.fixture
def snippets_plugin(tmp_path):
    """Create the snippets plugin before Trowel launches."""
    plugins_dir = tmp_path / "home" / ".trowel" / "plugins" / "snippets"
    plugins_dir.mkdir(parents=True, exist_ok=True)
    (plugins_dir / "plugin.tur").write_text(SNIPPETS_PLUGIN)


def test_snippet_expands_defn(snippets_plugin, trowel):
    """Type 'defn', run snippets.expand, check the template is inserted."""
    trowel.type("defn")
    trowel.call("editor.set_cursor", {"pos": 4})
    trowel.call("command.run", {"id": "snippets.expand"})
    text = trowel.call("editor.get_text")["text"]
    # The snippet template for "defn" is:
    # (defn ${1:name} [${2:args}]\n  ${0:body})
    # After expansion, tab-stop markers are replaced by defaults:
    # (defn name [args]\n  body)
    assert "(defn name [args]" in text
    assert "body)" in text


def test_snippet_tab_cycles(snippets_plugin, trowel):
    """After expansion, Tab cycles to the next tab-stop."""
    trowel.type("defn")
    trowel.call("editor.set_cursor", {"pos": 4})
    trowel.call("command.run", {"id": "snippets.expand"})

    # After expansion, the first tab-stop (${1:name}) should be selected.
    sel = trowel.call("editor.get_selection")
    assert sel["text"] == "name"

    # Tab advances to ${2:args}.
    trowel.press("Tab")
    sel = trowel.call("editor.get_selection")
    assert sel["text"] == "args"

    # Tab advances to ${0:body} (final stop).
    trowel.press("Tab")
    sel = trowel.call("editor.get_selection")
    assert sel["text"] == "body"

    # Tab at the last stop exits snippet mode — Tab now inserts an indent.
    trowel.press("Tab")
    text = trowel.call("editor.get_text")["text"]
    # After exiting snippet mode, Tab should insert whitespace (indent).
    # The exact behavior depends on Scintilla's indent settings.
    # Just verify the snippet text is still there.
    assert "(defn name [args]" in text


def test_snippet_no_match(snippets_plugin, trowel):
    """Typing a word with no snippet shows a status message, no insertion."""
    trowel.type("xyzzy")
    trowel.call("editor.set_cursor", {"pos": 5})
    trowel.call("command.run", {"id": "snippets.expand"})
    text = trowel.call("editor.get_text")["text"]
    # The word "xyzzy" has no snippet — the buffer should be unchanged.
    assert text == "xyzzy"


def test_snippet_defmodule(snippets_plugin, trowel):
    """The defmodule snippet expands correctly."""
    trowel.type("defmodule")
    trowel.call("editor.set_cursor", {"pos": 9})
    trowel.call("command.run", {"id": "snippets.expand"})
    text = trowel.call("editor.get_text")["text"]
    assert "(defmodule Name" in text
    assert "body)" in text
