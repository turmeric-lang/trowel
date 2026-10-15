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

    // Subscribe a Turmeric closure to an event.  `env` is the plugin env the
    // closure was created in -- it is the env the closure must be called in.
    // The closure value is kept alive by that env, so the subscription must be
    // dropped (unsubscribeEnv) before the env is freed.
    void subscribe(const QString& event, TuriEnv* env, TuriValue closure);

    // Drop every subscription owned by `env`.  Called when a plugin unloads.
    void unsubscribeEnv(TuriEnv* env);

    // Dispatch an event with the given arguments.  Each registered closure is
    // called via turi_call.  Errors are logged to the status bar and skipped.
    // (Named "dispatch" because `emit` is a Qt macro.)
    void dispatch(const QString& event, const QVector<TuriValue>& args = {});

    // True if at least one closure is subscribed to `event`.
    bool hasSubscribers(const QString& event) const;

private:
    PluginHost* host_;
    struct Hook {
        TuriEnv* env;
        TuriValue closure;
    };
    QHash<QString, QVector<Hook>> hooks_;
};

}  // namespace trowel
