#pragma once

#include "provider_interface.h"

#include <QJsonObject>

class WindowsOcrProvider : public IPluginProvider
{
    Q_OBJECT

public:
    explicit WindowsOcrProvider(const PluginManifest& manifest, QObject* parent = nullptr);

    const PluginManifest& manifest() const override { return m_manifest; }
    QJsonObject settings() const override { return m_settings; }
    bool updateSettings(const QJsonObject& settings) override;
    QJsonObject defaultSettings() const override;
    void executeAsync(const QString& capability, const QJsonObject& params,
                      PluginCallback callback) override;

private:
    bool loadSettings();
    bool saveSettings();

    PluginManifest m_manifest;
    QJsonObject m_settings;
};
