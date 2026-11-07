#pragma once

#include "provider_interface.h"

#include <QStringList>

class PluginManager;
class SettingsManager;

// A connector that does not translate by itself: it forwards the request to a
// primary connector and, when that one fails or times out, walks through the
// list of enabled fallback connectors in order.
class BalancerProvider : public IPluginProvider
{
    Q_OBJECT

public:
    BalancerProvider(const PluginManifest& manifest, PluginManager* plugins,
                     SettingsManager* settings, QObject* parent = nullptr);

    const PluginManifest& manifest() const override { return m_manifest; }
    QJsonObject settings() const override { return {}; }
    bool updateSettings(const QJsonObject& settings) override;
    QJsonObject defaultSettings() const override { return {}; }
    void executeAsync(const QString& capability, const QJsonObject& params,
                      PluginCallback callback) override;

private:
    QStringList candidateIds(const QString& capability) const;
    int timeoutSeconds(const QString& providerId) const;

    PluginManifest m_manifest;
    PluginManager* m_plugins = nullptr;
    SettingsManager* m_settings = nullptr;
};
