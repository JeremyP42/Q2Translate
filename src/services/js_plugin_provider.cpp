#include "js_plugin_provider.h"

#include "http_bridge.h"
#include "js_bridge.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QThread>
#include <QtConcurrent>

namespace {

QJsonObject errorResult(const QString& message)
{
    QJsonObject result;
    result.insert(QStringLiteral("error"), message);
    return result;
}

QJsonObject runPlugin(const PluginManifest& manifest, const QJsonObject& settings,
                      const QJsonObject& networkConfig, const QString& capability,
                      const QJsonObject& params)
{
    QFile scriptFile(manifest.serviceScript);
    if (!scriptFile.open(QIODevice::ReadOnly)) {
        return errorResult(QStringLiteral("Не удалось открыть service.js: %1")
                               .arg(scriptFile.errorString()));
    }
    const QString source = QString::fromUtf8(scriptFile.readAll());

    QJSEngine engine;
    engine.installExtensions(QJSEngine::ConsoleExtension);

    HttpBridge httpBridge(networkConfig);
    JsBridge jsBridge(manifest.directory,
                      QFileInfo(manifest.settingsPath).absolutePath());
    engine.globalObject().setProperty(QStringLiteral("http"),
                                      engine.newQObject(&httpBridge));
    engine.globalObject().setProperty(QStringLiteral("qtranslate"),
                                      engine.newQObject(&jsBridge));
    engine.globalObject().setProperty(QStringLiteral("settings"),
                                      engine.toScriptValue(settings.toVariantMap()));
    engine.evaluate(QStringLiteral(
        "var json = JSON;"
        "function loadJSON(name) { return qtranslate.loadJSON(name); }"));

    const QJSValue scriptResult = engine.evaluate(source, manifest.serviceScript);
    if (scriptResult.isError()) {
        return errorResult(QStringLiteral("Ошибка service.js: %1 (строка %2)")
                               .arg(scriptResult.toString())
                               .arg(scriptResult.property(QStringLiteral("lineNumber")).toInt()));
    }

    const QJSValue execute = engine.globalObject().property(QStringLiteral("execute"));
    if (!execute.isCallable()) {
        return errorResult(QStringLiteral("service.js не содержит функцию execute(capability, params)"));
    }

    QJSValueList arguments;
    arguments << QJSValue(capability) << engine.toScriptValue(params.toVariantMap());
    const QJSValue result = execute.call(arguments);
    if (result.isError()) {
        return errorResult(QStringLiteral("Ошибка выполнения: %1 (строка %2)")
                               .arg(result.toString())
                               .arg(result.property(QStringLiteral("lineNumber")).toInt()));
    }

    QJsonObject output;
    if (result.isObject()) {
        output = QJsonObject::fromVariantMap(result.toVariant().toMap());
    } else if (result.isString()) {
        output.insert(QStringLiteral("text"), result.toString());
    } else if (!result.isUndefined() && !result.isNull()) {
        output.insert(QStringLiteral("text"), result.toVariant().toString());
    }

    if (output.isEmpty())
        output.insert(QStringLiteral("error"), QStringLiteral("Плагин вернул пустой результат"));
    return output;
}

}

JsPluginProvider::JsPluginProvider(const PluginManifest& manifest,
                                   const QJsonObject& networkConfig, QObject* parent)
    : IPluginProvider(parent)
    , m_manifest(manifest)
    , m_networkConfig(networkConfig)
{
    loadSettings();
}

QJsonObject JsPluginProvider::defaultSettings() const
{
    QJsonObject settings;
    for (const QJsonValue& value : m_manifest.settingsSchema) {
        const QJsonObject field = value.toObject();
        const QString key = field.value(QStringLiteral("key")).toString();
        if (key.isEmpty())
            continue;
        if (field.contains(QStringLiteral("default"))) {
            settings.insert(key, field.value(QStringLiteral("default")));
            continue;
        }
        const QString type = field.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("boolean"))
            settings.insert(key, false);
        else if (type == QStringLiteral("number"))
            settings.insert(key, 0);
        else if (type == QStringLiteral("select")) {
            const QJsonArray options = field.value(QStringLiteral("options")).toArray();
            settings.insert(key, options.isEmpty()
                                     ? QJsonValue(QString())
                                     : options.first().toObject().value(QStringLiteral("value")));
        } else {
            settings.insert(key, QString());
        }
    }
    return settings;
}

bool JsPluginProvider::loadSettings()
{
    m_settings = defaultSettings();
    if (m_manifest.settingsPath.isEmpty())
        return true;

    QFile file(m_manifest.settingsPath);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error == QJsonParseError::NoError && document.isObject()) {
            const QJsonObject stored = document.object();
            for (auto it = stored.constBegin(); it != stored.constEnd(); ++it)
                m_settings.insert(it.key(), it.value());
        }
    }
    return true;
}

bool JsPluginProvider::saveSettings()
{
    if (m_manifest.settingsPath.isEmpty())
        return false;

    QDir().mkpath(QFileInfo(m_manifest.settingsPath).absolutePath());
    QSaveFile file(m_manifest.settingsPath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray data = QJsonDocument(m_settings).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit())
        return false;
    return true;
}

bool JsPluginProvider::updateSettings(const QJsonObject& settings)
{
    m_settings = settings;
    const bool saved = saveSettings();
    emit settingsChanged();
    return saved;
}

void JsPluginProvider::executeAsync(const QString& capability, const QJsonObject& params,
                                    PluginCallback callback)
{
    const PluginManifest manifest = m_manifest;
    const QJsonObject settings = m_settings;
    const QJsonObject networkConfig = m_networkConfig;

    auto future = QtConcurrent::run([manifest, settings, networkConfig, capability, params] {
        return runPlugin(manifest, settings, networkConfig, capability, params);
    });

    auto* watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this,
            [watcher, callback = std::move(callback)] {
                const QJsonObject result = watcher->result();
                watcher->deleteLater();
                if (callback)
                    callback(result);
            });
    watcher->setFuture(future);
}
