# Examples menu — plan

> **Status:** Not started. Split from the Try Turmeric triage in
> [`try-turmeric-improvements.md`](try-turmeric-improvements.md), which was
> Part H of
> [`dialects-saffron-and-r7rs.md`](dialects-saffron-and-r7rs.md).
> **Related:** [`try-turmeric-improvements.md`](try-turmeric-improvements.md)
> (the full triage),
> [`dialects-saffron-and-r7rs.md`](dialects-saffron-and-r7rs.md)
> (the dialect survey, Parts A–G and I executed),
> [`docs-pane.md`](docs-pane.md) (the docs pane — examples may overlap with
> its content surface).

An Examples menu: a discovery path for the language that Trowel currently
lacks entirely. Upstream's examples now exist in both s-expression and sweet
form across the guides, and `tutorials/` ships in the Turmeric repo. This is
the other half of the dialect picker — the picker teaches that the syntax
exists, examples show what it looks like.

## Context

Trowel has no way for a new user to see a working Turmeric program without
leaving the app. The dialect picker (Part E of the dialect plan, executed)
introduced the `#lang` axis; an Examples menu gives that axis something to
point at. Cheap to build, outsized payoff for onboarding.

## Part A — source the examples

Two candidate sources, not mutually exclusive:

- **`tutorials/`** in the Turmeric repo — ships with the source, already
  structured as a sequence. These are the canonical "start here" examples.
- **The guides** — examples are embedded throughout, in both s-expression and
  sweet form. Extracting them gives a larger, more topical set (effects,
  macros, FFI, etc.) but needs a scrape.

Recommend starting with `tutorials/`: it is small, self-contained, and
already sequenced. The guides are a follow-on if the menu wants topical
categories beyond the tutorial path.

If the docs pack (see [`docs-pane.md`](docs-pane.md)) is bundled, the
tutorials may already be inside it under `guides/` — check before duplicating
the source.

## Part B — the menu

A `Help > Examples` submenu (or `File > Open Example`, depending on where it
fits the menu structure — check `main_window.cpp`'s menu construction).
Each entry opens a new editor tab with the example loaded, in the right
dialect — the `#lang` line travels with the file, so highlighting and the run
path both work without extra wiring.

Group by topic, not by reader: a user browsing examples does not think in
terms of s-expression vs. sweet, and the dialect picker already handles that
axis. If an example exists in both forms, list it once and let the user
switch via the picker.

## Part C — runnability

Every example should be immediately runnable: open it, hit Run, see output.
This is free if the `#lang` line is present and the run path (Part D of the
dialect plan, executed) switches the session correctly — verify a few
examples end-to-end rather than assuming it. The one thing to watch is
examples that depend on the prelude autoload, which only fires when the
entry file is the right `#lang` (§1.4 of the dialect survey).

## Open questions

- Whether to source from `tutorials/` alone or also scrape the guides (Part A).
- Menu placement — `Help` vs. `File` (Part B).
- Whether the docs pack bundles the tutorials, which would make this a
  consumer of [`docs-pane.md`](docs-pane.md) rather than a consumer of the
  repo.
