#pragma once

#include "turi_api.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

namespace trowel {

class PluginHost;

// HookBus dispatches named events to Turmeric closures registered by plugins.
// When a C++ signal fires (e.g. EditorView::contentChanged), the host calls
// HookBus::emit("buffer:changed", args), which calls each registered closure
// via turi_call.
//
// All dispatch happens on the Qt main thread.  A closure that returns
// TURI_ERROR is logged and skipped; it does not abort the chain.
class HookBus : public QObject {
    Q_OBJECT
public:
    HookBus(PluginHost* host, QObject* parent = nullptr);

    // Subscribe a Turmeric closure to an event.  The closure value is kept
    // alive by the TuriEnv (which outlives the HookBus).
    void subscribe(const QString& event, TuriValue closure);

    // Dispatch an event with the given arguments.  Each registered closure is
    // called via turi_call.  Errors are logged to the status bar and skipped.
    // (Named "dispatch" because `emit` is a Qt macro.)
    void dispatch(const QString& event, const QVector<TuriValue>& args = {});

    // True if at least one closure is subscribed to `event`.
    bool hasSubscribers(const QString& event) const;

private:
    PluginHost* host_;
    QHash<QString, QVector<TuriValue>> hooks_;
};

}  // namespace trowel
