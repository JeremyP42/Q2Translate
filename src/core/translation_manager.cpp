#include "translation_manager.h"

#include "history_manager.h"
#include "language_catalog.h"
#include "language_router.h"
#include "settings_manager.h"
#include "services/plugin_manager.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonValue>
#include <QSharedPointer>
#include <QUuid>

namespace {

struct RequestState {
    QString requestId;
    QString source;
    QString from;
    QString to;
    int pending = 0;
    bool historyRecorded = false;
};

QJsonObject errorObject(const QString& message)
{
    QJsonObject result;
    result.insert(QStringLiteral("error"), message);
    return result;
}

}

TranslationManager::TranslationManager(PathManager* paths, SettingsManager* settings,
                                       PluginManager* plugins, LanguageRouter* router,
                                       HistoryManager* history, QObject* parent)
    : QObject(parent)
    , m_paths(paths)
    , m_settings(settings)
    , m_plugins(plugins)
    , m_router(router)
    , m_history(history)
{
}

void TranslationManager::translate(const QString& text, const QString& fromCode,
                                   const QString& toCode, const QStringList& serviceIds)
{
    const QString source = text.trimmed();
    if (source.isEmpty())
        return;

    QString from = normalizeLanguageCode(fromCode);
    if (from.isEmpty())
        from = QStringLiteral("auto");

    const bool serviceDetection = m_settings
        && m_settings->stringValue(QStringLiteral("General.LanguageDetection"),
                                   QStringLiteral("local")) == QStringLiteral("service");
    QString detected;
    if (from == QStringLiteral("auto")) {
        if (!serviceDetection)
            detected = LanguageRouter::detectLanguage(source);
    } else {
        detected = from;
    }

    QString to = normalizeLanguageCode(toCode);
    if (to.isEmpty() || to == QStringLiteral("auto")) {
        to = detected.isEmpty()
            ? m_router->fallbackTarget()
            : m_router->resolveTarget(detected);
    }
    if (to == QStringLiteral("auto"))
        to = m_router->fallbackTarget();

    QStringList ids = serviceIds;
    if (ids.isEmpty() && m_plugins)
        ids = m_plugins->activeTranslationIds();
    if (ids.isEmpty()) {
        const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        emit translationStarted(requestId);
        QJsonObject result = errorObject(QStringLiteral("Не выбран ни один сервис перевода"));
        result.insert(QStringLiteral("service_id"), QString());
        emit serviceResult(requestId, QString(), result);
        emit translationFinished(requestId);
        return;
    }

    auto state = QSharedPointer<RequestState>::create();
    state->requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    state->source = source;
    state->from = detected;
    state->to = to;
    state->pending = int(ids.size());

    QJsonObject params;
    params.insert(QStringLiteral("text"), source);
    params.insert(QStringLiteral("from"), from);
    params.insert(QStringLiteral("to"), to);
    params.insert(QStringLiteral("detected_from"), detected);
    params.insert(QStringLiteral("from_name"),
                  languageNameByCode(from == QStringLiteral("auto") ? detected : from));
    params.insert(QStringLiteral("to_name"), languageNameByCode(to));
    params.insert(QStringLiteral("source_language_name"), languageNameByCode(detected));

    emit translationStarted(state->requestId);

    for (const QString& id : ids) {
        IPluginProvider* provider = m_plugins ? m_plugins->provider(id) : nullptr;
        if (!provider || !provider->capabilities().contains(QStringLiteral("translate"))) {
            QJsonObject result = errorObject(QStringLiteral("Сервис недоступен: %1").arg(id));
            result.insert(QStringLiteral("service_id"), id);
            emit serviceResult(state->requestId, id, result);
            if (--state->pending == 0) {
                emit translationFinished(state->requestId);
            }
            continue;
        }

        const QString serviceId = id;
        const QString serviceDisplayName = provider->manifest().name;
        provider->executeAsync(QStringLiteral("translate"), params,
            [this, state, serviceId, serviceDisplayName, serviceDetection, from](const QJsonObject& response) {
                QJsonObject result = response;
                result.insert(QStringLiteral("service_id"), serviceId);
                result.insert(QStringLiteral("service_name"), serviceDisplayName);
                QString detectedLang = state->from;
                if (serviceDetection && from == QStringLiteral("auto")) {
                    const QString reported = response.value(QStringLiteral("detected_lang")).toString();
                    if (!reported.isEmpty())
                        detectedLang = reported;
                }
                result.insert(QStringLiteral("detected_lang"), detectedLang);

                const QString error = result.value(QStringLiteral("error")).toString();
                const QString translated = result.value(QStringLiteral("text")).toString();
                if (error.isEmpty() && !translated.trimmed().isEmpty()
                    && !state->historyRecorded && m_history
                    && (!m_settings || m_settings->boolValue(QStringLiteral("General.EnableHistory"), true))) {
                    m_history->addEntry(state->source, translated, detectedLang,
                                        state->to, serviceDisplayName);
                    state->historyRecorded = true;
                }

                emit serviceResult(state->requestId, serviceId, result);
                if (--state->pending == 0)
                    emit translationFinished(state->requestId);
            });
    }
}

void TranslationManager::translateDocumentSegment(
    const QString& text, const QString& fromCode, const QString& toCode,
    const QString& serviceId, const std::function<void(const QJsonObject&)>& callback,
    const QJsonObject& extraParams)
{
    if (!callback)
        return;

    IPluginProvider* provider = m_plugins ? m_plugins->provider(serviceId) : nullptr;
    if (!provider || !provider->capabilities().contains(QStringLiteral("translate"))) {
        callback(errorObject(QStringLiteral("Сервис перевода недоступен: %1").arg(serviceId)));
        return;
    }

    QString from = normalizeLanguageCode(fromCode);
    if (from.isEmpty())
        from = QStringLiteral("auto");
    const bool serviceDetection = m_settings
        && m_settings->stringValue(QStringLiteral("General.LanguageDetection"),
                                   QStringLiteral("local")) == QStringLiteral("service");
    QString detected;
    if (from == QStringLiteral("auto")) {
        if (!serviceDetection)
            detected = LanguageRouter::detectLanguage(text);
    } else {
        detected = from;
    }

    QString to = normalizeLanguageCode(toCode);
    if (to.isEmpty() || to == QStringLiteral("auto"))
        to = m_router ? m_router->fallbackTarget() : QStringLiteral("ru");

    QJsonObject params;
    params.insert(QStringLiteral("text"), text);
    params.insert(QStringLiteral("from"), from);
    params.insert(QStringLiteral("to"), to);
    params.insert(QStringLiteral("detected_from"), detected);
    params.insert(QStringLiteral("from_name"),
                  languageNameByCode(from == QStringLiteral("auto") ? detected : from));
    params.insert(QStringLiteral("to_name"), languageNameByCode(to));
    params.insert(QStringLiteral("source_language_name"), languageNameByCode(detected));

    // LLM connectors accept optional translation hints (scene context, length
    // budget). Non-LLM connectors simply ignore unknown keys.
    for (auto it = extraParams.constBegin(); it != extraParams.constEnd(); ++it)
        params.insert(it.key(), it.value());

    provider->executeAsync(QStringLiteral("translate"), params, callback);
}

void TranslationManager::executeCapability(const QString& capability,
                                           const QJsonObject& params,
                                           const QString& providerId)
{
    if (!m_plugins)
        return;

    QString id = providerId;
    if (id.isEmpty()) {
        const QList<IPluginProvider*> providers = m_plugins->providersFor(capability);
        if (!providers.isEmpty())
            id = providers.first()->id();
    }

    IPluginProvider* provider = m_plugins->provider(id);
    if (!provider || !provider->capabilities().contains(capability)) {
        QJsonObject result = errorObject(QStringLiteral("Сервис не поддерживает %1").arg(capability));
        result.insert(QStringLiteral("service_id"), id);
        emit capabilityResult(capability, id, result);
        return;
    }

    const QString serviceId = id;
    provider->executeAsync(capability, params,
        [this, capability, serviceId](const QJsonObject& result) {
            emit capabilityResult(capability, serviceId, result);
        });
}

void TranslationManager::ocrAsync(const QImage& image, const QString& providerId,
                                  const QString& language)
{
    ocrRequestAsync(image, providerId, language,
                    [this, providerId](const QJsonObject& result) {
                        emit ocrResult(providerId, result);
                    });
}

void TranslationManager::ocrRequestAsync(const QImage& image, const QString& providerId,
                                         const QString& language,
                                         const std::function<void(const QJsonObject&)>& callback,
                                         bool skipDeskew, const QString& profile, int psm)
{
    const auto finish = [callback](const QJsonObject& result) {
        if (callback)
            callback(result);
    };

    if (!m_plugins || image.isNull()) {
        finish(errorObject(QStringLiteral("Нет изображения для OCR")));
        return;
    }

    QString id = providerId;
    if (id.isEmpty() && m_settings) {
        // The engine is chosen per file type; an empty value falls back to the
        // general OCR provider below.
        QString key = QStringLiteral("Ocr.Provider");
        if (profile == QStringLiteral("screen"))
            key = QStringLiteral("Ocr.ProviderScreen");
        else if (profile == QStringLiteral("image"))
            key = QStringLiteral("Ocr.ProviderImages");
        else if (profile == QStringLiteral("pdf"))
            key = QStringLiteral("Ocr.ProviderPdf");
        else if (profile == QStringLiteral("djvu"))
            key = QStringLiteral("Ocr.ProviderDjvu");
        id = m_settings->stringValue(key);
    }
    if (id.isEmpty())
        id = defaultOcrProviderId();

    IPluginProvider* provider = m_plugins->provider(id);
    if (!provider || !provider->capabilities().contains(QStringLiteral("ocr"))) {
        finish(errorObject(QStringLiteral("OCR-сервис недоступен: %1").arg(id)));
        return;
    }

    QString ocrLanguage = language;
    if (ocrLanguage.isEmpty() && m_settings) {
        ocrLanguage = m_settings->stringValue(QStringLiteral("Ocr.OcrLanguage"),
                                               QStringLiteral("eng"));
    }
    if (ocrLanguage.isEmpty())
        ocrLanguage = QStringLiteral("eng");
    ocrLanguage.replace(QLatin1Char('-'), QLatin1Char('+'));

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");

    QJsonObject params;
    params.insert(QStringLiteral("image_base64"), QString::fromLatin1(bytes.toBase64()));
    params.insert(QStringLiteral("lang"), ocrLanguage);
    params.insert(QStringLiteral("language"), ocrLanguage);
    if (skipDeskew)
        params.insert(QStringLiteral("skip_deskew"), true);
    if (!profile.isEmpty())
        params.insert(QStringLiteral("profile"), profile);
    if (psm > 0)
        params.insert(QStringLiteral("psm"), psm);

    provider->executeAsync(QStringLiteral("ocr"), params, finish);
}

QList<PluginManifest> TranslationManager::translationServices() const
{
    if (!m_plugins)
        return {};
    QList<PluginManifest> result;
    for (const PluginManifest& manifest : m_plugins->manifests()) {
        if (manifest.valid
            && manifest.capabilities.contains(QStringLiteral("translate"))
            && !m_plugins->isDisabled(manifest.id)
            && m_plugins->provider(manifest.id)) {
            result.append(manifest);
        }
    }
    return result;
}

QList<PluginManifest> TranslationManager::ocrServices() const
{
    if (!m_plugins)
        return {};
    QList<PluginManifest> result;
    for (const PluginManifest& manifest : m_plugins->manifests()) {
        if (manifest.valid
            && manifest.capabilities.contains(QStringLiteral("ocr"))
            && !m_plugins->isDisabled(manifest.id)
            && m_plugins->provider(manifest.id)) {
            result.append(manifest);
        }
    }
    return result;
}

QString TranslationManager::defaultOcrProviderId() const
{
    if (m_settings) {
        const QString configured = m_settings->stringValue(QStringLiteral("Ocr.Provider"));
        if (m_plugins && m_plugins->provider(configured))
            return configured;
    }
    const QList<PluginManifest> services = ocrServices();
    return services.isEmpty() ? QString() : services.first().id;
}

QStringList TranslationManager::ocrLanguages(const QString& providerId) const
{
    if (!m_plugins)
        return {};
    IPluginProvider* provider = m_plugins->provider(providerId);
    if (!provider)
        provider = m_plugins->provider(defaultOcrProviderId());
    return provider ? provider->availableLanguages() : QStringList();
}
