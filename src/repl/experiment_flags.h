#pragma once

#include <QString>
#include <QStringList>

namespace trowel {

// The resolved set of experimental features that will be active when `tur`
// runs. Trowel reads this for display only (the REPL banner, the control
// API) — `tur` itself reads `~/.config/turmeric/experiments.tur` and the
// project's `build.tur` `:enable` key at process start, so Trowel does not
// forward `--enable=` on the command line.
//
// Precedence (matching `tur`'s own):
//   1. `build.tur` `:enable` — the project owner's decision. An explicit
//      empty `:enable []` means "opted out" and wins over the user file.
//   2. `~/.config/turmeric/experiments.tur` `:enable` — the user's global
//      default, applied only when the project has no `:enable` key.
//   3. Empty — no experiments.
struct ResolvedExperiments {
    // Experiment names, in source order. Empty when no source is set.
    QStringList names;
    // "build.tur", "user settings", or empty when no experiments are set.
    QString source;
    // True when the source is `build.tur` with an explicit empty `:enable []`.
    // Distinguishes "project opted out" from "no source found" — both have
    // empty `names`, but the banner should say "from build.tur" for the former.
    bool optedOut = false;
};

// Resolve the active experiments for a project at `workingDir`.
//
// Scans upward from `workingDir` for a `build.tur` with an `:enable` key. If
// found (including an explicit empty list), that is the source. Otherwise
// reads `~/.config/turmeric/experiments.tur` for an `:enable` key.
//
// The scanner is deliberately trivial: skip `;` line comments and `#| ... |#`
// block comments, find `:enable` followed by `[`, collect symbol tokens until
// `]`. If either file doesn't match the trivial shape (e.g. computed at read
// time), the scanner returns empty and the caller falls through — no full
// turmeric evaluator is embedded in Trowel.
ResolvedExperiments ResolveExperiments(const QString& workingDir);

// The path to `~/.config/turmeric/experiments.tur`, honoring `XDG_CONFIG_HOME`.
QString ExperimentsFilePath();

// Create `experiments.tur` with a commented stub if it does not exist.
// Returns the path. Safe to call when the file already exists.
QString EnsureExperimentsFile();

} // namespace trowel
