"""Help > Turmeric Documentation: the bundled docs pack in a tab."""


def open_docs(trowel, **args):
    trowel.call("menu.invoke", {"path": ["Help", "Turmeric Documentation"]})
    return trowel.call("docs.open", args)


def test_menu_opens_the_pack_on_its_home_page(trowel):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("menu.invoke", {"path": ["Help", "Turmeric Documentation"]})
    state = trowel.call("docs.state")
    assert state["article"] == ""
    assert state["version"]
    # The home page leads on the quickstart and the recently-added list.
    assert "Start here" in state["text"]
    assert "Recently Added" in state["text"]
    assert len(state["recent"]) == 10


def test_second_open_reuses_the_tab(trowel):
    trowel.wait_output("turmeric>", timeout_ms=5000)

    def tab_count():
        return trowel.call("window.list")["windows"][0]["tab_count"]

    open_docs(trowel)
    after_first = tab_count()
    open_docs(trowel, article="quickstart")
    assert tab_count() == after_first


def test_article_by_slug_renders_its_body(trowel):
    state = open_docs(trowel, article="quickstart")
    assert state["article"] == "guides/quickstart.html"
    assert "Turmeric is a Lisp-family language" in state["text"]


def test_code_toggle_chrome_is_gone_and_one_syntax_shows(trowel):
    state = open_docs(trowel, article="quickstart", code_syntax="turmeric")
    assert "(+ 1 2)" in state["text"]
    assert "{1 + 2}" not in state["text"]
    assert "sweet-exp" not in state["text"]  # the toggle buttons
    state = trowel.call("docs.open", {"code_syntax": "sweet-exp"})
    assert "{1 + 2}" in state["text"]
    assert "(+ 1 2)" not in state["text"]


def test_search_ranks_the_exact_module_first(trowel):
    state = open_docs(trowel, query="arc")
    assert state["results"][0] == "api/arc.html"


def test_search_matches_api_symbols_and_requires_every_term(trowel):
    state = open_docs(trowel, query="arc-downgrade")
    assert "api/arc.html" in state["results"]
    state = trowel.call("docs.open", {"query": "arc-downgrade zzzznotaword"})
    assert state["results"] == []


def test_clearing_search_restores_the_nav_tree(trowel):
    state = open_docs(trowel, query="arc")
    narrowed = len(state["results"])
    state = trowel.call("docs.open", {"query": ""})
    assert len(state["results"]) > narrowed
