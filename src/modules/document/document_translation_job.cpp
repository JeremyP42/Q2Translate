#include "document_translation_job.h"

#include "core/app_context.h"
#include "core/language_catalog.h"
#include "core/language_router.h"
#include "core/path_manager.h"
#include "core/settings_manager.h"
#include "core/translation_manager.h"
#include "services/image_deskew.h"
#include "services/spell_service.h"

#include <QtCore/private/qzipreader_p.h>
#include <QtCore/private/qzipwriter_p.h>

#include <QDomDocument>
#include <QDomElement>
#include <QColor>
#include <QBuffer>
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPageSize>
#include <QPainter>
#include <memory>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>
#include <QPdfWriter>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSharedPointer>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <yaml-cpp/yaml.h>

#ifdef Q_OS_WIN
#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>
#endif

#include <algorithm>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace {


// Join the next line to a paragraph, removing a line-break hyphen
// ("ней-" + "рон" -> "нейрон") instead of leaving it in the source text.
void appendWrappedText(QString& target, const QString& next)
{
    const QString chunk = next.trimmed();
    if (chunk.isEmpty())
        return;
    if (target.endsWith(QLatin1Char('-')) && !target.endsWith(QStringLiteral("--"))) {
        const QChar first = chunk.at(0);
        if (first.isLetter() && first.isLower()) {
            target.chop(1);
            target += chunk;
            return;
        }
    }
    if (!target.isEmpty())
        target += QLatin1Char(' ');
    target += chunk;
}

// A list item or a heading must not be glued to its neighbour: the translation
// would be reflowed across the bullet/list structure.
bool looksLikeListItem(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return false;
    if (trimmed.startsWith(QLatin1Char('-')) || trimmed.startsWith(QChar(0x2022))
        || trimmed.startsWith(QChar(0x2013)) || trimmed.startsWith(QChar(0x2014)))
        return true;
    int index = 0;
    while (index < trimmed.size()
           && (trimmed.at(index).isDigit() || trimmed.at(index) == QLatin1Char('.')))
        ++index;
    if (index > 0 && index < trimmed.size()) {
        const QChar next = trimmed.at(index);
        if (next == QLatin1Char('-') || next == QLatin1Char(')')
            || next == QChar(0x2014) || next.isSpace())
            return true;
    }
    return false;
}


// djvutxt writes the whole book as one long string; translation services reject
// segments of that size, so the text is cut into paragraph-sized pieces at
// sentence boundaries before it becomes translation segments.
QString splitDjvuText(const QString& text)
{
    QString flat = text;
    flat.replace(QChar(0x0C), QLatin1Char('\n'));
    flat.replace(QRegularExpression(QStringLiteral("[ \t\r]+")), QStringLiteral(" "));
    flat.replace(QRegularExpression(QStringLiteral("\\n+")), QStringLiteral(" "));

    const QStringList sentences = flat.split(
        QRegularExpression(QStringLiteral("(?<=[.!?\u2026])\\s+")), Qt::SkipEmptyParts);

    QString result;
    QString current;
    for (const QString& sentence : sentences) {
        const QString trimmed = sentence.trimmed();
        if (trimmed.isEmpty())
            continue;
        if (!current.isEmpty() && current.size() + trimmed.size() > 700) {
            result += current.trimmed() + QLatin1Char('\n');
            current.clear();
        }
        current += trimmed + QLatin1Char(' ');
    }
    if (!current.trimmed().isEmpty())
        result += current.trimmed() + QLatin1Char('\n');
    return result;
}



// Scanned pages are denoised and contrast-normalised before OCR: old books have
// grey paper, bleed-through and isolated specks that pull Tesseract's
// binarisation in the wrong direction.
QImage preparePageForOcr(const QImage& source)
{
    if (source.isNull())
        return source;
    const int width = source.width();
    const int height = source.height();
    QImage gray(width, height, QImage::Format_Grayscale8);
    int histogram[256] = {};
    for (int y = 0; y < height; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(source.constScanLine(y));
        uchar* target = gray.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const QRgb pixel = row[x];
            const int value =
                (qRed(pixel) * 299 + qGreen(pixel) * 587 + qBlue(pixel) * 114) / 1000;
            target[x] = uchar(value);
            ++histogram[value];
        }
    }
    const int total = width * height;
    int low = 0;
    int high = 255;
    int accumulated = 0;
    for (int value = 0; value < 256; ++value) {
        accumulated += histogram[value];
        if (accumulated >= total * 2 / 100) {
            low = value;
            break;
        }
    }
    accumulated = 0;
    for (int value = 255; value >= 0; --value) {
        accumulated += histogram[value];
        if (accumulated >= total * 2 / 100) {
            high = value;
            break;
        }
    }
    if (high - low < 16) {
        low = 0;
        high = 255;
    }
    const double stretch = 255.0 / double(high - low);
    for (int y = 0; y < height; ++y) {
        uchar* row = gray.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const double value = (row[x] - low) * stretch;
            row[x] = uchar(qBound(0.0, value, 255.0));
        }
    }
    // Remove isolated dark pixels (scanner dust, JPEG ringing) without touching
    // letter strokes.
    if (width * height <= 8 * 1000 * 1000) {
        const QImage copy = gray;
        for (int y = 1; y < height - 1; ++y) {
            const uchar* above = copy.constScanLine(y - 1);
            const uchar* current = copy.constScanLine(y);
            const uchar* below = copy.constScanLine(y + 1);
            uchar* target = gray.scanLine(y);
            for (int x = 1; x < width - 1; ++x) {
                if (current[x] >= 128)
                    continue;
                int dark = 0;
                dark += above[x - 1] < 128; dark += above[x] < 128; dark += above[x + 1] < 128;
                dark += current[x - 1] < 128; dark += current[x + 1] < 128;
                dark += below[x - 1] < 128; dark += below[x] < 128; dark += below[x + 1] < 128;
                if (dark <= 1)
                    target[x] = 255;
            }
        }
    }
    return gray;
}

int resultWordCount(const QJsonObject& result)
{
    int words = 0;
    for (const QJsonValue& value : result.value(QStringLiteral("lines")).toArray()) {
        const QString text = value.toObject().value(QStringLiteral("text")).toString().trimmed();
        words += text.split(QRegularExpression(QStringLiteral("[\\s]+")),
                            Qt::SkipEmptyParts).size();
    }
    return words;
}

double resultConfidence(const QJsonObject& result)
{
    double weighted = 0.0;
    int letters = 0;
    for (const QJsonValue& value : result.value(QStringLiteral("lines")).toArray()) {
        const QJsonObject line = value.toObject();
        const QString text = line.value(QStringLiteral("text")).toString();
        const int count = text.size();
        const double confidence = line.value(QStringLiteral("confidence")).toDouble();
        if (count <= 0 || confidence <= 0.0)
            continue;
        weighted += confidence * count;
        letters += count;
    }
    return letters > 0 ? weighted / letters : 0.0;
}

double resultScore(const QJsonObject& result)
{
    // PaddleOCR reports no per-line confidence; treat it as a neutral 0.7 so the
    // two engines can still be compared by the amount of readable text.
    double confidence = resultConfidence(result);
    if (confidence <= 0.0)
        confidence = 0.7;
    return confidence * std::sqrt(double(resultWordCount(result) + 1));
}


// Vertical projection of ink: two or more dense bands separated by an empty
// gutter is a column layout. OCR then runs per column so lines never merge
// across the gutter.
QVector<QRect> detectTextColumns(const QImage& page)
{
    QVector<QRect> columns;
    if (page.isNull() || page.width() < 200)
        return columns;
    const int scaledWidth = qMin(600, page.width());
    const double factor = double(page.width()) / double(scaledWidth);
    QImage small = page.scaled(scaledWidth, qMax(1, int(page.height() / factor)),
                               Qt::IgnoreAspectRatio, Qt::FastTransformation);
    const int margin = small.height() / 20;
    QVector<int> ink(small.width(), 0);
    for (int y = margin; y < small.height() - margin; ++y) {
        const uchar* row = small.constScanLine(y);
        for (int x = 0; x < small.width(); ++x) {
            if (row[x] < 128)
                ++ink[x];
        }
    }
    QVector<int> smooth = ink;
    for (int x = 1; x + 1 < ink.size(); ++x)
        smooth[x] = qMax(ink[x - 1], qMax(ink[x], ink[x + 1]));
    const int textThreshold = qMax(1, small.height() / 200);
    QVector<QPair<int, int>> runs;
    int start = -1;
    for (int x = 0; x < smooth.size(); ++x) {
        const bool text = smooth.at(x) > textThreshold;
        if (text && start < 0)
            start = x;
        else if (!text && start >= 0) {
            runs.append({start, x - 1});
            start = -1;
        }
    }
    if (start >= 0)
        runs.append({start, smooth.size() - 1});
    QVector<QPair<int, int>> merged;
    const int minGap = qMax(4, int(smooth.size() * 0.015));
    for (const auto& run : std::as_const(runs)) {
        if (!merged.isEmpty() && run.first - merged.last().second <= minGap)
            merged.last().second = run.second;
        else
            merged.append(run);
    }
    QVector<QPair<int, int>> wide;
    int covered = 0;
    for (const auto& run : std::as_const(merged)) {
        if (run.second - run.first + 1 >= int(smooth.size() * 0.08)) {
            wide.append(run);
            covered += run.second - run.first + 1;
        }
    }
    if (wide.size() < 2 || covered < int(smooth.size() * 0.35))
        return columns;
    for (const auto& run : std::as_const(wide)) {
        const int x0 = qMax(0, int(run.first * factor) - 2);
        const int x1 = qMin(page.width(), int((run.second + 1) * factor) + 2);
        columns.append(QRect(x0, 0, x1 - x0, page.height()));
    }
    return columns;
}

// Native resolution of the page image. Rendering a 300 dpi scan at 200 dpi
// throws detail away; rendering a 150 dpi scan at 300 adds nothing. The OCR
// raster follows the source, capped to a sane range.
int detectImageDpi(FPDF_PAGE page)
{
    if (!page)
        return 0;
    const int count = FPDFPage_CountObjects(page);
    int best = 0;
    for (int index = 0; index < count; ++index) {
        FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
        if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_IMAGE)
            continue;
        FPDF_IMAGEOBJ_METADATA metadata{};
        if (!FPDFImageObj_GetImageMetadata(object, page, &metadata))
            continue;
        if (metadata.width < 400 || metadata.height < 400)
            continue;
        const int dpi = qRound(qMax(metadata.horizontal_dpi, metadata.vertical_dpi));
        best = qMax(best, dpi);
    }
    return best;
}

// Decides whether an extracted PDF text layer is worth translating. Layers with a
// broken encoding come back as replacement characters or symbol soup, and some
// scanners write an empty layer that carries coordinates only.
bool textLayerLooksReadable(const QString& text)
{
    const int length = text.size();
    if (length < 40)
        return false;

    int letters = 0;
    int spaces = 0;
    int replacement = 0;
    for (const QChar& character : text) {
        if (character.isLetterOrNumber())
            ++letters;
        else if (character.isSpace())
            ++spaces;
        if (character == QChar(0xFFFD))
            ++replacement;
    }

    if (replacement * 100 > length)
        return false;
    if (letters * 2 < length)
        return false;
    if (spaces == 0 && letters > 200)
        return false;

    // Some DJVU files carry a text layer with no spaces at all
    // ("Нейронныесетиосновныемодели"). Translating it produces word soup, so
    // such layers are treated as unreadable and the pages go through OCR.
    int cjk = 0;
    for (const QChar& character : text) {
        const uint code = character.unicode();
        if ((code >= 0x4E00 && code <= 0x9FFF) || (code >= 0x3040 && code <= 0x30FF)
            || (code >= 0xAC00 && code <= 0xD7AF))
            ++cjk;
    }
    if (letters > 200 && cjk * 2 < letters && spaces * 100 < letters * 2)
        return false;
    return true;
}

QString localName(const QDomElement& element)
{
    const QString name = element.tagName();
    const int colon = name.indexOf(QLatin1Char(':'));
    return colon >= 0 ? name.mid(colon + 1) : name;
}

void collectParagraphs(const QDomElement& parent, QVector<QDomElement>& paragraphs)
{
    for (QDomElement child = parent.firstChildElement(); !child.isNull();
         child = child.nextSiblingElement()) {
        if (localName(child) == QLatin1String("p"))
            paragraphs.append(child);
        collectParagraphs(child, paragraphs);
    }
}

void collectTextNodes(const QDomElement& parent, QVector<QDomElement>& nodes)
{
    for (QDomElement child = parent.firstChildElement(); !child.isNull();
         child = child.nextSiblingElement()) {
        const QString name = localName(child);
        if (name == QLatin1String("t")) {
            nodes.append(child);
        } else if (name != QLatin1String("p")) {
            // Nested paragraphs are collected as separate segments; skip them
            // here so text boxes are not translated twice.
            collectTextNodes(child, nodes);
        }
    }
}

void setTextNode(QDomDocument* document, QDomElement element, const QString& value)
{
    QDomNode child = element.firstChild();
    if (child.isText()) {
        child.setNodeValue(value);
        QDomNode next = child.nextSibling();
        while (!next.isNull()) {
            QDomNode following = next.nextSibling();
            if (next.isText() || next.isCDATASection())
                element.removeChild(next);
            next = following;
        }
    } else {
        while (!element.firstChild().isNull())
            element.removeChild(element.firstChild());
        if (!value.isEmpty())
            element.appendChild(document->createTextNode(value));
    }
    element.setAttribute(QStringLiteral("xml:space"), QStringLiteral("preserve"));
}

QString flattenLine(const QString& value)
{
    QString text = value;
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text;
}

int boundaryOffset(const QString& text, int desired, int lower)
{
    desired = qBound(lower, desired, text.size());
    for (int distance = 0; distance <= 24; ++distance) {
        const int before = desired - distance;
        if (before >= lower && before > 0 && before <= text.size()
            && (text.at(before - 1).isSpace() || text.at(before - 1) == QLatin1Char('\n'))) {
            return before;
        }
        const int after = desired + distance;
        if (after >= lower && after > 0 && after < text.size()
            && (text.at(after - 1).isSpace() || text.at(after - 1) == QLatin1Char('\n'))) {
            return after;
        }
    }
    return desired;
}

bool isDocxTextPart(const QString& name)
{
    static const QRegularExpression pattern(
        QStringLiteral("^word/(document|header[0-9]*|footer[0-9]*|footnotes|endnotes)\\.xml$"));
    return pattern.match(name).hasMatch();
}

bool isPptxTextPart(const QString& name)
{
    static const QRegularExpression pattern(
        QStringLiteral("^ppt/(slides/slide[0-9]+|notesSlides/notesSlide[0-9]+"
                       "|charts/chart[0-9]+|diagrams/data[0-9]+)\\.xml$"));
    return pattern.match(name).hasMatch();
}

void collectElementsByLocalName(const QDomElement& parent, const QString& name,
                                QVector<QDomElement>& elements)
{
    for (QDomElement child = parent.firstChildElement(); !child.isNull();
         child = child.nextSiblingElement()) {
        if (localName(child) == name)
            elements.append(child);
        collectElementsByLocalName(child, name, elements);
    }
}

void collectFb2Blocks(const QDomElement& parent, QVector<QDomElement>& blocks)
{
    for (QDomElement child = parent.firstChildElement(); !child.isNull();
         child = child.nextSiblingElement()) {
        const QString name = localName(child);
        if (name == QLatin1String("binary") || name == QLatin1String("code")
            || name == QLatin1String("stylesheet")) {
            continue;
        }
        if (name == QLatin1String("p") || name == QLatin1String("subtitle")
            || name == QLatin1String("text-author") || name == QLatin1String("v")) {
            blocks.append(child);
            continue;
        }
        collectFb2Blocks(child, blocks);
    }
}

void collectFb2TextNodes(const QDomNode& parent, QVector<QDomNode>& nodes)
{
    for (QDomNode child = parent.firstChild(); !child.isNull(); child = child.nextSibling()) {
        if (child.isText()) {
            nodes.append(child);
            continue;
        }
        if (!child.isElement())
            continue;
        const QString name = localName(child.toElement());
        if (name == QLatin1String("binary") || name == QLatin1String("code")
            || name == QLatin1String("stylesheet")) {
            continue;
        }
        collectFb2TextNodes(child, nodes);
    }
}

#ifdef Q_OS_WIN
void ensurePdfium()
{
    static std::once_flag once;
    std::call_once(once, [] {
        FPDF_LIBRARY_CONFIG config{};
        config.version = 2;
        FPDF_InitLibraryWithConfig(&config);
    });
}

QString pdfiumErrorText()
{
    switch (FPDF_GetLastError()) {
    case FPDF_ERR_SUCCESS:
        return QStringLiteral("успешно");
    case FPDF_ERR_FILE:
        return QStringLiteral("файл не найден или не читается");
    case FPDF_ERR_FORMAT:
        return QStringLiteral("неподдерживаемый или повреждённый PDF");
    case FPDF_ERR_PASSWORD:
        return QStringLiteral("PDF защищён паролем");
    case FPDF_ERR_SECURITY:
        return QStringLiteral("PDF использует неподдерживаемую защиту");
    case FPDF_ERR_PAGE:
        return QStringLiteral("страница не найдена");
    default:
        return QStringLiteral("неизвестная ошибка PDFium");
    }
}

QString cleanPdfFontName(const QString& value)
{
    QString name = value.trimmed();
    const int plus = name.indexOf(QLatin1Char('+'));
    if (plus == 6)
        name = name.mid(plus + 1);
    const int comma = name.indexOf(QLatin1Char(','));
    if (comma > 0)
        name = name.left(comma);
    return name.trimmed();
}

QColor sampleBackground(const QImage& image, const QRectF& box)
{
    if (image.isNull())
        return Qt::white;

    const int left = qBound(0, int(box.left()) - 2, image.width() - 1);
    const int right = qBound(0, int(box.right()) + 2, image.width() - 1);
    const int top = qBound(0, int(box.top()) - 2, image.height() - 1);
    const int bottom = qBound(0, int(box.bottom()) + 2, image.height() - 1);

    qint64 red = 0;
    qint64 green = 0;
    qint64 blue = 0;
    int samples = 0;
    const int stepX = qMax(1, (right - left) / 12);
    const int stepY = qMax(1, (bottom - top) / 6);
    for (int x = left; x <= right; x += stepX) {
        const QColor topColor = image.pixelColor(x, top);
        const QColor bottomColor = image.pixelColor(x, bottom);
        red += topColor.red() + bottomColor.red();
        green += topColor.green() + bottomColor.green();
        blue += topColor.blue() + bottomColor.blue();
        samples += 2;
    }
    for (int y = top; y <= bottom; y += stepY) {
        const QColor leftColor = image.pixelColor(left, y);
        const QColor rightColor = image.pixelColor(right, y);
        red += leftColor.red() + rightColor.red();
        green += leftColor.green() + rightColor.green();
        blue += leftColor.blue() + rightColor.blue();
        samples += 2;
    }
    if (samples == 0)
        return Qt::white;
    return QColor(int(red / samples), int(green / samples), int(blue / samples));
}
#endif

bool looksTranslatable(const QString& value)
{
    const QString text = value.trimmed();
    if (text.isEmpty())
        return false;

    int letters = 0;
    for (const QChar ch : text) {
        if (ch.isLetter())
            ++letters;
    }
    if (letters == 0)
        return false;

    static const QRegularExpression url(
        QStringLiteral("^(?:https?|ftp|file|data|mailto):"),
        QRegularExpression::CaseInsensitiveOption);
    if (url.match(text).hasMatch())
        return false;
    if (text.contains(QLatin1Char('@')) && !text.contains(QLatin1Char(' ')))
        return false;

    static const QRegularExpression technical(
        QStringLiteral("^(?:[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"
                       "|\\d{4}-\\d{2}-\\d{2}(?:[T ].*)?"
                       "|v?\\d+(?:\\.\\d+)+"
                       "|[a-z][a-z0-9.+-]*/[a-z0-9.+-]+"
                       "|#?[0-9a-fA-F]{6,8}"
                       "|[A-Z]{1,6}[-_]?\\d+"
                       "|[-+]?\\d+(?:[.,]\\d+)?%?"
                       ")$"));
    if (technical.match(text).hasMatch())
        return false;
    if (text.startsWith(QLatin1Char('/')) || text.startsWith(QLatin1String(".\\"))
        || text.startsWith(QLatin1String("../")) || text.startsWith(QLatin1String("~/"))) {
        return false;
    }
    return true;
}

QString jsonDecode(const QString& raw)
{
    QString result;
    for (int i = 0; i < raw.size(); ++i) {
        const QChar ch = raw.at(i);
        if (ch != QLatin1Char('\\') || i + 1 >= raw.size()) {
            result += ch;
            continue;
        }
        const QChar escape = raw.at(++i);
        switch (escape.unicode()) {
        case '"': result += QLatin1Char('"'); break;
        case '\\': result += QLatin1Char('\\'); break;
        case '/': result += QLatin1Char('/'); break;
        case 'b': result += QLatin1Char('\b'); break;
        case 'f': result += QLatin1Char('\f'); break;
        case 'n': result += QLatin1Char('\n'); break;
        case 'r': result += QLatin1Char('\r'); break;
        case 't': result += QLatin1Char('\t'); break;
        case 'u': {
            if (i + 4 >= raw.size()) {
                result += escape;
                break;
            }
            bool ok = false;
            const uint code = raw.mid(i + 1, 4).toUInt(&ok, 16);
            if (ok) {
                result += QChar(ushort(code));
                i += 4;
            } else {
                result += escape;
            }
            break;
        }
        default:
            result += escape;
            break;
        }
    }
    return result;
}

QString jsonEncode(const QString& value)
{
    QString result;
    for (const QChar ch : value) {
        switch (ch.unicode()) {
        case '"': result += QLatin1String("\\\""); break;
        case '\\': result += QLatin1String("\\\\"); break;
        case '\b': result += QLatin1String("\\b"); break;
        case '\f': result += QLatin1String("\\f"); break;
        case '\n': result += QLatin1String("\\n"); break;
        case '\r': result += QLatin1String("\\r"); break;
        case '\t': result += QLatin1String("\\t"); break;
        default:
            if (ch.unicode() < 0x20)
                result += QStringLiteral("\\u%1").arg(int(ch.unicode()), 4, 16, QLatin1Char('0'));
            else
                result += ch;
            break;
        }
    }
    return result;
}

QString protectAssText(const QString& text, QStringList& tokens)
{
    QString visible;
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('{')) {
            const int end = text.indexOf(QLatin1Char('}'), i + 1);
            if (end > i) {
                tokens.append(text.mid(i, end - i + 1));
                visible += QStringLiteral("[[%1]]").arg(tokens.size() - 1);
                i = end;
                continue;
            }
        }
        if (text.at(i) == QLatin1Char('\\') && i + 1 < text.size()) {
            const QChar next = text.at(i + 1);
            if (next == QLatin1Char('N') || next == QLatin1Char('n') || next == QLatin1Char('h')) {
                tokens.append(text.mid(i, 2));
                visible += QStringLiteral("[[%1]]").arg(tokens.size() - 1);
                ++i;
                continue;
            }
        }
        visible += text.at(i);
    }
    return visible;
}

QString restoreAssText(const QString& translated, const QStringList& tokens)
{
    QString result = translated;
    for (int i = 0; i < tokens.size(); ++i)
        result.replace(QStringLiteral("[[%1]]").arg(i), tokens.at(i));
    return result;
}

QString inlineText(const QDomNode& source, QVector<QDomNode>& textNodes)
{
    QString text;
    for (QDomNode child = source.firstChild(); !child.isNull(); child = child.nextSibling()) {
        if (child.isText()) {
            textNodes.append(child);
            text += child.nodeValue();
        } else if (child.isElement()) {
            text += inlineText(child, textNodes);
        }
    }
    return text;
}

void setInlineText(QDomDocument* document, const QDomElement& source, QDomElement target,
                   const QString& translated)
{
    while (!target.firstChild().isNull())
        target.removeChild(target.firstChild());
    for (QDomNode child = source.firstChild(); !child.isNull(); child = child.nextSibling())
        target.appendChild(child.cloneNode(true));

    QVector<QDomNode> textNodes;
    inlineText(target, textNodes);
    if (textNodes.isEmpty()) {
        if (!translated.isEmpty())
            target.appendChild(document->createTextNode(translated));
        return;
    }

    int total = 0;
    for (const QDomNode& node : std::as_const(textNodes))
        total += std::max(1, int(node.nodeValue().size()));

    int offset = 0;
    int accumulated = 0;
    for (int i = 0; i < textNodes.size(); ++i) {
        accumulated += std::max(1, int(textNodes[i].nodeValue().size()));
        const int end = (i == textNodes.size() - 1)
            ? translated.size()
            : boundaryOffset(translated, int(qint64(translated.size()) * accumulated / total),
                                       offset);
        QDomNode node = textNodes.at(i);
        node.setNodeValue(translated.mid(offset, end - offset));
        offset = end;
    }
}

} // namespace

struct DocumentTranslationJob::Impl
{
    struct Segment {
        QString source;
        std::function<void(const QString&)> apply;
    };

    AppContext* context = nullptr;
    QString inputPath;
    QString outputPath;
    QString sourceCode = QStringLiteral("auto");
    QString targetCode = QStringLiteral("ru");
    QString providerId;
    bool cancelled = false;
    bool failed = false;
    int index = 0;
    QString error;

    QVector<Segment> segments;
    QVector<QDomDocument*> documents;
    QHash<QString, QDomDocument*> documentParts;
    QVector<QPair<QString, QByteArray>> zipEntries;

    QSharedPointer<QStringList> textLines;
    QString lineEnding = QStringLiteral("\n");
    QSharedPointer<QStringList> htmlPieces;
    QSharedPointer<QStringList> tokenPieces;
    QDomDocument* fb2Document = nullptr;
    QDomDocument* xmlDocument = nullptr;
    QHash<QString, QSharedPointer<QStringList>> epubPieces;
    QSharedPointer<QVector<QStringList>> csvRows;
    QSharedPointer<QVector<QVector<bool>>> csvQuoted;
    QChar csvDelimiter = QLatin1Char(',');
    QString csvLineEnding = QStringLiteral("\r\n");
    bool csvTrailingNewline = false;
    std::vector<YAML::Node> yamlRoots;

#ifdef Q_OS_WIN
    struct PdfLine {
        QString source;
        QString translated;
        int page = 0;
        QRectF box;
        double fontSize = 10.0;
        QColor color = Qt::black;
        QString fontName;
        bool bold = false;
        bool italic = false;
    };
    QByteArray pdfData;
    QVector<PdfLine> pdfLines;
    int pdfPageCount = 0;
    QVector<int> pdfPageDpi;
    int documentDpiOverride = 0;
    QSizeF pdfFirstPageSize;
    // Scanned PDFs have no text layer: the pages are recognized first and the
    // straightened bitmap becomes the background of the translated page, so the
    // word boxes and the image share one coordinate space.
    std::unique_ptr<QTemporaryDir> djvuTextDir;
    bool pdfOcrPending = false;
    int pdfOcrPage = 0;
    QVector<QByteArray> pdfOcrPageImages;
#endif

    bool addHtmlSegments(const QString& html,
                         const QSharedPointer<QStringList>& pieces);
    bool addJsonSegments(const QString& json);
    bool addXmlSegments(const QString& xml);
    void addXmlUnit(QDomDocument* document, const QDomElement& source,
                    QDomElement target);

    ~Impl()
    {
        for (QDomDocument* document : documents)
            delete document;
    }
};

DocumentTranslationJob::DocumentTranslationJob(AppContext* context, QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    m_impl->context = context;
}

DocumentTranslationJob::~DocumentTranslationJob() = default;

void DocumentTranslationJob::start(const QString& inputPath, const QString& outputPath,
                                   const QString& sourceCode, const QString& targetCode,
                                   const QString& providerId)
{
    m_impl->inputPath = inputPath;
    m_impl->outputPath = outputPath;
    m_impl->sourceCode = sourceCode;
    m_impl->targetCode = targetCode;
    m_impl->providerId = providerId;
    m_impl->cancelled = false;
    m_impl->failed = false;
    m_impl->error.clear();
    m_impl->index = 0;
    m_impl->segments.clear();
    m_impl->zipEntries.clear();
    m_impl->documentParts.clear();
    for (QDomDocument* document : m_impl->documents)
        delete document;
    m_impl->documents.clear();
    m_impl->textLines.clear();
    m_impl->htmlPieces.clear();
    m_impl->tokenPieces.clear();
    m_impl->epubPieces.clear();
    m_impl->fb2Document = nullptr;
    m_impl->xmlDocument = nullptr;
    m_impl->csvRows.clear();
    m_impl->csvQuoted.clear();
    m_impl->yamlRoots.clear();
#ifdef Q_OS_WIN
    m_impl->pdfData.clear();
    m_impl->pdfLines.clear();
    m_impl->pdfPageCount = 0;
    m_impl->pdfFirstPageSize = QSizeF();
    m_impl->pdfOcrPending = false;
    m_impl->pdfOcrPage = 0;
    m_impl->pdfOcrPageImages.clear();
#endif

    const QFileInfo inputInfo(inputPath);
    if (!inputInfo.isFile()) {
        emit failed(QStringLiteral("Файл не найден: %1").arg(inputPath));
        return;
    }
    if (outputPath.isEmpty()) {
        emit failed(QStringLiteral("Не задан путь для сохранения перевода."));
        return;
    }
    if (!m_impl->context || !m_impl->context->translation || !m_impl->context->plugins) {
        emit failed(QStringLiteral("Сервис перевода недоступен."));
        return;
    }

    const QString suffix = inputInfo.suffix().toLower();

    // DJVU has no text layer we can read here, so convert it to a rasterized PDF
    // with the external ddjvu.exe and reuse the scanned-PDF path.
    if (suffix == QStringLiteral("djvu") || suffix == QStringLiteral("djv")) {
        // Files converted from electronic sources already carry a text layer, which
        // is both faster and more accurate than OCR. Only scans fall through.
        QString textPath;
        if (extractDjvuText(inputPath, &textPath)) {
            m_impl->inputPath = textPath;
            const int dot = m_impl->outputPath.lastIndexOf(QLatin1Char('.'));
            if (dot > 0)
                m_impl->outputPath = m_impl->outputPath.left(dot) + QStringLiteral(".txt");
            if (!preparePlainText()) {
                emit failed(m_impl->error.isEmpty()
                                ? QStringLiteral("Не удалось прочитать текст DJVU.")
                                : m_impl->error);
                return;
            }
            beginTranslation();
            return;
        }
        if (!convertDjvuToPdf(inputPath)) {
            emit failed(m_impl->error.isEmpty()
                            ? QStringLiteral("Не удалось прочитать DJVU.")
                            : m_impl->error);
            return;
        }
        const int dot = m_impl->outputPath.lastIndexOf(QLatin1Char('.'));
        if (dot > 0)
            m_impl->outputPath = m_impl->outputPath.left(dot) + QStringLiteral(".pdf");
    }

    bool prepared = false;
    if (suffix == QStringLiteral("docx"))
        prepared = prepareDocx();
    else if (suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown"))
        prepared = prepareMarkdown();
    else if (suffix == QStringLiteral("html") || suffix == QStringLiteral("htm"))
        prepared = prepareHtml();
    else if (suffix == QStringLiteral("fb2"))
        prepared = prepareFb2();
    else if (suffix == QStringLiteral("epub"))
        prepared = prepareEpub();
    else if (suffix == QStringLiteral("pdf") || suffix == QStringLiteral("djvu")
             || suffix == QStringLiteral("djv"))
        prepared = preparePdf();
    else if (suffix == QStringLiteral("xliff") || suffix == QStringLiteral("xlf"))
        prepared = prepareXliff();
    else if (suffix == QStringLiteral("ts"))
        prepared = prepareTs();
    else if (suffix == QStringLiteral("json"))
        prepared = prepareJson();
    else if (suffix == QStringLiteral("xml"))
        prepared = prepareXml();
    else if (suffix == QStringLiteral("properties"))
        prepared = prepareProperties();
    else if (suffix == QStringLiteral("yaml") || suffix == QStringLiteral("yml"))
        prepared = prepareYaml();
    else if (suffix == QStringLiteral("srt"))
        prepared = prepareSrt();
    else if (suffix == QStringLiteral("ini"))
        prepared = prepareIni();
    else if (suffix == QStringLiteral("csv"))
        prepared = prepareCsv();
    else if (suffix == QStringLiteral("vtt"))
        prepared = prepareVtt();
    else if (suffix == QStringLiteral("ass") || suffix == QStringLiteral("ssa"))
        prepared = prepareAss();
    else if (suffix == QStringLiteral("pptx"))
        prepared = preparePptx();
    else if (suffix == QStringLiteral("txt"))
        prepared = preparePlainText();
    else {
        emit failed(QStringLiteral("Формат .%1 не поддерживается. "
                                   "Поддерживаются DOCX, PPTX, PDF, FB2, EPUB, XLIFF, TS, JSON, XML, "
                                   "Properties, YAML, SRT, VTT, ASS/SSA, CSV, INI, Markdown, TXT и HTML.").arg(suffix));
        return;
    }

    if (!prepared) {
        emit failed(m_impl->error.isEmpty() ? QStringLiteral("Не удалось прочитать документ.")
                                           : m_impl->error);
        return;
    }

    // A scanned PDF is recognized asynchronously page by page, and that chain calls
    // beginTranslation() once the segments exist.
    if (m_impl->pdfOcrPending)
        return;

    beginTranslation();
}

void DocumentTranslationJob::beginTranslation()
{
    if (m_impl->cancelled || m_impl->failed)
        return;

    if (m_impl->segments.isEmpty()) {
        emit failed(QStringLiteral("В документе нет текста для перевода."));
        return;
    }

    if (m_impl->sourceCode.isEmpty() || m_impl->sourceCode == QStringLiteral("auto")) {
        QString sample;
        for (const auto& segment : std::as_const(m_impl->segments)) {
            if (!sample.isEmpty())
                sample += QLatin1Char(' ');
            sample += segment.source;
            if (sample.size() > 2000)
                break;
        }
        const QString detected = LanguageRouter::detectLanguage(sample);
        if (!detected.isEmpty())
            m_impl->sourceCode = detected;
    }
    if (m_impl->targetCode.isEmpty() || m_impl->targetCode == QStringLiteral("auto")) {
        m_impl->targetCode = m_impl->context->router
            ? m_impl->context->router->fallbackTarget()
            : QStringLiteral("ru");
    }

    emit progress(0, int(m_impl->segments.size()), QString());
    translateNext();
}

void DocumentTranslationJob::cancel()
{
    m_impl->cancelled = true;
}

bool DocumentTranslationJob::prepareDocx()
{
    QZipReader zip(m_impl->inputPath);
    if (!zip.isReadable()) {
        m_impl->error = QStringLiteral(
            "Не удалось открыть DOCX. Файл повреждён, зашифрован или не является документом Word.");
        return false;
    }

    for (const QZipReader::FileInfo& info : zip.fileInfoList()) {
        if (info.isDir)
            continue;
        const QByteArray data = zip.fileData(info.filePath);
        m_impl->zipEntries.append({info.filePath, data});
        if (isDocxTextPart(info.filePath) && !addOfficePart(info.filePath, data))
            return false;
    }

    if (m_impl->documentParts.isEmpty()) {
        m_impl->error = QStringLiteral("В DOCX не найден текст для перевода.");
        return false;
    }
    return true;
}

bool DocumentTranslationJob::preparePlainText()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    m_impl->lineEnding = text.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n")
                                                               : QStringLiteral("\n");

    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_impl->textLines = QSharedPointer<QStringList>::create(
        normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts));

    for (int i = 0; i < m_impl->textLines->size(); ++i) {
        const QString line = m_impl->textLines->at(i);
        if (line.trimmed().isEmpty())
            continue;
        int prefixLength = 0;
        while (prefixLength < line.size() && line.at(prefixLength).isSpace())
            ++prefixLength;
        const QString source = line.mid(prefixLength);
        if (source.trimmed().isEmpty())
            continue;
        const QString prefix = line.left(prefixLength);
        Impl::Segment segment;
        segment.source = source;
        segment.apply = [lines = m_impl->textLines, i, prefix](const QString& translated) {
            (*lines)[i] = prefix + flattenLine(translated);
        };
        m_impl->segments.append(segment);
    }
    return true;
}

bool DocumentTranslationJob::prepareMarkdown()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    m_impl->lineEnding = text.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n")
                                                               : QStringLiteral("\n");

    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_impl->textLines = QSharedPointer<QStringList>::create(
        normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts));

    static const QRegularExpression prefixPattern(
        QStringLiteral("^(\\s*(?:#{1,6}\\s+|>\\s*|[-*+]\\s+|\\d+[.)]\\s+)?)(.*)$"));
    static const QRegularExpression referenceDefinition(
        QStringLiteral("^\\s*\\[[^\\]]+\\]:\\s*\\S+"));

    bool inFence = false;
    bool inFrontMatter = false;
    for (int i = 0; i < m_impl->textLines->size(); ++i) {
        const QString line = m_impl->textLines->at(i);
        const QString trimmed = line.trimmed();

        if (i == 0 && trimmed == QLatin1String("---")) {
            inFrontMatter = true;
            continue;
        }
        if (inFrontMatter) {
            if (trimmed == QLatin1String("---") || trimmed == QLatin1String("..."))
                inFrontMatter = false;
            continue;
        }
        if (trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1String("~~~"))) {
            inFence = !inFence;
            continue;
        }
        if (inFence || trimmed.isEmpty())
            continue;
        if (trimmed.startsWith(QLatin1String("<!--")) || trimmed.startsWith(QLatin1String("    ")))
            continue;
        if (referenceDefinition.match(line).hasMatch())
            continue;

        if (trimmed.startsWith(QLatin1Char('|')) && trimmed.contains(QLatin1String("---")))
            continue;

        if (trimmed.startsWith(QLatin1Char('|')) && trimmed.endsWith(QLatin1Char('|'))) {
            QStringList cells = line.split(QLatin1Char('|'), Qt::KeepEmptyParts);
            for (int cellIndex = 0; cellIndex < cells.size(); ++cellIndex) {
                const QString cell = cells.at(cellIndex);
                if (cell.trimmed().isEmpty())
                    continue;
                int start = 0;
                while (start < cell.size() && cell.at(start).isSpace())
                    ++start;
                int end = cell.size();
                while (end > start && cell.at(end - 1).isSpace())
                    --end;
                const QString lead = cell.left(start);
                const QString core = cell.mid(start, end - start);
                const QString trail = cell.mid(end);
                if (core.isEmpty())
                    continue;
                Impl::Segment segment;
                segment.source = core;
                segment.apply = [lines = m_impl->textLines, i, cellIndex, lead, trail](
                                    const QString& translated) {
                    QStringList current = lines->at(i).split(QLatin1Char('|'), Qt::KeepEmptyParts);
                    if (cellIndex < current.size())
                        current[cellIndex] = lead + flattenLine(translated) + trail;
                    (*lines)[i] = current.join(QLatin1Char('|'));
                };
                m_impl->segments.append(segment);
            }
            continue;
        }

        const QRegularExpressionMatch match = prefixPattern.match(line);
        const QString prefix = match.captured(1);
        const QString source = match.captured(2);
        if (source.trimmed().isEmpty())
            continue;

        Impl::Segment segment;
        segment.source = source;
        segment.apply = [lines = m_impl->textLines, i, prefix](const QString& translated) {
            (*lines)[i] = prefix + flattenLine(translated);
        };
        m_impl->segments.append(segment);
    }
    return true;
}

bool DocumentTranslationJob::Impl::addHtmlSegments(
    const QString& html, const QSharedPointer<QStringList>& pieces)
{
    static const QRegularExpression tagPattern(QStringLiteral("<[^>]*>"));
    auto matches = tagPattern.globalMatch(html);

    int position = 0;
    bool inScript = false;
    bool inStyle = false;
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString textPart = html.mid(position, match.capturedStart() - position);
        const int textIndex = pieces->size();
        pieces->append(textPart);

        if (!inScript && !inStyle && !textPart.trimmed().isEmpty()
            && textPart.trimmed() != QLatin1String("&nbsp;")) {
            int start = 0;
            while (start < textPart.size() && textPart.at(start).isSpace())
                ++start;
            int end = textPart.size();
            while (end > start && textPart.at(end - 1).isSpace())
                --end;
            const QString lead = textPart.left(start);
            const QString core = textPart.mid(start, end - start);
            const QString trail = textPart.mid(end);
            if (!core.isEmpty()) {
                Segment segment;
                segment.source = core;
                segment.apply = [pieces, textIndex, lead, trail](const QString& translated) {
                    (*pieces)[textIndex] = lead + translated + trail;
                };
                segments.append(segment);
            }
        }

        const QString tag = match.captured(0);
        pieces->append(tag);
        const QString lower = tag.toLower();
        if (lower.startsWith(QLatin1String("<script")))
            inScript = true;
        else if (lower.startsWith(QLatin1String("</script")))
            inScript = false;
        else if (lower.startsWith(QLatin1String("<style")))
            inStyle = true;
        else if (lower.startsWith(QLatin1String("</style")))
            inStyle = false;
        position = match.capturedEnd();
    }
    pieces->append(html.mid(position));
    return true;
}

bool DocumentTranslationJob::prepareHtml()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);

    m_impl->htmlPieces = QSharedPointer<QStringList>::create();
    return m_impl->addHtmlSegments(text, m_impl->htmlPieces);
}

bool DocumentTranslationJob::Impl::addJsonSegments(const QString& json)
{
    tokenPieces = QSharedPointer<QStringList>::create();
    int i = 0;
    int last = 0;
    while (i < json.size()) {
        if (json.at(i) != QLatin1Char('"')) {
            ++i;
            continue;
        }
        const int quoteStart = i++;
        bool escaped = false;
        while (i < json.size()) {
            const QChar ch = json.at(i);
            if (escaped) {
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == QLatin1Char('"')) {
                break;
            }
            ++i;
        }
        if (i >= json.size())
            break;
        const int quoteEnd = i++;
        const QString raw = json.mid(quoteStart + 1, quoteEnd - quoteStart - 1);

        int next = i;
        while (next < json.size() && json.at(next).isSpace())
            ++next;
        const bool isKey = next < json.size() && json.at(next) == QLatin1Char(':');
        const QString decoded = jsonDecode(raw);
        if (isKey || !looksTranslatable(decoded))
            continue;

        tokenPieces->append(json.mid(last, quoteStart - last));
        tokenPieces->append(QStringLiteral("\""));
        const int contentIndex = tokenPieces->size();
        tokenPieces->append(raw);
        tokenPieces->append(QStringLiteral("\""));
        last = i;

        Segment segment;
        segment.source = decoded;
        segment.apply = [pieces = tokenPieces, contentIndex](const QString& translated) {
            (*pieces)[contentIndex] = jsonEncode(translated);
        };
        segments.append(segment);
    }
    tokenPieces->append(json.mid(last));
    return true;
}

bool DocumentTranslationJob::Impl::addXmlSegments(const QString& xml)
{
    tokenPieces = QSharedPointer<QStringList>::create();
    static const QRegularExpression skipElements(
        QStringLiteral("^(?:script|style|code|binary|base64)$"),
        QRegularExpression::CaseInsensitiveOption);

    int i = 0;
    int textStart = 0;
    int skipDepth = 0;
    QString skipName;

    auto addText = [&](int start, int end) {
        const QString text = xml.mid(start, end - start);
        const int pieceIndex = tokenPieces->size();
        tokenPieces->append(text);
        if (skipDepth > 0 || text.trimmed().isEmpty() || !looksTranslatable(text))
            return;
        int lead = 0;
        while (lead < text.size() && text.at(lead).isSpace())
            ++lead;
        int trail = text.size();
        while (trail > lead && text.at(trail - 1).isSpace())
            --trail;
        const QString prefix = text.left(lead);
        const QString source = text.mid(lead, trail - lead);
        const QString suffix = text.mid(trail);
        if (source.isEmpty())
            return;
        Segment segment;
        segment.source = source;
        segment.apply = [pieces = tokenPieces, pieceIndex, prefix, suffix](const QString& translated) {
            (*pieces)[pieceIndex] = prefix + translated + suffix;
        };
        segments.append(segment);
    };

    while (i < xml.size()) {
        if (xml.at(i) != QLatin1Char('<')) {
            ++i;
            continue;
        }
        addText(textStart, i);

        if (xml.mid(i, 4) == QLatin1String("<!--")) {
            const int end = xml.indexOf(QStringLiteral("-->"), i + 4);
            const int stop = end < 0 ? xml.size() : end + 3;
            tokenPieces->append(xml.mid(i, stop - i));
            i = stop;
            textStart = i;
            continue;
        }
        if (xml.mid(i, 9) == QLatin1String("<![CDATA[")) {
            const int end = xml.indexOf(QStringLiteral("]]>"), i + 9);
            const int stop = end < 0 ? xml.size() : end + 3;
            tokenPieces->append(xml.mid(i, stop - i));
            i = stop;
            textStart = i;
            continue;
        }
        if (xml.mid(i, 2) == QLatin1String("<?") || xml.mid(i, 2) == QLatin1String("<!")) {
            const int end = xml.indexOf(QLatin1Char('>'), i + 2);
            const int stop = end < 0 ? xml.size() : end + 1;
            tokenPieces->append(xml.mid(i, stop - i));
            i = stop;
            textStart = i;
            continue;
        }

        bool inQuote = false;
        QChar quote;
        int end = i + 1;
        for (; end < xml.size(); ++end) {
            const QChar ch = xml.at(end);
            if (inQuote) {
                if (ch == quote)
                    inQuote = false;
            } else if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
                inQuote = true;
                quote = ch;
            } else if (ch == QLatin1Char('>')) {
                break;
            }
        }
        if (end >= xml.size())
            break;
        const QString tag = xml.mid(i, end - i + 1);
        tokenPieces->append(tag);
        const QString trimmed = tag.mid(1, tag.size() - 2).trimmed();
        const bool closing = trimmed.startsWith(QLatin1Char('/'));
        const bool selfClosing = trimmed.endsWith(QLatin1Char('/'));
        QString name = trimmed;
        if (closing)
            name = name.mid(1);
        const int space = name.indexOf(QRegularExpression(QStringLiteral("[\\s/]")));
        if (space > 0)
            name = name.left(space);
        if (closing) {
            if (skipDepth > 0 && name.compare(skipName, Qt::CaseInsensitive) == 0) {
                --skipDepth;
                if (skipDepth == 0)
                    skipName.clear();
            }
        } else if (!selfClosing && skipDepth == 0 && skipElements.match(name).hasMatch()) {
            skipName = name;
            ++skipDepth;
        }
        i = end + 1;
        textStart = i;
    }
    addText(textStart, xml.size());
    return true;
}

void DocumentTranslationJob::Impl::addXmlUnit(QDomDocument* document,
                                               const QDomElement& source,
                                               QDomElement target)
{
    QVector<QDomNode> textNodes;
    const QString sourceText = inlineText(source, textNodes);
    if (!looksTranslatable(sourceText))
        return;

    Segment segment;
    segment.source = sourceText;
    segment.apply = [document, source, target](const QString& translated) mutable {
        setInlineText(document, source, target, translated);
    };
    segments.append(segment);
}

bool DocumentTranslationJob::prepareJson()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    return m_impl->addJsonSegments(text);
}

bool DocumentTranslationJob::prepareXml()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    return m_impl->addXmlSegments(text);
}

bool DocumentTranslationJob::prepareFb2()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }

    auto* document = new QDomDocument;
    const auto parseResult = document->setContent(
        file.readAll(), QDomDocument::ParseOption::Default);
    if (!parseResult) {
        delete document;
        m_impl->error = QStringLiteral("Не удалось разобрать FB2: %1 (строка %2)")
                            .arg(parseResult.errorMessage)
                            .arg(parseResult.errorLine);
        return false;
    }

    QVector<QDomElement> blocks;
    collectFb2Blocks(document->documentElement(), blocks);
    for (const QDomElement& block : std::as_const(blocks)) {
        QVector<QDomNode> nodes;
        collectFb2TextNodes(block, nodes);
        if (nodes.isEmpty())
            continue;

        QString source;
        for (const QDomNode& node : std::as_const(nodes))
            source += node.nodeValue();
        if (source.trimmed().isEmpty())
            continue;

        Impl::Segment segment;
        segment.source = source;
        segment.apply = [document, nodes](const QString& translated) {
            int total = 0;
            for (const QDomNode& node : nodes)
                total += std::max(1, int(node.nodeValue().size()));

            int offset = 0;
            int accumulated = 0;
            for (int i = 0; i < nodes.size(); ++i) {
                accumulated += std::max(1, int(nodes[i].nodeValue().size()));
                const int end = (i == nodes.size() - 1)
                    ? translated.size()
                    : boundaryOffset(translated,
                                     int(qint64(translated.size()) * accumulated / total),
                                     offset);
                QDomNode node = nodes.at(i);
                node.setNodeValue(translated.mid(offset, end - offset));
                offset = end;
            }
        };
        m_impl->segments.append(segment);
    }

    m_impl->documents.append(document);
    m_impl->fb2Document = document;
    return true;
}

bool DocumentTranslationJob::prepareProperties()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    m_impl->lineEnding = text.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n")
                                                               : QStringLiteral("\n");
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_impl->textLines = QSharedPointer<QStringList>::create(
        normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts));

    for (int i = 0; i < m_impl->textLines->size(); ++i) {
        const QString line = m_impl->textLines->at(i);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))
            || trimmed.startsWith(QLatin1Char('!'))) {
            continue;
        }

        int separator = -1;
        bool escaped = false;
        for (int c = 0; c < line.size(); ++c) {
            const QChar ch = line.at(c);
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == QLatin1Char('\\')) {
                escaped = true;
                continue;
            }
            if (ch == QLatin1Char('=') || ch == QLatin1Char(':') || ch.isSpace()) {
                separator = c;
                break;
            }
        }
        if (separator < 0)
            continue;

        int valueStart = separator + 1;
        while (valueStart < line.size() && line.at(valueStart).isSpace())
            ++valueStart;
        const QString value = line.mid(valueStart);
        if (value.isEmpty() || value.contains(QLatin1String("\\u")) || !looksTranslatable(value))
            continue;

        const QString prefix = line.left(valueStart);
        Impl::Segment segment;
        segment.source = value;
        segment.apply = [lines = m_impl->textLines, i, prefix](const QString& translated) {
            (*lines)[i] = prefix + translated;
        };
        m_impl->segments.append(segment);
    }
    return true;
}

bool DocumentTranslationJob::prepareIni()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    m_impl->lineEnding = text.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n")
                                                               : QStringLiteral("\n");
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_impl->textLines = QSharedPointer<QStringList>::create(
        normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts));

    for (int i = 0; i < m_impl->textLines->size(); ++i) {
        const QString line = m_impl->textLines->at(i);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('['))
            || trimmed.startsWith(QLatin1Char(';')) || trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals < 0)
            continue;

        QString value = line.mid(equals + 1);
        int lead = 0;
        while (lead < value.size() && value.at(lead).isSpace())
            ++lead;
        QString body = value.mid(lead);

        int comment = -1;
        bool quoted = false;
        QChar quote;
        for (int c = 0; c < body.size(); ++c) {
            const QChar ch = body.at(c);
            if ((ch == QLatin1Char('"') || ch == QLatin1Char('\''))) {
                if (quoted && ch == quote)
                    quoted = false;
                else if (!quoted) {
                    quoted = true;
                    quote = ch;
                }
            } else if (!quoted && (ch == QLatin1Char(';') || ch == QLatin1Char('#'))) {
                comment = c;
                break;
            }
        }
        QString trailing;
        if (comment >= 0) {
            trailing = body.mid(comment);
            body = body.left(comment);
        }

        QString open;
        QString close;
        QString core = body.trimmed();
        if (core.size() >= 2 && (core.front() == QLatin1Char('"') || core.front() == QLatin1Char('\''))
            && core.back() == core.front()) {
            open = core.left(1);
            close = core.right(1);
            core = core.mid(1, core.size() - 2);
        }
        if (core.isEmpty() || !looksTranslatable(core))
            continue;

        const QString prefix = line.left(equals + 1) + QString(lead, QLatin1Char(' '));
        Impl::Segment segment;
        segment.source = core;
        segment.apply = [lines = m_impl->textLines, i, prefix, open, close, trailing](
                            const QString& translated) {
            (*lines)[i] = prefix + open + translated + close + trailing;
        };
        m_impl->segments.append(segment);
    }
    return true;
}

bool DocumentTranslationJob::prepareSrt()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    m_impl->lineEnding = text.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n")
                                                               : QStringLiteral("\n");
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_impl->textLines = QSharedPointer<QStringList>::create(
        normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts));

    int index = 0;
    while (index < m_impl->textLines->size()) {
        if (!m_impl->textLines->at(index).contains(QLatin1String("-->"))) {
            ++index;
            continue;
        }
        int start = index + 1;
        int end = start;
        while (end < m_impl->textLines->size()
               && !m_impl->textLines->at(end).trimmed().isEmpty()) {
            ++end;
        }
        QStringList cue;
        for (int i = start; i < end; ++i)
            cue.append(m_impl->textLines->at(i));
        const QString source = cue.join(QLatin1Char(' '));
        if (looksTranslatable(source)) {
            Impl::Segment segment;
            segment.source = source;
            segment.apply = [lines = m_impl->textLines, start, end](const QString& translated) {
                (*lines)[start] = translated;
                for (int i = start + 1; i < end; ++i)
                    (*lines)[i].clear();
            };
            m_impl->segments.append(segment);
        }
        index = end;
    }
    return true;
}

bool DocumentTranslationJob::prepareVtt()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    m_impl->lineEnding = text.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n")
                                                               : QStringLiteral("\n");
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_impl->textLines = QSharedPointer<QStringList>::create(
        normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts));

    int index = 0;
    while (index < m_impl->textLines->size()) {
        const QString trimmed = m_impl->textLines->at(index).trimmed();
        if (trimmed.startsWith(QLatin1String("NOTE")) || trimmed.startsWith(QLatin1String("STYLE"))
            || trimmed.startsWith(QLatin1String("REGION"))) {
            while (index < m_impl->textLines->size()
                   && !m_impl->textLines->at(index).trimmed().isEmpty()) {
                ++index;
            }
            continue;
        }
        if (!m_impl->textLines->at(index).contains(QLatin1String("-->"))) {
            ++index;
            continue;
        }
        int start = index + 1;
        int end = start;
        while (end < m_impl->textLines->size()
               && !m_impl->textLines->at(end).trimmed().isEmpty()) {
            ++end;
        }
        QStringList cue;
        for (int i = start; i < end; ++i)
            cue.append(m_impl->textLines->at(i));
        const QString source = cue.join(QLatin1Char(' '));
        if (looksTranslatable(source)) {
            Impl::Segment segment;
            segment.source = source;
            segment.apply = [lines = m_impl->textLines, start, end](const QString& translated) {
                (*lines)[start] = translated;
                for (int i = start + 1; i < end; ++i)
                    (*lines)[i].clear();
            };
            m_impl->segments.append(segment);
        }
        index = end;
    }
    return true;
}

bool DocumentTranslationJob::prepareAss()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    m_impl->lineEnding = text.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n")
                                                               : QStringLiteral("\n");
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_impl->textLines = QSharedPointer<QStringList>::create(
        normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts));

    bool inEvents = false;
    for (int i = 0; i < m_impl->textLines->size(); ++i) {
        const QString line = m_impl->textLines->at(i);
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1Char('['))) {
            inEvents = trimmed.compare(QLatin1String("[Events]"), Qt::CaseInsensitive) == 0;
            continue;
        }
        if (!inEvents || !trimmed.startsWith(QLatin1String("Dialogue:"), Qt::CaseInsensitive))
            continue;

        const int firstComma = line.indexOf(QLatin1Char(','));
        if (firstComma < 0)
            continue;
        QStringList parts = line.split(QLatin1Char(','));
        if (parts.size() <= 9)
            continue;

        const QString visible = parts.mid(9).join(QLatin1Char(','));
        QStringList tokens;
        const QString source = protectAssText(visible, tokens);
        if (!looksTranslatable(source))
            continue;

        const QString prefix = parts.mid(0, 9).join(QLatin1Char(',')) + QLatin1Char(',');
        Impl::Segment segment;
        segment.source = source;
        segment.apply = [lines = m_impl->textLines, i, prefix, tokens](const QString& translated) {
            (*lines)[i] = prefix + restoreAssText(translated, tokens);
        };
        m_impl->segments.append(segment);
    }
    return true;
}



bool DocumentTranslationJob::prepareCsv()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    m_impl->csvLineEnding = text.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n")
                                                                   : QStringLiteral("\n");
    m_impl->csvTrailingNewline = text.endsWith(QLatin1Char('\n'));

    QChar delimiter = QLatin1Char(',');
    int bestCount = -1;
    for (const QChar candidate : {QLatin1Char(','), QLatin1Char(';'), QLatin1Char('\t')}) {
        int count = 0;
        bool quoted = false;
        for (int i = 0; i < text.size(); ++i) {
            const QChar ch = text.at(i);
            if (ch == QLatin1Char('"'))
                quoted = !quoted;
            else if (!quoted && ch == candidate)
                ++count;
            else if (!quoted && (ch == QLatin1Char('\n') || ch == QLatin1Char('\r')))
                break;
        }
        if (count > bestCount) {
            bestCount = count;
            delimiter = candidate;
        }
    }
    m_impl->csvDelimiter = delimiter;

    auto rows = QSharedPointer<QVector<QStringList>>::create();
    auto quotedRows = QSharedPointer<QVector<QVector<bool>>>::create();
    QStringList row;
    QVector<bool> quotedRow;
    QString field;
    bool inQuotes = false;
    bool fieldQuoted = false;

    auto finishField = [&] {
        row.append(field);
        quotedRow.append(fieldQuoted);
        field.clear();
        fieldQuoted = false;
    };
    auto finishRow = [&] {
        finishField();
        rows->append(row);
        quotedRows->append(quotedRow);
        row.clear();
        quotedRow.clear();
    };

    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (inQuotes) {
            if (ch == QLatin1Char('"')) {
                if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                    field += QLatin1Char('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += ch;
            }
            continue;
        }
        if (ch == QLatin1Char('"')) {
            inQuotes = true;
            fieldQuoted = true;
        } else if (ch == delimiter) {
            finishField();
        } else if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
            if (ch == QLatin1Char('\r') && i + 1 < text.size()
                && text.at(i + 1) == QLatin1Char('\n')) {
                ++i;
            }
            finishRow();
        } else {
            field += ch;
        }
    }
    if (!field.isEmpty() || !row.isEmpty() || !quotedRow.isEmpty())
        finishRow();

    m_impl->csvRows = rows;
    m_impl->csvQuoted = quotedRows;

    for (int r = 0; r < rows->size(); ++r) {
        for (int c = 0; c < rows->at(r).size(); ++c) {
            const QString value = rows->at(r).at(c);
            if (!looksTranslatable(value))
                continue;
            Impl::Segment segment;
            segment.source = value;
            segment.apply = [rows, r, c](const QString& translated) {
                (*rows)[r][c] = translated;
            };
            m_impl->segments.append(segment);
        }
    }
    return true;
}

bool DocumentTranslationJob::prepareYaml()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);

    try {
        m_impl->yamlRoots = YAML::LoadAll(text.toStdString());
    } catch (const YAML::Exception& error) {
        m_impl->error = QStringLiteral("Не удалось разобрать YAML: %1")
                            .arg(QString::fromUtf8(error.what()));
        return false;
    }

    std::function<void(YAML::Node)> walk = [&](YAML::Node node) {
        if (node.IsMap()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                YAML::Node value = it->second;
                if (value.IsScalar()) {
                    const QString source = QString::fromStdString(value.as<std::string>());
                    if (!looksTranslatable(source))
                        continue;
                    const std::string key = it->first.as<std::string>();
                    Impl::Segment segment;
                    segment.source = source;
                    segment.apply = [node, key](const QString& translated) mutable {
                        node[key] = translated.toStdString();
                    };
                    m_impl->segments.append(segment);
                } else {
                    walk(value);
                }
            }
        } else if (node.IsSequence()) {
            for (std::size_t i = 0; i < node.size(); ++i) {
                YAML::Node value = node[i];
                if (value.IsScalar()) {
                    const QString source = QString::fromStdString(value.as<std::string>());
                    if (!looksTranslatable(source))
                        continue;
                    const std::size_t index = i;
                    Impl::Segment segment;
                    segment.source = source;
                    segment.apply = [node, index](const QString& translated) mutable {
                        node[index] = translated.toStdString();
                    };
                    m_impl->segments.append(segment);
                } else {
                    walk(value);
                }
            }
        }
    };

    for (const YAML::Node& root : m_impl->yamlRoots)
        walk(root);
    return true;
}

bool DocumentTranslationJob::writeCsv()
{
    if (!m_impl->csvRows || !m_impl->csvQuoted)
        return false;

    QString content;
    for (int r = 0; r < m_impl->csvRows->size(); ++r) {
        QStringList cells;
        for (int c = 0; c < m_impl->csvRows->at(r).size(); ++c) {
            const QString value = m_impl->csvRows->at(r).at(c);
            const bool forceQuote = c < m_impl->csvQuoted->at(r).size()
                && m_impl->csvQuoted->at(r).at(c);
            const bool needsQuote = forceQuote || value.contains(m_impl->csvDelimiter)
                || value.contains(QLatin1Char('"')) || value.contains(QLatin1Char('\n'))
                || value.contains(QLatin1Char('\r'));
            if (needsQuote) {
                QString escaped = value;
                escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
                cells.append(QLatin1Char('"') + escaped + QLatin1Char('"'));
            } else {
                cells.append(value);
            }
        }
        content += cells.join(m_impl->csvDelimiter);
        if (r + 1 < m_impl->csvRows->size())
            content += m_impl->csvLineEnding;
    }
    if (m_impl->csvTrailingNewline && !content.isEmpty())
        content += m_impl->csvLineEnding;

    QSaveFile file(m_impl->outputPath);
    if (!file.open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("Не удалось сохранить %1: %2")
                 .arg(m_impl->outputPath, file.errorString()));
        return false;
    }
    file.write(content.toUtf8());
    if (!file.commit()) {
        fail(QStringLiteral("Не удалось сохранить %1: %2")
                 .arg(m_impl->outputPath, file.errorString()));
        return false;
    }
    return true;
}

bool DocumentTranslationJob::writeYaml()
{
    std::string output;
    try {
        for (std::size_t i = 0; i < m_impl->yamlRoots.size(); ++i) {
            if (i > 0)
                output += "---\n";
            output += YAML::Dump(m_impl->yamlRoots[i]);
        }
    } catch (const YAML::Exception& error) {
        fail(QStringLiteral("Не удалось сохранить YAML: %1")
                 .arg(QString::fromUtf8(error.what())));
        return false;
    }

    QSaveFile file(m_impl->outputPath);
    if (!file.open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("Не удалось сохранить %1: %2")
                 .arg(m_impl->outputPath, file.errorString()));
        return false;
    }
    file.write(QByteArray::fromStdString(output));
    if (!file.commit()) {
        fail(QStringLiteral("Не удалось сохранить %1: %2")
                 .arg(m_impl->outputPath, file.errorString()));
        return false;
    }
    return true;
}

bool DocumentTranslationJob::prepareXliff()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    auto* document = new QDomDocument;
    const auto parseResult = document->setContent(
        file.readAll(), QDomDocument::ParseOption::Default);
    if (!parseResult) {
        delete document;
        m_impl->error = QStringLiteral("Не удалось разобрать XLIFF: %1 (строка %2)")
                            .arg(parseResult.errorMessage)
                            .arg(parseResult.errorLine);
        return false;
    }

    QVector<QDomElement> sources;
    collectElementsByLocalName(document->documentElement(), QStringLiteral("source"), sources);
    for (const QDomElement& source : std::as_const(sources)) {
        QDomElement parent = source.parentNode().toElement();
        if (parent.isNull())
            continue;
        QDomElement target;
        for (QDomElement child = parent.firstChildElement(); !child.isNull();
             child = child.nextSiblingElement()) {
            if (localName(child) == QLatin1String("target")) {
                target = child;
                break;
            }
        }
        if (target.isNull()) {
            const QString prefix = source.prefix();
            target = document->createElement(prefix.isEmpty()
                                                 ? QStringLiteral("target")
                                                 : prefix + QStringLiteral(":target"));
            parent.insertAfter(target, source);
        }
        m_impl->addXmlUnit(document, source, target);
    }

    m_impl->documents.append(document);
    m_impl->xmlDocument = document;
    return true;
}

bool DocumentTranslationJob::prepareTs()
{
    QFile file(m_impl->inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    auto* document = new QDomDocument;
    const auto parseResult = document->setContent(
        file.readAll(), QDomDocument::ParseOption::Default);
    if (!parseResult) {
        delete document;
        m_impl->error = QStringLiteral("Не удалось разобрать TS: %1 (строка %2)")
                            .arg(parseResult.errorMessage)
                            .arg(parseResult.errorLine);
        return false;
    }

    QVector<QDomElement> sources;
    collectElementsByLocalName(document->documentElement(), QStringLiteral("source"), sources);
    for (const QDomElement& source : std::as_const(sources)) {
        QDomElement parent = source.parentNode().toElement();
        if (parent.isNull())
            continue;
        QDomElement target;
        for (QDomElement child = parent.firstChildElement(); !child.isNull();
             child = child.nextSiblingElement()) {
            if (localName(child) == QLatin1String("translation")) {
                target = child;
                break;
            }
        }
        if (target.isNull()) {
            target = document->createElement(QStringLiteral("translation"));
            parent.insertAfter(target, source);
        }
        if (target.attribute(QStringLiteral("type")) == QLatin1String("unfinished"))
            target.removeAttribute(QStringLiteral("type"));
        m_impl->addXmlUnit(document, source, target);
    }

    m_impl->documents.append(document);
    m_impl->xmlDocument = document;
    return true;
}

bool DocumentTranslationJob::preparePptx()
{
    QZipReader zip(m_impl->inputPath);
    if (!zip.isReadable()) {
        m_impl->error = QStringLiteral("Не удалось открыть PPTX как ZIP-архив.");
        return false;
    }

    for (const QZipReader::FileInfo& info : zip.fileInfoList()) {
        if (info.isDir)
            continue;
        const QByteArray data = zip.fileData(info.filePath);
        m_impl->zipEntries.append({info.filePath, data});
        if (isPptxTextPart(info.filePath) && !addOfficePart(info.filePath, data))
            return false;
    }

    if (m_impl->documentParts.isEmpty()) {
        m_impl->error = QStringLiteral("В PPTX не найден текст для перевода.");
        return false;
    }
    return true;
}

bool DocumentTranslationJob::addOfficePart(const QString& name, const QByteArray& data)
{
    auto* document = new QDomDocument;
    const auto parseResult = document->setContent(
        data, QDomDocument::ParseOption::Default);
    if (!parseResult) {
        delete document;
        m_impl->error = QStringLiteral("Не удалось разобрать %1: %2 (строка %3)")
                            .arg(name, parseResult.errorMessage)
                            .arg(parseResult.errorLine);
        return false;
    }

    QVector<QDomElement> paragraphs;
    collectParagraphs(document->documentElement(), paragraphs);
    for (const QDomElement& paragraph : std::as_const(paragraphs)) {
        QVector<QDomElement> nodes;
        collectTextNodes(paragraph, nodes);
        if (nodes.isEmpty())
            continue;

        QString source;
        for (const QDomElement& node : std::as_const(nodes))
            source += node.text();
        if (source.trimmed().isEmpty())
            continue;

        Impl::Segment segment;
        segment.source = source;
        segment.apply = [document, nodes](const QString& translated) {
            const QString text = flattenLine(translated);
            int total = 0;
            for (const QDomElement& node : nodes)
                total += std::max(1, int(node.text().size()));

            int offset = 0;
            int accumulated = 0;
            for (int i = 0; i < nodes.size(); ++i) {
                accumulated += std::max(1, int(nodes[i].text().size()));
                const int end = (i == nodes.size() - 1)
                    ? text.size()
                    : boundaryOffset(text, int(qint64(text.size()) * accumulated / total),
                                 offset);
                setTextNode(document, nodes[i], text.mid(offset, end - offset));
                offset = end;
            }
        };
        m_impl->segments.append(segment);
    }

    m_impl->documents.append(document);
    m_impl->documentParts.insert(name, document);
    return true;
}

bool DocumentTranslationJob::writeXmlDocument()
{
    if (!m_impl->xmlDocument)
        return false;
    QSaveFile file(m_impl->outputPath);
    if (!file.open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("Не удалось сохранить %1: %2")
                 .arg(m_impl->outputPath, file.errorString()));
        return false;
    }
    file.write(m_impl->xmlDocument->toByteArray(0));
    if (!file.commit()) {
        fail(QStringLiteral("Не удалось сохранить %1: %2")
                 .arg(m_impl->outputPath, file.errorString()));
        return false;
    }
    return true;
}

bool DocumentTranslationJob::writePptx()
{
    return writeZipWithReplacements();
}



bool DocumentTranslationJob::prepareEpub()
{
    QZipReader zip(m_impl->inputPath);
    if (!zip.isReadable()) {
        m_impl->error = QStringLiteral("Не удалось открыть EPUB как ZIP-архив.");
        return false;
    }

    for (const QZipReader::FileInfo& info : zip.fileInfoList()) {
        if (info.isDir)
            continue;
        const QByteArray data = zip.fileData(info.filePath);
        m_impl->zipEntries.append({info.filePath, data});

        const QString lower = info.filePath.toLower();
        if (!(lower.endsWith(QLatin1String(".xhtml")) || lower.endsWith(QLatin1String(".html"))
              || lower.endsWith(QLatin1String(".htm")))) {
            continue;
        }

        QString text = QString::fromUtf8(data);
        if (text.startsWith(QChar(0xFEFF)))
            text.remove(0, 1);
        auto pieces = QSharedPointer<QStringList>::create();
        m_impl->addHtmlSegments(text, pieces);
        m_impl->epubPieces.insert(info.filePath, pieces);
    }

    if (m_impl->epubPieces.isEmpty()) {
        m_impl->error = QStringLiteral("В EPUB не найдены XHTML-документы.");
        return false;
    }
    return true;
}

bool DocumentTranslationJob::writeFb2()
{
    if (!m_impl->fb2Document)
        return false;

    QSaveFile file(m_impl->outputPath);
    if (!file.open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("Не удалось сохранить %1: %2")
                 .arg(m_impl->outputPath, file.errorString()));
        return false;
    }
    file.write(m_impl->fb2Document->toByteArray(0));
    if (!file.commit()) {
        fail(QStringLiteral("Не удалось сохранить %1: %2")
                 .arg(m_impl->outputPath, file.errorString()));
        return false;
    }
    return true;
}

bool DocumentTranslationJob::writeEpub()
{
    QHash<QString, QByteArray> replacements;
    for (auto it = m_impl->epubPieces.constBegin();
         it != m_impl->epubPieces.constEnd(); ++it) {
        replacements.insert(it.key(), it.value()->join(QString()).toUtf8());
    }

    QByteArray mimetype = "application/epub+zip";
    for (const auto& entry : std::as_const(m_impl->zipEntries)) {
        if (entry.first == QLatin1String("mimetype")) {
            mimetype = entry.second;
            break;
        }
    }

    const QString temporary = m_impl->outputPath + QStringLiteral(".qtranslate.tmp");
    QFile::remove(temporary);
    QZipWriter writer(temporary);
    if (!writer.isWritable()) {
        fail(QStringLiteral("Не удалось создать архив EPUB."));
        return false;
    }

    writer.setCompressionPolicy(QZipWriter::NeverCompress);
    writer.addFile(QStringLiteral("mimetype"), mimetype);
    writer.setCompressionPolicy(QZipWriter::AutoCompress);
    for (const auto& entry : std::as_const(m_impl->zipEntries)) {
        if (entry.first == QLatin1String("mimetype"))
            continue;
        const auto replacement = replacements.constFind(entry.first);
        writer.addFile(entry.first, replacement != replacements.constEnd()
                                           ? replacement.value()
                                           : entry.second);
    }
    writer.close();
    if (writer.status() != QZipWriter::NoError) {
        QFile::remove(temporary);
        fail(QStringLiteral("Не удалось записать EPUB."));
        return false;
    }

    QFile::remove(m_impl->outputPath);
    if (!QFile::rename(temporary, m_impl->outputPath)) {
        QFile::remove(temporary);
        fail(QStringLiteral("Не удалось сохранить %1.").arg(m_impl->outputPath));
        return false;
    }
    return true;
}

bool DocumentTranslationJob::preparePdf()
{
#ifdef Q_OS_WIN
    if (m_impl->pdfData.isEmpty()) {
        QFile file(m_impl->inputPath);
        if (!file.open(QIODevice::ReadOnly)) {
            m_impl->error = file.errorString();
            return false;
        }
        m_impl->pdfData = file.readAll();
    }
    if (m_impl->pdfData.isEmpty()) {
        m_impl->error = QStringLiteral("PDF-файл пуст.");
        return false;
    }

    ensurePdfium();
    FPDF_DOCUMENT document = FPDF_LoadMemDocument64(
        m_impl->pdfData.constData(), size_t(m_impl->pdfData.size()), nullptr);
    if (!document) {
        m_impl->error = QStringLiteral("Не удалось открыть PDF: %1.").arg(pdfiumErrorText());
        return false;
    }

    m_impl->pdfPageCount = FPDF_GetPageCount(document);
    m_impl->pdfPageDpi = QVector<int>(m_impl->pdfPageCount, 200);
    if (m_impl->context && m_impl->context->settings) {
        m_impl->documentDpiOverride = qBound(
            0, m_impl->context->settings->intValue(QStringLiteral("Ocr.DocumentDpi"), 0), 400);
    }
    if (m_impl->pdfPageCount <= 0) {
        FPDF_CloseDocument(document);
        m_impl->error = QStringLiteral("В PDF нет страниц.");
        return false;
    }

    for (int pageIndex = 0; pageIndex < m_impl->pdfPageCount; ++pageIndex) {
        FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
        if (!page)
            continue;
        const double pageWidth = FPDF_GetPageWidthF(page);
        const double pageHeight = FPDF_GetPageHeightF(page);
        if (pageIndex == 0)
            m_impl->pdfFirstPageSize = QSizeF(pageWidth, pageHeight);

        FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
        if (textPage) {
            struct Line {
                QString text;
                QRectF box;
                double size = 10.0;
                double bottom = 0.0;
                double right = 0.0;
                QColor color = Qt::black;
                QString fontName;
                bool bold = false;
                bool italic = false;
            };

            QVector<Line> pageLines;
            Line current;
            auto flush = [&] {
                current.text = current.text.trimmed();
                if (!current.text.isEmpty())
                    pageLines.append(current);
                current = Line{};
            };

            const int count = FPDFText_CountChars(textPage);
            for (int i = 0; i < count; ++i) {
                const unsigned int code = FPDFText_GetUnicode(textPage, i);
                if (code == 0 || code == '\r' || code == '\n')
                    continue;

                double left = 0;
                double right = 0;
                double top = 0;
                double bottom = 0;
                if (!FPDFText_GetCharBox(textPage, i, &left, &right, &top, &bottom))
                    continue;
                if (right < left)
                    std::swap(left, right);
                if (top < bottom)
                    std::swap(top, bottom);

                double size = FPDFText_GetFontSize(textPage, i);
                if (size <= 0.0)
                    size = 10.0;

                QRectF box(left, pageHeight - top, right - left, top - bottom);
                const bool space = QChar::isSpace(code);
                if (box.width() <= 0.0 || box.height() <= 0.0) {
                    if (!space)
                        continue;
                    box.setWidth(qMax(size * 0.25, 0.1));
                    box.setHeight(size);
                }

                if (!current.text.isEmpty()
                    && (qAbs(box.bottom() - current.bottom)
                            > qMax(size, current.size) * 0.65
                        || box.left() > current.right + qMax(size, current.size) * 2.0)) {
                    flush();
                }
                if (space && current.text.isEmpty())
                    continue;

                if (current.text.isEmpty()) {
                    current.color = Qt::black;
                    unsigned int r = 0;
                    unsigned int g = 0;
                    unsigned int b = 0;
                    unsigned int a = 0;
                    if (FPDFText_GetFillColor(textPage, i, &r, &g, &b, &a))
                        current.color = QColor(int(r), int(g), int(b), int(a > 0 ? a : 255));
                    char fontBuffer[256] = {};
                    int flags = 0;
                    const unsigned long fontLength = FPDFText_GetFontInfo(
                        textPage, i, fontBuffer, sizeof(fontBuffer), &flags);
                    if (fontLength > 0)
                        current.fontName = cleanPdfFontName(QString::fromUtf8(fontBuffer));
                    current.bold = (flags & (1 << 18)) != 0
                        || FPDFText_GetFontWeight(textPage, i) >= 600;
                    current.italic = (flags & 64) != 0;
                }

                current.text += QString::fromUcs4(reinterpret_cast<const char32_t*>(&code), 1);
                current.box = current.box.isNull() ? box : current.box.united(box);
                current.size = qMax(current.size, size);
                current.bottom = box.bottom();
                current.right = qMax(current.right, box.right());
            }
            flush();
            FPDFText_ClosePage(textPage);

            std::sort(pageLines.begin(), pageLines.end(), [](const Line& a, const Line& b) {
                if (qAbs(a.box.top() - b.box.top()) > 2.0)
                    return a.box.top() < b.box.top();
                return a.box.left() < b.box.left();
            });

            // Join consecutive lines of one paragraph: translating whole
            // paragraphs reads better and gives one box to fit the text into.
            QVector<Line> paragraphs;
            for (const Line& pageLine : std::as_const(pageLines)) {
                if (!paragraphs.isEmpty()) {
                    Line& last = paragraphs.last();
                    const double lineHeight = qMax(1.0, last.box.height());
                    const double gap = pageLine.box.top() - last.box.bottom();
                    const bool sameLeft =
                        qAbs(pageLine.box.left() - last.box.left()) <= lineHeight * 1.2;
                    const bool similarWidth = pageLine.box.width() <= last.box.width() * 1.35
                        && pageLine.box.width() >= last.box.width() * 0.55;
                    const bool similarSize = pageLine.size >= last.size * 0.7
                        && pageLine.size <= last.size * 1.45;
                    const bool close = gap >= -lineHeight * 0.8
                        && gap <= lineHeight * 1.15;
                    const QString nextText = pageLine.text.trimmed();
                    const bool hyphenated = last.text.trimmed().endsWith(QLatin1Char('-'))
                        && !nextText.isEmpty() && nextText.at(0).isLower()
                        && gap <= lineHeight * 2.5
                        && qAbs(pageLine.box.left() - last.box.left()) <= lineHeight * 2.0;
                    if (hyphenated) {
                        appendWrappedText(last.text, pageLine.text);
                        last.box = last.box.united(pageLine.box);
                        last.right = qMax(last.right, pageLine.right);
                        last.bottom = pageLine.bottom;
                        continue;
                    }
                    if (sameLeft && similarWidth && similarSize && close
                        && last.text.size() < 800
                        && !looksLikeListItem(pageLine.text)
                        && !last.text.trimmed().endsWith(QLatin1Char(':'))) {
                        appendWrappedText(last.text, pageLine.text);
                        last.box = last.box.united(pageLine.box);
                        last.right = qMax(last.right, pageLine.right);
                        last.bottom = pageLine.bottom;
                        continue;
                    }
                }
                paragraphs.append(pageLine);
            }

            for (const Line& line : std::as_const(paragraphs)) {
                Impl::PdfLine pdfLine;
                pdfLine.source = line.text;
                pdfLine.page = pageIndex;
                pdfLine.box = line.box;
                pdfLine.fontSize = line.size;
                pdfLine.color = line.color;
                pdfLine.fontName = line.fontName;
                pdfLine.bold = line.bold;
                pdfLine.italic = line.italic;
                m_impl->pdfLines.append(pdfLine);

                const int index = m_impl->pdfLines.size() - 1;
                Impl::Segment segment;
                segment.source = pdfLine.source;
                segment.apply = [this, index](const QString& translated) {
                    m_impl->pdfLines[index].translated = translated;
                };
                m_impl->segments.append(segment);
            }
        }
        FPDF_ClosePage(page);
    }
    FPDF_CloseDocument(document);

    QString layerSample;
    for (const Impl::PdfLine& line : std::as_const(m_impl->pdfLines)) {
        layerSample += line.source + QLatin1Char(' ');
        if (layerSample.size() > 4000)
            break;
    }
    if (!m_impl->pdfLines.isEmpty() && !textLayerLooksReadable(layerSample)) {
        // A text layer can exist and still be useless: broken encodings produce
        // replacement characters or symbol soup, and some scans carry an empty layer
        // with coordinates only. Fall through to recognition instead of translating it.
        m_impl->pdfLines.clear();
        m_impl->segments.clear();
    }

    if (m_impl->pdfLines.isEmpty()) {
        // Scanned document: there is no text layer to extract, so recognize the pages.
        if (startPdfOcr())
            return true;
        m_impl->error = QStringLiteral(
            "В PDF не найден текстовый слой, а OCR-сервис недоступен. Включите OCR в настройках.");
        return false;
    }
    return true;
#else
    m_impl->error = QStringLiteral("Перевод PDF поддерживается только в Windows-сборке.");
    return false;
#endif
}

bool DocumentTranslationJob::extractDjvuText(const QString& inputPath, QString* textPath)
{
    const QString applicationDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    candidates << QDir(applicationDir).filePath(QStringLiteral("services/ocr/djvu/djvutxt.exe"));
    candidates << QDir(applicationDir).filePath(QStringLiteral("data/services/ocr/djvu/djvutxt.exe"));
    if (m_impl->context && m_impl->context->paths) {
        candidates << QDir(m_impl->context->paths->dataDir())
                          .filePath(QStringLiteral("services/ocr/djvu/djvutxt.exe"));
    }

    QString executable;
    for (const QString& candidate : std::as_const(candidates)) {
        if (QFileInfo::exists(candidate)) {
            executable = candidate;
            break;
        }
    }
    if (executable.isEmpty())
        return false;

    m_impl->djvuTextDir = std::make_unique<QTemporaryDir>();
    if (!m_impl->djvuTextDir->isValid())
        return false;
    const QString output = QDir(m_impl->djvuTextDir->path()).filePath(QStringLiteral("text.txt"));

    QProcess process;
    process.setProgram(executable);
    process.setArguments({QDir::toNativeSeparators(inputPath),
                          QDir::toNativeSeparators(output)});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(15000) || !process.waitForFinished(300000))
        return false;
    if (process.exitCode() != 0 || !QFileInfo::exists(output))
        return false;

    QFile file(output);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QString text = QString::fromUtf8(file.readAll());
    if (!textLayerLooksReadable(text))
        return false;

    const QString paragraphs = splitDjvuText(text);
    if (paragraphs.trimmed().isEmpty())
        return false;

    const QString paragraphPath =
        QDir(m_impl->djvuTextDir->path()).filePath(QStringLiteral("text_paragraphs.txt"));
    QFile paragraphFile(paragraphPath);
    if (!paragraphFile.open(QIODevice::WriteOnly))
        return false;
    paragraphFile.write(paragraphs.toUtf8());
    paragraphFile.close();

    *textPath = paragraphPath;
    return true;
}

bool DocumentTranslationJob::convertDjvuToPdf(const QString& inputPath)
{
    const QString applicationDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    candidates << QDir(applicationDir).filePath(QStringLiteral("services/ocr/djvu/ddjvu.exe"));
    candidates << QDir(applicationDir).filePath(QStringLiteral("data/services/ocr/djvu/ddjvu.exe"));
    if (m_impl->context && m_impl->context->paths) {
        candidates << QDir(m_impl->context->paths->dataDir())
                          .filePath(QStringLiteral("services/ocr/djvu/ddjvu.exe"));
    }

    QString executable;
    for (const QString& candidate : std::as_const(candidates)) {
        if (QFileInfo::exists(candidate)) {
            executable = candidate;
            break;
        }
    }
    if (executable.isEmpty()) {
        m_impl->error = QStringLiteral(
            "Для DJVU нужен внешний ddjvu.exe. Положите его в services/ocr/djvu/.");
        return false;
    }

    QTemporaryDir workspace;
    if (!workspace.isValid()) {
        m_impl->error = QStringLiteral("Не удалось создать временную папку для DJVU.");
        return false;
    }
    const QString pdfPath = QDir(workspace.path()).filePath(QStringLiteral("converted.pdf"));

    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("-format=pdf"), QDir::toNativeSeparators(inputPath),
                          QDir::toNativeSeparators(pdfPath)});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(15000)) {
        m_impl->error = QStringLiteral("Не удалось запустить ddjvu.exe.");
        return false;
    }
    if (!process.waitForFinished(600000)) {
        process.kill();
        process.waitForFinished(5000);
        m_impl->error = QStringLiteral("ddjvu.exe не завершился за 10 минут.");
        return false;
    }

    if (process.exitCode() != 0 || !QFileInfo::exists(pdfPath)) {
        const QString details = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        m_impl->error = details.isEmpty()
            ? QStringLiteral("ddjvu.exe завершился с кодом %1.").arg(process.exitCode())
            : details;
        return false;
    }

    QFile file(pdfPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_impl->error = file.errorString();
        return false;
    }
    m_impl->pdfData = file.readAll();
    if (m_impl->pdfData.isEmpty()) {
        m_impl->error = QStringLiteral("ddjvu.exe вернул пустой файл.");
        return false;
    }
    return true;
}

bool DocumentTranslationJob::startPdfOcr()
{
#ifdef Q_OS_WIN
    if (!m_impl->context || !m_impl->context->translation || m_impl->pdfData.isEmpty()
        || m_impl->pdfPageCount <= 0) {
        return false;
    }

    m_impl->pdfOcrPending = true;
    m_impl->pdfOcrPage = 0;
    m_impl->pdfOcrPageImages = QVector<QByteArray>(m_impl->pdfPageCount);
    QTimer::singleShot(0, this, &DocumentTranslationJob::recognizeNextPdfPage);
    return true;
#else
    return false;
#endif
}

void DocumentTranslationJob::recognizeNextPdfPage()
{
#ifdef Q_OS_WIN
    if (!m_impl || m_impl->cancelled || m_impl->failed)
        return;

    if (m_impl->pdfOcrPage >= m_impl->pdfPageCount) {
        m_impl->pdfOcrPending = false;
        if (m_impl->pdfLines.isEmpty()) {
            fail(QStringLiteral("Не удалось распознать текст в отсканированном PDF."));
            return;
        }
        beginTranslation();
        return;
    }

    const int pageIndex = m_impl->pdfOcrPage;
    ensurePdfium();
    FPDF_DOCUMENT document = FPDF_LoadMemDocument64(
        m_impl->pdfData.constData(), size_t(m_impl->pdfData.size()), nullptr);
    if (!document) {
        fail(QStringLiteral("Не удалось открыть PDF для распознавания: %1.")
                 .arg(pdfiumErrorText()));
        return;
    }

    FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
    if (!page) {
        FPDF_CloseDocument(document);
        fail(QStringLiteral("Не удалось открыть страницу %1 PDF.").arg(pageIndex + 1));
        return;
    }

    const double pageWidth = FPDF_GetPageWidthF(page);
    const double pageHeight = FPDF_GetPageHeightF(page);
    int dpi = m_impl->documentDpiOverride;
    if (dpi > 0) {
        dpi = qBound(150, dpi, 400);
    } else {
        // Never go below 200 (Tesseract likes the extra pixels) and never above
        // 300 (no gain, only time and file size).
        dpi = detectImageDpi(page);
        dpi = dpi > 0 ? qBound(200, dpi, 300) : 200;
    }
    if (pageIndex < m_impl->pdfPageDpi.size())
        m_impl->pdfPageDpi[pageIndex] = dpi;
    const int pixelWidth = qMax(1, int(qRound(pageWidth / 72.0 * dpi)));
    const int pixelHeight = qMax(1, int(qRound(pageHeight / 72.0 * dpi)));

    FPDF_BITMAP bitmap = FPDFBitmap_Create(pixelWidth, pixelHeight, 1);
    if (!bitmap) {
        FPDF_ClosePage(page);
        FPDF_CloseDocument(document);
        fail(QStringLiteral("Не удалось подготовить страницу %1 к распознаванию.")
                 .arg(pageIndex + 1));
        return;
    }
    FPDFBitmap_FillRect(bitmap, 0, 0, pixelWidth, pixelHeight, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, pixelWidth, pixelHeight, 0, FPDF_ANNOT);
    QImage rendered(reinterpret_cast<uchar*>(FPDFBitmap_GetBuffer(bitmap)), pixelWidth,
                    pixelHeight, FPDFBitmap_GetStride(bitmap), QImage::Format_ARGB32);
    rendered = rendered.copy();
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);
    FPDF_CloseDocument(document);

    // Scans are often captured slightly askew, which breaks line grouping. The
    // straightened bitmap is kept as the page background below, so OCR boxes and
    // the drawn image stay in the same coordinate space.
    const double angle = estimateSkewAngle(rendered);
    const QImage straightened = deskewImageCropped(rendered, angle);

    QString language;
    if (m_impl->context->settings) {
        language = m_impl->context->settings->stringValue(QStringLiteral("Ocr.OcrLanguage"),
                                                          QStringLiteral("eng"));
    }
    if (language.isEmpty())
        language = QStringLiteral("eng");

    const QImage ocrImage = preparePageForOcr(straightened);
    const QString profile =
        (QFileInfo(m_impl->inputPath).suffix().compare(QStringLiteral("djvu"), Qt::CaseInsensitive) == 0
         || QFileInfo(m_impl->inputPath).suffix().compare(QStringLiteral("djv"), Qt::CaseInsensitive) == 0)
            ? QStringLiteral("djvu")
            : QStringLiteral("pdf");
    QPointer<DocumentTranslationJob> self(this);

    // OCR is the expensive part of a book run; keep the chosen page result in a
    // content-addressed cache so a repeated translation is instant.
    QString cachePath;
    if (m_impl->context && m_impl->context->paths) {
        QByteArray imageBytes;
        QBuffer imageBuffer(&imageBytes);
        imageBuffer.open(QIODevice::WriteOnly);
        ocrImage.save(&imageBuffer, "PNG");
        QCryptographicHash hash(QCryptographicHash::Sha1);
        hash.addData(imageBytes);
        hash.addData(language.toUtf8());
        hash.addData(profile.toUtf8());
        hash.addData(QByteArray::number(dpi));
        hash.addData("ocr-v1");
        const QString cacheDir =
            QDir(m_impl->context->paths->dataDir()).filePath(QStringLiteral("cache/ocr"));
        QDir().mkpath(cacheDir);
        cachePath = QDir(cacheDir).filePath(
            QString::fromLatin1(hash.result().toHex()) + QStringLiteral(".json"));
    }

    const auto handlePage = [self, pageIndex, straightened, dpi, cachePath](const QJsonObject& result) {
        if (!cachePath.isEmpty() && result.value(QStringLiteral("error")).toString().isEmpty()) {
            QFile cacheFile(cachePath);
            if (cacheFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
                cacheFile.write(QJsonDocument(result).toJson(QJsonDocument::Compact));
        }
            if (!self || self->m_impl->cancelled || self->m_impl->failed)
                return;

            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            // The OCR page may be rasterized at 300 dpi for accuracy, but the
            // background stored for the output PDF stays at 200 dpi: otherwise a
            // 300-page book would hold gigabytes of PNGs in memory.
            QImage background = straightened;
            if (dpi > 200) {
                background = straightened.scaled(
                    qMax(1, int(std::lround(straightened.width() * 200.0 / dpi))),
                    qMax(1, int(std::lround(straightened.height() * 200.0 / dpi))),
                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
            background.save(&buffer, "PNG");
            self->m_impl->pdfOcrPageImages[pageIndex] = png;

            const QString error = result.value(QStringLiteral("error")).toString();
            if (!error.isEmpty()) {
                self->fail(error);
                return;
            }

            self->appendPdfOcrLines(result, pageIndex, dpi);
            ++self->m_impl->pdfOcrPage;
            emit self->progress(pageIndex + 1, self->m_impl->pdfPageCount,
                                QStringLiteral("OCR: страница %1").arg(pageIndex + 1));
            QTimer::singleShot(0, self, &DocumentTranslationJob::recognizeNextPdfPage);    };

    if (!cachePath.isEmpty()) {
        QFile cacheFile(cachePath);
        if (cacheFile.exists() && cacheFile.open(QIODevice::ReadOnly)) {
            const QJsonDocument cached = QJsonDocument::fromJson(cacheFile.readAll());
            if (cached.isObject() && !cached.object().isEmpty()) {
                handlePage(cached.object());
                return;
            }
        }
    }

    const auto runPass = [self, ocrImage, language, profile](
                             const QString& providerId, int psm,
                             const std::function<void(const QJsonObject&)>& done) {
        if (!self || self->m_impl->cancelled || self->m_impl->failed)
            return;
        self->m_impl->context->translation->ocrRequestAsync(
            ocrImage, providerId, language,
            [done](const QJsonObject& result) { done(result); }, true, profile, psm);
    };

    const auto runCropPass = [self, ocrImage, language, profile](
                                 const QRect& crop, int psm,
                                 const std::function<void(const QJsonObject&)>& done) {
        if (!self || self->m_impl->cancelled || self->m_impl->failed)
            return;
        const QImage part = ocrImage.copy(crop);
        self->m_impl->context->translation->ocrRequestAsync(
            part, QString(), language,
            [done, crop](const QJsonObject& result) {
                QJsonObject adjusted = result;
                QJsonArray shifted;
                for (const QJsonValue& value : result.value(QStringLiteral("lines")).toArray()) {
                    QJsonObject line = value.toObject();
                    line.insert(QStringLiteral("left"),
                                line.value(QStringLiteral("left")).toInt() + crop.left());
                    line.insert(QStringLiteral("top"),
                                line.value(QStringLiteral("top")).toInt() + crop.top());
                    shifted.append(line);
                }
                adjusted.insert(QStringLiteral("lines"), shifted);
                done(adjusted);
            },
            true, profile, psm);
    };

    const auto maybePaddle = [self, handlePage, runPass](const QJsonObject& tesseractBest) {
        if (!self || self->m_impl->cancelled || self->m_impl->failed)
            return;
        if (resultConfidence(tesseractBest) >= 0.70 && resultWordCount(tesseractBest) >= 10) {
            handlePage(tesseractBest);
            return;
        }
        runPass(QStringLiteral("paddleocr"), 0,
                [self, handlePage, tesseractBest](const QJsonObject& paddle) {
            if (!self || self->m_impl->cancelled || self->m_impl->failed)
                return;
            if (!paddle.value(QStringLiteral("error")).toString().isEmpty()) {
                handlePage(tesseractBest);
                return;
            }
            handlePage(resultScore(paddle) > resultScore(tesseractBest) ? paddle : tesseractBest);
        });
    };

    // A two-column page is read column by column, otherwise Tesseract merges the
    // two columns into one long line.
    const QVector<QRect> columns = detectTextColumns(ocrImage);
    if (columns.size() >= 2) {
        auto combinedLines = QSharedPointer<QJsonArray>::create();
        auto combinedText = QSharedPointer<QString>::create();
        auto step = std::make_shared<std::function<void(int)>>();
        *step = [self, columns, combinedLines, combinedText, step, runCropPass, handlePage](int index) {
            if (!self || self->m_impl->cancelled || self->m_impl->failed)
                return;
            if (index >= columns.size()) {
                QJsonObject combined;
                combined.insert(QStringLiteral("lines"), *combinedLines);
                combined.insert(QStringLiteral("text"), *combinedText);
                handlePage(combined);
                return;
            }
            runCropPass(columns.at(index), 6,
                        [self, index, step, combinedLines, combinedText, handlePage](
                            const QJsonObject& result) {
                if (!self || self->m_impl->cancelled || self->m_impl->failed)
                    return;
                for (const QJsonValue& value :
                     result.value(QStringLiteral("lines")).toArray())
                    combinedLines->append(value);
                *combinedText += result.value(QStringLiteral("text")).toString()
                    + QLatin1Char('\n');
                (*step)(index + 1);
            });
        };
        (*step)(0);
        return;
    }

    // PSM 6 suits book pages; when it is unsure, compare with the free-layout
    // PSM 3 and keep the better reading.
    runPass(QString(), 6, [self, handlePage, runPass, maybePaddle](const QJsonObject& first) {
        if (!self || self->m_impl->cancelled || self->m_impl->failed)
            return;
        if (!first.value(QStringLiteral("error")).toString().isEmpty()) {
            handlePage(first);
            return;
        }
        if (resultConfidence(first) >= 0.80 && resultWordCount(first) >= 20) {
            handlePage(first);
            return;
        }
        const double firstScore = resultScore(first);
        runPass(QString(), 3, [self, maybePaddle, first, firstScore](const QJsonObject& second) {
            if (!self || self->m_impl->cancelled || self->m_impl->failed)
                return;
            if (!second.value(QStringLiteral("error")).toString().isEmpty()) {
                maybePaddle(first);
                return;
            }
            maybePaddle(resultScore(second) > firstScore ? second : first);
        });
    });
#endif
}

void DocumentTranslationJob::appendPdfOcrLines(const QJsonObject& result, int pageIndex, int dpi)
{
#ifdef Q_OS_WIN
    const double pageDpi = dpi > 0 ? double(dpi) : 200.0;
    SpellService spell(m_impl->context->plugins, m_impl->context->settings);
    const bool hasSpell = spell.provider() != nullptr;
    QString dictionaryLanguage = QStringLiteral("en");
    if (m_impl->context && m_impl->context->settings) {
        dictionaryLanguage = languageCode(m_impl->context->settings->intValue(
            QStringLiteral("General.LanguageFrom"), 17));
    }
    const QJsonArray lines = result.value(QStringLiteral("lines")).toArray();
    const double toPoints = 72.0 / dpi;
    QVector<Impl::PdfLine> pageLines;
    QVector<Impl::PdfLine> rawLines;
    for (const QJsonValue& value : lines) {
        const QJsonObject entry = value.toObject();
        const QString text = entry.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty())
            continue;

        const int left = entry.value(QStringLiteral("left")).toInt();
        const int top = entry.value(QStringLiteral("top")).toInt();
        const int width = entry.value(QStringLiteral("width")).toInt();
        const int height = entry.value(QStringLiteral("height")).toInt();
        if (width <= 0 || height <= 0)
            continue;

        Impl::PdfLine pdfLine;
        pdfLine.source = text;
        pdfLine.page = pageIndex;
        pdfLine.box = QRectF(left * toPoints, top * toPoints,
                             width * toPoints, height * toPoints);
        pdfLine.fontSize = qMax(4.0, height * toPoints);
        pdfLine.color = Qt::black;

        rawLines.append(pdfLine);
        const double confidence = entry.value(QStringLiteral("confidence")).toDouble();
        int letters = 0;
        for (const QChar& character : text)
            letters += character.isLetter() ? 1 : 0;
        if (letters < 2)
            continue;
        if (confidence > 0.0 && confidence < 0.35)
            continue;
        // Low-confidence lines made of words the dictionary does not know are
        // scanner noise; dropping them keeps the translation readable.
        if (hasSpell && confidence > 0.0 && confidence < 0.55) {
            int longWords = 0;
            int known = 0;
            const QStringList words = text.split(
                QRegularExpression(QStringLiteral("[\\s]+")), Qt::SkipEmptyParts);
            for (const QString& word : words) {
                QString clean;
                for (const QChar& character : word)
                    if (character.isLetter())
                        clean.append(character);
                if (clean.size() < 3)
                    continue;
                ++longWords;
                if (spell.checkWord(clean, dictionaryLanguage))
                    ++known;
            }
            if (longWords >= 3 && known == 0)
                continue;
        }

        pageLines.append(pdfLine);
    }
    // A page whose lines all failed the confidence/dictionary check is better
    // translated roughly than skipped entirely.
    if (pageLines.isEmpty())
        pageLines = rawLines;

    std::sort(pageLines.begin(), pageLines.end(), [](const Impl::PdfLine& left,
                                                     const Impl::PdfLine& right) {
        if (qAbs(left.box.top() - right.box.top()) > 1.0)
            return left.box.top() < right.box.top();
        return left.box.left() < right.box.left();
    });

    QVector<Impl::PdfLine> paragraphs;
    for (const Impl::PdfLine& line : std::as_const(pageLines)) {
        if (!paragraphs.isEmpty()) {
            Impl::PdfLine& last = paragraphs.last();
            const double lineHeight = qMax(1.0, last.box.height());
            const double gap = line.box.top() - last.box.bottom();
            const double overlap = qMax(0.0, qMin(last.box.right(), line.box.right())
                                                - qMax(last.box.left(), line.box.left()));
            const double minWidth = qMin(last.box.width(), line.box.width());
            const bool sameColumn = overlap >= minWidth * 0.5;
            const bool similarHeight = line.box.height() >= last.box.height() * 0.6
                && line.box.height() <= last.box.height() * 1.6;
            const bool close = gap >= -lineHeight * 0.7 && gap <= lineHeight * 1.3;
            const QString nextText = line.source.trimmed();
            const bool hyphenated = last.source.trimmed().endsWith(QLatin1Char('-'))
                && !nextText.isEmpty() && nextText.at(0).isLower()
                && gap <= lineHeight * 2.5
                && qAbs(line.box.left() - last.box.left()) <= lineHeight * 2.0;
            if (hyphenated) {
                appendWrappedText(last.source, line.source);
                last.box = last.box.united(line.box);
                last.fontSize = qMax(last.fontSize, line.fontSize);
                continue;
            }
            if (sameColumn && similarHeight && close
                && last.source.size() < 800
                && !looksLikeListItem(line.source)
                && !last.source.trimmed().endsWith(QLatin1Char(':'))) {
                appendWrappedText(last.source, line.source);
                last.box = last.box.united(line.box);
                last.fontSize = qMax(last.fontSize, line.fontSize);
                continue;
            }
        }
        paragraphs.append(line);
    }

    for (const Impl::PdfLine& paragraph : std::as_const(paragraphs)) {
        m_impl->pdfLines.append(paragraph);
        const int index = m_impl->pdfLines.size() - 1;
        Impl::Segment segment;
        segment.source = paragraph.source;
        segment.apply = [this, index](const QString& translated) {
            m_impl->pdfLines[index].translated = translated;
        };
        m_impl->segments.append(segment);
    }
#else
    Q_UNUSED(result);
    Q_UNUSED(pageIndex);
#endif
}

bool DocumentTranslationJob::writePdf()
{
#ifdef Q_OS_WIN
    if (m_impl->pdfData.isEmpty() || m_impl->pdfLines.isEmpty()
        || m_impl->pdfFirstPageSize.isEmpty()) {
        return false;
    }

    ensurePdfium();
    FPDF_DOCUMENT document = FPDF_LoadMemDocument64(
        m_impl->pdfData.constData(), size_t(m_impl->pdfData.size()), nullptr);
    if (!document) {
        fail(QStringLiteral("Не удалось повторно открыть PDF: %1.").arg(pdfiumErrorText()));
        return false;
    }

    const QString temporary = m_impl->outputPath + QStringLiteral(".qtranslate.tmp");
    QFile::remove(temporary);

    const int dpi = 200;
    QPdfWriter writer(temporary);
    writer.setResolution(dpi);
    writer.setPageSize(QPageSize(m_impl->pdfFirstPageSize, QPageSize::Point));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    QPainter painter(&writer);
    if (!painter.isActive()) {
        FPDF_CloseDocument(document);
        fail(QStringLiteral("Не удалось создать PDF-файл перевода."));
        return false;
    }

    QString fallbackFamily;
    if (m_impl->context && m_impl->context->settings) {
        fallbackFamily = m_impl->context->settings->stringValue(
            QStringLiteral("Appearance.FallbackFont"));
    }
    const QFont fallbackFont = fallbackFamily.isEmpty()
        ? QFontDatabase::systemFont(QFontDatabase::GeneralFont)
        : QFont(fallbackFamily);
    bool wrotePage = false;
    for (int pageIndex = 0; pageIndex < m_impl->pdfPageCount; ++pageIndex) {
        FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
        if (!page)
            continue;

        const double pageWidth = FPDF_GetPageWidthF(page);
        const double pageHeight = FPDF_GetPageHeightF(page);
        if (pageWidth <= 0.0 || pageHeight <= 0.0) {
            FPDF_ClosePage(page);
            continue;
        }

        const int pageDpi =
            (pageIndex < m_impl->pdfPageDpi.size() && m_impl->pdfPageDpi.at(pageIndex) > 0)
            ? m_impl->pdfPageDpi.at(pageIndex)
            : 200;
        // OCR may use 300 dpi, but the page image in the output PDF is stored at
        // 200 dpi to keep book files reasonable; the writer stays at 200 dpi too.
        const int imageDpi = qMin(pageDpi, 200);
        const double imageScale = imageDpi / 72.0;
        const int pixelWidth = qMax(1, int(qRound(pageWidth * imageScale)));
        const int pixelHeight = qMax(1, int(qRound(pageHeight * imageScale)));

        // Recognized pages already have a straightened bitmap that matches the OCR
        // boxes, so draw that instead of the original scan.
        QImage image;
        if (pageIndex < m_impl->pdfOcrPageImages.size())
            image.loadFromData(m_impl->pdfOcrPageImages.at(pageIndex), "PNG");

        if (image.isNull()) {
            FPDF_BITMAP bitmap = FPDFBitmap_Create(pixelWidth, pixelHeight, 1);
            if (!bitmap) {
                FPDF_ClosePage(page);
                continue;
            }
            FPDFBitmap_FillRect(bitmap, 0, 0, pixelWidth, pixelHeight, 0xFFFFFFFF);
            FPDF_RenderPageBitmap(bitmap, page, 0, 0, pixelWidth, pixelHeight, 0, FPDF_ANNOT);
            image = QImage(reinterpret_cast<uchar*>(FPDFBitmap_GetBuffer(bitmap)), pixelWidth,
                           pixelHeight, FPDFBitmap_GetStride(bitmap), QImage::Format_ARGB32)
                        .copy();
            FPDFBitmap_Destroy(bitmap);
        }
        FPDF_ClosePage(page);

        // PdfLine boxes are points; the rendered page image is at `dpi` and the
        // writer also works at `dpi`, so one factor converts both.
        const double pointToPixel = dpi / 72.0;
        const QRectF pageBounds(0, 0, writer.width(), writer.height());

        struct RenderItem {
            const Impl::PdfLine* line = nullptr;
            QRectF rect;
            int pixelSize = 8;
            bool centered = false;
        };
        QVector<RenderItem> items;
        for (const Impl::PdfLine& line : std::as_const(m_impl->pdfLines)) {
            if (line.page != pageIndex || line.translated.trimmed().isEmpty())
                continue;
            QRectF rect(line.box.left() * pointToPixel, line.box.top() * pointToPixel,
                        line.box.width() * pointToPixel,
                        line.box.height() * pointToPixel);
            const double lineHeightPoints = qMax(line.box.height(), line.fontSize * 1.15);
            const double centerY = line.box.center().y();
            rect.setTop((centerY - lineHeightPoints / 2.0) * pointToPixel);
            rect.setHeight(lineHeightPoints * pointToPixel);
            rect.adjust(0, 0, qMax(2.0, line.fontSize * 0.35 * pointToPixel), 0);
            rect = rect.intersected(pageBounds);
            if (rect.width() < 4.0 || rect.height() < 4.0)
                continue;

            QFont font = fallbackFont;
            const QString family = cleanPdfFontName(line.fontName);
            if (!family.isEmpty())
                font.setFamily(family);
            font.setBold(line.bold);
            font.setItalic(line.italic);

            // Short lines that sit on the page centre are headings: keep them
            // centred instead of justifying them edge to edge.
            const bool centered = line.box.width() < pageWidth * 0.65
                && qAbs(line.box.center().x() - pageWidth / 2.0) < pageWidth * 0.04;

            const auto metricsAt = [&](int pixelSize, double* widestWord) {
                QFont probe = font;
                probe.setPixelSize(qMax(1, pixelSize));
                QTextLayout layout(line.translated, probe);
                QTextOption option;
                option.setWrapMode(QTextOption::WordWrap);
                option.setAlignment(centered ? Qt::AlignHCenter : Qt::AlignJustify);
                layout.setTextOption(option);
                layout.beginLayout();
                int lineCount = 0;
                while (true) {
                    QTextLine textLine = layout.createLine();
                    if (!textLine.isValid())
                        break;
                    textLine.setLineWidth(rect.width());
                    ++lineCount;
                }
                layout.endLayout();
                if (widestWord) {
                    const QFontMetricsF metrics(probe, &writer);
                    double widest = 0.0;
                    const QStringList words = line.translated.split(
                        QLatin1Char(' '), Qt::SkipEmptyParts);
                    for (const QString& word : std::as_const(words))
                        widest = qMax(widest, metrics.horizontalAdvance(word));
                    *widestWord = widest;
                }
                return lineCount * qMax(1.0, pixelSize * 1.15);
            };

            double pixelSize = qMax(6.0, line.fontSize * pointToPixel);
            bool fits = false;
            for (; pixelSize >= 5.0; pixelSize -= 1.0) {
                double widestWord = 0.0;
                const double height = metricsAt(qMax(1, int(qRound(pixelSize))), &widestWord);
                fits = height <= rect.height() + 1.0 && widestWord <= rect.width();
                if (fits)
                    break;
            }
            QRectF finalRect = rect;
            int finalSize = qMax(1, int(qRound(pixelSize)));
            if (!fits) {
                finalSize = 6;
                double widestWord = 0.0;
                const double needed = metricsAt(finalSize, &widestWord);
                // Grow downwards, but never into the next block on the page.
                double limit = pageBounds.bottom() - 2.0;
                for (const Impl::PdfLine& other : std::as_const(m_impl->pdfLines)) {
                    if (&other == &line || other.page != pageIndex)
                        continue;
                    if (other.box.top() <= line.box.top() + 1.0)
                        continue;
                    const double otherLeft = other.box.left() * pointToPixel;
                    const double otherRight = other.box.right() * pointToPixel;
                    const double overlap = qMin(rect.right(), otherRight)
                        - qMax(rect.left(), otherLeft);
                    if (overlap > 2.0)
                        limit = qMin(limit, other.box.top() * pointToPixel - 2.0);
                }
                finalRect.setHeight(qMax(6.0,
                    qMin(qMax(finalRect.height(), needed + 2.0), limit - finalRect.top())));
            }
            items.append(RenderItem{ &line, finalRect, finalSize, centered });
        }

        QPainter imagePainter(&image);
        const double toImage = pointToPixel > 0.0 ? imageScale / pointToPixel : 1.0;
        for (const RenderItem& item : std::as_const(items)) {
            QRectF box(item.rect.left() * toImage, item.rect.top() * toImage,
                       item.rect.width() * toImage, item.rect.height() * toImage);
            box = box.adjusted(-1.0, -1.0, 2.0, 2.0);
            const QColor background = sampleBackground(image, box);
            imagePainter.fillRect(box, background);
        }
        imagePainter.end();

        if (wrotePage)
            writer.newPage();
        wrotePage = true;

        const QRectF pageRect(0, 0, writer.width(), writer.height());
        painter.drawImage(pageRect, image);

        for (const RenderItem& item : std::as_const(items)) {
            const Impl::PdfLine& line = *item.line;
            QFont font = fallbackFont;
            const QString family = cleanPdfFontName(line.fontName);
            if (!family.isEmpty())
                font.setFamily(family);
            font.setBold(line.bold);
            font.setItalic(line.italic);
            font.setPixelSize(item.pixelSize);

            QTextLayout layout(line.translated, font);
            QTextOption option;
            option.setWrapMode(QTextOption::WordWrap);
            option.setAlignment(item.centered ? Qt::AlignHCenter : Qt::AlignJustify);
            layout.setTextOption(option);
            layout.beginLayout();
            const double step = qMax(1.0, item.pixelSize * 1.15);
            double y = 0.0;
            while (true) {
                QTextLine textLine = layout.createLine();
                if (!textLine.isValid())
                    break;
                textLine.setLineWidth(item.rect.width());
                textLine.setPosition(QPointF(0.0, y));
                y += step;
                if (y > item.rect.height() + step)
                    break;
            }
            layout.endLayout();

            painter.save();
            painter.setClipRect(item.rect.adjusted(0, 0, 0, 1));
            painter.setPen(line.color.isValid() ? line.color : Qt::black);
            layout.draw(&painter, QPointF(item.rect.left(), item.rect.top()));
            painter.restore();
        }
    }

    painter.end();
    FPDF_CloseDocument(document);

    if (!wrotePage) {
        QFile::remove(temporary);
        fail(QStringLiteral("В PDF нет страниц для сохранения."));
        return false;
    }

    QFile::remove(m_impl->outputPath);
    if (!QFile::rename(temporary, m_impl->outputPath)) {
        QFile::remove(temporary);
        fail(QStringLiteral("Не удалось сохранить %1.").arg(m_impl->outputPath));
        return false;
    }
    return true;
#else
    return false;
#endif
}

void DocumentTranslationJob::translateNext()
{
    if (!m_impl || m_impl->cancelled || m_impl->failed)
        return;

    if (m_impl->index >= m_impl->segments.size()) {
        writeOutput();
        return;
    }

    const int current = m_impl->index;
    const Impl::Segment& segment = m_impl->segments.at(current);
    QPointer<DocumentTranslationJob> self(this);
    m_impl->context->translation->translateDocumentSegment(
        segment.source, m_impl->sourceCode, m_impl->targetCode, m_impl->providerId,
        [self, current](const QJsonObject& result) {
            if (!self || self->m_impl->cancelled || self->m_impl->failed)
                return;

            const QString error = result.value(QStringLiteral("error")).toString();
            if (!error.isEmpty()) {
                self->fail(error);
                return;
            }
            const QString translated = result.value(QStringLiteral("text")).toString();
            if (translated.trimmed().isEmpty()) {
                self->fail(QStringLiteral("Сервис вернул пустой перевод."));
                return;
            }

            self->m_impl->segments[current].apply(translated);
            ++self->m_impl->index;
            emit self->progress(self->m_impl->index, int(self->m_impl->segments.size()),
                                self->m_impl->segments.at(current).source.left(80));
            QTimer::singleShot(0, self, &DocumentTranslationJob::translateNext);
        });
}

void DocumentTranslationJob::writeOutput()
{
    if (m_impl->cancelled || m_impl->failed)
        return;

    QFileInfo info(m_impl->inputPath);
    const QString suffix = info.suffix().toLower();
    bool ok = false;
    if (suffix == QStringLiteral("docx"))
        ok = writeDocx();
    else if (suffix == QStringLiteral("pptx"))
        ok = writePptx();
    else if (suffix == QStringLiteral("fb2"))
        ok = writeFb2();
    else if (suffix == QStringLiteral("epub"))
        ok = writeEpub();
    else if (suffix == QStringLiteral("pdf") || suffix == QStringLiteral("djvu")
             || suffix == QStringLiteral("djv"))
        ok = writePdf();
    else if (suffix == QStringLiteral("xliff") || suffix == QStringLiteral("xlf")
             || suffix == QStringLiteral("ts")) {
        ok = writeXmlDocument();
    } else if (suffix == QStringLiteral("json") || suffix == QStringLiteral("xml")) {
        ok = writeTokenText();
    } else if (suffix == QStringLiteral("csv")) {
        ok = writeCsv();
    } else if (suffix == QStringLiteral("yaml") || suffix == QStringLiteral("yml")) {
        ok = writeYaml();
    } else {
        ok = writeText();
    }
    if (!ok)
        return;

    emit progress(int(m_impl->segments.size()), int(m_impl->segments.size()), QString());
    emit finished(m_impl->outputPath);
}

bool DocumentTranslationJob::writeTokenText()
{
    if (!m_impl->tokenPieces)
        return false;

    QSaveFile file(m_impl->outputPath);
    if (!file.open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("Не удалось сохранить %1: %2")
                 .arg(m_impl->outputPath, file.errorString()));
        return false;
    }
    file.write(m_impl->tokenPieces->join(QString()).toUtf8());
    if (!file.commit()) {
        fail(QStringLiteral("Не удалось сохранить %1: %2")
                 .arg(m_impl->outputPath, file.errorString()));
        return false;
    }
    return true;
}

bool DocumentTranslationJob::writeText()
{
    const QString suffix = QFileInfo(m_impl->inputPath).suffix().toLower();
    QString content;
    if (suffix == QStringLiteral("html") || suffix == QStringLiteral("htm")) {
        if (!m_impl->htmlPieces)
            return false;
        content = m_impl->htmlPieces->join(QString());
    } else {
        if (!m_impl->textLines)
            return false;
        content = m_impl->textLines->join(m_impl->lineEnding);
    }

    QSaveFile file(m_impl->outputPath);
    if (!file.open(QIODevice::WriteOnly)) {
        m_impl->error = file.errorString();
        fail(QStringLiteral("Не удалось сохранить %1: %2")
                 .arg(m_impl->outputPath, file.errorString()));
        return false;
    }
    file.write(content.toUtf8());
    if (!file.commit()) {
        fail(QStringLiteral("Не удалось сохранить %1: %2")
                 .arg(m_impl->outputPath, file.errorString()));
        return false;
    }
    return true;
}

bool DocumentTranslationJob::writeDocx()
{
    return writeZipWithReplacements();
}

bool DocumentTranslationJob::writeZipWithReplacements()
{
    QHash<QString, QByteArray> replacements;
    for (auto it = m_impl->documentParts.constBegin();
         it != m_impl->documentParts.constEnd(); ++it) {
        replacements.insert(it.key(), it.value()->toByteArray(0));
    }

    const QString temporary = m_impl->outputPath + QStringLiteral(".qtranslate.tmp");
    QFile::remove(temporary);
    QZipWriter writer(temporary);
    writer.setCompressionPolicy(QZipWriter::AutoCompress);
    if (!writer.isWritable()) {
        fail(QStringLiteral("Не удалось создать архив перевода."));
        return false;
    }

    for (const auto& entry : std::as_const(m_impl->zipEntries)) {
        const auto replacement = replacements.constFind(entry.first);
        writer.addFile(entry.first, replacement != replacements.constEnd()
                                           ? replacement.value()
                                           : entry.second);
    }
    writer.close();
    if (writer.status() != QZipWriter::NoError) {
        QFile::remove(temporary);
        fail(QStringLiteral("Не удалось записать DOCX."));
        return false;
    }

    QFile::remove(m_impl->outputPath);
    if (!QFile::rename(temporary, m_impl->outputPath)) {
        QFile::remove(temporary);
        fail(QStringLiteral("Не удалось сохранить %1.").arg(m_impl->outputPath));
        return false;
    }
    return true;
}

void DocumentTranslationJob::fail(const QString& message)
{
    if (m_impl && m_impl->context && m_impl->context->paths) {
        QDir().mkpath(m_impl->context->paths->logDir());
        QFile log(QDir(m_impl->context->paths->logDir())
                      .filePath(QStringLiteral("document-job.log")));
        if (log.open(QIODevice::WriteOnly | QIODevice::Append))
            log.write((message + QLatin1Char('\n')).toUtf8());
    }
    if (m_impl->failed)
        return;
    m_impl->failed = true;
    emit failed(message);
}
