"""Help > Examples: each bundled example opens in its own dialect and runs."""

import pytest

# (menu title, a line the example prints, the base dialect its #lang names)
EXAMPLES = [
    ("Hello, Turmeric", "Hello, world!", "turmeric"),
    ("Functions and Control Flow", "3628800", "turmeric"),
    ("Option and Result", "99", "turmeric"),
    ("Structs and Vectors", "3", "turmeric"),
    ("Algebraic Effects", "-- logged", "turmeric"),
    ("Sweet Syntax", "3628800", "turmeric/sweet"),
    ("R7RS Scheme", "3628800", "r7rs"),
]


def open_example(trowel, title):
    trowel.call("menu.invoke", {"path": ["Help", "Examples", title]})


@pytest.mark.parametrize("title,expected,base", EXAMPLES)
def test_example_opens_and_runs(trowel, title, expected, base):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    open_example(trowel, title)
    assert trowel.call("lang.get")["base"] == base
    trowel.call("run.buffer")
    hit = trowel.wait_output(expected, timeout_ms=15000)
    assert expected in hit["matched"]


def test_example_opens_in_a_new_tab_without_a_save_prompt(trowel):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    before = trowel.call("editor.get_text")["text"]
    open_example(trowel, "Hello, Turmeric")
    after = trowel.call("editor.get_text")["text"]
    assert after != before and "Hello, Turmeric" in after
    # Untouched, so closing it must not leave the window waiting on a dialog.
    trowel.call("menu.invoke", {"path": ["File", "Close Tab"]})
    assert trowel.call("editor.get_text")["text"] == before
