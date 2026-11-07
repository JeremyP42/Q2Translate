#pragma once

#include "provider_interface.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

class QNetworkReply;

// Native TTS connector.  Handles the offline Windows SAPI engine and the
// network engines (Google, Yandex SpeechKit, Azure/Bing Speech).
class TtsProvider : public IPluginProvider
{
    Q_OBJECT

public:
    explicit TtsProvider(const PluginManifest& manifest, QObject* parent = nullptr);

    const PluginManifest& manifest() const override { return m_manifest; }
    QJsonObject settings() const override { return m_settings; }
    bool updateSettings(const QJsonObject& settings) override;
    QJsonObject defaultSettings() const override;
    void executeAsync(const QString& capability, const QJsonObject& params,
                      PluginCallback callback) override;

private:
    bool loadSettings();
    bool saveSettings();
    void speakGoogle(const QString& text, const QString& language, PluginCallback callback);
    void speakYandex(const QString& text, const QString& language, PluginCallback callback);
    void speakBing(const QString& text, const QString& language, PluginCallback callback);
    void playAudio(const QByteArray& audio, const QString& suffix, PluginCallback callback);
    void stopPlayback();
    QString languageTag(const QString& code, bool googleStyle = false) const;

    PluginManifest m_manifest;
    QJsonObject m_settings;
    QNetworkAccessManager* m_network = nullptr;
    QString m_tempFile;
};
