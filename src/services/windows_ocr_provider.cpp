#include "windows_ocr_provider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtConcurrent>

namespace {

QJsonObject ocrError(const QString& message)
{
    QJsonObject result;
    result.insert(QStringLiteral("error"), message);
    return result;
}

QString windowsLanguage(const QString& language)
{
    QString code = language;
    code.replace(QLatin1Char('+'), QLatin1Char(','));
    const QString first = code.section(QLatin1Char(','), 0, 0).trimmed().toLower();
    if (first.isEmpty() || first == QStringLiteral("auto"))
        return QString();

    static const QHash<QString, QString> map = {
        {QStringLiteral("eng"), QStringLiteral("en-US")},
        {QStringLiteral("en"), QStringLiteral("en-US")},
        {QStringLiteral("rus"), QStringLiteral("ru-RU")},
        {QStringLiteral("ru"), QStringLiteral("ru-RU")},
        {QStringLiteral("deu"), QStringLiteral("de-DE")},
        {QStringLiteral("ger"), QStringLiteral("de-DE")},
        {QStringLiteral("de"), QStringLiteral("de-DE")},
        {QStringLiteral("fra"), QStringLiteral("fr-FR")},
        {QStringLiteral("fre"), QStringLiteral("fr-FR")},
        {QStringLiteral("fr"), QStringLiteral("fr-FR")},
        {QStringLiteral("spa"), QStringLiteral("es-ES")},
        {QStringLiteral("es"), QStringLiteral("es-ES")},
        {QStringLiteral("ita"), QStringLiteral("it-IT")},
        {QStringLiteral("it"), QStringLiteral("it-IT")},
        {QStringLiteral("por"), QStringLiteral("pt-PT")},
        {QStringLiteral("pt"), QStringLiteral("pt-PT")},
        {QStringLiteral("ukr"), QStringLiteral("uk-UA")},
        {QStringLiteral("uk"), QStringLiteral("uk-UA")},
        {QStringLiteral("pol"), QStringLiteral("pl-PL")},
        {QStringLiteral("pl"), QStringLiteral("pl-PL")},
        {QStringLiteral("tur"), QStringLiteral("tr-TR")},
        {QStringLiteral("tr"), QStringLiteral("tr-TR")},
        {QStringLiteral("jpn"), QStringLiteral("ja-JP")},
        {QStringLiteral("ja"), QStringLiteral("ja-JP")},
        {QStringLiteral("chi_sim"), QStringLiteral("zh-Hans")},
        {QStringLiteral("zh"), QStringLiteral("zh-Hans")},
        {QStringLiteral("chi_tra"), QStringLiteral("zh-Hant")},
        {QStringLiteral("kor"), QStringLiteral("ko-KR")},
        {QStringLiteral("ko"), QStringLiteral("ko-KR")},
        {QStringLiteral("ara"), QStringLiteral("ar-SA")},
        {QStringLiteral("ar"), QStringLiteral("ar-SA")},
        {QStringLiteral("heb"), QStringLiteral("he-IL")},
        {QStringLiteral("he"), QStringLiteral("he-IL")},
        {QStringLiteral("ell"), QStringLiteral("el-GR")},
        {QStringLiteral("gre"), QStringLiteral("el-GR")},
        {QStringLiteral("el"), QStringLiteral("el-GR")},
        {QStringLiteral("ces"), QStringLiteral("cs-CZ")},
        {QStringLiteral("cze"), QStringLiteral("cs-CZ")},
        {QStringLiteral("cs"), QStringLiteral("cs-CZ")},
        {QStringLiteral("nld"), QStringLiteral("nl-NL")},
        {QStringLiteral("dut"), QStringLiteral("nl-NL")},
        {QStringLiteral("nl"), QStringLiteral("nl-NL")},
        {QStringLiteral("swe"), QStringLiteral("sv-SE")},
        {QStringLiteral("sv"), QStringLiteral("sv-SE")},
        {QStringLiteral("dan"), QStringLiteral("da-DK")},
        {QStringLiteral("da"), QStringLiteral("da-DK")},
        {QStringLiteral("fin"), QStringLiteral("fi-FI")},
        {QStringLiteral("fi"), QStringLiteral("fi-FI")},
        {QStringLiteral("nor"), QStringLiteral("nb-NO")},
        {QStringLiteral("nb"), QStringLiteral("nb-NO")},
        {QStringLiteral("hun"), QStringLiteral("hu-HU")},
        {QStringLiteral("hu"), QStringLiteral("hu-HU")},
        {QStringLiteral("ron"), QStringLiteral("ro-RO")},
        {QStringLiteral("rum"), QStringLiteral("ro-RO")},
        {QStringLiteral("ro"), QStringLiteral("ro-RO")},
        {QStringLiteral("bul"), QStringLiteral("bg-BG")},
        {QStringLiteral("bg"), QStringLiteral("bg-BG")},
        {QStringLiteral("srp"), QStringLiteral("sr-Latn-RS")},
        {QStringLiteral("sr"), QStringLiteral("sr-Latn-RS")},
        {QStringLiteral("hrv"), QStringLiteral("hr-HR")},
        {QStringLiteral("hr"), QStringLiteral("hr-HR")},
        {QStringLiteral("slk"), QStringLiteral("sk-SK")},
        {QStringLiteral("slo"), QStringLiteral("sk-SK")},
        {QStringLiteral("sk"), QStringLiteral("sk-SK")},
        {QStringLiteral("slv"), QStringLiteral("sl-SI")},
        {QStringLiteral("sl"), QStringLiteral("sl-SI")},
        {QStringLiteral("est"), QStringLiteral("et-EE")},
        {QStringLiteral("et"), QStringLiteral("et-EE")},
        {QStringLiteral("lav"), QStringLiteral("lv-LV")},
        {QStringLiteral("lv"), QStringLiteral("lv-LV")},
        {QStringLiteral("lit"), QStringLiteral("lt-LT")},
        {QStringLiteral("lt"), QStringLiteral("lt-LT")},
        {QStringLiteral("vie"), QStringLiteral("vi-VN")},
        {QStringLiteral("vi"), QStringLiteral("vi-VN")},
        {QStringLiteral("tha"), QStringLiteral("th-TH")},
        {QStringLiteral("th"), QStringLiteral("th-TH")},
        {QStringLiteral("hin"), QStringLiteral("hi-IN")},
        {QStringLiteral("hi"), QStringLiteral("hi-IN")}
    };

    if (map.contains(first))
        return map.value(first);
    if (first.contains(QLatin1Char('-')) && first.size() >= 5)
        return first;
    return QString();
}

QJsonObject runWindowsOcr(const PluginManifest& manifest, const QJsonObject& params)
{
    const QString script = QDir(manifest.directory).filePath(QStringLiteral("ocr.ps1"));
    if (!QFile::exists(script))
        return ocrError(QStringLiteral("Скрипт Windows OCR не найден: %1").arg(script));

    const QString base64 = params.value(QStringLiteral("image_base64")).toString();
    if (base64.isEmpty())
        return ocrError(QStringLiteral("Не передано изображение для OCR"));

    QImage image;
    if (!image.loadFromData(QByteArray::fromBase64(base64.toLatin1())))
        return ocrError(QStringLiteral("Не удалось декодировать изображение"));

    QTemporaryDir tempDir;
    if (!tempDir.isValid())
        return ocrError(QStringLiteral("Не удалось создать временную папку"));

    const QString inputPath = QDir(tempDir.path()).filePath(QStringLiteral("input.png"));
    const QString outputPath = QDir(tempDir.path()).filePath(QStringLiteral("output.txt"));
    if (!image.save(inputPath, "PNG"))
        return ocrError(QStringLiteral("Не удалось сохранить изображение"));

    const QString language =
        windowsLanguage(params.value(QStringLiteral("lang")).toString());

    QProcess process;
    const QString systemRoot = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
    const QString powershellPath = QDir(systemRoot)
        .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
    process.setProgram(QFile::exists(powershellPath)
                           ? QDir::toNativeSeparators(powershellPath)
                           : QStringLiteral("powershell.exe"));
    QStringList arguments;
    arguments << QStringLiteral("-NoProfile") << QStringLiteral("-NonInteractive")
              << QStringLiteral("-ExecutionPolicy") << QStringLiteral("Bypass")
              << QStringLiteral("-File") << QDir::toNativeSeparators(script)
              << QStringLiteral("-ImagePath") << QDir::toNativeSeparators(inputPath)
              << QStringLiteral("-OutputPath") << QDir::toNativeSeparators(outputPath);
    if (!language.isEmpty())
        arguments << QStringLiteral("-Language") << language;
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(15000))
        return ocrError(QStringLiteral("Не удалось запустить Windows PowerShell"));

    if (!process.waitForFinished(90000)) {
        process.kill();
        process.waitForFinished(5000);
        return ocrError(QStringLiteral("Windows OCR не завершился за 90 секунд"));
    }

    const QString errorOutput = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return ocrError(errorOutput.isEmpty()
                            ? QStringLiteral("Windows OCR завершился с кодом %1")
                                  .arg(process.exitCode())
                            : errorOutput);

    QString text;
    QFile output(outputPath);
    if (output.open(QIODevice::ReadOnly)) {
        QByteArray data = output.readAll();
        if (data.startsWith("\xEF\xBB\xBF"))
            data.remove(0, 3);
        text = QString::fromUtf8(data).trimmed();
    } else {
        text = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }

    QJsonObject result;
    result.insert(QStringLiteral("text"), text);
    result.insert(QStringLiteral("detected_lang"),
                  language.isEmpty() ? QStringLiteral("auto") : language);
    return result;
}

} // namespace

WindowsOcrProvider::WindowsOcrProvider(const PluginManifest& manifest, QObject* parent)
    : IPluginProvider(parent)
    , m_manifest(manifest)
{
    loadSettings();
}

QJsonObject WindowsOcrProvider::defaultSettings() const
{
    QJsonObject settings;
    for (const QJsonValue& value : m_manifest.settingsSchema) {
        const QJsonObject field = value.toObject();
        const QString key = field.value(QStringLiteral("key")).toString();
        if (key.isEmpty())
            continue;
        settings.insert(key, field.contains(QStringLiteral("default"))
                                 ? field.value(QStringLiteral("default"))
                                 : QJsonValue(QString()));
    }
    return settings;
}

bool WindowsOcrProvider::loadSettings()
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

bool WindowsOcrProvider::saveSettings()
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

bool WindowsOcrProvider::updateSettings(const QJsonObject& settings)
{
    m_settings = settings;
    const bool saved = saveSettings();
    emit settingsChanged();
    return saved;
}

void WindowsOcrProvider::executeAsync(const QString& capability, const QJsonObject& params,
                                      PluginCallback callback)
{
    if (capability != QStringLiteral("ocr")) {
        QJsonObject result;
        result.insert(QStringLiteral("error"),
                      QStringLiteral("Неподдерживаемая возможность: %1").arg(capability));
        if (callback)
            callback(result);
        return;
    }

    const PluginManifest manifest = m_manifest;
    auto future = QtConcurrent::run([manifest, params] {
        return runWindowsOcr(manifest, params);
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
