# Docs pane — plan

> **Status:** Done. Bundled pack, docs tab, nav tree, search and home page
> shipped; see "Decisions" at the end. Split from the Try Turmeric triage in
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

## Decisions

Measured and probed against `turmeric-docs-v0.62.0.tar.gz`, not assumed.

- **Bundle vs. fetch: bundled.** The pack is 2.9 MB compressed (6.5 MB
  unpacked: `index.json` 0.5 MB, `guides/` 3.9 MB, `api/` 2.0 MB) against a
  ~6 MB toolchain archive. It is a second `FetchContent` in `CMakeLists.txt`,
  pinned to the toolchain tag, with one platform-independent SHA-256
  (`TROWEL_TURMERIC_DOCS_SHA`, bumped alongside `TROWEL_TURMERIC_VERSION`).
  Only `index.json`, `guides/` and `api/` are staged, to
  `Resources/turmeric-docs/` (macOS) or `turmeric-docs/` beside the binary.
- **Tab, not docked panel.** `TabContent::Kind::Docs`; it reuses the tab
  machinery wholesale and sits beside the code. One docs tab per window.
- **Article bodies are HTML fragments**, not markdown, so the existing
  markdown renderer is not involved. They render in a `QTextBrowser` (no
  WebEngine dependency). Two things in them need a script that does not run
  there: every code sample carries both a turmeric and a sweet-exp variant with
  a toggle, so `DocsView` picks one itself (toolbar combo) and strips the
  toggle chrome.
- **The pack layout differs from this plan's description in one way:** there
  is no `spices/` content (an empty list), and the nav is `index.json`'s
  `guides` and `api` lists grouped by `category`, not a stored tree.
- **Search is the pre-built `words` strings** (every term must match; titles,
  slugs and API symbols outrank body hits). No full-text index.
- **Recently Added is by `added` date, newest 10, not "since the user's
  version".** The index records dates, not versions, so a since-version cut is
  not derivable; revisit if the pack grows a version field per entry.

Not done: opening the pane on the symbol under the cursor (the API pages carry
`symbols`, so it is a lookup away), and a full-text index.
