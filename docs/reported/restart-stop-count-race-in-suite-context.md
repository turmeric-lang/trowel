# `test_restart_respawns_the_same_program` fails in suite context

**Found:** 2026-10-05, against `main` (9616c3a). Pre-existing — observed
while reproducing the step-back flake; it failed in 2 of 5 full
`test_debugger.py` suite runs both before and after the step-back fix.
**Status:** reproduced 2026-10-05 against `main` (9616c3a). Flaky. Passes
in isolation; fails in suite context.
**Fixed:** 2026-10-05. See below.
**Impact:** the test is flaky. A green suite run is not reliable evidence
that restart respawns a fresh session.

## What happens

The test starts a debug session with `stop_on_entry`, steps in twice,
invokes "Restart Debug Session", and asserts the new session is back at
its first stop:

```python
trowel.call("menu.invoke", {"path": ["Run", "Restart Debug Session"]})
st = _wait_state(trowel, "paused", timeout=10.0)
assert st["state"] == "paused", st
assert st["stop_count"] == 1, st
```

The assertion fails: `assert 0 == 1`. The state is `"paused"` but
`stop_count` is still `0`.

## Why it happens

Same race class as the step-back flake. In `DebugSession::onStopped`
(`src/debug/debug_session.cpp:193`), `setState(State::Paused)` is
synchronous, but `++stopCount_` happens inside the `refreshFrames`
callback — an async `stackTrace` DAP round trip:

```cpp
void DebugSession::onStopped(const QJsonObject& body) {
    setState(State::Paused);          // state flips immediately
    // ...
    refreshFrames([this, reason] {
        ++stopCount_;                  // stop_count increments later
        emit framesUpdated();
        // ...
    });
}
```

`_wait_state` returns as soon as `state == "paused"`, but `stop_count`
has not been incremented yet because `refreshFrames` is still in flight.
Under suite load (a real `tur dap` subprocess per test, no process reuse),
the gap between the state flip and the `stackTrace` response widens, and
the test reads `stop_count == 0`.

## Suggested fix

Make `_wait_state` optionally wait for `stop_count` to reach a minimum
before returning, so a `state == "paused"` return also guarantees
`stop_count` has been incremented (and, since the increment is inside the
`refreshFrames` callback, that frames are populated). This is the same
shape as the `_step` fix for the step-back flake: wait for the data to
actually land, not just for the state to flip.

## Fix applied

`_wait_state` (`tests/smoke/test_debugger.py`) now accepts a
`min_stop_count` parameter. `test_restart_respawns_the_same_program` uses
`min_stop_count=1`. Three other tests that read `stop_count` or `frames`
immediately after a fresh `debug.start` pause were updated for the same
race: `test_step_back_refused_on_a_live_session`,
`test_a_top_level_file_stops_at_a_breakpoint`, and
`test_a_file_with_main_does_stop`.

Verified: 5/5 full `test_debugger.py` suite runs, 33/33 pass each time.
