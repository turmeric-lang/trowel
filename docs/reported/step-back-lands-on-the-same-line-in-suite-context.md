# `test_step_back_moves_the_cursor_backwards` fails in suite context, passes alone

**Found:** 2026-10-05, against `main` (8a3546b). Pre-existing — reproduces on
the clean tree with the minimap changes stashed.
**Impact:** the test is flaky. It passes in isolation (3/3 runs) and fails
when run as part of the full `test_debugger.py` suite. A green suite run is
not reliable evidence that replay step-back works.

## What happens

The test opens `tests/smoke/fixtures/trace_loop.tur`, starts a replay debug
session, steps forward three times, then steps back once, and asserts the
line moves backward:

```python
assert back != forward, (seen, forward, back)
assert back in seen, (seen, forward, back)
```

In suite context the first assertion fails — the backward step lands on the
same line as the forward step. Two observed failures:

| run | `seen` | `forward` | `back` |
|---|---|---|---|
| 1 | `[3, 4, 4]` | 6 | 5 |
| 2 | `[3, 3, 5]` | 5 | 5 |

Run 2 is the clean failure: `back == forward == 5`. Run 1 is subtler — `back`
(5) differs from `forward` (6) but is not in `seen` (`[3, 4, 4]`), so the
second assertion would catch it too.

In isolation the test passes every time. The fixture is a `while` loop that
runs 200 iterations, so every line is recorded many times; the exact line
numbers depend on where in the recording the replay cursor sits, which
depends on timing.

## Why it happens

`_step` (`tests/smoke/test_debugger.py:151`) waits for `stop_count` to
increment, which is the right sync primitive — a step is Paused to Paused, so
state never changes and a line poll returns instantly with stale frames. But
`stop_count` only confirms a stop arrived; it does not confirm the frames
were refreshed before the next `debug.frames` call.

The failure pattern — `back == forward` — is what you get when the `stepBack`
DAP request is still in flight when `debug.frames` is read: the frames
returned are from the previous stop (the forward step), not the backward
step. The `_step` helper saw `stop_count` increment, but the `stopped` event
and the `stackTrace` response that populates frames are two separate DAP
round trips. Under suite load (a real `tur dap` subprocess per test, no
process reuse), the gap between `stop_count` incrementing and frames being
populated widens.

This is test-ordering/timing sensitivity, not a product bug. The replay
adapter's step-back works — it is exercised by
`test_reverse_step_over_is_accepted_in_a_recording` and
`test_seeking_backwards_rewinds_the_console`, which poll on output rather
than reading frames immediately after a step.

## Suggested fix

Make `_step` wait for the frames to actually change, not just for
`stop_count` to increment. Record the line before the step, and after
`stop_count` increments, poll `debug.frames` until the line differs (or the
session ends). This is the same shape as `_wait_output`, which was written
to fix an identical race in the seek tests — a fixed `time.sleep` that
passed in isolation and failed under load.

Alternatively, have `debug.frames` block until the pending `stopped` event
has been fully processed (frames populated), so a `stop_count` increment
guarantees fresh frames. That is a server-side fix and would close the race
for every caller, not just this test.
