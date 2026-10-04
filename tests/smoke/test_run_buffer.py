"""§3.5 — Run buffer: the core edit → run → inspect loop."""

from pathlib import Path


def test_run_buffer_loads_definitions(trowel, fixture_files: Path):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(fixture_files / "defs.tur")})
    trowel.call("run.buffer")
    trowel.wait_idle(quiet_ms=400, timeout_ms=5000)
    trowel.send("smoke-x")
    hit = trowel.wait_output("42", timeout_ms=3000)
    assert "42" in hit["matched"]


def test_run_buffer_untitled_gets_scratched(trowel):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.type("(def smoke-untitled 99)")
    trowel.call("run.buffer")
    trowel.wait_idle(quiet_ms=400, timeout_ms=5000)
    trowel.send("smoke-untitled")
    hit = trowel.wait_output("99", timeout_ms=3000)
    assert "99" in hit["matched"]


def test_run_sweet_buffer_runs_main(trowel, fixture_files: Path):
    # A clean, saved .tur.sweet file runs in place; `tur` picks the
    # sweet-expression reader off the extension.
    #
    # Asserted on the program's OUTPUT, not on a definition being readable
    # afterwards. That is the contract Run Buffer has: `:run` evaluates the file
    # and then invokes `main`. These two tests used to assert the definition
    # instead, which passed only because Run Buffer sent `(load ...)` for sweet
    # buffers -- a workaround for an upstream defect -- and `load` defines
    # without running. The weaker assertion could not tell a program that ran
    # from one that was merely defined, which is the bug that shipped.
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(fixture_files / "sweet_main.tur.sweet")})
    trowel.call("run.buffer")
    hit = trowel.wait_output("sweet main ran", timeout_ms=8000)
    assert "sweet main ran" in hit["matched"]


def test_run_dirty_sweet_buffer_keeps_sweet_extension(trowel, fixture_files: Path):
    # The dirty path writes a scratch file instead, which only parses as sweet
    # if it inherits the .tur.sweet suffix — the extension is the only signal
    # a scratch file has, since it carries no `#lang` line of its own unless the
    # buffer had one.
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(fixture_files / "sweet_main.tur.sweet")})
    trowel.call("editor.set_text",
                {"text": 'defn main []\n  println("sweet dirty ran")\n  0\n'})
    trowel.call("run.buffer")
    hit = trowel.wait_output("sweet dirty ran", timeout_ms=8000)
    assert "sweet dirty ran" in hit["matched"]


def test_run_sweet_buffer_leaves_its_definitions_in_the_session(
        trowel, fixture_files: Path):
    """`:run` on a sweet file leaves its definitions behind, same as a `.tur`.

    Asserted with a COMPLETE sweet expression. A bare `sweet-main-x` does not
    answer here, and reading that as "the definition is missing" is exactly the
    mistake the test this replaced was built on -- see the next test for what is
    actually going on.
    """
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(fixture_files / "sweet_main.tur.sweet")})
    trowel.call("run.buffer")
    trowel.wait_output("sweet main ran", timeout_ms=8000)
    trowel.wait_idle(quiet_ms=400, timeout_ms=5000)

    # Asserted against the rendered SCREEN, not wait_output, for the reason
    # test_run_selection_keeps_the_lang_directive gives below: the REPL colours
    # a result, so `=> 8` spans an ANSI boundary.
    #
    # `wait_output` is racy for such a pattern rather than simply broken, which
    # is worse. It checks the rendered screen FIRST -- where the codes are
    # already interpreted and `=> 8` is contiguous -- and only if the output has
    # not landed yet does it fall back to accumulating RAW bytes, where it never
    # matches. So it passes whenever the result beats the call and times out
    # whenever it does not. This test passed on the v0.61.0 pin and failed on
    # v0.62.0 purely on that coin-flip; nothing about either toolchain changed.
    trowel.send("(+ sweet-main-x 1)")
    trowel.wait_idle(quiet_ms=400, timeout_ms=8000)
    screen = trowel.call("repl.get_screen", {"lines": 40})["text"]
    assert "=> 8" in screen, screen


def test_run_selection_keeps_the_lang_directive(trowel, tmp_path: Path):
    # The selection starts below line 1, so the `#lang` line is not in it and
    # has to be re-attached. This uses a *layer* (`stringed`, which enables the
    # `#s"..."` literal) rather than a base dialect on purpose: the scratch
    # file's extension can carry the sweet base, but no extension can express a
    # layer, so only re-attaching the directive itself makes this pass.
    src = tmp_path / "sel.tur"
    src.write_text('#lang turmeric stringed\n(def ignored 1)\n(def sel-s #s"hi")\n')
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(src)})
    body = src.read_text()
    start = body.index("(def sel-s")
    trowel.call("editor.set_selection", {"start": start, "end": len(body)})
    trowel.call("run.selection")
    trowel.wait_idle(quiet_ms=400, timeout_ms=5000)
    trowel.send("sel-s")
    trowel.wait_idle(quiet_ms=400, timeout_ms=5000)
    # Asserted against the rendered screen rather than wait_output: the REPL
    # wraps string results in ANSI colour codes, so `=> "hi"` is contiguous
    # only after the terminal has interpreted them.
    screen = trowel.call("repl.get_screen", {"lines": 40})["text"]
    assert '=> "hi"' in screen, screen


def test_run_syntax_error_does_not_kill_repl(trowel, fixture_files: Path):
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(fixture_files / "syntax_error.tur")})
    trowel.call("run.buffer")
    trowel.wait_idle(quiet_ms=400, timeout_ms=5000)
    assert trowel.call("repl.is_running")["running"] is True
    trowel.send("(+ 1 1)")
    hit = trowel.wait_output("2", timeout_ms=3000)
    assert "2" in hit["matched"]


def test_run_buffer_invokes_main(trowel, tmp_path: Path):
    # The bug this guards: Run Buffer used `(load ...)`, which evaluates the
    # top-level forms and stops. A program shaped the way Turmeric programs are
    # shaped — a `defn main` and nothing else at the top level — therefore
    # defined `main`, printed `=> #<fn main>`, and never ran a line of it,
    # while Run Buffer reported success.
    src = tmp_path / "hasmain.tur"
    src.write_text(
        '(defn main [] : int\n'
        '  (println "main-ran-marker")\n'
        '  0)\n')
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(src)})
    trowel.call("run.buffer")
    hit = trowel.wait_output("main-ran-marker", timeout_ms=5000)
    assert "main-ran-marker" in hit["matched"]


def test_run_buffer_without_main_still_runs_top_level(trowel, tmp_path: Path):
    # The other half: auto-invoking main must not have made a script of bare
    # top-level forms stop working, and must not error about a missing `main`.
    src = tmp_path / "nomain.tur"
    src.write_text('(println "toplevel-ran-marker")\n')
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(src)})
    trowel.call("run.buffer")
    hit = trowel.wait_output("toplevel-ran-marker", timeout_ms=5000)
    assert "toplevel-ran-marker" in hit["matched"]
    trowel.wait_idle(quiet_ms=400, timeout_ms=5000)
    screen = trowel.call("repl.get_screen", {"lines": 40})["text"]
    assert "unbound symbol 'main'" not in screen, screen


def test_run_dirty_buffer_invokes_main(trowel, tmp_path: Path):
    # The scratch-file path has to invoke main too, not just the clean one.
    src = tmp_path / "dirtymain.tur"
    src.write_text('(defn main [] : int 0)\n')
    trowel.wait_output("turmeric>", timeout_ms=5000)
    trowel.call("editor.open", {"path": str(src)})
    trowel.call("editor.set_text", {"text":
        '(defn main [] : int\n'
        '  (println "dirty-main-marker")\n'
        '  0)\n'})
    trowel.call("run.buffer")
    hit = trowel.wait_output("dirty-main-marker", timeout_ms=5000)
    assert "dirty-main-marker" in hit["matched"]
