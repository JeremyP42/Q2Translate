#include "tts_service.h"

#include "core/app_context.h"
#include "core/settings_manager.h"
#include "plugin_manager.h"
#include "provider_interface.h"

#include <QDebug>
#include <QJsonObject>

namespace {

QString providerId(SettingsManager* settings)
{
    const QString id = TtsService::normalizedProviderId(
        settings ? settings->stringValue(QStringLiteral("ServiceProviders.tts")) : QString());
    return id.isEmpty() ? QStringLiteral("windows_sapi") : id;
}

} // namespace

QString TtsService::normalizedProviderId(const QString& value)
{
    if (value.isEmpty())
        return QString();
    if (value == QStringLiteral("Windows SAPI"))
        return QStringLiteral("windows_sapi");
    if (value == QStringLiteral("Google TTS"))
        return QStringLiteral("google_tts");
    if (value == QStringLiteral("Yandex TTS"))
        return QStringLiteral("yandex_tts");
    if (value == QStringLiteral("Bing TTS"))
        return QStringLiteral("bing_tts");
    return value;
}

void TtsService::speak(AppContext* context, const QString& text, const QString& language,
                       bool slower)
{
    if (!context || text.trimmed().isEmpty())
        return;
    if (context->settings
        && !context->settings->boolValue(QStringLiteral("ServiceProviders.ttsOn"), true)) {
        return;
    }

    QString id = providerId(context->settings);
    IPluginProvider* provider =
        context->plugins ? context->plugins->provider(id) : nullptr;
    if (!provider
        || (context->plugins && context->plugins->isDisabled(id))
        || !provider->capabilities().contains(QStringLiteral("tts"))) {
        qWarning() << "[tts] selected provider is unavailable:" << id;
        return;
    }

    QJsonObject params;
    params.insert(QStringLiteral("text"), text);
    params.insert(QStringLiteral("lang"), language.isEmpty() ? QStringLiteral("en") : language);
    params.insert(QStringLiteral("slower"), slower);
    provider->executeAsync(QStringLiteral("tts"), params, [](const QJsonObject& result) {
        const QString error = result.value(QStringLiteral("error")).toString();
        if (!error.isEmpty())
            qWarning() << "[tts]" << error;
    });
}
