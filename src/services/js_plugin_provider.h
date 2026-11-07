#pragma once

#include "provider_interface.h"

#include <QJsonObject>
#include <QString>

class JsPluginProvider : public IPluginProvider
{
    Q_OBJECT

public:
    JsPluginProvider(const PluginManifest& manifest, const QJsonObject& networkConfig,
                     QObject* parent = nullptr);

    const PluginManifest& manifest() const override { return m_manifest; }
    QJsonObject settings() const override { return m_settings; }
    bool updateSettings(const QJsonObject& settings) override;
    QJsonObject defaultSettings() const override;
    void executeAsync(const QString& capability, const QJsonObject& params,
                      PluginCallback callback) override;
    void setNetworkConfig(const QJsonObject& networkConfig)
    {
        m_networkConfig = networkConfig;
    }

private:
    bool loadSettings();
    bool saveSettings();

    PluginManifest m_manifest;
    QJsonObject m_networkConfig;
    QJsonObject m_settings;
};
