# Code formatting

Trowel formats the current buffer with `tur fmt`, the formatter shipped inside
the bundled Turmeric toolchain. It runs on the buffer's current text and
replaces it in place, preserving the caret and selection.

## How to run it

- **Edit → Format File** (`Ctrl+Shift+F` on Linux, `Cmd+Shift+F` on macOS).
- The side-bar button with the wrench icon.

Format File is disabled on read-only buffers (the same rule as Save).

## What it does

Trowel pipes the buffer's text to `tur fmt --stdin --lang <dialect>`, reads the
formatted output back, and replaces the buffer if it differs. The dialect flag
matches the buffer's active dialect — a Scheme buffer formats with
`--lang r7rs`, a Turmeric buffer with `--lang turmeric`, and so on. This
matters: without the flag, `tur fmt` defaults to Turmeric s-expression syntax,
which would re-indent a Scheme file differently.

Sweet dialects (`turmeric/sweet`, `saffron/sweet`, `r7rs/sweet`) go through
unchanged — `tur fmt` parse-checks a sweet buffer and keeps it as written, so
formatting is safe but will not restructure sweet syntax.

If the buffer is already formatted, the status bar says "Already formatted."
and nothing changes. On a format error, the status bar shows the error from
`tur fmt`.

## Requirements

- The file does not need to be saved — formatting works on unsaved buffers,
  because `tur fmt --stdin` reads from the pipe, not from disk.
- The `tur` binary must be resolvable (bundled copy, or `tur` on `PATH`, or
  [`turmeric.path`](settings.md) in settings). If Trowel cannot find it, the
  status bar says so.

## See also

- [Settings](settings.md) — `turmeric.path` to override the `tur` binary.
- [Dialects](dialects.md) — the dialect picker that determines the `--lang`
  flag.
