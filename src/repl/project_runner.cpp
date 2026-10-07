#include "repl/project_runner.h"

#include "repl/repl_session.h"
#include "repl/terminal_view.h"
#include "repl/tur_invocation.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>

namespace trowel {

namespace {

// The terminal renders a pty stream, so bare \n from a pipe would stair-step.
QByteArray toTerminalNewlines(const QByteArray& bytes) {
    QByteArray out;
    out.reserve(bytes.size() + 8);
    for (int i = 0; i < bytes.size(); ++i) {
        const char c = bytes.at(i);
        if (c == '\n' && (i == 0 || bytes.at(i - 1) != '\r')) out.append('\r');
        out.append(c);
    }
    return out;
}

}  // namespace

ProjectRunner::ProjectRunner(TerminalView* terminal, QObject* parent)
    : QObject(parent)
    , terminal_(terminal)
{}

bool ProjectRunner::isRunning() const {
    return proc_ && proc_->state() != QProcess::NotRunning;
}

RunResult ProjectRunner::run(const QString& projectDir) {
    RunResult r;
    if (projectDir.isEmpty() || !QDir(projectDir).exists()) {
        r.message = QString("No project directory at %1").arg(projectDir);
        return r;
    }
    if (isRunning()) {
        r.message = "A project build is already running.";
        return r;
    }

    const TurInvocation inv = MakeTurInvocation({"build", projectDir}, projectDir);
    if (inv.binary.isEmpty()) {
        r.message = "Could not locate `tur` executable.";
        return r;
    }

    if (proc_) {
        proc_->deleteLater();
        proc_ = nullptr;
    }
    proc_ = new QProcess(this);
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    proc_->setWorkingDirectory(projectDir);
    proc_->setProcessEnvironment(inv.env);

    connect(proc_, &QProcess::readyReadStandardOutput,
            this, &ProjectRunner::onReadyRead);
    connect(proc_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus) { onFinished(code); });

    if (terminal_) {
        const QString turEngine = inv.env.value("TUR_ENGINE");
        QString banner = QString("[trowel] tur build %1")
                             .arg(QDir::toNativeSeparators(projectDir));
        if (!turEngine.isEmpty())
            banner += QString("  [engine: %1]").arg(turEngine);
        terminal_->showBanner(banner);
    }

    // Pass the directory explicitly rather than relying on the cwd: `tur build`
    // needs a positional argument to take the manifest-driven project path
    // (a bare `tur build` has no input to discover from).
    proc_->start(inv.binary, inv.args);
    if (!proc_->waitForStarted(3000)) {
        r.message = "Failed to start `tur build`.";
        return r;
    }
    r.ok = true;
    r.message = QString("Building %1…").arg(QFileInfo(projectDir).fileName());
    return r;
}

void ProjectRunner::onReadyRead() {
    if (!proc_) return;
    const QByteArray chunk = proc_->readAllStandardOutput();
    if (chunk.isEmpty()) return;
    if (terminal_) terminal_->appendOutput(toTerminalNewlines(chunk));
}

void ProjectRunner::onFinished(int exitCode) {
    onReadyRead();  // drain whatever arrived with the exit
    if (terminal_) {
        terminal_->showBanner(exitCode == 0
            ? QStringLiteral("[trowel] build succeeded")
            : QString("[trowel] build failed with code %1").arg(exitCode));
    }
    emit finished(exitCode);
}

}
