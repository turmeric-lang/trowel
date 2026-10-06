# Dialects

Turmeric is not one syntax. It has three **languages** — Turmeric, Saffron,
and R7RS (Scheme) — and each can be read with either the s-expression reader
or the **sweet** reader. Trowel supports all ten combinations, matching what
`tur dialects` reports.

A dialect is a (language, reader) pair. The `#lang` line at the top of a file
declares which one it uses.

## The ten dialects

| `#lang` token | Language | Reader | Extension |
|---|---|---|---|
| `turmeric` | Turmeric | s-expr | `.tur` |
| `turmeric/curly-infix` | Turmeric | curly-infix | `.tur` |
| `turmeric/neoteric` | Turmeric | neoteric | `.tur` |
| `turmeric/sweet` | Turmeric | sweet | `.tur.sweet` |
| `saffron` | Saffron | s-expr | `.tur` |
| `saffron/curly-infix` | Saffron | curly-infix | `.tur` |
| `saffron/neoteric` | Saffron | neoteric | `.tur` |
| `saffron/sweet` | Saffron | sweet | `.tur.sweet` |
| `r7rs` | R7RS | scheme | `.scm` |
| `r7rs/sweet` | R7RS | sweet | `.tur` (carries `#lang`) |

All ten are stable — `r7rs` graduated out of experimental status upstream, so
there is nothing to enable and no lifecycle warning.

## How Trowel picks a dialect

A buffer's dialect is determined in this order:

1. **The `#lang` line** — if the file starts with `#lang <token>` (optionally
   after a `#!` shebang), that token sets the dialect. This is the authoritative
   source; it always wins.
2. **The file extension** — if there is no `#lang` line:
   - `.tur.sweet` → `turmeric/sweet` (checked before `.tur`)
   - `.scm` → `r7rs`
   - `.tur` or `.sweet` → `turmeric` (s-expr)

   A bare `.sweet` extension is read as **ordinary** Turmeric upstream — only
   `.tur.sweet` maps to the sweet reader — so Trowel treats it the same way.
   Such a file gets the sweet reader only by carrying a `#lang` line.

## Switching dialects

**Run → Dialect** opens the dialect picker. It shows the six most common
dialects grouped by language, plus a row for the buffer's current dialect if
it is one the picker does not offer (so the menu never disagrees with the
source).

Picking a dialect writes a `#lang <token>` line at the top of the buffer (or
updates the existing one). If the buffer is already in that dialect, the
status bar says so and nothing changes.

## The REPL and dialects

The REPL runs in a dialect too. A fresh REPL starts in the active buffer's
dialect — the REPL banner names it, so this is never silent.

Switching a buffer to a dialect in a **different language** (e.g. from
Turmeric to R7RS) requires restarting the REPL in that language. Trowel handles
this automatically: the REPL restarts in the new language when needed. A
reader-only switch within the same language (e.g. `turmeric` to
`turmeric/sweet`) does not require a restart.

**Run → Restart REPL** restarts the REPL in the current file's directory and
the active buffer's dialect. **Run → Restart REPL In…** lets you pick a
directory.

## Sweet reader

The sweet reader is an alternative to s-expression syntax that drops most
parentheses in favor of indentation. A sweet file carries `#lang
turmeric/sweet` (or `saffron/sweet`, `r7rs/sweet`) and uses the `.tur.sweet`
extension for Turmeric/Saffron, or a `#lang` line for R7RS (which has no
dedicated extension for sweet).

[Formatting](formatting.md) a sweet buffer is safe: `tur fmt` parse-checks it
and keeps it as written, so formatting will not restructure sweet syntax.

## See also

- [Formatting](formatting.md) — `tur fmt` respects the buffer's dialect.
- [Windows, tabs & the REPL](windows-tabs-and-repl.md) — where the REPL is
  rooted and how it restarts.
- [Settings](settings.md) — `turmeric.path` to override the `tur` binary.
