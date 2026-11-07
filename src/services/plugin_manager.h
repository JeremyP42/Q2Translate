#pragma once

#include "provider_interface.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

class PathManager;
class SettingsManager;

class PluginManager : public QObject
{
    Q_OBJECT

public:
    PluginManager(PathManager* paths, SettingsManager* settings, QObject* parent = nullptr);

    void scan();

    QList<PluginManifest> manifests() const { return m_manifests; }
    IPluginProvider* provider(const QString& id) const;
    QList<IPluginProvider*> providers() const;
    QList<IPluginProvider*> providersFor(const QString& capability) const;

    QStringList activeTranslationIds() const;
    void setActiveServices(const QStringList& ids);

    bool isDisabled(const QString& id) const;
    void setDisabled(const QString& id, bool disabled);

    QJsonObject networkConfig() const;
    void refreshNetworkConfig();

signals:
    void changed();

private:
    void scanRoot(const QString& root);
    PluginManifest parseManifest(const QString& manifestPath, const QString& root);
    QString normalizedServiceId(const QJsonValue& value) const;
    QString settingsPathFor(const QString& id) const;

    PathManager* m_paths = nullptr;
    SettingsManager* m_settings = nullptr;
    QList<PluginManifest> m_manifests;
    QHash<QString, IPluginProvider*> m_providers;
};
