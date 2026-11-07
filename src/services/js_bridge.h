#pragma once

#include <QJsonObject>
#include <QObject>
#include <QVariantMap>

class JsBridge : public QObject
{
    Q_OBJECT

public:
    JsBridge(const QString& pluginDir, const QString& userDir, QObject* parent = nullptr);

    Q_INVOKABLE QVariantMap loadJSON(const QString& fileName);
    Q_INVOKABLE QString md5(const QString& value) const;
    Q_INVOKABLE QString aesDecryptBase64(const QString& payload, const QString& key,
                                         const QString& iv) const;
    Q_INVOKABLE QVariantMap cacheGet(const QString& key) const;
    Q_INVOKABLE void cacheSet(const QString& key, const QVariantMap& value);

private:
    QString m_pluginDir;
    QString m_userDir;
};
