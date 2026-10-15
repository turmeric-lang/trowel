#include "hook_bus.h"

#include "plugin_host.h"

#include <QDebug>

namespace trowel {

HookBus::HookBus(PluginHost* host, QObject* parent)
    : QObject(parent), host_(host)
{
}

void HookBus::subscribe(const QString& event, TuriEnv* env, TuriValue closure)
{
    hooks_[event].append({env, closure});
}

void HookBus::unsubscribeEnv(TuriEnv* env)
{
    for (auto it = hooks_.begin(); it != hooks_.end(); ++it) {
        it.value().removeIf([env](const Hook& h) { return h.env == env; });
    }
}

void HookBus::dispatch(const QString& event, const QVector<TuriValue>& args)
{
    auto it = hooks_.constFind(event);
    if (it == hooks_.constEnd()) return;

    // Copy the args into a C array for turi_call.
    QVector<TuriValue> argCopy = args;

    // Iterate a copy: a hook may unload its own plugin (reload), which
    // mutates hooks_ underneath us.
    const QVector<Hook> snapshot = it.value();
    for (const auto& hook : snapshot) {
        const TuriValue& closure = hook.closure;
        if (closure.tag != TURI_CLOSURE) {
            qWarning("[HookBus] non-closure subscribed to %s", qPrintable(event));
            continue;
        }
        // PluginHost::callClosure does the turi_call and error handling.
        TuriValue result = host_->callClosure(hook.env, closure, argCopy);
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
