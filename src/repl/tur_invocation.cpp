#include "repl/tur_invocation.h"

#include "app/settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace trowel {

namespace {

QString ifExecutable(const QString& path) {
    if (path.isEmpty()) return {};
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile() || !fi.isExecutable()) return {};
    return fi.absoluteFilePath();
}

// Path to the bundled `tur` shipped inside Trowel.app. Empty string on dev
// builds where no binary was staged (falls through to PATH).
//
// Both published archive shapes are probed, because Turmeric has shipped both.
// The windows-x86_64 .zip has always used the PREFIX layout -- bin/, lib/,
// include/, share/turmeric/stdlib/ -- while the three .tar.gz targets shipped
// FLAT (`tur` and `stdlib/` at the archive root) through v0.46.0 before being
// unified onto the prefix layout. CMake stages the fetched archive verbatim, so
// the staged tree is whichever shape the pinned TROWEL_TURMERIC_VERSION
// happened to publish. Accepting either is what keeps bumping that pin across
// the change a one-line edit instead of a coordinated one. See the
// turmeric-side report `unify-release-archive-layout`.
QString bundledTurPath() {
    const QString appDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
    const QString root = QDir::cleanPath(appDir + "/../Resources/turmeric");
#else
    const QString root = QDir::cleanPath(appDir + "/turmeric");
#endif
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("/tur.exe");
#else
    const QString exe = QStringLiteral("/tur");
#endif
    const QString prefixShape = QDir::cleanPath(root + "/bin" + exe);
    if (QFileInfo::exists(prefixShape)) return prefixShape;
    const QString flatShape = QDir::cleanPath(root + exe);
    if (QFileInfo::exists(flatShape)) return flatShape;
    // Nothing staged -- a dev build. Name the prefix shape: it is what a
    // current release unpacks to, so it is the more useful of the two to print
    // in the "could not locate tur" banner.
    return prefixShape;
}

} // namespace

QString TurStdlibDirFor(const QString& turBinary) {
    if (turBinary.isEmpty()) return {};
    const QString turDir = QFileInfo(turBinary).absolutePath();
    const QString flat = turDir + QStringLiteral("/stdlib");
    if (QDir(flat).exists()) return flat;
    const QString prefix =
        QDir::cleanPath(turDir + QStringLiteral("/../share/turmeric/stdlib"));
    if (QDir(prefix).exists()) return prefix;
    return {};
}

QString ResolveTurBinary() {
    const QString override = Settings::instance().turmericPath();
    QString resolved = ifExecutable(override);
    if (resolved.isEmpty()) resolved = ifExecutable(bundledTurPath());
    if (resolved.isEmpty()) resolved = QStandardPaths::findExecutable("tur");
    return resolved;
}

TurInvocation MakeTurInvocation(const QStringList& subcommand,
                                 const QString& workingDir) {
    TurInvocation inv;
    inv.args = subcommand;

    // Resolution order:
    //   1. settings.json "turmeric.path" — user override (absolute path).
    //   2. Bundled binary inside Trowel.app (drag-install path).
    //   3. `tur` on the user's PATH (dev builds, homebrew, mise, ...).
    const QString override = Settings::instance().turmericPath();
    const QString bundled = bundledTurPath();
    const QString onPath = QStandardPaths::findExecutable("tur");

    inv.binary = ifExecutable(override);
    if (inv.binary.isEmpty()) inv.binary = ifExecutable(bundled);
    if (inv.binary.isEmpty()) inv.binary = onPath;

    if (inv.binary.isEmpty()) {
        QString msg = QStringLiteral("[trowel] could not locate `tur`. Tried:");
        msg += QString("\n  1. settings.json turmeric.path = %1")
                   .arg(override.isEmpty() ? QStringLiteral("(unset)") : override);
        msg += QString("\n  2. bundled = %1").arg(bundled);
        msg += QString("\n  3. PATH lookup for `tur`");
        msg += "\nTerminal input is disabled until this is resolved.";
        inv.error = msg;
        return inv;
    }

    // Pin the stdlib to whichever `tur` we resolved, so the ambient
    // environment cannot pair it with another version's. See
    // TurStdlibDirFor.
    inv.env = QProcessEnvironment::systemEnvironment();
    const QString siblingStdlib = TurStdlibDirFor(inv.binary);
    if (!siblingStdlib.isEmpty()) {
        inv.env.insert("TUR_STDLIB_DIR", siblingStdlib);
    }

    // Engine selection. `tur build` does not accept --engine on argv (it
    // exits 2 with a usage error), but both `tur build` and `tur run` honor
    // the TUR_ENGINE env var. Inject it here so every call site gets the
    // engine consistently. "default" passes no override, letting the
    // project's build.tur :engine decide. See docs/plans/engine-selection.md.
    const QString engine = Settings::instance().runEngine();
    if (engine != QStringLiteral("default") && !engine.isEmpty()) {
        inv.env.insert("TUR_ENGINE", engine);
    }

    // workingDir is not an env or argv concern; it is passed to the process
    // spawn separately by each caller. Kept in the signature so later parts
    // of the engine-selection plan (experiment-flag resolution from the
    // project manifest) can use it without changing the call sites again.
    (void)workingDir;

    return inv;
}

QStringList TurInvocation::envEntries() const {
    QStringList entries;
    if (env.contains("TUR_STDLIB_DIR"))
        entries << QStringLiteral("TUR_STDLIB_DIR=") + env.value("TUR_STDLIB_DIR");
    if (env.contains("TUR_ENGINE"))
        entries << QStringLiteral("TUR_ENGINE=") + env.value("TUR_ENGINE");
    return entries;
}

} // namespace trowel
