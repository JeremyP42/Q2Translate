#include "tesseract_provider.h"

#include "image_deskew.h"
#include "ocr_layout.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtConcurrent>

namespace {

QJsonObject tesseractError(const QString& message)
{
    QJsonObject result;
    result.insert(QStringLiteral("error"), message);
    return result;
}

struct TesseractPass {
    QString output;
    QString error;
};

bool hasReadableText(const QString& text)
{
    int letters = 0;
    for (const QChar& ch : text) {
        if (ch.isLetterOrNumber() && ++letters >= 2)
            return true;
    }
    return false;
}

TesseractPass runTesseractPass(const QString& executable, const QStringList& arguments)
{
    TesseractPass pass;

    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(10000)) {
        pass.error = QStringLiteral("Не удалось запустить tesseract.exe");
        return pass;
    }

    if (!process.waitForFinished(90000)) {
        process.kill();
        process.waitForFinished(5000);
        pass.error = QStringLiteral("Tesseract не завершился за 90 секунд");
        return pass;
    }

    pass.output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    const QString errorOutput = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        pass.error = errorOutput.isEmpty()
                         ? QStringLiteral("Tesseract завершился с кодом %1").arg(process.exitCode())
                         : errorOutput;
    }
    return pass;
}

QJsonObject runTesseract(const PluginManifest& manifest, const QJsonObject& params)
{
    const QString executable = manifest.nativeExecutable;
    if (executable.isEmpty() || !QFile::exists(executable))
        return tesseractError(QStringLiteral("tesseract.exe не найден: %1").arg(executable));

    const QString base64 = params.value(QStringLiteral("image_base64")).toString();
    if (base64.isEmpty())
        return tesseractError(QStringLiteral("Не передано изображение для OCR"));

    QImage image;
    if (!image.loadFromData(QByteArray::fromBase64(base64.toLatin1())))
        return tesseractError(QStringLiteral("Не удалось декодировать изображение"));

    // Straighten the page before recognition: from about three degrees of tilt the
    // engine starts merging text lines across columns, and past five degrees it
    // returns nothing at all. Screen captures measure at 0.0-0.04 degrees, so the
    // screen profile skips the estimate and saves the pass; documents and images
    // always measure. Callers that already straightened the image (document pages,
    // where the bitmap is drawn back) pass skip_deskew so boxes match their copy.
    const QString profile = params.value(QStringLiteral("profile")).toString();
    if (!params.value(QStringLiteral("skip_deskew")).toBool()
        && profile != QStringLiteral("screen") && profile != QStringLiteral("image")) {
        image = deskewImage(image, estimateSkewAngle(image));
    }

    QTemporaryDir tempDir;
    if (!tempDir.isValid())
        return tesseractError(QStringLiteral("Не удалось создать временную папку"));

    const QString inputPath = QDir(tempDir.path()).filePath(QStringLiteral("input.png"));
    if (!image.save(inputPath, "PNG"))
        return tesseractError(QStringLiteral("Не удалось сохранить изображение"));
    QString language = params.value(QStringLiteral("lang")).toString();
    if (language.isEmpty())
        language = QStringLiteral("eng");
    language.replace(QLatin1Char('-'), QLatin1Char('+'));

    QStringList options;
    options << inputPath << QStringLiteral("stdout")
            << QStringLiteral("-l") << language;

    const int requestedPsm = params.value(QStringLiteral("psm")).toInt();
    if (requestedPsm > 0)
        options << QStringLiteral("--psm") << QString::number(requestedPsm);

    const QString tessdata = manifest.tessdataPath;
    if (!tessdata.isEmpty()) {
        options << QStringLiteral("--tessdata-dir") << tessdata;
    }

    // The TSV config carries word boxes next to the text, which is what table
    // reconstruction needs. Rebuilding the text from it keeps the plain output
    // identical to the text renderer.
    const auto runPass = [&](const QStringList& passOptions) {
        QStringList arguments = passOptions;
        arguments << QStringLiteral("tsv");
        return runTesseractPass(executable, arguments);
    };

    TesseractPass pass = runPass(options);
    if (!pass.error.isEmpty())
        return tesseractError(pass.error);

    OcrDocument document = parseTesseractTsv(pass.output);

    // The default psm 3 segments the page freely, which suits screenshots with scattered
    // labels, but it gives up on a skewed page: text captured at 4-20 degrees comes back
    // empty. psm 6 assumes a single text block and keeps following skewed baselines, so one
    // retry recovers the page. Run it only when the first pass found no readable text at all,
    // so normal captures — including one-word selections — stay single-pass.
    if (!hasReadableText(document.text)) {
        QStringList fallback = options;
        fallback << QStringLiteral("--psm") << QStringLiteral("6");
        const TesseractPass retry = runPass(fallback);
        if (retry.error.isEmpty()) {
            OcrDocument retryDocument = parseTesseractTsv(retry.output);
            if (hasReadableText(retryDocument.text)) {
                document = retryDocument;
            }
        }
    }

    // Vertical CJK has no horizontal baselines, so the page comes back empty even
    // when the text is perfectly legible. Rotate a portrait page and read the
    // columns as lines; the boxes are mapped back to the caller's coordinates.
    if (!hasReadableText(document.text) && image.height() > image.width()) {
        const QImage rotated = image.transformed(QTransform().rotate(90),
                                                 Qt::SmoothTransformation);
        const QString rotatedPath = QDir(tempDir.path()).filePath(QStringLiteral("rotated.png"));
        if (!rotated.isNull() && rotated.save(rotatedPath, "PNG")) {
            QStringList rotatedOptions = options;
            rotatedOptions[0] = rotatedPath;
            const TesseractPass rotatedPass = runPass(rotatedOptions);
            if (rotatedPass.error.isEmpty()) {
                OcrDocument rotatedDocument = parseTesseractTsv(rotatedPass.output);
                if (hasReadableText(rotatedDocument.text)) {
                    const int originalHeight = image.height();
                    for (OcrWord& word : rotatedDocument.words) {
                        const QRect box = word.box;
                        word.box = QRect(box.top(), originalHeight - (box.left() + box.width()),
                                         box.height(), box.width());
                    }
                    document = rotatedDocument;
                }
            }
        }
    }

    QJsonObject result;
    result.insert(QStringLiteral("text"), document.text);

    const QString markdown = tableMarkdown(document);
    if (!markdown.isEmpty())
        result.insert(QStringLiteral("table_markdown"), markdown);

    QJsonArray wordArray;
    for (const OcrWord& word : document.words) {
        if (word.text.trimmed().isEmpty())
            continue;
        QJsonObject entry;
        entry.insert(QStringLiteral("text"), word.text);
        entry.insert(QStringLiteral("left"), word.box.left());
        entry.insert(QStringLiteral("top"), word.box.top());
        entry.insert(QStringLiteral("width"), word.box.width());
        entry.insert(QStringLiteral("height"), word.box.height());
        entry.insert(QStringLiteral("confidence"), word.confidence);
        entry.insert(QStringLiteral("block"), word.block);
        entry.insert(QStringLiteral("paragraph"), word.paragraph);
        entry.insert(QStringLiteral("line"), word.line);
        wordArray.append(entry);
    }
    if (!wordArray.isEmpty())
        result.insert(QStringLiteral("words"), wordArray);

    QJsonArray lineArray;
    for (const OcrLineBox& line : ocrLines(document)) {
        QJsonObject entry;
        entry.insert(QStringLiteral("text"), line.text);
        entry.insert(QStringLiteral("left"), line.box.left());
        entry.insert(QStringLiteral("top"), line.box.top());
        entry.insert(QStringLiteral("width"), line.box.width());
        entry.insert(QStringLiteral("height"), line.box.height());
        entry.insert(QStringLiteral("confidence"), line.confidence);
        entry.insert(QStringLiteral("block"), line.block);
        entry.insert(QStringLiteral("paragraph"), line.paragraph);
        entry.insert(QStringLiteral("line"), line.line);
        entry.insert(QStringLiteral("vertical"), line.box.height() > line.box.width() * 2);
        lineArray.append(entry);
    }
    if (!lineArray.isEmpty()) {
        result.insert(QStringLiteral("lines"), lineArray);
        result.insert(QStringLiteral("coordinate_space"), QStringLiteral("image-pixels"));
    }
    if (!profile.isEmpty())
        result.insert(QStringLiteral("profile"), profile);

    result.insert(QStringLiteral("detected_lang"), language);
    return result;
}

}

TesseractProvider::TesseractProvider(const PluginManifest& manifest, QObject* parent)
    : IPluginProvider(parent)
    , m_manifest(manifest)
{
    loadSettings();
}

QJsonObject TesseractProvider::defaultSettings() const
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

bool TesseractProvider::loadSettings()
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

bool TesseractProvider::saveSettings()
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

bool TesseractProvider::updateSettings(const QJsonObject& settings)
{
    m_settings = settings;
    const bool saved = saveSettings();
    emit settingsChanged();
    return saved;
}

void TesseractProvider::executeAsync(const QString& capability, const QJsonObject& params,
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
        return runTesseract(manifest, params);
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

QStringList TesseractProvider::availableLanguages() const
{
    QStringList languages;
    if (m_manifest.tessdataPath.isEmpty())
        return languages;
    QDir dir(m_manifest.tessdataPath);
    for (const QFileInfo& info : dir.entryInfoList({QStringLiteral("*.traineddata")}, QDir::Files)) {
        const QString code = info.completeBaseName();
        if (code.compare(QStringLiteral("osd"), Qt::CaseInsensitive) == 0)
            continue;
        languages.append(code);
    }
    languages.sort();
    return languages;
}
