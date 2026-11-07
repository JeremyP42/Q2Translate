#pragma once

#include "services/provider_interface.h"

#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <functional>

class PathManager;
class SettingsManager;
class PluginManager;
class LanguageRouter;
class HistoryManager;

class TranslationManager : public QObject
{
    Q_OBJECT

public:
    TranslationManager(PathManager* paths, SettingsManager* settings,
                       PluginManager* plugins, LanguageRouter* router,
                       HistoryManager* history, QObject* parent = nullptr);

    void translate(const QString& text, const QString& fromCode, const QString& toCode,
                   const QStringList& serviceIds = {});
    void translateDocumentSegment(const QString& text, const QString& fromCode,
                                  const QString& toCode, const QString& serviceId,
                                  const std::function<void(const QJsonObject&)>& callback,
                                  const QJsonObject& extraParams = QJsonObject());
    void executeCapability(const QString& capability, const QJsonObject& params,
                           const QString& providerId = QString());
    void ocrAsync(const QImage& image, const QString& providerId = QString(),
                  const QString& language = QString());
    // Same request with a callback instead of the ocrResult signal, for callers that
    // chain several pages and must know when each one finished.
    void ocrRequestAsync(const QImage& image, const QString& providerId,
                         const QString& language,
                         const std::function<void(const QJsonObject&)>& callback,
                         bool skipDeskew = false,
                         const QString& profile = QStringLiteral("screen"),
                         int psm = 0);

    QList<PluginManifest> translationServices() const;
    QList<PluginManifest> ocrServices() const;
    QString defaultOcrProviderId() const;
    QStringList ocrLanguages(const QString& providerId) const;

signals:
    void translationStarted(const QString& requestId);
    void serviceResult(const QString& requestId, const QString& serviceId,
                       const QJsonObject& result);
    void translationFinished(const QString& requestId);
    void capabilityResult(const QString& capability, const QString& serviceId,
                          const QJsonObject& result);
    void ocrResult(const QString& serviceId, const QJsonObject& result);

private:
    PathManager* m_paths = nullptr;
    SettingsManager* m_settings = nullptr;
    PluginManager* m_plugins = nullptr;
    LanguageRouter* m_router = nullptr;
    HistoryManager* m_history = nullptr;
};
