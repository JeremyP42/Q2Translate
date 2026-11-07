#include "balancer_provider.h"

#include "core/settings_manager.h"
#include "plugin_manager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

#include <memory>

BalancerProvider::BalancerProvider(const PluginManifest& manifest, PluginManager* plugins,
                                   SettingsManager* settings, QObject* parent)
    : IPluginProvider(parent)
    , m_manifest(manifest)
    , m_plugins(plugins)
    , m_settings(settings)
{
}

bool BalancerProvider::updateSettings(const QJsonObject& settings)
{
    Q_UNUSED(settings);
    return true;
}

QStringList BalancerProvider::candidateIds(const QString& capability) const
{
    QStringList ids;
    if (m_settings) {
        const QString primary =
            m_settings->stringValue(QStringLiteral("Balancer.Primary"));
        if (!primary.isEmpty())
            ids.append(primary);
        for (const QJsonValue& value :
             m_settings->value(QStringLiteral("Balancer.Fallbacks")).toArray()) {
            const QString id = value.toString();
            if (!id.isEmpty())
                ids.append(id);
        }
    }

    QStringList result;
    for (const QString& id : std::as_const(ids)) {
        if (id == m_manifest.id || result.contains(id))
            continue;
        if (!m_plugins)
            continue;
        if (m_plugins->isDisabled(id))
            continue;
        IPluginProvider* provider = m_plugins->provider(id);
        if (!provider || !provider->capabilities().contains(capability))
            continue;
        result.append(id);
    }
    return result;
}

int BalancerProvider::timeoutSeconds(const QString& providerId) const
{
    if (!m_settings)
        return 30;
    const QJsonObject overrides =
        m_settings->value(QStringLiteral("Balancer.Timeouts")).toObject();
    if (overrides.contains(providerId)) {
        const int value = overrides.value(providerId).toInt();
        if (value > 0)
            return value;
    }
    const int fallback = m_settings->intValue(QStringLiteral("Balancer.TimeoutSeconds"), 30);
    return fallback > 0 ? fallback : 30;
}

void BalancerProvider::executeAsync(const QString& capability, const QJsonObject& params,
                                    PluginCallback callback)
{
    const QStringList ids = candidateIds(capability);
    if (ids.isEmpty()) {
        QJsonObject error;
        error.insert(QStringLiteral("error"),
                     QStringLiteral("Балансировщик: нет доступных коннекторов"));
        if (callback)
            callback(error);
        return;
    }

    const auto finish = [callback](const QJsonObject& result) {
        if (callback)
            callback(result);
    };

    auto attempt = std::make_shared<std::function<void(int)>>();
    *attempt = [this, ids, capability, params, finish, attempt](int index) {
        if (index >= ids.size()) {
            QJsonObject error;
            error.insert(QStringLiteral("error"),
                         QStringLiteral("Балансировщик: все коннекторы вернули ошибку"));
            finish(error);
            return;
        }
        const QString id = ids.at(index);
        IPluginProvider* provider = m_plugins ? m_plugins->provider(id) : nullptr;
        if (!provider) {
            (*attempt)(index + 1);
            return;
        }

        auto finished = std::make_shared<bool>(false);
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [attempt, finished, timer, index]() {
            if (*finished)
                return;
            *finished = true;
            timer->deleteLater();
            (*attempt)(index + 1);
        });
        timer->start(timeoutSeconds(id) * 1000);

        provider->executeAsync(
            capability, params,
            [attempt, finished, timer, index, finish, id](const QJsonObject& result) {
                if (*finished)
                    return;
                *finished = true;
                timer->stop();
                timer->deleteLater();
                const QString error = result.value(QStringLiteral("error")).toString();
                const QString text = result.value(QStringLiteral("text")).toString();
                const bool isTranslation = result.contains(QStringLiteral("text"));
                if (error.isEmpty() && (!isTranslation || !text.isEmpty())) {
                    QJsonObject forwarded = result;
                    forwarded.insert(QStringLiteral("service_id"), id);
                    finish(forwarded);
                    return;
                }
                (*attempt)(index + 1);
            });
    };
    (*attempt)(0);
}
