#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

struct PluginManifest {
    QString id;
    QString type;
    QString name;
    QString version;
    QString engine;
    QString description;
    QString access;
    QString directory;
    QString serviceScript;
    QString settingsPath;
    QString iconPath;
    QString nativeExecutable;
    QString nativeProvider;
    QString tessdataPath;
    QStringList capabilities;
    QJsonArray settingsSchema;
    QJsonObject raw;
    int order = 100;
    bool enabledByDefault = true;
    bool valid = false;
    QString error;
};

using PluginCallback = std::function<void(const QJsonObject&)>;

class IPluginProvider : public QObject
{
    Q_OBJECT

public:
    explicit IPluginProvider(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    ~IPluginProvider() override = default;

    virtual const PluginManifest& manifest() const = 0;
    virtual QString id() const { return manifest().id; }
    virtual QStringList capabilities() const { return manifest().capabilities; }
    virtual QJsonObject settings() const = 0;
    virtual bool updateSettings(const QJsonObject& settings) = 0;
    virtual QJsonObject defaultSettings() const = 0;
    virtual void executeAsync(const QString& capability, const QJsonObject& params,
                              PluginCallback callback) = 0;
    virtual QStringList availableLanguages() const { return {}; }

signals:
    void settingsChanged();
};
