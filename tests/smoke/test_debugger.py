"""Phase 1 of debugger-support.md — headless debug session.

Drives `debug.start` / `debug.status` / `debug.stop` through the control
socket against a real fixture. Phase 1 is "run the current file under the
interpreter with captured output": no breakpoints, no stack, no stepping.
The test proves transport, lifecycle, and teardown before any UI is built on
top of it.
"""

from pathlib import Path

import pytest

from trowel_ctl import ControlError

# A debug session spawns a real `tur dap` subprocess that runs a real program.
DEBUG_MS = 15000


def _open(trowel, path: Path) -> None:
    trowel.call("editor.open", {"path": str(path)})


def test_debug_runs_a_program_and_reports_its_exit_code(trowel, fixture_files: Path):
    """The happy path: launch, run to completion, see the exit code."""
    _open(trowel, fixture_files / "trace_trivial.tur")
    trowel.call("debug.start", {"timeout_ms": DEBUG_MS})

    # Poll until the session reports terminated. The program runs to
    # completion asynchronously; debug.status reflects the live state.
    import time
    deadline = time.time() + DEBUG_MS / 1000.0
    last = {}
    while time.time() < deadline:
        last = trowel.call("debug.status")
        if last.get("state") in ("terminated", "idle") and not last.get("running"):
            break
        time.sleep(0.05)
    assert last.get("running") is False, last
    # trace_trivial's main evaluates `(+ 1 2)` and returns 3; `tur dap`
    # propagates the program's real return value (constraint 10), so the exit
    # code is 3, not a fake 0 from a user stop.
    assert last.get("exit_code") == 3, last


def test_debug_captures_debuggee_output(trowel, fixture_files: Path):
    """`output` events stream into the session's output buffer."""
    # A program that prints something visible.
    prog = fixture_files / "debug_print.tur"
    prog.write_text('(defn main [] : int\n  (println "hello-from-debuggee")\n  0)\n')
    try:
        _open(trowel, prog)
        trowel.call("debug.start", {"timeout_ms": DEBUG_MS})
        import time
        deadline = time.time() + DEBUG_MS / 1000.0
        out = ""
        while time.time() < deadline:
            st = trowel.call("debug.status")
            out = st.get("output", "")
            if "hello-from-debuggee" in out:
                break
            if not st.get("running"):
                break
            time.sleep(0.05)
        assert "hello-from-debuggee" in out, st
    finally:
        prog.unlink(missing_ok=True)


def test_debug_stop_kills_a_running_session(trowel, fixture_files: Path):
    """stop_on_entry pauses; debug.stop then reports not-running."""
    # A program that loops so it stays alive long enough to stop.
    prog = fixture_files / "debug_loop.tur"
    prog.write_text(
        '(defn main [] : int\n  (let [^mut i 0]\n'
        '    (while (< i 1000000)\n      (set! i (+ i 1))))\n  0)\n')
    try:
        _open(trowel, prog)
        # stop_on_entry so we are paused (and thus stoppable) immediately.
        trowel.call("debug.start", {"stop_on_entry": True, "timeout_ms": DEBUG_MS})
        import time
        # Wait for paused.
        deadline = time.time() + 5.0
        paused = False
        while time.time() < deadline:
            st = trowel.call("debug.status")
            if st.get("state") == "paused":
                paused = True
                break
            time.sleep(0.05)
        assert paused, st

        trowel.call("debug.stop")
        st = trowel.call("debug.status")
        assert st.get("running") is False, st
        # A user-stopped session reports -1, not a fake 0 (constraint 10).
        assert st.get("exit_code") == -1, st
    finally:
        prog.unlink(missing_ok=True)


def test_debug_refuses_a_non_turmeric_file(trowel, fixture_files: Path):
    """The gate mirrors Run Buffer: a .py is not debuggable."""
    _open(trowel, fixture_files / "sample.py")
    with pytest.raises(ControlError) as exc:
        trowel.call("debug.start")
    assert exc.value.code == "not_debuggable"


# --- Phase 3: breakpoints and stepping -----------------------------------

def _wait_state(trowel, target, timeout=8.0):
    """Poll debug.status until `state` matches `target` or time out."""
    import time
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = trowel.call("debug.status")
        if st.get("state") == target:
            return st
        time.sleep(0.05)
    return trowel.call("debug.status")


def _wait_output(trowel, predicate, timeout=8.0):
    """Poll `debug.status`'s console transcript until `predicate(output)` or a
    timeout, and return the last transcript seen.

    A seek is answered asynchronously -- the adapter re-sends the whole
    transcript as `replayOutput` and the console swaps -- so there is no reply
    to await, only a state to observe. The two tests below used a fixed
    `time.sleep(0.8)` and asserted straight after, which is a bet that the
    round trip fits in 800ms: on a loaded machine it does not, and
    `test_seeking_backwards_rewinds_the_console` then failed on an empty
    transcript while passing in isolation.

    Polling is also what makes the ASSERTION honest. Waiting for
    `output == ""` and waiting for `"tick" in output` are different questions,
    and a shared sleep cannot tell "not yet" from "never" for either of them.
    """
    import time
    deadline = time.time() + timeout
    out = ""
    while time.time() < deadline:
        out = trowel.call("debug.status")["output"]
        if predicate(out):
            return out
        time.sleep(0.05)
    return out


def _step(trowel, kind="in", timeout=5.0):
    """Step, then wait for the resulting stop to actually land.

    Waiting on `state` does not work for stepping: a step is Paused → Paused,
    so the state never changes and a poll returns instantly with the *previous*
    stop's frames. Neither does waiting on the line, since a step often stays
    on one. `stop_count` is the only thing that moves exactly once per stop.
    """
    import time
    before = trowel.call("debug.status").get("stop_count", 0)
    trowel.call("debug.step", {"kind": kind})
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = trowel.call("debug.status")
        if st.get("stop_count", 0) > before:
            return st
        # A step that runs off the end of the program ends the session rather
        # than stopping again; that is a legitimate outcome, not a timeout.
        if not st.get("running"):
            return st
        time.sleep(0.02)
    return trowel.call("debug.status")


def test_breakpoint_toggle_round_trips_through_the_model(trowel, fixture_files: Path):
    """debug.breakpoint.toggle adds then removes; debug.breakpoints reads it."""
    path = fixture_files / "trace_trivial.tur"
    _open(trowel, path)
    r = trowel.call("debug.breakpoint.toggle", {"path": str(path), "line": 7})
    assert r["added"] is True
    bps = trowel.call("debug.breakpoints", {"path": str(path)})["breakpoints"]
    assert len(bps) == 1
    assert bps[0]["line"] == 7
    assert bps[0]["enabled"] is True
    r = trowel.call("debug.breakpoint.toggle", {"path": str(path), "line": 7})
    assert r["added"] is False
    bps = trowel.call("debug.breakpoints", {"path": str(path)})["breakpoints"]
    assert len(bps) == 0


def test_breakpoint_stops_a_running_program(trowel, fixture_files: Path):
    """A breakpoint set before launch causes a `stopped` event."""
    # A program with a loop so it would run past the breakpoint without one.
    prog = fixture_files / "debug_bp.tur"
    prog.write_text(
        '(defn main [] : int\n'
        '  (let [^mut i 0]\n'
        '    (while (< i 100)\n'
        '      (set! i (+ i 1))))\n'
        '  0)\n')
    try:
        _open(trowel, prog)
        # Set a breakpoint on the loop body line (4: the set!).
        trowel.call("debug.breakpoint.toggle", {"path": str(prog), "line": 5})
        trowel.call("debug.start", {"timeout_ms": DEBUG_MS})
        st = _wait_state(trowel, "paused", timeout=8.0)
        assert st["state"] == "paused", st
        trowel.call("debug.stop")
    finally:
        prog.unlink(missing_ok=True)


def test_step_over_advances_and_stays_paused(trowel, fixture_files: Path):
    """stop_on_entry pauses; step_over keeps us paused."""
    prog = fixture_files / "debug_step.tur"
    prog.write_text(
        '(defn main [] : int\n'
        '  (let [^mut i 0]\n'
        '    (set! i (+ i 1))\n'
        '    (set! i (+ i 1)))\n'
        '  0)\n')
    try:
        _open(trowel, prog)
        trowel.call("debug.start", {"stop_on_entry": True, "timeout_ms": DEBUG_MS})
        st = _wait_state(trowel, "paused", timeout=8.0)
        assert st["state"] == "paused", st
        st = _step(trowel, "over")
        assert st["state"] == "paused", st
        trowel.call("debug.continue")
        _wait_state(trowel, "idle", timeout=8.0)
    finally:
        prog.unlink(missing_ok=True)


def test_continue_runs_to_completion(trowel, fixture_files: Path):
    """stop_on_entry then continue reaches the end and exits."""
    _open(trowel, fixture_files / "trace_trivial.tur")
    trowel.call("debug.start", {"stop_on_entry": True, "timeout_ms": DEBUG_MS})
    st = _wait_state(trowel, "paused", timeout=8.0)
    assert st["state"] == "paused", st
    trowel.call("debug.continue")
    st = _wait_state(trowel, "idle", timeout=8.0)
    assert st["state"] in ("idle", "terminated"), st
    assert st.get("running") is False

def test_toggle_breakpoint_menu_action_uses_the_caret_line(trowel, fixture_files: Path):
    """Run ▸ Toggle Breakpoint (F9) sets one where the caret is.

    The gutter margin is the other way in, but it is 14px wide and nothing
    labels it — the menu action is the discoverable one, and it is the only
    path a keyboard user has.
    """
    path = fixture_files / "trace_trivial.tur"
    _open(trowel, path)
    text = trowel.call("editor.get_text")["text"]
    # Put the caret on the third line, wherever that falls.
    line3 = len("\n".join(text.split("\n")[:2]).encode("utf-8")) + 1
    trowel.call("editor.set_cursor", {"pos": line3})

    trowel.call("menu.invoke", {"path": ["Run", "Toggle Breakpoint"]})
    bps = trowel.call("debug.breakpoints", {"path": str(path)})["breakpoints"]
    assert [b["line"] for b in bps] == [3], bps

    # And it toggles: a second invocation on the same line clears it.
    trowel.call("menu.invoke", {"path": ["Run", "Toggle Breakpoint"]})
    bps = trowel.call("debug.breakpoints", {"path": str(path)})["breakpoints"]
    assert bps == []


def test_toggle_breakpoint_is_disabled_for_a_non_turmeric_file(trowel, fixture_files: Path):
    """Same gate as Debug Buffer: a breakpoint the debugger will never load."""
    _open(trowel, fixture_files / "sample.py")
    with pytest.raises(ControlError) as exc:
        trowel.call("menu.invoke", {"path": ["Run", "Toggle Breakpoint"]})
    assert exc.value.code == "action_disabled"


# --- Phase 4: inspection --------------------------------------------------

def test_stopping_populates_frames_and_variables(trowel, fixture_files: Path):
    """`stopped` is deferred until stackTrace and variables have both answered.

    The obvious spelling — emit on the event, refresh in the background — makes
    every consumer read an empty frame list exactly once per stop. This asserts
    the data is there at the first moment anything could look.
    """
    prog = fixture_files / "debug_frames.tur"
    prog.write_text(
        '(defn main [] : int\n'
        '  (let [^mut i 7]\n'
        '    (set! i (+ i 1)))\n'
        '  0)\n')
    try:
        _open(trowel, prog)
        trowel.call("debug.start", {"stop_on_entry": True, "timeout_ms": DEBUG_MS})
        assert _wait_state(trowel, "paused")["state"] == "paused"

        frames = trowel.call("debug.frames")
        assert frames["frames"], frames
        top = frames["frames"][0]
        assert top["name"] == "main", top
        assert top["path"].endswith("debug_frames.tur"), top
        assert top["line"] >= 1, top
        # The innermost frame is selected on every new stop.
        assert frames["selected"] == top["id"]
        trowel.call("debug.stop")
    finally:
        prog.unlink(missing_ok=True)


def test_variables_track_the_program(trowel, fixture_files: Path):
    """Stepping past a binding makes it show up in the locals."""
    prog = fixture_files / "debug_vars.tur"
    prog.write_text(
        '(defn main [] : int\n'
        '  (let [^mut i 7]\n'
        '    (set! i (+ i 1))\n'
        '    (set! i (+ i 1)))\n'
        '  0)\n')
    try:
        _open(trowel, prog)
        trowel.call("debug.start", {"stop_on_entry": True, "timeout_ms": DEBUG_MS})
        assert _wait_state(trowel, "paused")["state"] == "paused"

        # Step until `i` is bound, or give up — the point is that locals are
        # reported at all, not the exact step it appears on.
        import time
        seen = []
        for _ in range(6):
            seen = trowel.call("debug.variables")["variables"]
            if any(v["name"] == "i" for v in seen):
                break
            _step(trowel, "in")
        assert any(v["name"] == "i" for v in seen), seen
        trowel.call("debug.stop")
    finally:
        prog.unlink(missing_ok=True)


def test_evaluate_answers_in_the_selected_frame(trowel, fixture_files: Path):
    """The evaluate line round-trips through the adapter."""
    _open(trowel, fixture_files / "trace_trivial.tur")
    trowel.call("debug.start", {"stop_on_entry": True, "timeout_ms": DEBUG_MS})
    assert _wait_state(trowel, "paused")["state"] == "paused"

    r = trowel.call("debug.evaluate", {"expression": "(+ 1 2)"})
    assert r["ok"] is True, r
    assert "3" in r["result"], r
    trowel.call("debug.stop")


def test_frames_are_empty_while_running(trowel, fixture_files: Path):
    """Frames belong to a stop; a running program has none."""
    _open(trowel, fixture_files / "trace_trivial.tur")
    trowel.call("debug.start", {"timeout_ms": DEBUG_MS})
    _wait_state(trowel, "idle", timeout=8.0)
    assert trowel.call("debug.frames")["frames"] == []


# --- T3: reverse execution over a recording -------------------------------
#
# `launch` with "replay": true records the whole run and then serves
# stackTrace / scopes / variables from a trace cursor. The decoder stays in C
# (editor-intelligence.md §5.4 option B); this end only speaks DAP.

def test_replay_session_reports_itself_as_a_recording(trowel, fixture_files: Path):
    _open(trowel, fixture_files / "trace_loop.tur")
    trowel.call("debug.start", {"replay": True, "timeout_ms": DEBUG_MS})
    st = _wait_state(trowel, "paused", timeout=15.0)
    assert st["state"] == "paused", st
    assert st["replay"] is True, st
    trowel.call("debug.stop")


def test_step_back_moves_the_cursor_backwards(trowel, fixture_files: Path):
    """Forward three, back one, and land where forward-two left off.

    The adapter maps replay steps onto *lines*, deliberately: an editor draws
    a line marker, and keypresses that leave it in place read as a hang.
    """
    _open(trowel, fixture_files / "trace_loop.tur")
    trowel.call("debug.start", {"replay": True, "timeout_ms": DEBUG_MS})
    assert _wait_state(trowel, "paused", timeout=15.0)["state"] == "paused"

    seen = []
    for _ in range(3):
        seen.append(trowel.call("debug.frames")["frames"][0]["line"])
        _step(trowel, "in")
    forward = trowel.call("debug.frames")["frames"][0]["line"]

    _step(trowel, "back")
    back = trowel.call("debug.frames")["frames"][0]["line"]

    # Back onto the line the previous forward step came from. Asserting on
    # membership in `seen` rather than on a literal keeps this about the
    # direction of travel and not about the fixture's exact line numbers.
    assert back != forward, (seen, forward, back)
    assert back in seen, (seen, forward, back)
    trowel.call("debug.stop")


def test_reverse_step_over_is_accepted_in_a_recording(trowel, fixture_files: Path):
    _open(trowel, fixture_files / "trace_loop.tur")
    trowel.call("debug.start", {"replay": True, "timeout_ms": DEBUG_MS})
    assert _wait_state(trowel, "paused", timeout=15.0)["state"] == "paused"

    for _ in range(3):
        _step(trowel, "in")
    st = _step(trowel, "reverse_over")
    assert st["state"] == "paused", st
    trowel.call("debug.stop")


def test_evaluate_refuses_in_a_recording(trowel, fixture_files: Path):
    """A deliberate upstream limit, surfaced rather than papered over.

    There is no live frame in a recording. The adapter's own wording comes
    back verbatim — it says more than anything this end could synthesize.
    """
    _open(trowel, fixture_files / "trace_loop.tur")
    trowel.call("debug.start", {"replay": True, "timeout_ms": DEBUG_MS})
    assert _wait_state(trowel, "paused", timeout=15.0)["state"] == "paused"

    r = trowel.call("debug.evaluate", {"expression": "i"})
    assert r["ok"] is False, r
    assert "recording" in r["result"].lower(), r
    trowel.call("debug.stop")


def test_reverse_steps_are_no_ops_in_a_live_session(trowel, fixture_files: Path):
    """A live interpreter cannot run backwards; the session guards it here.

    Sending `stepBack` to a live adapter gets "not supported while paused",
    which would land in the log as an error the user cannot act on.
    """
    _open(trowel, fixture_files / "trace_trivial.tur")
    trowel.call("debug.start", {"stop_on_entry": True, "timeout_ms": DEBUG_MS})
    assert _wait_state(trowel, "paused")["state"] == "paused"
    before = trowel.call("debug.frames")["frames"][0]["line"]
    stops = trowel.call("debug.status")["stop_count"]

    # Deliberately not `_step`: nothing is expected to land, so waiting for a
    # stop that will never come would only cost the timeout.
    trowel.call("debug.step", {"kind": "back"})
    trowel.call("debug.reverse_continue")
    import time
    time.sleep(0.3)
    st = trowel.call("debug.status")
    assert st["state"] == "paused", st
    assert st["replay"] is False, st
    assert st["stop_count"] == stops, st
    assert trowel.call("debug.frames")["frames"][0]["line"] == before
    trowel.call("debug.stop")


# --- Phase 5: polish ------------------------------------------------------

def test_breakpoints_follow_lines_inserted_above_them(trowel, fixture_files: Path):
    """The classic breakpoint bug, and the reason markerAdd's handle is kept.

    Scintilla tracks a marker across insertions and deletions. Storing a bare
    line number and hoping is what makes a breakpoint drift onto an unrelated
    statement the moment anyone edits above it.
    """
    prog = fixture_files / "debug_move.tur"
    prog.write_text(
        '(defn main [] : int\n'
        '  (let [^mut i 0]\n'
        '    (set! i (+ i 1)))\n'
        '  0)\n')
    try:
        _open(trowel, prog)
        trowel.call("debug.breakpoint.toggle", {"path": str(prog), "line": 3})
        assert [b["line"] for b in
                trowel.call("debug.breakpoints", {"path": str(prog)})["breakpoints"]] == [3]

        # Insert a blank line at the very top; line 3 becomes line 4.
        trowel.call("editor.set_cursor", {"pos": 0})
        trowel.type("\n")

        bps = trowel.call("debug.breakpoints", {"path": str(prog)})["breakpoints"]
        assert [b["line"] for b in bps] == [4], bps
    finally:
        prog.unlink(missing_ok=True)


def test_deleting_a_line_merges_its_breakpoint_onto_the_next(trowel, fixture_files: Path):
    """Scintilla's rule, pinned because the intuition is the other one.

    Deleting the line a marker sits on does not drop the marker — Scintilla
    merges it onto the line that takes its place. So the breakpoint survives on
    the following statement. This test exists so that behaviour is a decision
    rather than a surprise; changing it means changing the reconcile step, not
    discovering it in a bug report.
    """
    prog = fixture_files / "debug_del.tur"
    prog.write_text("(def a 1)\n(def b 2)\n(def c 3)\n")
    try:
        _open(trowel, prog)
        trowel.call("debug.breakpoint.toggle", {"path": str(prog), "line": 2})
        assert len(trowel.call("debug.breakpoints", {"path": str(prog)})["breakpoints"]) == 1

        # Select the whole of line 2 including its newline, and delete it.
        text = trowel.call("editor.get_text")["text"]
        start = len(text.split("\n")[0].encode("utf-8")) + 1
        end = start + len(text.split("\n")[1].encode("utf-8")) + 1
        trowel.call("editor.set_selection", {"start": start, "end": end})
        trowel.call("editor.press", {"key": "Backspace"})

        assert trowel.call("editor.get_text")["text"] == "(def a 1)\n(def c 3)\n"
        bps = trowel.call("debug.breakpoints", {"path": str(prog)})["breakpoints"]
        # Still line 2 — but line 2 is now `(def c 3)`, the statement that
        # moved up into the deleted line's place.
        assert [b["line"] for b in bps] == [2], bps
    finally:
        prog.unlink(missing_ok=True)


def test_a_disabled_breakpoint_does_not_stop_the_program(trowel, fixture_files: Path):
    """DAP has no disabled breakpoint: a disabled one is simply not sent."""
    prog = fixture_files / "debug_disabled.tur"
    prog.write_text(
        '(defn main [] : int\n'
        '  (let [^mut i 0]\n'
        '    (while (< i 20)\n'
        '      (set! i (+ i 1))))\n'
        '  0)\n')
    try:
        _open(trowel, prog)
        trowel.call("debug.breakpoint.toggle", {"path": str(prog), "line": 5})
        trowel.call("debug.breakpoint.set", {"path": str(prog), "line": 5,
                                             "enabled": False})
        trowel.call("debug.start", {"timeout_ms": DEBUG_MS})
        st = _wait_state(trowel, "idle", timeout=10.0)
        # Ran to completion rather than stopping: the disabled breakpoint was
        # never sent.
        assert st.get("running") is False, st
        assert st.get("stop_count", 0) == 0, st
    finally:
        prog.unlink(missing_ok=True)


def test_a_condition_is_carried_to_the_adapter(trowel, fixture_files: Path):
    """Both spellings are accepted upstream; this pins the Lisp one."""
    prog = fixture_files / "debug_cond.tur"
    prog.write_text(
        '(defn main [] : int\n'
        '  (let [^mut i 0]\n'
        '    (while (< i 20)\n'
        '      (set! i (+ i 1))))\n'
        '  0)\n')
    try:
        _open(trowel, prog)
        # Line 4 is the `set!` inside the loop — inside the `let`, so `i` is in
        # scope there. Line 5 is the trailing `0)`, where it is not.
        trowel.call("debug.breakpoint.toggle", {"path": str(prog), "line": 4})
        trowel.call("debug.breakpoint.set", {"path": str(prog), "line": 4,
                                             "condition": "(> i 10)"})
        bps = trowel.call("debug.breakpoints", {"path": str(prog)})["breakpoints"]
        assert bps[0]["condition"] == "(> i 10)", bps

        trowel.call("debug.start", {"timeout_ms": DEBUG_MS})
        assert _wait_state(trowel, "paused", timeout=10.0)["state"] == "paused"
        # It stopped, and it stopped late — the condition was honoured rather
        # than dropped on the floor.
        i = [v for v in trowel.call("debug.variables")["variables"]
             if v["name"] == "i"]
        assert i and int(i[0]["value"]) > 10, i
        trowel.call("debug.stop")
    finally:
        prog.unlink(missing_ok=True)


def test_restart_respawns_the_same_program(trowel, fixture_files: Path):
    """`tur dap` has no restart request; a restart is a fresh process."""
    # A loop, so there are several stops to get away from before restarting —
    # a two-expression fixture runs off the end on the first step.
    prog = fixture_files / "debug_restart.tur"
    prog.write_text(
        '(defn main [] : int\n'
        '  (let [^mut i 0]\n'
        '    (while (< i 20)\n'
        '      (set! i (+ i 1))))\n'
        '  0)\n')
    try:
        _open(trowel, prog)
        trowel.call("debug.start", {"stop_on_entry": True, "timeout_ms": DEBUG_MS})
        assert _wait_state(trowel, "paused")["state"] == "paused"
        _step(trowel, "in")
        _step(trowel, "in")
        assert trowel.call("debug.status")["stop_count"] >= 2

        trowel.call("menu.invoke", {"path": ["Run", "Restart Debug Session"]})
        st = _wait_state(trowel, "paused", timeout=10.0)
        assert st["state"] == "paused", st
        # Back at the first stop of a brand-new session: a respawn, not a
        # rewind. The recording-based rewind is what replay is for.
        assert st["stop_count"] == 1, st
        trowel.call("debug.stop")
    finally:
        prog.unlink(missing_ok=True)


# --- The debugger's appearance -------------------------------------------

def test_screenshot_captures_the_window(trowel, fixture_files: Path, tmp_path):
    """`window.screenshot` renders the window to a PNG.

    This exists so UI work can be *looked at* rather than only asserted about.
    The debugger shipped its first version styled in Qt's defaults — near-white
    column headers on empty panes, bright scrollbars under every one — because
    nothing in the loop ever rendered it.
    """
    _open(trowel, fixture_files / "trace_trivial.tur")
    out = tmp_path / "shot.png"
    r = trowel.call("window.screenshot", {"path": str(out)})
    assert out.exists() and out.stat().st_size > 0
    assert r["width"] > 0 and r["height"] > 0
    # A PNG, not an empty file with a hopeful name.
    assert out.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"


def test_screenshot_needs_a_path(trowel):
    with pytest.raises(ControlError) as exc:
        trowel.call("window.screenshot")
    assert exc.value.code == "bad_args"


def test_a_top_level_file_stops_at_a_breakpoint(trowel, fixture_files: Path):
    """A top-level file is debuggable — the inverse of what this used to assert.

    It read: "`tur dap` instruments what `(main)` evaluates and nothing else",
    and asserted that a top-level file ran to completion with `stop_count == 0`,
    because a breakpoint could be verified and still never bind.

    Turmeric v0.44.0 removed that limitation — "`tur dap` and `tur trace` now
    instrument top-level programs, not only `(main)` ... the launch path now
    pre-scans for a top-level `main` and arms the debugger around the file load
    itself when there isn't one." Trowel bundles v0.46.0, so the breakpoint
    binds and the session stops. MainWindow's matching pre-launch warning is
    gone for the same reason.

    Kept as a test rather than deleted: it is the assertion that catches the
    bundled Turmeric being rolled back under Trowel's feet.
    """
    prog = fixture_files / "debug_toplevel.tur"
    prog.write_text('(defn use-ask [] : int 41)\n(println (use-ask))\n')
    try:
        _open(trowel, prog)
        trowel.call("debug.breakpoint.toggle", {"path": str(prog), "line": 2})
        trowel.call("debug.start", {"stop_on_entry": True, "timeout_ms": DEBUG_MS})
        st = _wait_state(trowel, "paused", timeout=10.0)
        assert st["state"] == "paused", st
        assert st["stop_count"] >= 1, st
        trowel.call("debug.stop")
    finally:
        prog.unlink(missing_ok=True)


def test_a_file_with_main_does_stop(trowel, fixture_files: Path):
    """The contrast that makes the rule a rule and not a guess."""
    prog = fixture_files / "debug_hasmain.tur"
    prog.write_text('(defn main [] : int\n  (let [x 1]\n    x))\n')
    try:
        _open(trowel, prog)
        trowel.call("debug.start", {"stop_on_entry": True, "timeout_ms": DEBUG_MS})
        st = _wait_state(trowel, "paused", timeout=10.0)
        assert st["state"] == "paused", st
        assert st["stop_count"] >= 1, st
        trowel.call("debug.stop")
    finally:
        prog.unlink(missing_ok=True)


# --- T4–T6: the timeline ---------------------------------------------------
#
# Needs `tur` v0.42.2 or newer, which advertises `supportsTurmericReplayTimeline`
# and serves `replayInfo` / `replaySeek` / `replaySites`. These skip against an
# older pinned binary rather than fail: the capability is read off the wire, so
# the tests start asserting the day TROWEL_TURMERIC_VERSION moves.

def _replay_with_timeline(trowel, fixture_files: Path, prog_text: str, name: str):
    prog = fixture_files / name
    prog.write_text(prog_text)
    _open(trowel, prog)
    trowel.call("debug.start", {"replay": True, "timeout_ms": 20000})
    st = _wait_state(trowel, "paused", timeout=20.0)
    if st.get("state") != "paused":
        prog.unlink(missing_ok=True)
        pytest.skip("replay session did not pause")
    tl = trowel.call("debug.timeline")
    if not tl.get("supported"):
        trowel.call("debug.stop")
        prog.unlink(missing_ok=True)
        pytest.skip("this `tur` has no replay timeline (needs v0.42.2+)")
    return prog, tl


_RECURSIVE = ('(defn work [n : int] : int\n'
              '  (if (< n 2) n (+ (work (- n 1)) (work (- n 2)))))\n\n'
              '(defn main [] : int\n  (let [^mut i 0]\n    (while (< i 3)\n'
              '      (println "tick")\n      (set! i (+ i 1))))\n  (work 7)\n  0)\n')


def test_timeline_reports_a_length_and_a_cursor(trowel, fixture_files: Path):
    prog, tl = _replay_with_timeline(trowel, fixture_files, _RECURSIVE, "tl_len.tur")
    try:
        assert tl["steps"] > 0, tl
        assert tl["index"] == 0, tl
        trowel.call("debug.stop")
    finally:
        prog.unlink(missing_ok=True)


def test_seek_moves_the_cursor_and_the_frames_follow(trowel, fixture_files: Path):
    prog, tl = _replay_with_timeline(trowel, fixture_files, _RECURSIVE, "tl_seek.tur")
    try:
        import time
        last = tl["steps"] - 1
        trowel.call("debug.seek", {"index": last})
        for _ in range(60):
            if trowel.call("debug.timeline")["index"] == last:
                break
            time.sleep(0.05)
        assert trowel.call("debug.timeline")["index"] == last
        # An out-of-range seek clamps rather than erroring: a scrubber dragged
        # past the end means "the end".
        trowel.call("debug.seek", {"index": 10 ** 9})
        time.sleep(0.4)
        assert trowel.call("debug.timeline")["index"] == last
        trowel.call("debug.stop")
    finally:
        prog.unlink(missing_ok=True)


def test_seeking_backwards_rewinds_the_console(trowel, fixture_files: Path):
    """T5. A backwards seek shortens the transcript, which a delta cannot say.

    The adapter re-sends everything as `replayOutput` and the console swaps
    rather than grows. Without it the console keeps showing output from steps
    the cursor has rewound past — the one thing a time-travel console must not
    do.
    """
    prog, tl = _replay_with_timeline(trowel, fixture_files, _RECURSIVE, "tl_out.tur")
    try:
        trowel.call("debug.seek", {"index": tl["steps"] - 1})
        out = _wait_output(trowel, lambda o: "tick" in o)
        assert "tick" in out, out

        # Rewinding to the start must SHORTEN the transcript back to nothing --
        # the one thing a time-travel console must not get wrong.
        trowel.call("debug.seek", {"index": 0})
        out = _wait_output(trowel, lambda o: o == "")
        assert out == "", out
        trowel.call("debug.stop")
    finally:
        prog.unlink(missing_ok=True)


def test_sites_carry_depth_and_position_together(trowel, fixture_files: Path):
    """T6's data source. `replaySites` is Try Turmeric's `trace-site-at` shape.

    Depth *and* position in one round trip, because a scrubber's cursor readout
    and a depth ribbon want the same steps. A bucket reports its range's maximum
    depth, so the recursion in `work` survives the downsample.
    """
    prog, _ = _replay_with_timeline(trowel, fixture_files, _RECURSIVE, "tl_sites.tur")
    try:
        sites = trowel.call("debug.sites", {"buckets": 16})["sites"]
        assert len(sites) == 16, sites
        assert all(s["line"] > 0 and s["file"] for s in sites), sites
        # `work` recurses, so the ribbon must show more than the one frame
        # `main` occupies.
        assert max(s["depth"] for s in sites) > 1, sites
        trowel.call("debug.stop")
    finally:
        prog.unlink(missing_ok=True)


def test_a_live_session_refuses_to_scrub(trowel, fixture_files: Path):
    """A live session has no recording, so there is no axis to scrub.

    `supported` and `available` are different questions, and conflating them is
    how this went wrong first time: the adapter advertises the capability on
    `initialize`, before it knows whether the launch is a replay, so
    `supported` is true for a live session too. Only `available` says whether
    *this* session can seek. With a capable `tur` and no such distinction,
    `debug.seek` replied Ok and silently did nothing.
    """
    _open(trowel, fixture_files / "trace_trivial.tur")
    trowel.call("debug.start", {"stop_on_entry": True, "timeout_ms": DEBUG_MS})
    _wait_state(trowel, "paused")
    tl = trowel.call("debug.timeline")
    assert tl["available"] is False, tl
    with pytest.raises(ControlError) as exc:
        trowel.call("debug.seek", {"index": 0})
    assert exc.value.code in ("no_timeline", "not_a_recording"), exc.value.code
    trowel.call("debug.stop")
