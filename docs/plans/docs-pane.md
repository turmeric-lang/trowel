# Docs pane — plan

> **Status:** Not started. Split from the Try Turmeric triage in
> [`try-turmeric-improvements.md`](try-turmeric-improvements.md), which was
> Part H of
> [`dialects-saffron-and-r7rs.md`](dialects-saffron-and-r7rs.md).
> **Related:** [`try-turmeric-improvements.md`](try-turmeric-improvements.md)
> (the full triage), [`dialects-saffron-and-r7rs.md`](dialects-saffron-and-r7rs.md)
> (the dialect survey, Parts A–G and I executed).

An in-app docs pane: the full Turmeric guides and API reference, searchable,
with the pane leading on a quickstart and a "Recently Added" list. Try
Turmeric's docs pane is the best thing it has that Trowel lacks, and Trowel is
better placed for it than the browser was.

## Context: the docs pack

The release publishes a `turmeric-docs-<tag>.tar.gz` artifact, and the **docs
pack** is a specified format: `index.json` (version, nav tree, search strings)
plus chrome-free article bodies under `guides/`, `api/`, `spices/`. Both the
Turmeric website and Try Turmeric render from that one pack, which is what
keeps them from drifting. A third consumer costs nothing new — the pack is
the contract, not any one renderer.

`tur docs --open` / `--serve` exists in the CLI and renders the pack, but that
is a browser surface. This plan is about rendering it inside Trowel.

Trowel's existing "Show Doc" is hover-level (a call-tip from the LSP) and is
not this.

## Part A — get the pack

The first decision: bundle the pack or fetch it on demand.

- **Bundle** — another `FetchContent` in CMake, staged beside the binary.
  Pro: works offline, no network dependency, version always matches the
  bundled toolchain. Con: more bundle weight (the pack is non-trivial), and
  it grows with every release.
- **Fetch on demand** — download `turmeric-docs-<tag>.tar.gz` on first use,
  cache under the user data directory, re-fetch when the bundled `tur`
  version changes. Pro: no bundle cost, always current. Con: needs a network
  round-trip on first use, and a fallback path when offline.

Recommend bundling, for the same reason the toolchain is bundled: the docs
should match the `tur` the user has, not the docs the website happens to be
serving today. But measure the pack size against CI limits before committing,
the same way Part A.4 of the dialect plan checked `libtur_mir.a`.

## Part B — the pane

A tab-type docs pane (alongside editor and directory tabs), or a docked
panel. Try Turmeric uses a pane; Trowel's chrome suggests a docked panel is
the more natural fit, but a tab lets it sit beside the code the way the
website's "open in new tab" does. Decide early; the rest follows from it.

The pane leads on:

- A **quickstart** — the getting-started guide, front and center.
- A **Recently Added** list — new guides/API entries since the user's version,
  driven by the version field in `index.json`.
- A **search** box — over the nav tree and the search strings in `index.json`.
- The **nav tree** itself — guides, API, spices, browsable.

Article bodies are chrome-free HTML (or markdown — check what the pack
actually ships). Trowel already has a markdown renderer for its own docs
view; verify it handles the pack's format before assuming it does.

## Part C — search

`index.json` carries search strings per article. The question is whether to
build a full-text index over the article bodies or rely on the pre-built
search strings. Try Turmeric does the latter and it is enough; the pack is
designed for it. Start there; full-text is a later enhancement if the
pre-built strings prove thin.

## Open questions

- Bundle vs. fetch (Part A) — needs a size measurement before deciding.
- Tab vs. docked panel (Part B) — a chrome decision.
- What format the article bodies are in (HTML vs. markdown) — check the
  actual pack contents before designing the renderer path.
