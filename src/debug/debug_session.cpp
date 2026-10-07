#include "debug/debug_session.h"

#include "repl/tur_invocation.h"  // ResolveTurBinary

#include <QJsonArray>

#include <algorithm>
#include <utility>

namespace trowel {

namespace {
// How long to wait for the adapter to answer `initialize` before concluding
// the bundled `tur` has no DAP server (version skew — see the plan's risks).
constexpr int kInitializeTimeoutMs = 5000;
}  // namespace

DebugSession::DebugSession(QObject* parent)
    : QObject(parent)
    , client_(new DapClient(this))
{
    connect(client_, &DapClient::eventReceived, this, &DebugSession::onEvent);
    connect(client_, &DapClient::startFailed, this, [this](const QString& why) {
        setState(State::Idle);
        emit sessionFailed(why);
    });
    connect(client_, &DapClient::finished, this, [this](int code) {
        // The adapter process died. If we were mid-run, this is the same as
        // the program exiting (the adapter exits with the debuggee's code);
        // if we were still starting, it's a failure.
        if (state_ == State::Idle || state_ == State::Terminated) return;
        finishTerminated(code, userStopped_);
    });
}

DebugSession::~DebugSession() {
    stop();
}

bool DebugSession::isRunning() const {
    return state_ != State::Idle && state_ != State::Terminated && client_->isRunning();
}

void DebugSession::start(const QString& program, const QString& workingDir,
                         const QStringList& extraEnv, bool stopOnEntry, bool replay) {
    if (state_ != State::Idle && state_ != State::Terminated) return;
    program_ = program;
    stopOnEntry_ = stopOnEntry;
    replay_ = replay;
    userStopped_ = false;
    exitCode_ = -1;
    stopCount_ = 0;
    timeline_ = false;
    timeline_info_ = Timeline{};
    seekPending_ = false;
    seekQueued_ = -1;
    frames_.clear();
    variables_.clear();
    output_.clear();

    const QString tur = ResolveTurBinary();
    if (tur.isEmpty()) {
        emit sessionFailed(QStringLiteral("could not locate the `tur` binary"));
        return;
    }

    setState(State::Starting);
    if (!client_->start(tur, {"dap"}, workingDir, extraEnv)) {
        setState(State::Idle);
        emit sessionFailed(QStringLiteral("could not start `tur dap`"));
        return;
    }
    onInitialize();
}

void DebugSession::onInitialize() {
    // `tur dap` answers initialize with its capabilities, then an
    // `initialized` event. We send `launch` once initialize succeeds, then
    // wait for the `initialized` event before sending configurationDone.
    client_->request(
        "initialize",
        QJsonObject{
            {"clientID", "trowel"},
            {"adapterID", "tur"},
            {"linesStartAt1", true},
            {"columnsStartAt1", true},
            {"pathFormat", "path"},
        },
        [this](const QJsonValue& body, const DapError* err) {
            if (err) {
                stop();
                setState(State::Idle);
                emit sessionFailed(err->message);
                return;
            }
            // The adapter's own answer, not the pinned version: ResolveTurBinary
            // honours a QSettings override and PATH, so the `tur` on the other
            // end of this pipe is not necessarily the one the pin names.
            timeline_ = body.toObject()
                            .value("supportsTurmericReplayTimeline").toBool(false);
            // Launch the program. `launch` carries no `cwd` (constraint 4);
            // the working directory was set on the child process at spawn.
            QJsonObject args{
                {"program", program_},
                {"stopOnEntry", stopOnEntry_},
            };
            // Only sent when set. A `"replay": false` would be read the same
            // way by this adapter, but the flag is the whole difference
            // between two very different sessions and it belongs in the wire
            // log only when it is doing something.
            if (replay_) args.insert("replay", true);
            client_->request("launch", args,
                [this](const QJsonValue&, const DapError* err) {
                    if (err) {
                        stop();
                        emit sessionFailed(err->message);
                    }
                    // The `initialized` event drives configurationDone next.
                });
        },
        kInitializeTimeoutMs);
}

void DebugSession::onInitialized() {
    // Push breakpoints (none in phase 1 — phase 3 adds the model) and send
    // configurationDone, which starts the program.
    setState(State::Configuring);
    // The owner pushes the program file's breakpoints now, before
    // configurationDone — the only guaranteed-processed send window.
    emit pushBreakpointsRequested();
    client_->request("configurationDone", QJsonObject{},
        [this](const QJsonValue&, const DapError* err) {
            if (err) {
                stop();
                emit sessionFailed(err->message);
                return;
            }
            // The program is now running. If stopOnEntry was set, a `stopped`
            // event with reason "entry" will arrive and flip us to Paused;
            // otherwise we stay Running until a breakpoint/step or the program
            // exits.
            setState(State::Running);
        });
}

void DebugSession::onEvent(const QString& event, const QJsonObject& body) {
    if (event == "initialized") {
        onInitialized();
        return;
    }
    if (event == "stopped") {
        onStopped(body);
        return;
    }
    if (event == "replayOutput") {
        // A backwards seek shortens the transcript, and a delta cannot express
        // a truncation — so the adapter re-sends the whole thing and the
        // console swaps rather than grows. Without this the console keeps
        // showing output from steps the cursor has rewound past, which is the
        // one thing a time-travel console must not do.
        output_ = body.value("output").toString();
        emit outputReplaced(output_);
        return;
    }
    if (event == "output") {
        const QString text = body.value("output").toString();
        if (!text.isEmpty()) {
            output_ += text;
            emit outputReceived(text);
        }
        return;
    }
    if (event == "exited") {
        const int code = body.value("exitCode").toInt(-1);
        finishTerminated(code, userStopped_);
        return;
    }
    if (event == "terminated") {
        finishTerminated(exitCode_, userStopped_);
        return;
    }
    // `thread` and other events are not relevant to a single-threaded adapter.
}

void DebugSession::onStopped(const QJsonObject& body) {
    const QString reason = body.value("reason").toString();
    setState(State::Paused);
    // Re-push breakpoints now that the adapter is reading stdin again —
    // mid-run setBreakpoints is not processed until the next stop (constraint 6).
    emit pushBreakpointsRequested();
    // `stopped` waits for the frames and the innermost frame's variables. A
    // consumer that reads `frames()` from this signal is the normal case — the
    // execution-line marker, the stack list, the variables pane are all it —
    // and every one of them would otherwise read the previous stop's data.
    if (replay_ && timeline_) refreshTimeline();
    refreshFrames([this, reason] {
        ++stopCount_;
        emit framesUpdated();
        emit variablesUpdated();
        emit stopped(reason);
    });
}

void DebugSession::refreshFrames(std::function<void()> done) {
    client_->request(
        "stackTrace", QJsonObject{{"threadId", 1}},
        [this, done = std::move(done)](const QJsonValue& body, const DapError* err) {
            frames_.clear();
            variables_.clear();
            if (err) { if (done) done(); return; }
            const QJsonArray arr = body.toObject().value("stackFrames").toArray();
            for (const QJsonValue& f : arr) {
                const QJsonObject fo = f.toObject();
                Frame frame;
                frame.id = fo.value("id").toInt(0);
                frame.name = fo.value("name").toString();
                frame.line = fo.value("line").toInt(0);
                frame.column = fo.value("column").toInt(0);
                const QJsonObject src = fo.value("source").toObject();
                frame.filePath = src.value("path").toString();
                frames_.append(frame);
            }
            if (frames_.isEmpty()) { if (done) done(); return; }
            // A new stop always lands on the innermost frame. Whatever the
            // user had selected belonged to the previous stop and its id may
            // not even exist in this stack.
            selectedFrameId_ = frames_.first().id;
            refreshVariables(selectedFrameId_, done);
        });
}

void DebugSession::refreshVariables(int frameId, std::function<void()> done) {
    // `scopes` returns one Locals scope whose reference is frameId + 1
    // (constraint 7). `variables` against that reference returns the flat
    // variable list.
    client_->request(
        "scopes", QJsonObject{{"frameId", frameId}},
        [this, done = std::move(done)](const QJsonValue& body, const DapError* err) {
            variables_.clear();
            if (err) { if (done) done(); return; }
            const QJsonArray scopes = body.toObject().value("scopes").toArray();
            int varRef = 0;
            for (const QJsonValue& s : scopes) {
                const QJsonObject so = s.toObject();
                if (so.value("name").toString() == "Locals" ||
                    so.value("variablesReference").toInt(0) != 0) {
                    varRef = so.value("variablesReference").toInt(0);
                    break;
                }
            }
            if (varRef == 0) { if (done) done(); return; }
            client_->request(
                "variables", QJsonObject{{"variablesReference", varRef}},
                [this, done](const QJsonValue& body, const DapError* err) {
                    variables_.clear();
                    if (err) { if (done) done(); return; }
                    const QJsonArray arr = body.toObject().value("variables").toArray();
                    for (const QJsonValue& v : arr) {
                        const QJsonObject vo = v.toObject();
                        Variable var;
                        var.name = vo.value("name").toString();
                        var.value = vo.value("value").toString();
                        var.type = vo.value("type").toString();
                        variables_.append(var);
                    }
                    if (done) done();
                });
        });
}

void DebugSession::selectFrame(int frameId) {
    if (state_ != State::Paused) return;
    // Guard against a stale id from a list that has since been rebuilt: the
    // adapter clamps a negative frameId to 0 (constraint 11) and would answer
    // for the wrong frame rather than fail.
    const bool known = std::any_of(frames_.cbegin(), frames_.cend(),
                                   [frameId](const Frame& f) { return f.id == frameId; });
    if (!known) return;
    selectedFrameId_ = frameId;
    refreshVariables(frameId, [this] { emit variablesUpdated(); });
}

void DebugSession::evaluate(const QString& expression, EvaluateCallback cb) {
    if (state_ != State::Paused) {
        if (cb) cb(false, QStringLiteral("not paused"));
        return;
    }
    client_->request(
        "evaluate",
        QJsonObject{{"expression", expression},
                    {"frameId", selectedFrameId_},
                    {"context", "repl"}},
        [cb = std::move(cb)](const QJsonValue& body, const DapError* err) {
            if (!cb) return;
            // The adapter's own message, verbatim — in a recording it is
            // "cannot evaluate in a recording -- relaunch without \"replay\"",
            // which says more than anything this end could synthesize.
            if (err) { cb(false, err->message); return; }
            cb(true, body.toObject().value("result").toString());
        });
}

void DebugSession::resume() {
    if (state_ != State::Paused) return;
    client_->request("continue", QJsonObject{{"threadId", 1}},
        [this](const QJsonValue&, const DapError* err) {
            if (err) return;
            setState(State::Running);
            frames_.clear();
            variables_.clear();
            emit resumed();
        });
}

void DebugSession::stepOver() {
    if (state_ != State::Paused) return;
    client_->request("next", QJsonObject{{"threadId", 1}}, nullptr);
}

void DebugSession::stepIn() {
    if (state_ != State::Paused) return;
    client_->request("stepIn", QJsonObject{{"threadId", 1}}, nullptr);
}

void DebugSession::stepOut() {
    if (state_ != State::Paused) return;
    client_->request("stepOut", QJsonObject{{"threadId", 1}}, nullptr);
}

void DebugSession::refreshTimeline() {
    if (!replay_ || !timeline_) return;
    client_->request("replayInfo", QJsonObject{},
        [this](const QJsonValue& body, const DapError* err) {
            if (err) return;
            const QJsonObject o = body.toObject();
            timeline_info_.steps = o.value("steps").toInt(0);
            timeline_info_.index = o.value("index").toInt(0);
            timeline_info_.depth = o.value("depth").toInt(0);
            timeline_info_.outputLength = o.value("outputLength").toInt(0);
            emit timelineUpdated();
        });
}

void DebugSession::seek(int index) {
    if (state_ != State::Paused || !replay_ || !timeline_) return;
    if (index < 0) index = 0;

    // Coalesce. The adapter answers strictly in order, so a slider drag that
    // sent one request per pixel would walk the cursor through every
    // intermediate step long after the user let go. Only the newest target
    // matters; the ones in between were never destinations.
    if (seekPending_) {
        seekQueued_ = index;
        return;
    }
    seekPending_ = true;
    client_->request("replaySeek", QJsonObject{{"index", index}},
        [this](const QJsonValue& body, const DapError* err) {
            seekPending_ = false;
            if (!err) {
                // Believe the adapter over our own arithmetic: it clamps into
                // range and reports where the cursor actually landed.
                timeline_info_.index = body.toObject().value("index").toInt(
                    timeline_info_.index);
                emit timelineUpdated();
            }
            // A `stopped` event follows the response and refreshes the frames,
            // so nothing else is needed here.
            if (seekQueued_ >= 0) {
                const int next = seekQueued_;
                seekQueued_ = -1;
                seek(next);
            }
        });
}

void DebugSession::requestSites(int buckets, SitesCallback cb) {
    if (!replay_ || !timeline_) {
        if (cb) cb({});
        return;
    }
    client_->request("replaySites", QJsonObject{{"buckets", buckets}},
        [cb = std::move(cb)](const QJsonValue& body, const DapError* err) {
            if (!cb) return;
            if (err) { cb({}); return; }
            QVector<Site> sites;
            for (const QJsonValue& v : body.toObject().value("sites").toArray()) {
                const QJsonObject o = v.toObject();
                Site s;
                s.index = o.value("index").toInt(0);
                s.line = o.value("line").toInt(0);
                s.depth = o.value("depth").toInt(0);
                s.filePath = o.value("file").toString();
                sites.append(s);
            }
            cb(sites);
        });
}

void DebugSession::stepBack() {
    if (state_ != State::Paused || !replay_) return;
    client_->request("stepBack", QJsonObject{{"threadId", 1}}, nullptr);
}

void DebugSession::reverseStepOver() {
    if (state_ != State::Paused || !replay_) return;
    client_->request("reverseNext", QJsonObject{{"threadId", 1}}, nullptr);
}

void DebugSession::reverseContinue() {
    if (state_ != State::Paused || !replay_) return;
    // Unlike forward `continue`, this does not leave us Running: the adapter
    // seeks the cursor backwards and immediately emits another `stopped`. It
    // stays Paused throughout, so there is no state change to make here and
    // nothing to clear — the next `stopped` replaces the frames wholesale.
    client_->request("reverseContinue", QJsonObject{{"threadId", 1}}, nullptr);
}

void DebugSession::setBreakpoints(const QString& path,
                                  const QVector<BreakpointSpec>& bps) {
    // `tur dap` matches breakpoints by basename (constraint 5), but the
    // request carries the full source.path — the adapter reduces it. We send
    // the absolute path; the basename collision is surfaced client-side by
    // the model.
    QJsonArray arr;
    for (const BreakpointSpec& b : bps) {
        // DAP has no notion of a disabled breakpoint — the set you send *is*
        // the set that binds. A disabled one is therefore simply not sent,
        // rather than sent and hoped to be ignored.
        if (!b.enabled) continue;
        QJsonObject bp{
            {"line", b.line},
        };
        if (!b.condition.isEmpty()) bp.insert("condition", b.condition);
        arr.append(bp);
    }
    QJsonObject args{
        {"source", QJsonObject{{"path", path}}},
        {"breakpoints", arr},
    };
    client_->request("setBreakpoints", args, nullptr);
}

void DebugSession::stop() {
    if (state_ == State::Idle || state_ == State::Terminated) {
        client_->stop();
        return;
    }
    // While paused, `terminate` makes the adapter `_exit(0)`. While running,
    // the adapter is not reading stdin (constraint 6), so we have to kill.
    userStopped_ = true;
    if (state_ == State::Paused && client_->isRunning()) {
        client_->request("terminate", QJsonObject{}, nullptr, 1000);
    }
    client_->stop();
    finishTerminated(-1, true);
}

void DebugSession::clearOutput() {
    output_.clear();
}

void DebugSession::finishTerminated(int code, bool userStopped) {
    const bool wasLive = state_ != State::Idle && state_ != State::Terminated;
    setState(State::Terminated);
    frames_.clear();
    variables_.clear();
    client_->stop();
    exitCode_ = userStopped ? -1 : code;
    if (wasLive) emit programExited(exitCode_);
    setState(State::Idle);
}

void DebugSession::setState(State s) {
    if (state_ == s) return;
    state_ = s;
    emit stateChanged(s);
}

}
