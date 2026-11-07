#pragma once

#include <QJsonObject>
#include <QObject>
#include <QVariantMap>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

class HttpBridge : public QObject
{
    Q_OBJECT

public:
    explicit HttpBridge(const QJsonObject& networkConfig, QObject* parent = nullptr);

    Q_INVOKABLE QVariantMap get(const QString& url, const QVariantMap& headers = {});
    Q_INVOKABLE QVariantMap post(const QString& url, const QVariant& body,
                                 const QVariantMap& headers = {});
    Q_INVOKABLE QVariantMap multipart_post(const QString& url, const QVariantMap& fields,
                                           const QVariantMap& headers = {});

private:
    // Runs one request (plus configured retries) and turns the reply into the
    // result map understood by connectors.
    QVariantMap execute(const std::function<QNetworkReply*()>& makeRequest);

    QNetworkAccessManager* m_manager = nullptr;
    int m_timeoutMs = 30000;
    int m_connectTimeoutMs = 10000;
    int m_dataTimeoutMs = 15000;
    bool m_retryFailed = false;
    int m_retryCount = 1;
    int m_retryDelayMs = 1000;
};
