#include "document_reader.h"

#include <QtCore/private/qzipreader_p.h>

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

#include <utility>

namespace {

void setError(QString* error, const QString& message)
{
    if (error)
        *error = message;
}

QString extractParagraphs(const QByteArray& xml, QString* error)
{
    QXmlStreamReader reader(xml);
    QStringList paragraphs;
    QString paragraph;
    bool inParagraph = false;
    bool inText = false;

    while (!reader.atEnd()) {
        reader.readNext();

        if (reader.isStartElement()) {
            const QStringView name = reader.name();
            if (name == QLatin1String("p")) {
                inParagraph = true;
                paragraph.clear();
            } else if (name == QLatin1String("t")) {
                inText = true;
            } else if (name == QLatin1String("tab")) {
                paragraph += QLatin1Char('\t');
            } else if (name == QLatin1String("br") || name == QLatin1String("cr")) {
                paragraph += QLatin1Char('\n');
            }
        } else if (reader.isCharacters() && inText) {
            paragraph += reader.text().toString();
        } else if (reader.isEndElement()) {
            const QStringView name = reader.name();
            if (name == QLatin1String("t")) {
                inText = false;
            } else if (name == QLatin1String("p") && inParagraph) {
                inParagraph = false;
                const QString text = paragraph.trimmed();
                if (!text.isEmpty())
                    paragraphs.append(text);
            }
        }
    }

    if (reader.hasError()) {
        setError(error, QStringLiteral("Не удалось разобрать DOCX: %1").arg(reader.errorString()));
        return {};
    }

    return paragraphs.join(QLatin1Char('\n'));
}

QString readDocx(const QString& path, QString* error)
{
    QZipReader zip(path);
    if (!zip.isReadable()) {
        setError(error, QStringLiteral(
            "Не удалось открыть DOCX. Файл повреждён, зашифрован или не является документом Word."));
        return {};
    }

    QStringList parts;
    static const QRegularExpression headerFooter(
        QStringLiteral("^word/(header|footer)[0-9]*\\.xml$"));
    for (const QZipReader::FileInfo& info : zip.fileInfoList()) {
        const QString name = info.filePath;
        if (name == QLatin1String("word/document.xml"))
            parts.prepend(name);
        else if (headerFooter.match(name).hasMatch())
            parts.append(name);
    }

    if (parts.isEmpty()) {
        setError(error, QStringLiteral("В DOCX не найден word/document.xml."));
        return {};
    }

    QStringList blocks;
    for (const QString& name : std::as_const(parts)) {
        const QByteArray xml = zip.fileData(name);
        if (xml.isEmpty())
            continue;
        QString parseError;
        const QString text = extractParagraphs(xml, &parseError);
        if (!parseError.isEmpty()) {
            setError(error, parseError);
            return {};
        }
        if (!text.isEmpty())
            blocks.append(text);
    }

    if (blocks.isEmpty()) {
        setError(error, QStringLiteral("В документе нет текста для перевода."));
        return {};
    }

    return blocks.join(QStringLiteral("\n\n"));
}

} // namespace

QString DocumentReader::filter()
{
    return QStringLiteral(
        "Документы (*.docx *.pptx *.pdf *.fb2 *.epub *.xliff *.xlf *.ts *.json *.xml "
        "*.properties *.yaml *.yml *.srt *.vtt *.ass *.ssa *.csv *.ini *.md *.markdown "
        "*.txt *.html *.htm);;"
        "Office (*.docx *.pptx);;"
        "PDF (*.pdf);;"
        "Книги (*.fb2 *.epub);;"
        "XLIFF (*.xliff *.xlf);;"
        "Qt TS (*.ts);;"
        "JSON (*.json);;"
        "XML (*.xml);;"
        "Properties (*.properties);;"
        "YAML (*.yaml *.yml);;"
        "Субтитры (*.srt *.vtt *.ass *.ssa);;"
        "CSV (*.csv);;"
        "INI (*.ini);;"
        "Markdown (*.md *.markdown);;"
        "Текст (*.txt);;"
        "HTML (*.html *.htm);;"
        "Все файлы (*.*)");
}

bool DocumentReader::isSupported(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QStringList supported = {
        QStringLiteral("docx"), QStringLiteral("pptx"), QStringLiteral("pdf"),
        QStringLiteral("fb2"), QStringLiteral("epub"), QStringLiteral("xliff"),
        QStringLiteral("xlf"), QStringLiteral("ts"), QStringLiteral("json"),
        QStringLiteral("xml"), QStringLiteral("properties"), QStringLiteral("yaml"),
        QStringLiteral("yml"), QStringLiteral("srt"), QStringLiteral("vtt"),
        QStringLiteral("ass"), QStringLiteral("ssa"), QStringLiteral("csv"),
        QStringLiteral("ini"), QStringLiteral("md"), QStringLiteral("markdown"),
        QStringLiteral("txt"), QStringLiteral("html"), QStringLiteral("htm"),
        QStringLiteral("djvu"), QStringLiteral("djv")
    };
    return supported.contains(suffix);
}

QString DocumentReader::readText(const QString& path, QString* error)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        setError(error, QStringLiteral("Файл не найден: %1").arg(path));
        return {};
    }

    if (info.suffix().compare(QStringLiteral("docx"), Qt::CaseInsensitive) == 0)
        return readDocx(path, error);

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return {};
    }

    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    return text;
}
