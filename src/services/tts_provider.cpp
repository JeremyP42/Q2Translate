#include "tts_provider.h"

#include "platform/win/speech_manager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {

QString xmlEscape(QString text)
{
    text.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    text.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    text.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    text.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    text.replace(QLatin1Char('\''), QStringLiteral("&apos;"));
    return text;
}

QString baseLanguage(QString code)
{
    code = code.trimmed().toLower();
    if (code.isEmpty() || code == QStringLiteral("auto"))
        return QStringLiteral("en");
    const int dash = code.indexOf(QLatin1Char('-'));
    if (dash > 0)
        code = code.left(dash);
    return code;
}

QJsonObject errorResult(const QString& text)
{
    QJsonObject result;
    result.insert(QStringLiteral("error"), text);
    return result;
}

} // namespace

TtsProvider::TtsProvider(const PluginManifest& manifest, QObject* parent)
    : IPluginProvider(parent)
    , m_manifest(manifest)
    , m_network(new QNetworkAccessManager(this))
{
    loadSettings();
}

QJsonObject TtsProvider::defaultSettings() const
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
        else
            settings.insert(key, QString());
    }
    return settings;
}

bool TtsProvider::loadSettings()
{
    m_settings = defaultSettings();
    if (m_manifest.settingsPath.isEmpty())
        return true;
    QFile file(m_manifest.settingsPath);
    if (!file.open(QIODevice::ReadOnly))
        return true;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return true;
    const QJsonObject stored = document.object();
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it)
        m_settings.insert(it.key(), it.value());
    return true;
}

bool TtsProvider::saveSettings()
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

bool TtsProvider::updateSettings(const QJsonObject& settings)
{
    m_settings = settings;
    const bool saved = saveSettings();
    emit settingsChanged();
    return saved;
}

QString TtsProvider::languageTag(const QString& code, bool googleStyle) const
{
    const QString base = baseLanguage(code);
    if (googleStyle)
        return base;
    static const QHash<QString, QString> tags = {
        {QStringLiteral("en"), QStringLiteral("en-US")},
        {QStringLiteral("ru"), QStringLiteral("ru-RU")},
        {QStringLiteral("de"), QStringLiteral("de-DE")},
        {QStringLiteral("fr"), QStringLiteral("fr-FR")},
        {QStringLiteral("es"), QStringLiteral("es-ES")},
        {QStringLiteral("it"), QStringLiteral("it-IT")},
        {QStringLiteral("pt"), QStringLiteral("pt-PT")},
        {QStringLiteral("tr"), QStringLiteral("tr-TR")},
        {QStringLiteral("uk"), QStringLiteral("uk-UA")},
        {QStringLiteral("pl"), QStringLiteral("pl-PL")},
        {QStringLiteral("nl"), QStringLiteral("nl-NL")},
        {QStringLiteral("cs"), QStringLiteral("cs-CZ")},
        {QStringLiteral("sv"), QStringLiteral("sv-SE")},
        {QStringLiteral("fi"), QStringLiteral("fi-FI")},
        {QStringLiteral("da"), QStringLiteral("da-DK")},
        {QStringLiteral("no"), QStringLiteral("nb-NO")},
        {QStringLiteral("ja"), QStringLiteral("ja-JP")},
        {QStringLiteral("ko"), QStringLiteral("ko-KR")},
        {QStringLiteral("zh"), QStringLiteral("zh-CN")},
        {QStringLiteral("ar"), QStringLiteral("ar-SA")},
        {QStringLiteral("hi"), QStringLiteral("hi-IN")}
    };
    return tags.value(base, base + QLatin1Char('-') + base.toUpper());
}

void TtsProvider::executeAsync(const QString& capability, const QJsonObject& params,
                               PluginCallback callback)
{
    if (capability != QStringLiteral("tts")) {
        callback(errorResult(QStringLiteral("TTS: неподдерживаемая возможность.")));
        return;
    }
    const QString text = params.value(QStringLiteral("text")).toString().trimmed();
    if (text.isEmpty()) {
        callback(errorResult(QStringLiteral("TTS: пустой текст.")));
        return;
    }
    const QString language = params.value(QStringLiteral("lang")).toString(
        params.value(QStringLiteral("to")).toString(QStringLiteral("en")));
    const bool slower = params.value(QStringLiteral("slower")).toBool(false);

    if (m_manifest.id == QStringLiteral("windows_sapi")) {
        SpeechManager::speak(text, slower);
        QJsonObject result;
        result.insert(QStringLiteral("ok"), true);
        callback(result);
        return;
    }
    if (m_manifest.id == QStringLiteral("google_tts")) {
        speakGoogle(text, language, callback);
        return;
    }
    if (m_manifest.id == QStringLiteral("yandex_tts")) {
        speakYandex(text, language, callback);
        return;
    }
    if (m_manifest.id == QStringLiteral("bing_tts")) {
        speakBing(text, language, callback);
        return;
    }
    callback(errorResult(QStringLiteral("TTS: неизвестный коннектор.")));
}

void TtsProvider::speakGoogle(const QString& text, const QString& language,
                              PluginCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ie"), QStringLiteral("UTF-8"));
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("tw-ob"));
    query.addQueryItem(QStringLiteral("tl"), languageTag(language, true));
    query.addQueryItem(QStringLiteral("q"), text.left(200));
    query.addQueryItem(QStringLiteral("total"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("idx"), QStringLiteral("0"));
    query.addQueryItem(QStringLiteral("textlen"), QString::number(text.size()));
    QUrl url(QStringLiteral("https://translate.google.com/translate_tts"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                     "AppleWebKit/537.36 (KHTML, like Gecko) "
                                     "Chrome/135.0.0.0 Safari/537.36"));
    request.setTransferTimeout(20000);
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(errorResult(QStringLiteral("Google TTS: %1").arg(reply->errorString())));
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray audio = reply->readAll();
        if (status != 200 || audio.isEmpty()) {
            callback(errorResult(QStringLiteral("Google TTS: HTTP %1.").arg(status)));
            return;
        }
        playAudio(audio, QStringLiteral("mp3"), callback);
    });
}

void TtsProvider::speakYandex(const QString& text, const QString& language,
                              PluginCallback callback)
{
    const QString key = m_settings.value(QStringLiteral("api_key")).toString().trimmed();
    const QString folder = m_settings.value(QStringLiteral("folder_id")).toString().trimmed();
    if (key.isEmpty() || folder.isEmpty()) {
        callback(errorResult(QStringLiteral("Yandex TTS: укажите API-ключ и folderId.")));
        return;
    }
    QString voice = m_settings.value(QStringLiteral("voice")).toString().trimmed();
    if (voice.isEmpty())
        voice = QStringLiteral("filipp");

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("text"), text.left(4000));
    form.addQueryItem(QStringLiteral("lang"), languageTag(language));
    form.addQueryItem(QStringLiteral("voice"), voice);
    form.addQueryItem(QStringLiteral("format"), QStringLiteral("mp3"));
    form.addQueryItem(QStringLiteral("folderId"), folder);

    QNetworkRequest request(QUrl(QStringLiteral(
        "https://tts.api.cloud.yandex.net/speech/v1/tts:synthesize")));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("Authorization", "Api-Key " + key.toUtf8());
    request.setTransferTimeout(30000);
    QNetworkReply* reply = m_network->post(request, form.query(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(errorResult(QStringLiteral("Yandex TTS: %1").arg(reply->errorString())));
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray audio = reply->readAll();
        if (status != 200 || audio.isEmpty()) {
            callback(errorResult(QStringLiteral("Yandex TTS: HTTP %1.").arg(status)));
            return;
        }
        playAudio(audio, QStringLiteral("mp3"), callback);
    });
}

void TtsProvider::speakBing(const QString& text, const QString& language,
                            PluginCallback callback)
{
    const QString key = m_settings.value(QStringLiteral("api_key")).toString().trimmed();
    QString region = m_settings.value(QStringLiteral("region")).toString().trimmed();
    if (key.isEmpty()) {
        callback(errorResult(QStringLiteral("Bing TTS: укажите ключ Azure Speech.")));
        return;
    }
    if (region.isEmpty())
        region = QStringLiteral("westeurope");

    const QString tag = languageTag(language);
    QString voice = m_settings.value(QStringLiteral("voice")).toString().trimmed();
    if (voice.isEmpty()) {
        if (tag.startsWith(QStringLiteral("ru")))
            voice = QStringLiteral("ru-RU-SvetlanaNeural");
        else if (tag.startsWith(QStringLiteral("de")))
            voice = QStringLiteral("de-DE-KatjaNeural");
        else if (tag.startsWith(QStringLiteral("fr")))
            voice = QStringLiteral("fr-FR-DeniseNeural");
        else if (tag.startsWith(QStringLiteral("es")))
            voice = QStringLiteral("es-ES-ElviraNeural");
        else
            voice = QStringLiteral("en-US-AriaNeural");
    }
    const QString ssml = QStringLiteral(
        "<speak version='1.0' xml:lang='%1'><voice name='%2'>%3</voice></speak>")
        .arg(tag, voice, xmlEscape(text.left(4000)));

    QNetworkRequest request(QUrl(
        QStringLiteral("https://%1.tts.speech.microsoft.com/cognitiveservices/v1").arg(region)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/ssml+xml"));
    request.setRawHeader("Ocp-Apim-Subscription-Key", key.toUtf8());
    request.setRawHeader("X-Microsoft-OutputFormat",
                         "audio-24khz-48kbitrate-mono-mp3");
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Q2Translate"));
    request.setTransferTimeout(30000);
    QNetworkReply* reply = m_network->post(request, ssml.toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(errorResult(QStringLiteral("Bing TTS: %1").arg(reply->errorString())));
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray audio = reply->readAll();
        if (status != 200 || audio.isEmpty()) {
            callback(errorResult(QStringLiteral("Bing TTS: HTTP %1.").arg(status)));
            return;
        }
        playAudio(audio, QStringLiteral("mp3"), callback);
    });
}

void TtsProvider::stopPlayback()
{
#ifdef Q_OS_WIN
    mciSendStringW(L"close qtranslate_tts", nullptr, 0, nullptr);
#endif
    if (!m_tempFile.isEmpty()) {
        QFile::remove(m_tempFile);
        m_tempFile.clear();
    }
}

void TtsProvider::playAudio(const QByteArray& audio, const QString& suffix,
                            PluginCallback callback)
{
    stopPlayback();
    m_tempFile = QDir(QDir::tempPath()).filePath(
        QStringLiteral("qtranslate_tts_%1.%2")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces), suffix));
    QFile file(m_tempFile);
    if (!file.open(QIODevice::WriteOnly) || file.write(audio) != audio.size()) {
        stopPlayback();
        callback(errorResult(QStringLiteral("TTS: не удалось сохранить аудио.")));
        return;
    }
    file.close();
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(m_tempFile);
    const QString command =
        QStringLiteral("open \"%1\" type mpegvideo alias qtranslate_tts").arg(nativePath);
    MCIERROR error =
        mciSendStringW(reinterpret_cast<LPCWSTR>(command.utf16()), nullptr, 0, nullptr);
    if (error != 0) {
        const QString waveCommand =
            QStringLiteral("open \"%1\" type waveaudio alias qtranslate_tts").arg(nativePath);
        error = mciSendStringW(reinterpret_cast<LPCWSTR>(waveCommand.utf16()), nullptr, 0,
                               nullptr);
    }
    if (error == 0)
        error = mciSendStringW(L"play qtranslate_tts", nullptr, 0, nullptr);
    if (error != 0) {
        stopPlayback();
        callback(errorResult(QStringLiteral("TTS: не удалось воспроизвести аудио.")));
        return;
    }
#endif
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    callback(result);
    QTimer::singleShot(90000, this, [this] { stopPlayback(); });
}
