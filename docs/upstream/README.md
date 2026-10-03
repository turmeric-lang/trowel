# Upstream reports, staged

Defects in the Turmeric toolchain found while bringing Trowel onto v0.60.1.
They are staged here rather than committed into `turmeric/docs/reported/`
because that repo is a bare checkout whose worktrees are each mid-work on their
own branch — copying these in is a deliberate act, not a side effect of Trowel's
build.

Each one was measured against the **pinned v0.60.1**, with the commands in the
report. Trowel carries a workaround for three of them, and each workaround's
comment names the report so the two can be deleted together.

| Report | Trowel-side workaround |
|---|---|
| [lsp-ignores-the-file-extension](lsp-ignores-the-file-extension.md) | none yet — Part G of `docs/plans/dialects-saffron-and-r7rs.md` |
| [fmt-reprints-sweet-as-s-expressions](fmt-reprints-sweet-as-s-expressions.md) | `DialectIsFormattable` declines the two sweet bases |
| [format-subcommand-shreds-a-sweet-buffer](format-subcommand-shreds-a-sweet-buffer.md) | Format File moved to `tur fmt --stdin` |
| [run-on-sweet-file-drops-its-definitions](run-on-sweet-file-drops-its-definitions.md) | none; pinned by a smoke test |
| [lang-flags-take-different-vocabularies](lang-flags-take-different-vocabularies.md) | two accessors, `DialectBaseToken` and `DialectFmtLangFlag` |
| [signature-help-declines-at-its-own-trigger-character](signature-help-declines-at-its-own-trigger-character.md) | signature help is explicit-only, never auto-triggered |
