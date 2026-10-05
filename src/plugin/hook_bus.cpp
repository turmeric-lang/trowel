#include "hook_bus.h"

#include "plugin_host.h"

#include <QDebug>

namespace trowel {

HookBus::HookBus(PluginHost* host, QObject* parent)
    : QObject(parent), host_(host)
{
}

void HookBus::subscribe(const QString& event, TuriValue closure)
{
    hooks_[event].append(closure);
}

void HookBus::dispatch(const QString& event, const QVector<TuriValue>& args)
{
    auto it = hooks_.constFind(event);
    if (it == hooks_.constEnd()) return;

    // Copy the args into a C array for turi_call.
    QVector<TuriValue> argCopy = args;

    for (const auto& closure : it.value()) {
        // turi_call needs the env; PluginHost provides it.
        // The closure is a TURI_CLOSURE value from the plugin env.
        if (closure.tag != TURI_CLOSURE) {
            qWarning("[HookBus] non-closure subscribed to %s", qPrintable(event));
            continue;
        }
        // PluginHost::callClosure does the turi_call and error handling.
        TuriValue result = host_->callClosure(closure, argCopy);
        if (turi_is_error(result)) {
            qWarning("[HookBus] hook %s returned error: %s",
                     qPrintable(event),
                     turi_error_message(result) ? turi_error_message(result) : "(unknown)");
        }
    }
}

bool HookBus::hasSubscribers(const QString& event) const
{
    auto it = hooks_.constFind(event);
    return it != hooks_.constEnd() && !it.value().isEmpty();
}

}  // namespace trowel
