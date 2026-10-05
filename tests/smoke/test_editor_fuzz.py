"""Randomised hammering of the editor and the whole control surface.

The point is not to assert behaviour. It is to assert that Trowel is *still
running* — a crash takes the control socket with it, so every operation here
doubles as a liveness check and the first one to raise is the one that killed
it.

Why this exists: a completion label containing `?` aborted the debug build,
because Scintilla's default autocomplete type separator is `?` and Turmeric
spells its predicates `empty?`. Nothing in the suite typed a `?` and then
opened a completion list, so nothing caught it. That bug is a *class*, not an
instance: the crashes come from features (completions, bracket guides, markers,
rename, the debugger) meeting buffer states no hand-written test thought to
construct. A fuzzer constructs them by not thinking.

Deterministic: each case runs a fixed seed, so a failure reproduces exactly and
the printed transcript replays it. `TROWEL_FUZZ_SEEDS` and `TROWEL_FUZZ_OPS`
crank it up for a longer local soak without slowing CI down.
"""

import os
import random
import string

import pytest

from trowel_ctl import ControlError

# Kept small enough that the suite stays a suite. A soak run is
# TROWEL_FUZZ_SEEDS=200 TROWEL_FUZZ_OPS=400.
SEEDS = [int(s) for s in os.environ.get("TROWEL_FUZZ_SEEDS_LIST", "").split(",") if s] \
        or list(range(int(os.environ.get("TROWEL_FUZZ_SEEDS", "6"))))
OPS_PER_CASE = int(os.environ.get("TROWEL_FUZZ_OPS", "70"))
# A couple of slow operations per case is ordinary; a stream of them means the
# UI thread is wedged, which is worth failing on even though nothing crashed.
MAX_STALLS = int(os.environ.get("TROWEL_FUZZ_MAX_STALLS", "4"))

# Characters chosen for what they mean to *this* editor, not for coverage of
# Unicode. Every one of these is load-bearing somewhere: brackets drive the
# pair-guide scan and the rainbow lexer, `;` and `"` drive the comment/string
# styling the scan consults, `?`/`!`/`-`/`*` are ordinary in Turmeric
# identifiers, `#` starts `#lang`, and the non-ASCII ones exercise the byte-vs-
# codepoint boundary that every `pos` in the control API sits on.
ALPHABET = (
    string.ascii_letters + string.digits +
    "()[]{}" * 3 +          # weighted: bracket handling is the richest code path
    "?!-*/+<>=_" * 2 +
    ' ;"\'`,@#\\.:^&|~%$' +
    "\n\n\t" +
    "λ→αβ…—’"               # multi-byte, to catch byte/char offset confusion
)

KEYS = [
    "Backspace", "Delete", "Return", "Tab", "Home", "End",
    "Left", "Right", "Up", "Down", "PageUp", "PageDown", "Escape",
]

# Menu actions, as they appear in the bar with the `&` accelerators stripped.
MENU_ACTIONS = [
    ["Run", "Run Buffer"], ["Run", "Run Selection"], ["Run", "Trace Buffer"],
    ["Run", "Toggle Breakpoint"], ["Run", "Format File"],
    ["Run", "Complete Symbol"], ["Run", "Show Documentation"],
    ["Run", "Show Symbols"], ["Run", "Go to Definition"],
    ["Run", "Find References"], ["Run", "Rename Symbol"],
    ["Run", "Go Back"], ["Run", "Go Forward"],
    ["Run", "Clear REPL"],
    ["View", "Toggle Split Orientation"], ["View", "Show/Hide REPL"],
]

SEED_TEXT = (
    "#lang turmeric\n"
    "(defn empty? [xs] (nil? xs))\n"
    "(defn add [a : int b : int] : int\n"
    "  (let [s (+ a b)]\n"
    "    s))\n"
    "(defn main [] : int\n"
    '  (println "hi")\n'
    "  (add 3 4))\n"
)


class Dead(Exception):
    """The app stopped answering — i.e. it crashed."""


class Stalled(Exception):
    """One operation outran the socket timeout, but the app is still there."""


def _socket_path(trowel):
    return trowel._ctl._path  # noqa: SLF001 — the fixture exposes no accessor


def _still_serving(trowel) -> bool:
    """Ask on a *fresh* connection whether the app is alive.

    A timed-out socket has an unread reply sitting in it, so every later call on
    it fails too — a single slow operation would otherwise look exactly like a
    crash and cascade into one. Only a refused connection means the process is
    actually gone.
    """
    from trowel_ctl import TrowelCtl
    try:
        probe = TrowelCtl(_socket_path(trowel), timeout=20.0)
    except OSError:
        return False
    try:
        probe.call("ping")
        return True
    except Exception:
        return False
    finally:
        probe.close()


def _reconnect(trowel):
    from trowel_ctl import TrowelCtl
    return TrowelCtl(_socket_path(trowel), timeout=20.0)


def _classify(trowel, exc) -> None:
    """Turn a transport failure into Dead or Stalled. Never returns normally."""
    if _still_serving(trowel):
        raise Stalled(str(exc)) from exc
    raise Dead(str(exc)) from exc


def _alive(trowel):
    """Cheapest round trip there is. Raises Dead if the process is gone."""
    try:
        trowel.call("ping")
    except (BrokenPipeError, ConnectionResetError, OSError) as exc:
        _classify(trowel, exc)


def _op(trowel, rng, log):
    """Run one random operation. ControlErrors are fine; death is not."""
    def do(desc, fn):
        log.append(desc)
        try:
            fn()
        except ControlError:
            # A refusal is a correct answer — "no symbol here", "not
            # debuggable", "bad args". Only silence means a crash.
            pass
        except (BrokenPipeError, ConnectionResetError, OSError) as exc:
            _classify(trowel, exc)

    choice = rng.random()
    if choice < 0.34:
        text = "".join(rng.choice(ALPHABET) for _ in range(rng.randint(1, 12)))
        do(f"type({text!r})", lambda: trowel.type(text))
    elif choice < 0.52:
        key = rng.choice(KEYS)
        mods = rng.choice([None, None, None, ["ctrl"], ["shift"], ["ctrl", "shift"]])
        do(f"press({key},{mods})", lambda: trowel.press(key, mods))
    elif choice < 0.64:
        # Deliberately unclamped: an out-of-range offset is exactly the kind of
        # argument a real client sends after an edit races a request.
        pos = rng.randint(-50, 4000)
        do(f"set_cursor({pos})", lambda: trowel.call("editor.set_cursor", {"pos": pos}))
    elif choice < 0.72:
        a, b = rng.randint(-20, 3000), rng.randint(-20, 3000)
        do(f"set_selection({a},{b})",
           lambda: trowel.call("editor.set_selection", {"start": a, "end": b}))
    elif choice < 0.86:
        path = rng.choice(MENU_ACTIONS)
        do(f"menu({'/'.join(path)})",
           lambda: trowel.call("menu.invoke", {"path": path}))
    elif choice < 0.92:
        # The read-back surface: these paint indicators and markers from
        # whatever state the buffer is in, which is where the guide and marker
        # code gets exercised.
        cmd = rng.choice(["lsp.decorations", "lsp.diagnostics", "editor.get_text",
                          "editor.get_cursor", "editor.get_selection",
                          "nav.history", "debug.breakpoints", "window.list"])
        do(f"read({cmd})", lambda: trowel.call(cmd))
    elif choice < 0.96:
        line = rng.randint(-5, 40)
        do(f"breakpoint({line})",
           lambda: trowel.call("debug.breakpoint.toggle",
                               {"path": trowel.call("window.geometry")["file_path"],
                                "line": line}))
    else:
        do("get_style_at",
           lambda: trowel.call("editor.get_style_at",
                               {"pos": rng.randint(0, 3000)}))


@pytest.mark.parametrize("seed", SEEDS)
def test_editor_survives_random_input(trowel, fixture_files, tmp_path, seed):
    rng = random.Random(seed)
    prog = tmp_path / f"fuzz_{seed}.tur"
    prog.write_text(SEED_TEXT)
    trowel.call("editor.open", {"path": str(prog)})
    _alive(trowel)

    log: list[str] = []
    stalls: list[str] = []
    try:
        for i in range(OPS_PER_CASE):
            try:
                _op(trowel, rng, log)
            except Stalled as exc:
                # Alive, just slow: some operations legitimately block (a run
                # spawns `tur`, a symbol lookup waits on the language server).
                # Recorded rather than failed, and the connection is replaced
                # because a timed-out one never recovers.
                stalls.append(f"{log[-1] if log else '?'}: {exc}")
                if len(stalls) > MAX_STALLS:
                    pytest.fail(
                        f"seed {seed}: {len(stalls)} operations outran the "
                        f"socket timeout — the UI is blocking, even if it is "
                        f"not crashing:\n  " + "\n  ".join(stalls))
                trowel._ctl = _reconnect(trowel)  # noqa: SLF001
                continue
            # Liveness every few ops rather than every op: the round trip
            # dominates the runtime otherwise, and the transcript still narrows
            # the culprit to a handful of operations.
            if i % 3 == 0:
                _alive(trowel)
        _alive(trowel)
    except Dead as exc:
        transcript = "\n  ".join(log[-25:])
        pytest.fail(
            f"Trowel died during fuzz seed {seed} after {len(log)} ops: {exc}\n"
            f"Last operations (replay with TROWEL_FUZZ_SEEDS_LIST={seed}):\n"
            f"  {transcript}"
        )


@pytest.mark.parametrize("seed", SEEDS)
def test_fold_levels_match_full_relex(trowel, tmp_path, seed):
    """After random edits, incremental fold levels equal a full re-lex.

    This is the property that catches invalidation bugs: if the incremental
    lexer skips a line, its fold level drifts from what a full re-lex
    produces.
    """
    rng = random.Random(seed)
    prog = tmp_path / f"fold_fuzz_{seed}.tur"
    prog.write_text(SEED_TEXT)
    trowel.call("editor.open", {"path": str(prog)})
    _alive(trowel)

    log: list[str] = []
    stalls: list[str] = []
    try:
        for i in range(OPS_PER_CASE):
            try:
                _op(trowel, rng, log)
            except Stalled as exc:
                stalls.append(f"{log[-1] if log else '?'}: {exc}")
                if len(stalls) > MAX_STALLS:
                    pytest.fail(
                        f"seed {seed}: {len(stalls)} operations outran the "
                        f"socket timeout:\n  " + "\n  ".join(stalls))
                trowel._ctl = _reconnect(trowel)  # noqa: SLF001
                continue

            # Every few ops, compare incremental vs full re-lex fold levels.
            if i % 5 == 4:
                incremental = trowel.call("editor.folds", {"force_relex": 0})
                full = trowel.call("editor.folds", {"force_relex": 1})
                if incremental != full:
                    pytest.fail(
                        f"seed {seed}, op {i}: incremental fold levels differ "
                        f"from full re-lex after {log[-1]!r}\n"
                        f"incremental: {incremental}\n"
                        f"full:        {full}"
                    )

            if i % 3 == 0:
                _alive(trowel)
        _alive(trowel)
    except Dead as exc:
        transcript = "\n  ".join(log[-25:])
        pytest.fail(
            f"Trowel died during fold fuzz seed {seed} after {len(log)} ops: "
            f"{exc}\nLast operations:\n  {transcript}"
        )
