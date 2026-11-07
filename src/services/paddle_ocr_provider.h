#pragma once

#include "provider_interface.h"

#include <QJsonObject>

// Runs the bundled PP-OCRv5 models through onnxruntime: DB text detection,
// per-box recognition and CTC decoding. Everything it needs lives next to the
// manifest in services/ocr/paddleocr (onnxruntime.dll, models/v5/*.onnx, dict.txt).
class PaddleOcrProvider : public IPluginProvider
{
    Q_OBJECT

public:
    explicit PaddleOcrProvider(const PluginManifest& manifest, QObject* parent = nullptr);

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
