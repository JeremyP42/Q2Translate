#include "http_bridge.h"

#include <QEventLoop>
#include <QHttpMultiPart>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <functional>

namespace {

// Cookies must survive individual bridge instances: HttpBridge is created per
// execute() call, but the site session (Set-Cookie on the first page request)
// has to be sent with the following requests.
QList<QNetworkCookie>& sharedCookies()
{
    static QList<QNetworkCookie> cookies;
    return cookies;
}

class PersistentCookieJar final : public QNetworkCookieJar
{
public:
    explicit PersistentCookieJar(QObject* parent = nullptr)
        : QNetworkCookieJar(parent)
    {
        setAllCookies(sharedCookies());
    }

    ~PersistentCookieJar() override
    {
        sharedCookies() = allCookies();
    }
};

QByteArray variantToBytes(const QVariant& value, QString* contentType)
{
    // QJSEngine hands a JavaScript object to an invokable QVariant parameter as a
    // QJSValue wrapper, not as a QVariantMap. Without unwrapping it the body
    // serialises to an empty request, which silently breaks every LLM connector.
    QVariant payload = value;
    if (payload.userType() == qMetaTypeId<QJSValue>())
        payload = payload.value<QJSValue>().toVariant();

    const QVariant& body = payload;
    if (body.userType() == QMetaType::QVariantMap) {
        if (contentType)
            *contentType = QStringLiteral("application/json");
        return QJsonDocument(QJsonObject::fromVariantMap(body.toMap()))
            .toJson(QJsonDocument::Compact);
    }
    if (body.userType() == QMetaType::QVariantList) {
        if (contentType)
            *contentType = QStringLiteral("application/json");
        return QJsonDocument(QJsonArray::fromVariantList(body.toList()))
            .toJson(QJsonDocument::Compact);
    }
    if (body.userType() == QMetaType::QByteArray)
        return body.toByteArray();
    if (body.userType() == QMetaType::QString) {
        if (contentType)
            *contentType = QStringLiteral("text/plain; charset=utf-8");
        return body.toString().toUtf8();
    }
    return body.toString().toUtf8();
}

}

HttpBridge::HttpBridge(const QJsonObject& networkConfig, QObject* parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
{
    m_manager->setCookieJar(new PersistentCookieJar(m_manager));
    m_timeoutMs = networkConfig.value(QStringLiteral("Timeout")).toInt(20000);
    if (m_timeoutMs < 3000)
        m_timeoutMs = 3000;
    if (m_timeoutMs > 180000)
        m_timeoutMs = 180000;

    // Internet.ConnectTimeout is the guard until the first response byte,
    // Internet.DataTimeout limits a stalled transfer, Internet.Timeout is the
    // total budget for the whole request.
    m_connectTimeoutMs = networkConfig.value(QStringLiteral("ConnectTimeout")).toInt(5000);
    if (m_connectTimeoutMs < 1000)
        m_connectTimeoutMs = 1000;
    m_dataTimeoutMs = networkConfig.value(QStringLiteral("DataTimeout")).toInt(15000);
    if (m_dataTimeoutMs < 1000)
        m_dataTimeoutMs = 1000;

    m_retryFailed = networkConfig.value(QStringLiteral("RetryFailed")).toBool(false);
    m_retryCount = std::clamp(networkConfig.value(QStringLiteral("RetryCount")).toInt(1), 0, 10);
    m_retryDelayMs = std::clamp(networkConfig.value(QStringLiteral("RetryDelay")).toInt(1), 0, 30) * 1000;

    const QJsonObject proxy = networkConfig.value(QStringLiteral("Proxy")).toObject();
    if (proxy.value(QStringLiteral("SendThroughProxy")).toBool(false)) {
        const int proxyType = proxy.value(QStringLiteral("ProxyType")).toInt(3);
        if (proxyType == 0) {
            m_manager->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        } else if (proxyType == 1 || proxyType == 2) {
            // 1 = system proxy; 2 = old "automatic" value, also system based.
            const QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(
                QNetworkProxyQuery(QUrl(QStringLiteral("https://www.google.com"))));
            if (!proxies.isEmpty())
                m_manager->setProxy(proxies.first());
        } else {
            // 3 = manual proxy.
            const QString host = proxy.value(QStringLiteral("Host")).toString().trimmed();
            if (host.isEmpty()) {
                m_manager->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
            } else {
                QNetworkProxy::ProxyType type = QNetworkProxy::HttpProxy;
                const QString scheme = proxy.value(QStringLiteral("Scheme")).toString();
                if (scheme.compare(QStringLiteral("socks5"), Qt::CaseInsensitive) == 0)
                    type = QNetworkProxy::Socks5Proxy;
                QNetworkProxy networkProxy(
                    type, host, quint16(proxy.value(QStringLiteral("Port")).toInt(0)),
                    proxy.value(QStringLiteral("Username")).toString(),
                    proxy.value(QStringLiteral("Password")).toString());
                m_manager->setProxy(networkProxy);
            }
        }
    }
}

QVariantMap HttpBridge::execute(const std::function<QNetworkReply*()>& makeRequest)
{
    const int attempts = m_retryFailed ? m_retryCount + 1 : 1;
    QVariantMap result;

    for (int attempt = 0; attempt < attempts; ++attempt) {
        QNetworkReply* reply = makeRequest();
        if (!reply) {
            result.insert(QStringLiteral("status"), 0);
            result.insert(QStringLiteral("body"), QString());
            result.insert(QStringLiteral("error"), QStringLiteral("HTTP request failed"));
            return result;
        }

        QEventLoop loop;
        QTimer totalTimer;
        QTimer phaseTimer;
        totalTimer.setSingleShot(true);
        phaseTimer.setSingleShot(true);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&totalTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&phaseTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        totalTimer.start(m_timeoutMs);
        phaseTimer.start(std::min(m_connectTimeoutMs, m_timeoutMs));
        const auto restartPhase = [this, &phaseTimer] {
            phaseTimer.start(std::min(m_dataTimeoutMs, m_timeoutMs));
        };
        QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, restartPhase);
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, restartPhase);
        if (!reply->isFinished())
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        totalTimer.stop();
        phaseTimer.stop();

        if (!reply->isFinished()) {
            reply->abort();
            reply->deleteLater();
            result.insert(QStringLiteral("status"), 0);
            result.insert(QStringLiteral("body"), QString());
            result.insert(QStringLiteral("error"), QStringLiteral("HTTP timeout"));
        } else {
            const QByteArray data = reply->readAll();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString responseContentType =
                reply->header(QNetworkRequest::ContentTypeHeader).toString();
            const QString networkError = reply->error() == QNetworkReply::NoError
                ? QString() : reply->errorString();
            reply->deleteLater();

            result.clear();
            result.insert(QStringLiteral("status"), status);
            result.insert(QStringLiteral("body"), QString::fromUtf8(data));
            result.insert(QStringLiteral("body_base64"), QString::fromLatin1(data.toBase64()));
            result.insert(QStringLiteral("content_type"), responseContentType);
            result.insert(QStringLiteral("error"), status == 0 ? networkError : QString());
        }

        const bool retryable = result.value(QStringLiteral("status")).toInt() == 0;
        if (!retryable || attempt + 1 >= attempts)
            break;
        if (m_retryDelayMs > 0) {
            QEventLoop pause;
            QTimer::singleShot(m_retryDelayMs, &pause, &QEventLoop::quit);
            pause.exec(QEventLoop::ExcludeUserInputEvents);
        }
    }

    return result;
}

QVariantMap HttpBridge::get(const QString& url, const QVariantMap& headers)
{
    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) Q2Translate/2.7.1"));
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());

    return execute([this, request] { return m_manager->get(request); });
}

QVariantMap HttpBridge::post(const QString& url, const QVariant& body,
                             const QVariantMap& headers)
{
    QString contentType;
    const QByteArray payload = variantToBytes(body, &contentType);

    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) Q2Translate/2.7.1"));
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
    if (!request.hasRawHeader("Content-Type") && !contentType.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);

    return execute([this, request, payload] { return m_manager->post(request, payload); });
}

QVariantMap HttpBridge::multipart_post(const QString& url, const QVariantMap& fields,
                                       const QVariantMap& headers)
{
    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) Q2Translate/2.7.1"));
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());

    const auto makeRequest = [this, request, fields]() -> QNetworkReply* {
        auto* multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
            QHttpPart part;
            const QVariant value = it.value();
            if (value.userType() == QMetaType::QVariantMap) {
                const QVariantMap description = value.toMap();
                const QString type = description.value(QStringLiteral("type")).toString();
                if (type == QStringLiteral("file")) {
                    const QString fileName =
                        description.value(QStringLiteral("filename")).toString();
                    QString mime = description.value(QStringLiteral("contentType")).toString();
                    if (mime.isEmpty())
                        mime = QStringLiteral("application/octet-stream");
                    const QByteArray data = QByteArray::fromBase64(
                        description.value(QStringLiteral("base64")).toString().toLatin1());
                    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                                   QStringLiteral("form-data; name=\"%1\"; filename=\"%2\"")
                                       .arg(it.key(), fileName));
                    part.setHeader(QNetworkRequest::ContentTypeHeader, mime);
                    part.setBody(data);
                } else {
                    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                                   QStringLiteral("form-data; name=\"%1\"").arg(it.key()));
                    part.setBody(description.value(QStringLiteral("value")).toString().toUtf8());
                }
            } else {
                part.setHeader(QNetworkRequest::ContentDispositionHeader,
                               QStringLiteral("form-data; name=\"%1\"").arg(it.key()));
                part.setBody(value.toString().toUtf8());
            }
            multipart->append(part);
        }
        QNetworkReply* reply = m_manager->post(request, multipart);
        multipart->setParent(reply);
        return reply;
    };

    return execute(makeRequest);
}
