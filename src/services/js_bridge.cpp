#include "js_bridge.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace {

QMutex g_cacheMutex;
QHash<QString, QVariantMap> g_cache;

QJsonObject readObject(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QJsonObject();
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return QJsonObject();
    return document.object();
}

}

JsBridge::JsBridge(const QString& pluginDir, const QString& userDir, QObject* parent)
    : QObject(parent)
    , m_pluginDir(pluginDir)
    , m_userDir(userDir)
{
}

QVariantMap JsBridge::loadJSON(const QString& fileName)
{
    const QString safeName = QFileInfo(fileName).fileName();
    if (safeName.isEmpty() || safeName != fileName)
        return {};

    QJsonObject shipped = readObject(QDir(m_pluginDir).filePath(safeName));
    const QJsonObject user = readObject(QDir(m_userDir).filePath(safeName));
    for (auto it = user.constBegin(); it != user.constEnd(); ++it)
        shipped.insert(it.key(), it.value());
    return shipped.toVariantMap();
}

QString JsBridge::md5(const QString& value) const
{
    return QString::fromLatin1(
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Md5).toHex());
}

QString JsBridge::aesDecryptBase64(const QString& payload, const QString& key,
                                   const QString& iv) const
{
#ifdef Q_OS_WIN
    QByteArray data = payload.toLatin1();
    data.replace('-', '+').replace('_', '/');
    while (data.size() % 4 != 0)
        data.append('=');
    data = QByteArray::fromBase64(data);
    if (data.isEmpty() || key.isEmpty() || iv.isEmpty())
        return {};

    const QByteArray keyHash = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5);
    const QByteArray ivHash = QCryptographicHash::hash(iv.toUtf8(), QCryptographicHash::Md5);

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    QByteArray keyObject;
    DWORD objectLength = 0;
    DWORD resultLength = 0;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0)
        return {};
    if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                          &resultLength, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    keyObject.resize(int(objectLength));
    if (BCryptGenerateSymmetricKey(algorithm, &keyHandle,
                                   reinterpret_cast<PUCHAR>(keyObject.data()), objectLength,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(keyHash.constData())),
                                   DWORD(keyHash.size()), 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    DWORD outputLength = 0;
    const NTSTATUS sizeStatus = BCryptDecrypt(
        keyHandle, reinterpret_cast<PUCHAR>(data.data()), DWORD(data.size()), nullptr,
        reinterpret_cast<PUCHAR>(const_cast<char*>(ivHash.constData())), DWORD(ivHash.size()),
        nullptr, 0, &outputLength, BCRYPT_BLOCK_PADDING);
    if (sizeStatus < 0) {
        BCryptDestroyKey(keyHandle);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    QByteArray output(int(outputLength), '\0');
    const NTSTATUS decryptStatus = BCryptDecrypt(
        keyHandle, reinterpret_cast<PUCHAR>(data.data()), DWORD(data.size()), nullptr,
        reinterpret_cast<PUCHAR>(const_cast<char*>(ivHash.constData())), DWORD(ivHash.size()),
        reinterpret_cast<PUCHAR>(output.data()), outputLength, &outputLength,
        BCRYPT_BLOCK_PADDING);
    BCryptDestroyKey(keyHandle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (decryptStatus < 0)
        return {};

    return QString::fromUtf8(output.constData(), int(outputLength));
#else
    Q_UNUSED(payload);
    Q_UNUSED(key);
    Q_UNUSED(iv);
    return {};
#endif
}

QVariantMap JsBridge::cacheGet(const QString& key) const
{
    QMutexLocker locker(&g_cacheMutex);
    return g_cache.value(m_pluginDir + QLatin1Char('\n') + key);
}

void JsBridge::cacheSet(const QString& key, const QVariantMap& value)
{
    QMutexLocker locker(&g_cacheMutex);
    g_cache.insert(m_pluginDir + QLatin1Char('\n') + key, value);
}
