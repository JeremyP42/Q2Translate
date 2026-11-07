#pragma once

#include "provider_interface.h"

#include <QJsonObject>
#include <QObject>
#include <QStringList>

// Reads XDXF dictionaries placed in the connector directory (including
// subdirectories) and exposes them through the regular dictionary capability.
class XdxfProvider : public IPluginProvider
{
    Q_OBJECT

public:
    explicit XdxfProvider(const PluginManifest& manifest, QObject* parent = nullptr);

    const PluginManifest& manifest() const override { return m_manifest; }
    QJsonObject settings() const override { return m_settings; }
    bool updateSettings(const QJsonObject& settings) override;
    QJsonObject defaultSettings() const override;
    void executeAsync(const QString& capability, const QJsonObject& params,
                      PluginCallback callback) override;

    // Subdirectories of the connector folder that contain at least one XDXF
    // file.  They become selectable variants in the connector card.
    static QStringList dictionaryFolders(const QString& root);

private:
    bool loadSettings();
    bool saveSettings();
    QStringList dictionaryFiles() const;

    PluginManifest m_manifest;
    QJsonObject m_settings;
};
