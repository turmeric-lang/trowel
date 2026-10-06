# Try Turmeric improvements — plan

> **Status:** Not started. Split from Part H of
> [`dialects-saffron-and-r7rs.md`](dialects-saffron-and-r7rs.md), whose
> Parts A–G and I are executed. This is the triage that followed that
> survey; the two items worth porting (a docs pane and an Examples menu)
> each deserve their own plan and are called out below.
> **Related:** [`dialects-saffron-and-r7rs.md`](dialects-saffron-and-r7rs.md)
> (the dialect survey this was surveyed from),
> [`minimap.md`](minimap.md), [`sweet-exp.md`](sweet-exp.md).

Surveyed from the Try Turmeric changelog across v0.46.1–v0.60.1 and the
archived `try-turmeric-*` plans. Most of the changes are web-platform fixes
with no Trowel analogue (service worker, PWA install, iOS safe area, CSP,
share links, project zip). What is left, triaged:

## Worth porting

- **A docs pane.** Try Turmeric's is the best thing it has that Trowel lacks:
  the full guides and API reference in-app, searchable, with the pane leading
  on a quickstart and a "Recently Added" list. Trowel is better placed for it
  than the browser was — `tur docs --open`/`--serve` exists, the release
  publishes a `turmeric-docs-<tag>.tar.gz`, and the **docs pack** is a
  specified artifact: `index.json` (version, nav tree, search strings) plus
  chrome-free article bodies under `guides/`, `api/`, `spices/`. Both the
  website and Try Turmeric render from that one pack, which is what keeps them
  from drifting; a third consumer costs nothing new. Trowel's existing
  "Show Doc" is hover-level and is not this. Deserves its own plan, including
  whether to bundle the pack (another `FetchContent`, more bundle weight) or
  fetch it on demand.
- **An Examples menu.** Trowel has no discovery path for the language at all.
  Upstream's examples now exist in both s-expression and sweet form across the
  guides, and `tutorials/` ships in the repo. Cheap, and it is the other half
  of the picker: the picker teaches the syntax exists, examples show it.

## Already in Trowel, in its own idiom — nothing to do

- Outline, go-to-definition, hover, completion, rename, references
  (the Run menu).
- The time-travel tracer. Try's `trace-*` panel and Trowel's
  `timeline_strip.cpp` + Replay are the two consumers of the same
  `tur trace` format and the same DAP reverse execution; T1 and T2 landed
  upstream and Trowel consumes both.
- Minimap: [`minimap.md`](minimap.md) exists and `editor_view.h` references it;
  check its actual state against the code before treating it as open work.
- Sweet auto-indent: `editor_view.h` already has indentation handling "only
  active for languages where indentation is load-bearing", so the
  [`sweet-exp.md`](sweet-exp.md) plan's Part C landed. Extend it to
  `r7rs/sweet`.

## Fixes Trowel inherits for free by bumping

These came with the Turmeric pin bump in
[`dialects-saffron-and-r7rs.md`](dialects-saffron-and-r7rs.md) Part A
(executed, now at `v0.61.0`):

- A later REPL turn can redefine any `def*` form (v0.49.1) — directly visible
  in Trowel's REPL pane.
- Output that stops mid-line reaches the consumer (v0.56.2); `#lang r7rs`
  plus `(display "x")` used to print nothing until the next newline. Trowel
  reads a pty rather than Emscripten's TTY, so it was probably never affected
  — but it is worth one smoke test now that Scheme buffers are runnable, since
  `display` without a trailing newline is idiomatic Scheme.
- `tur lsp` / `tur dap` reject malformed or oversized `Content-Length`
  (v0.58.0); a `-1` was a heap overflow. Trowel speaks both protocols.

## Deliberately not porting

Share links, project zip import/export, the PWA/mobile work, the in-page
tutorial overlay (`:tutorial` already works in the REPL pane, which is the
native idiom), and the "Solve this" button.
