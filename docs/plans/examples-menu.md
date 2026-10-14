# Examples menu — plan

> **Status:** Done. `Help > Examples` ships seven bundled examples; see
> "Resolution" at the end. Split from the Try Turmeric triage in
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

## Resolution

- **Source (Part A):** `tutorials/` turned out to be three interactive YAML
  lessons for the REPL (`quickstart.yaml`, `basics.yaml`, `eavt.yaml`), not
  runnable programs, and the bundled toolchain (v0.62.0) ships no examples. So
  the examples are hand-written `.tur` files in `resources/examples/`, embedded
  through `resources.qrc` under `:/examples/`, and each was checked with
  `tur interpret`. The guides were not scraped; that remains a follow-on if
  the menu wants more topics. The docs pack (`docs-pane.md`) is not started,
  so there was nothing to consume.
- **Menu (Part B):** `Help > Examples`, built in `main_window.cpp`. Entries are
  grouped by topic, with the sweet and R7RS variants after a separator.
  `openExample()` opens an untitled tab, loads the text, and resets the save
  point so an untouched example closes without a prompt.
- **Runnability (Part C):** `test_examples_menu.py` opens every entry through
  `menu.invoke`, checks the dialect, runs it, and asserts on its output.
  The examples avoid `str-concat` and `print`: `str-concat` warns under
  `tur interpret` and fails under `tur run`, and `print` does not exist in
  v0.62.0.
