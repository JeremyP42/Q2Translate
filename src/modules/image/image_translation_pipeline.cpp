#include "image_translation_pipeline.h"

#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "core/app_context.h"
#include "core/path_manager.h"
#include "core/settings_manager.h"
#include "core/translation_manager.h"
#include "modules/image/bubble_detector.h"
#include "modules/image/image_overlay.h"
#include "services/plugin_manager.h"
#include "services/spell_service.h"


// One recognized text line: the box is in original image coordinates. Defined
// before Impl because the pipeline keeps the OCR line list between the first pass
// and the recovery pass.
struct OcrLine {
    QRect box;
    QString text;
    int height = 0;
    double confidence = 0.0;
};

// Word-level output of Tesseract, used to rebuild lines from confident words the
// way the Firefox translator does (its cleanLines step).
struct OcrWordBox {
    QRect box;
    QString text;
    double confidence = 0.0;
    int block = 0;
    int paragraph = 0;
    int line = 0;
};

int alnumCount(const QString& text)
{
    int count = 0;
    for (const QChar& character : text)
        count += character.isLetterOrNumber() ? 1 : 0;
    return count;
}

QVector<OcrWordBox> parseOcrWords(const QJsonObject& result)
{
    QVector<OcrWordBox> words;
    const QJsonArray array = result.value(QStringLiteral("words")).toArray();
    for (const QJsonValue& value : array) {
        const QJsonObject entry = value.toObject();
        const QString text = entry.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty())
            continue;
        OcrWordBox word;
        word.text = text;
        word.box = QRect(entry.value(QStringLiteral("left")).toInt(),
                         entry.value(QStringLiteral("top")).toInt(),
                         entry.value(QStringLiteral("width")).toInt(),
                         entry.value(QStringLiteral("height")).toInt());
        if (word.box.width() <= 0 || word.box.height() <= 0)
            continue;
        word.confidence = entry.value(QStringLiteral("confidence")).toDouble();
        word.block = entry.value(QStringLiteral("block")).toInt();
        word.paragraph = entry.value(QStringLiteral("paragraph")).toInt();
        word.line = entry.value(QStringLiteral("line")).toInt();
        words.append(word);
    }
    return words;
}

QVector<OcrLine> cleanLinesFromWords(const QVector<OcrWordBox>& words, double minConfidence,
                                     const QSize& imageSize)
{
    QVector<OcrLine> lines;
    if (words.isEmpty())
        return lines;
    const double minConf = minConfidence > 0.0 ? minConfidence : 0.6;
    const double wordMin = qMax(0.30, minConf - 0.20);
    const QRegularExpression punctuationOnly(
        QStringLiteral("^[.!?…,;:'\\\"¿¡-]+$"));

    QVector<QVector<OcrWordBox>> groups;
    for (const OcrWordBox& word : words) {
        if (!groups.isEmpty()) {
            const OcrWordBox& previous = groups.last().last();
            if (previous.block == word.block && previous.paragraph == word.paragraph
                && previous.line == word.line) {
                groups.last().append(word);
                continue;
            }
        }
        groups.append(QVector<OcrWordBox>{ word });
    }

    for (const QVector<OcrWordBox>& group : groups) {
        QVector<OcrWordBox> kept;
        bool hasAlnum = false;
        for (const OcrWordBox& word : group) {
            const int alnum = alnumCount(word.text);
            if (alnum > 0 && word.confidence >= wordMin) {
                kept.append(word);
                hasAlnum = true;
            } else if (alnum == 0 && punctuationOnly.match(word.text).hasMatch()
                       && word.confidence >= minConf) {
                kept.append(word);
            }
        }
        if (!hasAlnum || kept.isEmpty())
            continue;
        QRect box;
        QString text;
        double weighted = 0.0;
        int letters = 0;
        for (const OcrWordBox& word : kept) {
            box = box.isNull() ? word.box : box.united(word.box);
            text += (text.isEmpty() ? QString() : QStringLiteral(" ")) + word.text;
            const int count = alnumCount(word.text);
            weighted += word.confidence * count;
            letters += count;
        }
        if (letters < 2)
            continue;
        const int height = box.height();
        if (height < 8 || height > imageSize.height() * 0.3)
            continue;
        if (height > box.width() * 1.6)
            continue;
        const QString compact = text.simplified();
        QString junkFree = compact;
        junkFree.remove(QRegularExpression(
            QStringLiteral("[\\s.!?…,;:'\\\"¿¡~\\-—–・。、！？～♡♥「」『』()（）]")));
        if (junkFree.isEmpty())
            continue;
        if (letters / double(junkFree.size()) < 0.55
            || letters / double(qMax(1, compact.size())) < 0.3)
            continue;
        OcrLine line;
        line.box = box;
        line.text = text;
        line.height = height;
        line.confidence = letters > 0 ? weighted / letters : 0.0;
        lines.append(line);
    }
    return lines;
}



// One extra OCR request: large-page tiles, ink-recovery regions or a bubble-pass
// cluster (with alternative image variants that are tried until the reading is
// plausible).
struct ExtraOcrJob {
    int stage = 0;  // 0 = tile, 1 = missed ink, 2 = letter cluster
    QRect region;
    QVector<QImage> variants;
    int variantIndex = 0;
    int textHeight = 0;
    bool firstDone = false;
    QString firstNormalized;
    int blockIndex = -1;
    QString bestText;
    double bestScore = -1e9;
    double baseScore = -1e9;
    int baseLetters = 0;
    int baseKnown = 0;
    TextBubble bubble;
    bool hasBubble = false;
    int psm = 0;
};


// A group of Tesseract lines that form one text block (a bubble or a caption).
// The grouping rules follow the Firefox image translator: lines join only when
// they are stacked (never side by side), close vertically and overlap or sit
// under the same centre; heavily overlapping readings keep the more confident one.
struct TesseractGroup {
    QRect box;
    QString text;
    int lineHeight = 0;
    double confidence = 0.0;
};

QVector<TesseractGroup> groupTesseractLines(QVector<OcrLine> lines, int imageWidth, int imageHeight)
{
    QVector<OcrLine> kept;
    for (const OcrLine& line : lines) {
        int letters = 0;
        for (const QChar& character : line.text)
            letters += character.isLetterOrNumber() ? 1 : 0;
        if (letters < 2)
            continue;
        const int height = line.box.height();
        if (height < 8 || height > imageHeight * 0.3)
            continue;
        if (height > line.box.width() * 1.6)
            continue;
        kept.append(line);
    }
    if (kept.isEmpty())
        return {};

    QVector<int> parent(kept.size());
    for (int index = 0; index < parent.size(); ++index)
        parent[index] = index;
    const auto findRoot = [&parent](int value) {
        while (parent[value] != value) {
            parent[value] = parent[parent[value]];
            value = parent[value];
        }
        return value;
    };
    for (int i = 0; i < kept.size(); ++i) {
        for (int j = i + 1; j < kept.size(); ++j) {
            const OcrLine& a = kept.at(i);
            const OcrLine& b = kept.at(j);
            const int minHeight = qMin(a.box.height(), b.box.height());
            const int maxHeight = qMax(a.box.height(), b.box.height());
            if (minHeight < maxHeight * 0.35 || minHeight > maxHeight * 1.8)
                continue;
            const int gap = qMax(0, qMax(a.box.top(), b.box.top())
                                        - qMin(a.box.bottom(), b.box.bottom()));
            if (gap > minHeight * 1.2)
                continue;
            const int overlap = qMin(a.box.right(), b.box.right())
                - qMax(a.box.left(), b.box.left());
            const int minWidth = qMin(a.box.width(), b.box.width());
            const int maxWidth = qMax(a.box.width(), b.box.width());
            const int centerDistance = qAbs(a.box.center().x() - b.box.center().x());
            const bool sameRow = qMin(a.box.bottom(), b.box.bottom())
                    - qMax(a.box.top(), b.box.top()) > minHeight * 0.5;
            if (sameRow)
                continue;
            if (overlap >= minWidth * 0.3)
                parent[findRoot(i)] = findRoot(j);
        }
    }

    QHash<int, QVector<int>> buckets;
    for (int index = 0; index < kept.size(); ++index)
        buckets[findRoot(index)].append(index);

    QVector<TesseractGroup> groups;
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it) {
        QVector<int> members = it.value();
        std::sort(members.begin(), members.end(), [&kept](int left, int right) {
            if (kept.at(left).box.top() != kept.at(right).box.top())
                return kept.at(left).box.top() < kept.at(right).box.top();
            return kept.at(left).box.left() < kept.at(right).box.left();
        });
        TesseractGroup group;
        QVector<int> heights;
        double confidenceWeighted = 0.0;
        int letters = 0;
        for (int member : members) {
            const OcrLine& line = kept.at(member);
            group.box = group.box.isNull() ? line.box : group.box.united(line.box);
            group.text += (group.text.isEmpty() ? QString() : QStringLiteral(" ")) + line.text;
            heights.append(line.box.height());
            int lineLetters = 0;
            for (const QChar& character : line.text)
                lineLetters += character.isLetterOrNumber() ? 1 : 0;
            confidenceWeighted += line.confidence * lineLetters;
            letters += lineLetters;
        }
        std::sort(heights.begin(), heights.end());
        group.lineHeight = heights.at(heights.size() / 2);
        group.confidence = letters > 0 ? confidenceWeighted / letters : 0.0;
        group.text = group.text.simplified();
        if (letters < 3)
            continue;
        if (qint64(group.box.width()) * group.box.height()
            > qint64(imageWidth) * imageHeight * 6 / 10)
            continue;
        if (group.confidence > 0.0 && group.confidence < 0.6)
            continue;
        // A single short line needs high confidence ("WHAT?!" is fine, "res" is not).
        if (members.size() == 1 && letters <= 4 && group.confidence < 0.85)
            continue;
        groups.append(group);
    }

    // Two readings of the same lettering must not both be drawn: keep the better.
    std::sort(groups.begin(), groups.end(), [](const TesseractGroup& left, const TesseractGroup& right) {
        const double leftScore = left.confidence * std::sqrt(double(qMax(1, left.text.size())));
        const double rightScore = right.confidence * std::sqrt(double(qMax(1, right.text.size())));
        return leftScore > rightScore;
    });
    QVector<TesseractGroup> result;
    for (const TesseractGroup& group : std::as_const(groups)) {
        bool duplicate = false;
        for (const TesseractGroup& keptGroup : std::as_const(result)) {
            const QRect intersection = group.box.intersected(keptGroup.box);
            if (intersection.isEmpty())
                continue;
            const int smaller = qMin(group.box.width() * group.box.height(),
                                     keptGroup.box.width() * keptGroup.box.height());
            const QPoint center = group.box.center();
            const bool centreInside = keptGroup.box.contains(center);
            if (centreInside && smaller > 0
                && intersection.width() * intersection.height() * 100 > smaller * 85) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            result.append(group);
    }
    std::sort(result.begin(), result.end(), [](const TesseractGroup& left, const TesseractGroup& right) {
        if (left.box.top() != right.box.top())
            return left.box.top() < right.box.top();
        return left.box.left() < right.box.left();
    });
    return result;
}

// ---- block re-reading (plugin-style) -------------------------------------
// A block is read several ways: contrast-stretched gray, binarised and thickened
// ink. Each reading is scored like the Firefox translator does it
// (confidence - junk*8 + known words*6) and the best one wins.

QImage grayCopy(const QImage& source)
{
    const QImage rgb = source.convertToFormat(QImage::Format_RGB32);
    QImage gray(rgb.size(), QImage::Format_RGB32);
    for (int y = 0; y < rgb.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        QRgb* target = reinterpret_cast<QRgb*>(gray.scanLine(y));
        for (int x = 0; x < rgb.width(); ++x) {
            const int value = qGray(row[x]);
            target[x] = qRgb(value, value, value);
        }
    }
    return gray;
}

QImage stretchedGray(const QImage& gray)
{
    int minimum = 255;
    int maximum = 0;
    for (int y = 0; y < gray.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(gray.constScanLine(y));
        for (int x = 0; x < gray.width(); ++x) {
            const int value = qGray(row[x]);
            minimum = qMin(minimum, value);
            maximum = qMax(maximum, value);
        }
    }
    if (maximum - minimum < 8)
        return gray;
    QImage result(gray.size(), QImage::Format_RGB32);
    const double span = maximum - minimum;
    for (int y = 0; y < gray.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(gray.constScanLine(y));
        QRgb* target = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < gray.width(); ++x) {
            const int value = qBound(0, int((qGray(row[x]) - minimum) * 255.0 / span), 255);
            target[x] = qRgb(value, value, value);
        }
    }
    return result;
}

QImage binarized(const QImage& gray, int threshold)
{
    QImage result(gray.size(), QImage::Format_RGB32);
    for (int y = 0; y < gray.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(gray.constScanLine(y));
        QRgb* target = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < gray.width(); ++x)
            target[x] = qGray(row[x]) < threshold ? qRgb(0, 0, 0) : qRgb(255, 255, 255);
    }
    return result;
}

QImage invertedGray(const QImage& gray)
{
    QImage result(gray.size(), QImage::Format_RGB32);
    for (int y = 0; y < gray.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(gray.constScanLine(y));
        QRgb* target = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < gray.width(); ++x) {
            const int value = 255 - qGray(row[x]);
            target[x] = qRgb(value, value, value);
        }
    }
    return result;
}

QImage thickenedInk(const QImage& binary)
{
    QImage result = binary;
    for (int y = 1; y + 1 < binary.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(binary.constScanLine(y));
        const QRgb* up = reinterpret_cast<const QRgb*>(binary.constScanLine(y - 1));
        const QRgb* down = reinterpret_cast<const QRgb*>(binary.constScanLine(y + 1));
        QRgb* target = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 1; x + 1 < binary.width(); ++x) {
            if (row[x] == qRgb(0, 0, 0))
                continue;
            if (row[x - 1] == qRgb(0, 0, 0) || row[x + 1] == qRgb(0, 0, 0)
                || up[x] == qRgb(0, 0, 0) || down[x] == qRgb(0, 0, 0))
                target[x] = qRgb(0, 0, 0);
        }
    }
    return result;
}

// Crops are upscaled before re-reading: Tesseract needs ~10+ px glyphs, and a
// manga bubble crop is often only 40-60 px wide.
QVector<QImage> buildReadingVariants(const QImage& crop)
{
    QImage source = crop;
    const double zoom = qBound(1.0, 700.0 / qMax(1, qMax(crop.width(), crop.height())), 3.0);
    if (zoom > 1.01)
        source = crop.scaled(int(std::lround(crop.width() * zoom)),
                             int(std::lround(crop.height() * zoom)),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QImage gray = stretchedGray(grayCopy(source));
    const QImage binary = binarized(gray, 150);
    QVector<QImage> variants;
    variants.append(gray);
    variants.append(binary);
    variants.append(thickenedInk(binary));
    variants.append(binarized(gray, 100));
    variants.append(invertedGray(gray));
    return variants;
}

// Everyday English words, as in the Firefox translator"s knownWords list: a large
// spelling dictionary counts obscure words ("oust") that appear in OCR noise.
int commonWordCount(const QString& text)
{
    static const QSet<QString> common = {
        QStringLiteral("a"), QStringLiteral("about"), QStringLiteral("after"),
        QStringLiteral("again"), QStringLiteral("all"), QStringLiteral("always"),
        QStringLiteral("am"), QStringLiteral("an"), QStringLiteral("and"),
        QStringLiteral("any"), QStringLiteral("are"), QStringLiteral("as"),
        QStringLiteral("at"), QStringLiteral("away"), QStringLiteral("back"),
        QStringLiteral("be"), QStringLiteral("because"), QStringLiteral("been"),
        QStringLiteral("before"), QStringLiteral("but"), QStringLiteral("by"),
        QStringLiteral("can"), QStringLiteral("come"), QStringLiteral("could"),
        QStringLiteral("dad"), QStringLiteral("day"), QStringLiteral("did"),
        QStringLiteral("die"), QStringLiteral("do"), QStringLiteral("don't"),
        QStringLiteral("down"), QStringLiteral("enough"), QStringLiteral("even"),
        QStringLiteral("ever"), QStringLiteral("every"), QStringLiteral("eyes"),
        QStringLiteral("feel"), QStringLiteral("find"), QStringLiteral("first"),
        QStringLiteral("for"), QStringLiteral("from"), QStringLiteral("get"),
        QStringLiteral("give"), QStringLiteral("go"), QStringLiteral("going"),
        QStringLiteral("gone"), QStringLiteral("good"), QStringLiteral("got"),
        QStringLiteral("had"), QStringLiteral("has"), QStringLiteral("have"),
        QStringLiteral("he"), QStringLiteral("hear"), QStringLiteral("hello"),
        QStringLiteral("help"), QStringLiteral("her"), QStringLiteral("here"),
        QStringLiteral("hey"), QStringLiteral("him"), QStringLiteral("his"),
        QStringLiteral("how"), QStringLiteral("i"), QStringLiteral("i'm"),
        QStringLiteral("if"), QStringLiteral("in"), QStringLiteral("is"),
        QStringLiteral("it"), QStringLiteral("it's"), QStringLiteral("just"),
        QStringLiteral("keep"), QStringLiteral("kill"), QStringLiteral("know"),
        QStringLiteral("last"), QStringLiteral("leave"), QStringLiteral("let"),
        QStringLiteral("life"), QStringLiteral("like"), QStringLiteral("little"),
        QStringLiteral("live"), QStringLiteral("look"), QStringLiteral("made"),
        QStringLiteral("make"), QStringLiteral("man"), QStringLiteral("many"),
        QStringLiteral("me"), QStringLiteral("mean"), QStringLiteral("mom"),
        QStringLiteral("more"), QStringLiteral("must"), QStringLiteral("my"),
        QStringLiteral("need"), QStringLiteral("never"), QStringLiteral("new"),
        QStringLiteral("no"), QStringLiteral("not"), QStringLiteral("now"),
        QStringLiteral("of"), QStringLiteral("off"), QStringLiteral("oh"),
        QStringLiteral("ok"), QStringLiteral("okay"), QStringLiteral("on"),
        QStringLiteral("once"), QStringLiteral("one"), QStringLiteral("only"),
        QStringLiteral("or"), QStringLiteral("other"), QStringLiteral("our"),
        QStringLiteral("out"), QStringLiteral("over"), QStringLiteral("please"),
        QStringLiteral("really"), QStringLiteral("right"), QStringLiteral("run"),
        QStringLiteral("said"), QStringLiteral("say"), QStringLiteral("see"),
        QStringLiteral("she"), QStringLiteral("should"), QStringLiteral("so"),
        QStringLiteral("some"), QStringLiteral("someone"), QStringLiteral("something"),
        QStringLiteral("stay"), QStringLiteral("stop"), QStringLiteral("take"),
        QStringLiteral("tell"), QStringLiteral("than"), QStringLiteral("thank"),
        QStringLiteral("that"), QStringLiteral("the"), QStringLiteral("their"),
        QStringLiteral("them"), QStringLiteral("then"), QStringLiteral("there"),
        QStringLiteral("these"), QStringLiteral("they"), QStringLiteral("thing"),
        QStringLiteral("think"), QStringLiteral("this"), QStringLiteral("those"),
        QStringLiteral("time"), QStringLiteral("to"), QStringLiteral("together"),
        QStringLiteral("too"), QStringLiteral("try"), QStringLiteral("two"),
        QStringLiteral("up"), QStringLiteral("us"), QStringLiteral("wait"),
        QStringLiteral("want"), QStringLiteral("was"), QStringLiteral("way"),
        QStringLiteral("we"), QStringLiteral("well"), QStringLiteral("went"),
        QStringLiteral("were"), QStringLiteral("what"), QStringLiteral("when"),
        QStringLiteral("where"), QStringLiteral("which"), QStringLiteral("who"),
        QStringLiteral("why"), QStringLiteral("will"), QStringLiteral("with"),
        QStringLiteral("without"), QStringLiteral("won't"), QStringLiteral("would"),
        QStringLiteral("yes"), QStringLiteral("yet"), QStringLiteral("you"),
        QStringLiteral("your"), QStringLiteral("you're"), QStringLiteral("sorry"),
        QStringLiteral("sir"), QStringLiteral("wait"), QStringLiteral("here"),
        QStringLiteral("now"), QStringLiteral("what"), QStringLiteral("watch"),
        QStringLiteral("learn"), QStringLiteral("nothing"), QStringLiteral("what's")
    };
    int count = 0;
    const QStringList words = text.toLower().split(
        QRegularExpression(QStringLiteral("[\\s]+")), Qt::SkipEmptyParts);
    for (const QString& word : words) {
        QString clean;
        for (const QChar& character : word)
            if (character.isLetter() || character == QLatin1Char('\''))
                clean.append(character);
        if (clean.size() >= 2 && common.contains(clean))
            ++count;
    }
    return count;
}

double readingScore(const QString& text, double confidence, SpellService& spell,
                    const QString& language)
{
    int letters = 0;
    int known = 0;
    int junk = 0;
    const QStringList words = text.split(QRegularExpression(QStringLiteral("[\\s]+")),
                                         Qt::SkipEmptyParts);
    for (const QString& word : words) {
        QString clean;
        for (const QChar& character : word) {
            if (character.isLetter()) {
                clean.append(character);
                ++letters;
            } else if (!character.isDigit() && !QStringLiteral(".,!?…:;'\\\"-—–()[]{}«»/\\\\|").contains(character)) {
                ++junk;
            }
        }
        if (clean.size() >= 3 && spell.provider() && spell.checkWord(clean, language))
            ++known;
    }
    const double conf = confidence > 0.0 ? confidence * 100.0 : 60.0;
    return conf - junk * 8.0 + known * 6.0;
}

struct ImageTranslationPipeline::Impl {
    AppContext* context = nullptr;
    QImage image;
    QString sourceCode;
    QString targetCode;
    QString providerId;
    QString sourceText;
    QString translatedText;
    QVector<TextBubble> bubbles;
    QVector<OverlayLine> blocks;
    QVector<QString> blockSources;
    QVector<QString> lineDump;  // diagnostics: raw OCR lines before grouping
    QVector<OcrLine> ocrLines;  // first pass + recovered lines
    QVector<ExtraOcrJob> extraJobs;
    int extraIndex = 0;
    int extraStage = 0;
    qint64 stageStartMs = 0;
    int tileJobs = 0;
    int inkJobs = 0;
    int bubbleJobs = 0;
    int rereadJobs = 0;
    bool paperPage = false;
    bool tileLargePages = true;
    bool bubblePass = false;
    QVector<QRect> recoveryRegions;
    QVector<QRect> recoveryDump;  // regions actually re-read (diagnostics)
    int recoveredLines = 0;
    int recoveryIndex = 0;
    QString ocrLanguage;
    int bubbleIndex = 0;
    int index = 0;
    bool cancelled = false;
    bool failed = false;
    // P0 options, read once per image.
    bool llmProvider = false;
    bool llmContext = true;
    bool lengthBudget = true;
    bool splitParagraphs = true;
    bool recoverMissedText = true;  // targeted second OCR pass for missed words
    bool balloonSweep = false;       // read balloons that OCR saw no lines in
    int sweepJobs = 0;
    int sweepBubbles = 0;
    int letterBlobs = 0;
    int letterClusters = 0;
    bool denseTextFallback = false;
    QRect denseColumnRect;
    double denseColumnZoom = 1.0;
    QVector<TextBubble> balloons;
    bool balloonsReady = false;
    bool tesseractPath = false;     // plugin-style Tesseract pipeline
    bool hybridMode = false;        // Paddle detection + Tesseract re-reading
    QString ocrProviderOverride;    // forced provider for the main pass
    double tesseractScale = 1.0;
    QImage tesseractImage;
    int maxRecoveryRegions = 1;
    double minOcrConfidence = 0.45;
};

namespace {

QString correctWithDictionary(const QString& text, SpellService& spell, const QString& language)
{
    const QStringList tokens = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList corrected;
    for (const QString& token : tokens) {
        if (token.size() < 3 || !token.at(0).isLetter() || !token.at(token.size() - 1).isLetter()) {
            corrected.append(token);
            continue;
        }
        // Names and stylised caps ("SAYA", "McDonald") must survive: an equal-length
        // dictionary suggestion would turn them into ordinary words.
        bool allUpper = token.size() >= 2;
        bool innerUpper = false;
        for (int index = 0; index < token.size(); ++index) {
            const QChar character = token.at(index);
            if (!character.isLetter())
                continue;
            if (!character.isUpper())
                allUpper = false;
            else if (index > 0)
                innerUpper = true;
        }
        if (allUpper || innerUpper) {
            corrected.append(token);
            continue;
        }
        QStringList suggestions;
        if (spell.checkWord(token, language, &suggestions)) {
            corrected.append(token);
            continue;
        }
        QString replacement;
        for (const QString& suggestion : suggestions) {
            if (suggestion.size() != token.size())
                continue;
            int differences = 0;
            for (int index = 0; index < token.size(); ++index) {
                if (suggestion.at(index) != token.at(index))
                    ++differences;
            }
            // One substituted letter is the typical OCR slip (DOTNG -> DOING).
            if (differences == 1) {
                replacement = suggestion;
                break;
            }
        }
        corrected.append(replacement.isEmpty() ? token : replacement);
    }
    return corrected.join(QLatin1Char(' '));
}

// Removes OCR debris inside a recognized line: lone digits and one-character
// fragments that the engine picked up from artwork or page numbers.
QString cleanOcrLine(const QString& text)
{
    const QStringList tokens = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        return text;
    QStringList kept;
    for (const QString& token : tokens) {
        int letters = 0;
        int digits = 0;
        for (const QChar& character : token) {
            if (character.isLetter())
                ++letters;
            else if (character.isDigit())
                ++digits;
        }
        if (letters == 0 && digits > 0 && tokens.size() > 1)
            continue;
        if (token.size() <= 1 && !token.at(0).isLetter())
            continue;
        kept.append(token);
    }
    return kept.isEmpty() ? text : kept.join(QLatin1Char(' '));
}

bool isDialogue(const QString& text)
{
    int letters = 0;
    int digits = 0;
    for (const QChar& character : text) {
        if (character.isLetter())
            ++letters;
        else if (character.isDigit())
            ++digits;
    }
    if (letters < 2)
        return false;
    if (letters == 0 && digits > 0)
        return false;
    return true;
}

// Credits, page numbers and scanlation stamps are not dialogue: translating them
// puts stray text over the artwork and wastes a block.
bool isServiceText(const QString& text)
{
    static const QRegularExpression englishCredit(
        QStringLiteral("^\\s*(translation|translated|scanlation|scanlated|proofread\\w*|"
                       "typeset\\w*|redraw\\w*|clean(?:ed|ing)?|lettering)\\s*(by|:|—|-)\\s*\\S"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression shortCredit(
        QStringLiteral("^\\s*(tl|ts|pr|qc)\\s*[:]\\s*\\S"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression russianCredit(
        QStringLiteral("^\\s*(перевод\\w*|озвучк\\w*|тайпер\\w*|клинер\\w*|редакт\\w*|"
                       "корректор\\w*)\\s*(от|by|:|—|-)\\s*\\S"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bareCredit(
        QStringLiteral("^\\s*(translation|translated|scanlation|scanlated|proofread\\w*|"
                       "typeset\\w*|redraw\\w*|lettering)\\s*(by)?\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression pageNumber(
        QStringLiteral("^\\s*\\d{1,4}\\s*[.!]?\\s*$"));
    static const QRegularExpression link(
        QStringLiteral("https?://|www\\.|@[A-Za-z0-9_]{3,}"));
    return englishCredit.match(text).hasMatch()
        || shortCredit.match(text).hasMatch()
        || russianCredit.match(text).hasMatch()
        || bareCredit.match(text).hasMatch()
        || pageNumber.match(text).hasMatch()
        || link.match(text).hasMatch();
}

QVector<OcrLine> parseOcrLines(const QJsonObject& result, const QPoint& offset,
                               double minConfidence = 0.0)
{
    struct RawLine {
        QRect box;
        QString text;
        int height = 0;
        double confidence = 0.0;
    };

    QVector<RawLine> rawLines;
    const QJsonArray array = result.value(QStringLiteral("lines")).toArray();
    for (const QJsonValue& value : array) {
        const QJsonObject entry = value.toObject();
        const QString text = entry.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty() || !isDialogue(text))
            continue;

        const QJsonValue confidence = entry.value(QStringLiteral("confidence"));
        const double confidenceValue = confidence.isDouble() ? confidence.toDouble() : 0.0;
        if (minConfidence > 0.0 && confidence.isDouble()
            && confidence.toDouble() > 0.0 && confidence.toDouble() < minConfidence)
            continue;

        const QRect box(entry.value(QStringLiteral("left")).toInt(),
                        entry.value(QStringLiteral("top")).toInt(),
                        entry.value(QStringLiteral("width")).toInt(),
                        entry.value(QStringLiteral("height")).toInt());
        if (box.width() <= 0 || box.height() <= 0)
            continue;
        rawLines.append(RawLine{ box.translated(offset), text, box.height(), confidenceValue });
    }

    // A credit stamp is usually split into two OCR lines ("TRANSLATION BY" and
    // the nick below it), so the area around a credit line is dropped as well.
    QVector<QRect> creditZones;
    for (const RawLine& raw : std::as_const(rawLines)) {
        if (!isServiceText(raw.text))
            continue;
        creditZones.append(raw.box.adjusted(-6, -raw.height, 6,
                                            raw.height * 2));
    }

    QVector<OcrLine> lines;
    for (const RawLine& raw : std::as_const(rawLines)) {
        if (isServiceText(raw.text))
            continue;
        bool insideCredit = false;
        for (const QRect& zone : std::as_const(creditZones)) {
            if (zone.contains(raw.box.center())) {
                insideCredit = true;
                break;
            }
        }
        if (insideCredit)
            continue;

        OcrLine line;
        line.box = raw.box;
        line.text = cleanOcrLine(raw.text);
        line.height = raw.height;
        line.confidence = raw.confidence;
        lines.append(line);
    }
    return lines;
}

int medianHeight(const QVector<OcrLine>& lines)
{
    if (lines.isEmpty())
        return 0;
    QVector<int> heights;
    heights.reserve(lines.size());
    for (const OcrLine& line : lines)
        heights.append(line.height);
    std::sort(heights.begin(), heights.end());
    return heights.at(heights.size() / 2);
}

// OCR boxes hug the glyphs (a 26 px caption can report a 10 px box), while the
// erase area and the font cap need the real line pitch. Line pitch is a better
// estimate for blocks of two or more lines.
int estimateLineHeight(const QVector<OcrLine>& lines)
{
    const int glyphHeight = qMax(8, medianHeight(lines));
    if (lines.size() < 2)
        return glyphHeight;

    QVector<OcrLine> sorted = lines;
    std::sort(sorted.begin(), sorted.end(), [](const OcrLine& left, const OcrLine& right) {
        return left.box.top() < right.box.top();
    });
    QVector<int> pitches;
    pitches.reserve(sorted.size());
    for (int index = 1; index < sorted.size(); ++index)
        pitches.append(sorted.at(index).box.top() - sorted.at(index - 1).box.top());
    std::sort(pitches.begin(), pitches.end());
    const int pitch = pitches.at(pitches.size() / 2);
    const int estimated = int(std::lround(pitch * 0.75));
    return qBound(glyphHeight, estimated, glyphHeight * 3);
}

// Detector recall is borderline for very short words ("IT"): on a full page such a
// line can stay below the confidence threshold, while the same word inside a
// smaller region is recognized. This finds regions with compact dark ink on bright
// paper that the first OCR pass did not cover, so only those areas are re-read.
QVector<QRect> findMissedTextRegions(const QImage& image, const QVector<OcrLine>& lines,
                                     int maxRegions)
{
    QVector<QRect> regions;
    if (image.isNull() || maxRegions <= 0)
        return regions;

    const double scale = qMin(1.0, 1200.0 / qMax(image.width(), image.height()));
    const int width = qMax(1, int(std::lround(image.width() * scale)));
    const int height = qMax(1, int(std::lround(image.height() * scale)));
    const QImage gray =
        image.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull())
        return regions;

    const int total = width * height;
    QVector<uchar> ink(total, 0);
    for (int y = 0; y < height; ++y) {
        const uchar* row = gray.constScanLine(y);
        for (int x = 0; x < width; ++x)
            ink[y * width + x] = row[x] < 120 ? uchar(1) : uchar(0);
    }

    // Pixels covered by a recognized line are not interesting.
    for (const OcrLine& line : lines) {
        QRect box(int(line.box.x() * scale) - 3, int(line.box.y() * scale) - 3,
                  int(line.box.width() * scale) + 6, int(line.box.height() * scale) + 6);
        box = box.intersected(QRect(0, 0, width, height));
        for (int y = box.top(); y <= box.bottom(); ++y) {
            uchar* row = ink.data() + y * width;
            std::fill(row + box.left(), row + box.right() + 1, uchar(0));
        }
    }

    const auto surroundingsAreBright = [&gray, width, height](const QRect& box) {
        const QRect ring = box.adjusted(-8, -8, 8, 8).intersected(QRect(0, 0, width, height));
        if (ring.isEmpty())
            return false;
        double sum = 0.0;
        int count = 0;
        for (int x = ring.left(); x <= ring.right(); x += 2) {
            const int top = qBound(0, ring.top(), height - 1);
            const int bottom = qBound(0, ring.bottom(), height - 1);
            sum += gray.constScanLine(top)[x] + gray.constScanLine(bottom)[x];
            count += 2;
        }
        for (int y = ring.top(); y <= ring.bottom(); y += 2) {
            const int left = qBound(0, ring.left(), width - 1);
            const int right = qBound(0, ring.right(), width - 1);
            sum += gray.constScanLine(y)[left] + gray.constScanLine(y)[right];
            count += 2;
        }
        return count > 0 && sum / count > 170.0;
    };

    const int median = qMax(8, medianHeight(lines));
    const int medianScaled = qMax(4, int(std::lround(median * scale)));
    // A missed word lives right next to recognized text (same balloon/caption);
    // isolated marks far from any line are artwork or page furniture.
    const int proximity = qMax(24, int(std::lround(median * 1.8)));

    const auto nearestLineGap = [&lines, scale](const QRect& box) {
        const QRect pageBox(int(box.x() / scale), int(box.y() / scale),
                            int(box.width() / scale), int(box.height() / scale));
        int best = 100000;
        for (const OcrLine& line : lines) {
            const int dx = qMax(0, qMax(pageBox.left() - line.box.right(),
                                        line.box.left() - pageBox.right()));
            const int dy = qMax(0, qMax(pageBox.top() - line.box.bottom(),
                                        line.box.top() - pageBox.bottom()));
            best = qMin(best, qMax(dx, dy));
        }
        return best;
    };

    // True when the cluster is the next line of an existing text block: it starts
    // below a recognized line and overlaps it horizontally.
    const auto sitsUnderExistingLine = [&lines, scale, proximity](const QRect& box) {
        const QRect pageBox(int(box.x() / scale), int(box.y() / scale),
                            int(box.width() / scale), int(box.height() / scale));
        for (const OcrLine& line : lines) {
            if (pageBox.top() < line.box.bottom())
                continue;
            if (pageBox.top() - line.box.bottom() > proximity)
                continue;
            const int overlap = qMin(pageBox.right(), line.box.right())
                - qMax(pageBox.left(), line.box.left());
            if (overlap > 0)
                return true;
        }
        return false;
    };

    QVector<int> labels(total, 0);
    QVector<QPoint> stack;
    QVector<QRect> candidates;
    QVector<int> candidateAreas;
    int label = 0;
    for (int start = 0; start < total; ++start) {
        if (!ink[start] || labels[start])
            continue;
        ++label;
        stack.clear();
        stack.append(QPoint(start % width, start / width));
        labels[start] = label;
        int left = start % width;
        int right = left;
        int top = start / width;
        int bottom = top;
        int area = 0;
        while (!stack.isEmpty()) {
            const QPoint point = stack.takeLast();
            ++area;
            left = qMin(left, point.x());
            right = qMax(right, point.x());
            top = qMin(top, point.y());
            bottom = qMax(bottom, point.y());
            const int neighbours[4][2] = { { point.x() - 1, point.y() },
                                           { point.x() + 1, point.y() },
                                           { point.x(), point.y() - 1 },
                                           { point.x(), point.y() + 1 } };
            for (const auto& neighbour : neighbours) {
                if (neighbour[0] < 0 || neighbour[0] >= width || neighbour[1] < 0
                    || neighbour[1] >= height)
                    continue;
                const int index = neighbour[1] * width + neighbour[0];
                if (!ink[index] || labels[index])
                    continue;
                labels[index] = label;
                stack.append(QPoint(neighbour[0], neighbour[1]));
            }
        }
        const QRect box(left, top, right - left + 1, bottom - top + 1);
        if (area < 22 || box.height() < qMax(4, medianScaled / 2)
            || box.height() > medianScaled * 2 || box.width() < 2
            || box.width() > medianScaled * 8 || box.width() * box.height() < 30)
            continue;
        if (double(area) / double(box.width() * box.height()) < 0.15)
            continue;
        if (!surroundingsAreBright(box))
            continue;
        // Marks that touch a recognized line are its punctuation or a fragment the
        // line box already covers. A missed last word of a bubble sits below the
        // line, horizontally overlapping it (like "IT" under "YOU KEEP").
        const int gap = nearestLineGap(box);
        if (gap < qMax(2, median / 3) || gap > proximity || !sitsUnderExistingLine(box))
            continue;
        candidates.append(box);
        candidateAreas.append(area);
    }
    if (candidates.isEmpty())
        return regions;

    // Group nearby glyphs into one word/phrase cluster.
    struct Cluster {
        QRect box;
        int area = 0;
    };
    QVector<Cluster> clusters;
    for (int index = 0; index < candidates.size(); ++index) {
        const QRect dilated = candidates.at(index).adjusted(-9, -7, 9, 7);
        Cluster* target = nullptr;
        for (Cluster& cluster : clusters) {
            if (cluster.box.adjusted(-9, -7, 9, 7).intersects(dilated)) {
                target = &cluster;
                break;
            }
        }
        if (!target) {
            clusters.append(Cluster{ candidates.at(index), candidateAreas.at(index) });
            continue;
        }
        target->box = target->box.united(candidates.at(index));
        target->area += candidateAreas.at(index);
    }

    std::sort(clusters.begin(), clusters.end(), [](const Cluster& left, const Cluster& right) {
        return left.area > right.area;
    });

    const int side = 520;
    for (const Cluster& cluster : clusters) {
        if (regions.size() >= maxRegions)
            break;
        if (cluster.area < 60 || cluster.box.height() < medianScaled * 0.6
            || cluster.box.height() > medianScaled * 2.2
            || cluster.box.width() > medianScaled * 8)
            continue;
        const QRect pageBox = QRect(int(cluster.box.x() / scale), int(cluster.box.y() / scale),
                                    int(cluster.box.width() / scale),
                                    int(cluster.box.height() / scale))
                                  .adjusted(-6, -6, 6, 6);
        const QPoint center = pageBox.center();
        QRect region(center.x() - side / 2, center.y() - side / 2, side, side);
        region = region.united(pageBox.adjusted(-40, -40, 40, 40));
        region = region.intersected(image.rect());
        if (region.width() < 120 || region.height() < 120)
            continue;
        bool duplicate = false;
        for (const QRect& existing : std::as_const(regions)) {
            const QRect intersection = existing.intersected(region);
            const int smaller = qMin(existing.width() * existing.height(),
                                     region.width() * region.height());
            if (smaller > 0 && intersection.width() * intersection.height() * 2 > smaller)
                duplicate = true;
        }
        if (!duplicate)
            regions.append(region);
    }
    return regions;
}

// A recovered line is accepted only when it is a plausible text line, was not cut
// by the region border, does not duplicate a first-pass line and is not a fragment
// of one (a word box that overlaps a known line is a re-read of that line).
bool isPlausibleRecoveredLine(const OcrLine& line, const QVector<OcrLine>& existing,
                              const QRect& region, int medianLineHeight)
{
    if (line.box.isEmpty() || line.box.width() < 6)
        return false;
    int letters = 0;
    for (const QChar& character : line.text)
        letters += character.isLetter() ? 1 : 0;
    if (letters < 2)
        return false;
    if (medianLineHeight > 0
        && (line.box.height() < medianLineHeight * 0.5
            || line.box.height() > medianLineHeight * 1.9))
        return false;
    if (line.box.left() <= region.left() + 2 || line.box.top() <= region.top() + 2
        || line.box.right() >= region.right() - 2 || line.box.bottom() >= region.bottom() - 2)
        return false;

    const auto normalize = [](const QString& text) {
        QString result;
        for (const QChar& character : text)
            if (character.isLetterOrNumber())
                result.append(character.toLower());
        return result;
    };
    const QString normalized = normalize(line.text);
    int nearestGap = 100000;
    for (const OcrLine& other : existing) {
        const QRect intersection = line.box.intersected(other.box);
        if (!intersection.isEmpty()) {
            const int overlap = intersection.width() * intersection.height();
            const int smaller = qMin(line.box.width() * line.box.height(),
                                     other.box.width() * other.box.height());
            if (smaller > 0 && overlap * 100 >= smaller * 40)
                return false;
        }
        if (!normalized.isEmpty() && normalized == normalize(other.text))
            return false;
        const int dx = qMax(0, qMax(line.box.left() - other.box.right(),
                                    other.box.left() - line.box.right()));
        const int dy = qMax(0, qMax(line.box.top() - other.box.bottom(),
                                    other.box.top() - line.box.bottom()));
        nearestGap = qMin(nearestGap, qMax(dx, dy));
    }
    // A recovered word must sit next to recognized text (inside the same balloon
    // or caption). This rejects isolated marks such as logos and page furniture.
    if (nearestGap > qMax(40, medianLineHeight * 2))
        return false;
    return true;
}


// A reading is plausible when it is mostly letters and not a pile of junk. Used
// for bubble-pass blocks, where the text comes as a whole without word boxes.
bool plausibleRecoveredText(const QString& text)
{
    int letters = 0;
    int alnum = 0;
    int junk = 0;
    for (const QChar& character : text) {
        if (character.isLetter()) {
            ++letters;
            ++alnum;
        } else if (character.isDigit()) {
            ++alnum;
        } else if (!character.isSpace()
                   && !QStringLiteral(".,!?…:;'\"-—–()[]{}«»/\\|").contains(character)) {
            ++junk;
        }
    }
    if (letters < 3 || alnum == 0)
        return false;
    if (double(letters) / double(alnum) < 0.6)
        return false;
    return junk * 2 <= letters;
}

// Stricter check for bubble-pass readings: a real block has several words and a
// vowel in the longer ones. Junk from logos and scratches usually fails here.
bool strictBubbleText(const QString& text)
{
    int letters = 0;
    int alnum = 0;
    int junk = 0;
    for (const QChar& character : text) {
        if (character.isLetter()) {
            ++letters;
            ++alnum;
        } else if (character.isDigit()) {
            ++alnum;
        } else if (!character.isSpace()
                   && !QStringLiteral(".,!?…:;'\"-—–()[]{}«»/\\|").contains(character)) {
            ++junk;
        }
    }
    if (letters < 6 || junk > 1 || alnum == 0)
        return false;
    if (double(letters) / double(alnum) < 0.8)
        return false;
    const QStringList words = text.split(QRegularExpression(QStringLiteral("[\\s]+")),
                                         Qt::SkipEmptyParts);
    if (words.size() < 2)
        return false;
    int longWords = 0;
    int vowelWords = 0;
    for (const QString& word : words) {
        const QString clean = word;
        if (clean.size() < 3)
            continue;
        ++longWords;
        const QString lower = clean.toLower();
        if (lower.contains(QLatin1Char('a')) || lower.contains(QLatin1Char('e'))
            || lower.contains(QLatin1Char('i')) || lower.contains(QLatin1Char('o'))
            || lower.contains(QLatin1Char('u')) || lower.contains(QLatin1Char('y')))
            ++vowelWords;
    }
    if (longWords == 0)
        return false;
    return vowelWords * 10 >= longWords * 6;
}

// Tiles for pages whose text is too small for the detector after it normalises the
// page to 960 px. Tiles are capped so the extra cost stays bounded, and pure
// artwork tiles are skipped.
QVector<ExtraOcrJob> buildTileJobs(const QImage& image)
{
    QVector<ExtraOcrJob> jobs;
    const int width = image.width();
    const int height = image.height();
    if (qMax(width, height) <= 1400)
        return jobs;

    int cols = qMax(1, int(std::ceil(double(width) / 1100.0)));
    int rows = qMax(1, int(std::ceil(double(height) / 1100.0)));
    while (cols * rows > 6) {
        if (cols >= rows && cols > 1)
            --cols;
        else if (rows > 1)
            --rows;
        else
            break;
    }
    if (cols * rows > 6)
        return jobs;

    const int overlap = 24;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            const QRect region(qMax(0, col * width / cols - overlap),
                               qMax(0, row * height / rows - overlap),
                               qMin(width, (col + 1) * width / cols + overlap)
                                   - qMax(0, col * width / cols - overlap),
                               qMin(height, (row + 1) * height / rows + overlap)
                                   - qMax(0, row * height / rows - overlap));
            if (region.width() < 160 || region.height() < 160)
                continue;
            const QImage sample =
                image.copy(region).scaled(120, 90, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            int white = 0;
            int dark = 0;
            int count = 0;
            for (int y = 0; y < sample.height(); ++y) {
                for (int x = 0; x < sample.width(); ++x) {
                    const int luminance = qGray(sample.pixel(x, y));
                    ++count;
                    if (luminance > 235)
                        ++white;
                    else if (luminance < 90)
                        ++dark;
                }
            }
            if (count == 0)
                continue;
            const double whiteRatio = double(white) / count;
            const double darkRatio = double(dark) / count;
            if (whiteRatio < 0.10 || darkRatio < 0.004 || darkRatio > 0.5)
                continue;
            ExtraOcrJob job;
            job.stage = 0;
            job.region = region;
            jobs.append(job);
        }
    }
    return jobs;
}

// Horizontal bands for Tesseract, like the Firefox translator: big balloon
// outlines make the engine skip bold lettering, but each band is read separately.
// Only bands that look like paper with ink are worth a pass.
QVector<ExtraOcrJob> buildBandJobs(const QImage& image)
{
    QVector<ExtraOcrJob> jobs;
    const int width = image.width();
    const int height = image.height();
    const int bandHeight = qMax(160, int(width * 0.3));
    const int step = qMax(80, bandHeight / 2);
    for (int y = 0; y < height && jobs.size() < 8; y += step) {
        const int bandSize = qMin(bandHeight, height - y);
        if (bandSize < 60)
            break;
        const QImage sample = image.copy(QRect(0, y, width, bandSize))
                                  .scaled(160, 120, Qt::IgnoreAspectRatio,
                                          Qt::FastTransformation);
        int white = 0;
        int dark = 0;
        int count = 0;
        for (int sy = 0; sy < sample.height(); ++sy) {
            for (int sx = 0; sx < sample.width(); ++sx) {
                const int luminance = qGray(sample.pixel(sx, sy));
                ++count;
                if (luminance > 235)
                    ++white;
                else if (luminance < 90)
                    ++dark;
            }
        }
        if (count > 0) {
            const double whiteRatio = double(white) / count;
            const double darkRatio = double(dark) / count;
            if (whiteRatio < 0.12 || darkRatio < 0.004 || darkRatio > 0.45) {
                if (y + bandHeight >= height)
                    break;
                continue;
            }
        }
        ExtraOcrJob job;
        job.stage = 0;
        job.region = QRect(0, y, width, bandSize);
        jobs.append(job);
        if (y + bandHeight >= height)
            break;
    }
    return jobs;
}

struct LetterCluster {
    QRect box;  // page coordinates
    QImage light;
    QImage bold;
    int height = 0;
    bool singleLine = false;
};

// Bubble pass: letters are found by shape, not by the OCR layout engine. Every
// dark blob enclosed by one large light area (a balloon or the paper) is a letter
// candidate; candidates are clustered into text blocks, drawn alone on white and
// handed to OCR. This recovers bold or stylised lettering that a full-page pass
// misses, and ignores balloon outlines and artwork.
QVector<LetterCluster> findLetterClusters(const QImage& image, const QVector<OcrLine>& lines,
                                          const QVector<TextBubble>& balloons, int maxClusters,
                                          int* blobOut = nullptr, int* clusterOut = nullptr,
                                          QImage* overlayOut = nullptr)
{
    QVector<LetterCluster> result;
    if (image.isNull() || maxClusters <= 0)
        return result;

    // Small pages are read upscaled (the Firefox translator's ~1000 px rule):
    // 6 px glyphs are too small for reliable blob detection.
    double scale = 1.0;
    if (image.width() < 700)
        scale = qMin(2.0, 1000.0 / qMax(1, image.width()));
    else
        scale = qMin(1.0, 1400.0 / qMax(image.width(), image.height()));
    const int width = qMax(1, int(std::lround(image.width() * scale)));
    const int height = qMax(1, int(std::lround(image.height() * scale)));
    const QImage small = image.scaled(width, height, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_Grayscale8);
    if (small.isNull())
        return result;

    const int total = width * height;
    QVector<uchar> luminance(total);
    for (int y = 0; y < height; ++y) {
        const uchar* row = small.constScanLine(y);
        for (int x = 0; x < width; ++x)
            luminance[y * width + x] = row[x];
    }

    const int lightThreshold = 220;
    QVector<int> component(total, 0);
    QVector<int> componentArea(1, 0);
    QVector<int> stack;
    stack.reserve(4096);
    int componentCount = 0;
    for (int start = 0; start < total; ++start) {
        if (component[start] || luminance[start] < lightThreshold)
            continue;
        ++componentCount;
        int area = 0;
        component[start] = componentCount;
        stack.clear();
        stack.append(start);
        while (!stack.isEmpty()) {
            const int point = stack.takeLast();
            ++area;
            const int x = point % width;
            if (x > 0 && !component[point - 1] && luminance[point - 1] >= lightThreshold) {
                component[point - 1] = componentCount;
                stack.append(point - 1);
            }
            if (x < width - 1 && !component[point + 1] && luminance[point + 1] >= lightThreshold) {
                component[point + 1] = componentCount;
                stack.append(point + 1);
            }
            if (point >= width && !component[point - width]
                && luminance[point - width] >= lightThreshold) {
                component[point - width] = componentCount;
                stack.append(point - width);
            }
            if (point + width < total && !component[point + width]
                && luminance[point + width] >= lightThreshold) {
                component[point + width] = componentCount;
                stack.append(point + width);
            }
        }
        componentArea.append(area);
    }
    const int bigArea = qMax(400, int(1500 * scale * scale));
    const auto isBig = [&componentArea, bigArea](int id) {
        return id > 0 && componentArea[id] >= bigArea;
    };

    struct Blob {
        int area = 0;
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        int neighbour = -1;
        bool multi = false;
        bool edge = false;
    };
    QVector<int> blobId(total, 0);
    QVector<Blob> blobs(1);
    for (int start = 0; start < total; ++start) {
        if (blobId[start] || isBig(component[start]))
            continue;
        const int id = blobs.size();
        Blob blob;
        blob.x0 = width;
        blob.y0 = height;
        blob.x1 = 0;
        blob.y1 = 0;
        blobId[start] = id;
        stack.clear();
        stack.append(start);
        while (!stack.isEmpty()) {
            const int point = stack.takeLast();
            ++blob.area;
            const int x = point % width;
            const int y = (point - x) / width;
            blob.x0 = qMin(blob.x0, x);
            blob.x1 = qMax(blob.x1, x);
            blob.y0 = qMin(blob.y0, y);
            blob.y1 = qMax(blob.y1, y);
            if (x == 0 || y == 0 || x == width - 1 || y == height - 1)
                blob.edge = true;
            for (int dy = -1; dy <= 1; ++dy) {
                const int yy = y + dy;
                if (yy < 0 || yy >= height)
                    continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0)
                        continue;
                    const int xx = x + dx;
                    if (xx < 0 || xx >= width)
                        continue;
                    const int index = yy * width + xx;
                    const int value = component[index];
                    if (isBig(value)) {
                        if (dx == 0 || dy == 0) {
                            if (blob.neighbour < 0)
                                blob.neighbour = value;
                            else if (blob.neighbour != value)
                                blob.multi = true;
                        }
                    } else if (!blobId[index]) {
                        blobId[index] = id;
                        stack.append(index);
                    }
                }
            }
        }
        blobs.append(blob);
    }

    struct Letter {
        QRect box;
        int neighbour = 0;
        int blob = 0;
    };
    QVector<Letter> letters;
    for (int index = 1; index < blobs.size(); ++index) {
        const Blob& blob = blobs.at(index);
        const int blobWidth = blob.x1 - blob.x0 + 1;
        const int blobHeight = blob.y1 - blob.y0 + 1;
        if (blob.multi || blob.edge || blob.neighbour <= 0
            || blob.area < int(18 * scale * scale)
            || blobHeight < qMax(5, int(7 * scale)) || blobHeight > int(120 * scale)
            || blobWidth > 3 * blobHeight + 6 || blobHeight > 9 * blobWidth)
            continue;
        if (blob.area > total / 50)
            continue;

        letters.append(Letter{ QRect(blob.x0, blob.y0, blobWidth, blobHeight), blob.neighbour,
                               index });
        if (letters.size() > 900)
            break;
    }
    QVector<uchar> acceptedBlob(blobs.size(), 0);
    for (const Letter& letter : std::as_const(letters))
        acceptedBlob[letter.blob] = 1;
    if (blobOut)
        *blobOut = letters.size();
    if (letters.size() < 3)
        return result;

    QVector<int> parent(letters.size());
    for (int index = 0; index < parent.size(); ++index)
        parent[index] = index;
    const auto findRoot = [&parent](int value) {
        while (parent[value] != value) {
            parent[value] = parent[parent[value]];
            value = parent[value];
        }
        return value;
    };
    for (int i = 0; i < letters.size(); ++i) {
        for (int j = i + 1; j < letters.size(); ++j) {
            const Letter& a = letters.at(i);
            const Letter& b = letters.at(j);
            if (a.neighbour != b.neighbour)
                continue;

            const int maxHeight = qMax(a.box.height(), b.box.height());
            const int minHeight = qMin(a.box.height(), b.box.height());
            if (minHeight < maxHeight * 0.35)
                continue;
            const int gapX = qMax(a.box.left(), b.box.left()) - qMin(a.box.right(), b.box.right());
            const int gapY = qMax(a.box.top(), b.box.top()) - qMin(a.box.bottom(), b.box.bottom());
            const bool sameLine = gapY < 0 && gapX <= maxHeight * 1.1;
            const bool nextLine = gapY <= maxHeight * 0.9 && gapY >= -maxHeight * 0.3
                && gapX <= maxHeight * 1.6
                && qAbs(a.box.center().x() - b.box.center().x()) < maxHeight * 6;
            if (sameLine || nextLine)
                parent[findRoot(i)] = findRoot(j);
        }
    }

    QHash<int, QVector<int>> groups;
    for (int index = 0; index < letters.size(); ++index)
        groups[findRoot(index)].append(index);

    struct Candidate {
        QRect box;
        QVector<int> blobIds;
        int medianHeight = 0;
        int count = 0;
    };
    QVector<Candidate> candidates;
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        QVector<int> members = it.value();
        if (members.size() < 3)
            continue;
        QVector<int> allHeights;
        allHeights.reserve(members.size());
        for (int member : members)
            allHeights.append(letters.at(member).box.height());
        std::sort(allHeights.begin(), allHeights.end());
        const int groupHeight = qMax(4, allHeights.at(allHeights.size() / 2));

        // A light component can cover a whole page panel, so letters of different
        // balloons land in the same union. Split where the letters are far apart.
        std::sort(members.begin(), members.end(), [&letters](int left, int right) {
            const int leftTop = letters.at(left).box.top();
            const int rightTop = letters.at(right).box.top();
            if (qAbs(leftTop - rightTop) > letters.at(left).box.height() / 2)
                return leftTop < rightTop;
            return letters.at(left).box.left() < letters.at(right).box.left();
        });
        QVector<QVector<int>> parts;
        for (int member : members) {
            bool joined = false;
            if (!parts.isEmpty()) {
                QVector<int>& last = parts.last();
                const QRect& previous = letters.at(last.last()).box;
                const QRect& current = letters.at(member).box;
                const int gapX = qMax(0, qMax(previous.left() - current.right(),
                                            current.left() - previous.right()));
                const int gapY = qMax(0, qMax(previous.top() - current.bottom(),
                                            current.top() - previous.bottom()));
                if (gapX <= groupHeight * 3 && gapY <= groupHeight * 2) {
                    last.append(member);
                    joined = true;
                }
            }
            if (!joined)
                parts.append(QVector<int>{ member });
        }

        for (const QVector<int>& part : std::as_const(parts)) {
            if (part.size() < 3)
                continue;
            QVector<int> heights;
            QRect box;
            QVector<int> ids;
            for (int member : part) {
                heights.append(letters.at(member).box.height());
                box = box.isNull() ? letters.at(member).box
                                   : box.united(letters.at(member).box);
                ids.append(letters.at(member).blob);
            }
            std::sort(heights.begin(), heights.end());
            const int medianHeight = heights.at(heights.size() / 2);
            if (box.width() < medianHeight * 2)
                continue;
            int sameLine = 0;
            for (int member : part) {
                if (qAbs(letters.at(member).box.top() - box.top()) <= medianHeight / 2)
                    ++sameLine;
            }
            if (sameLine < 2)
                continue;
            if (box.height() > int(medianHeight * 6)
                || box.width() > int(medianHeight * 16))
                continue;
            int covered = 0;
            for (int member : part) {
                const QPoint center = letters.at(member).box.center();
                const QPoint pageCenter(int(center.x() / scale), int(center.y() / scale));
                for (const OcrLine& line : lines) {
                    if (line.box.adjusted(-3, -3, 3, 3).contains(pageCenter)) {
                        ++covered;
                        break;
                    }
                }
            }
            if (covered * 100 >= part.size() * 80)
                continue;
            Candidate candidate;
            candidate.box = box;
            candidate.blobIds = ids;
            candidate.medianHeight = medianHeight;
            candidate.count = part.size();
            candidates.append(candidate);
        }
    }
    if (candidates.isEmpty())
        return result;
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.count > right.count;
              });
    if (candidates.size() > maxClusters)
        candidates.resize(maxClusters);

    for (const Candidate& candidate : candidates) {
        const int pad = qMax(4, int(candidate.medianHeight * 0.5));
        const QRect crop =
            candidate.box.adjusted(-pad, -pad, pad, pad).intersected(QRect(0, 0, width, height));
        if (crop.width() < 12 || crop.height() < 8)
            continue;
        QImage lettersOnly(crop.width(), crop.height(), QImage::Format_RGB32);
        lettersOnly.fill(Qt::white);
        const QSet<int> ids(candidate.blobIds.begin(), candidate.blobIds.end());
        for (int y = crop.top(); y <= crop.bottom(); ++y) {
            QRgb* row = reinterpret_cast<QRgb*>(lettersOnly.scanLine(y - crop.top()));
            for (int x = crop.left(); x <= crop.right(); ++x) {
                if (ids.contains(blobId.at(y * width + x))) {
                    const int value = luminance.at(y * width + x);
                    row[x - crop.left()] = qRgb(value, value, value);
                }
            }
        }
        QImage bold = lettersOnly;
        for (int y = 1; y + 1 < lettersOnly.height(); ++y) {
            const QRgb* source = reinterpret_cast<const QRgb*>(lettersOnly.constScanLine(y));
            const QRgb* up = reinterpret_cast<const QRgb*>(lettersOnly.constScanLine(y - 1));
            const QRgb* down = reinterpret_cast<const QRgb*>(lettersOnly.constScanLine(y + 1));
            QRgb* destination = reinterpret_cast<QRgb*>(bold.scanLine(y));
            for (int x = 1; x + 1 < lettersOnly.width(); ++x) {
                if (source[x] == qRgb(0, 0, 0))
                    continue;
                if (source[x - 1] == qRgb(0, 0, 0) || source[x + 1] == qRgb(0, 0, 0)
                    || up[x] == qRgb(0, 0, 0) || down[x] == qRgb(0, 0, 0)
                    || up[x - 1] == qRgb(0, 0, 0) || up[x + 1] == qRgb(0, 0, 0)
                    || down[x - 1] == qRgb(0, 0, 0) || down[x + 1] == qRgb(0, 0, 0))
                    destination[x] = qRgb(0, 0, 0);
            }
        }

        const int factor = qBound(2, 900 / qMax(1, qMax(crop.width(), crop.height())), 4);
        const auto prepare = [factor](const QImage& source) {
            QImage scaled = source.scaled(source.width() * factor, source.height() * factor,
                                          Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            QImage bordered(scaled.width() + 48, scaled.height() + 48, QImage::Format_RGB32);
            bordered.fill(Qt::white);
            QPainter painter(&bordered);
            painter.drawImage(24, 24, scaled);
            return bordered;
        };

        LetterCluster cluster;
        cluster.box = QRect(int(crop.x() / scale), int(crop.y() / scale),
                            int(crop.width() / scale), int(crop.height() / scale));
        cluster.light = prepare(lettersOnly);
        cluster.bold = prepare(bold);
        cluster.height = qMax(8, int(candidate.medianHeight / scale));
        cluster.singleLine = candidate.box.height() <= int(candidate.medianHeight * 1.8);
        result.append(cluster);
    }
    if (clusterOut)
        *clusterOut = result.size();
    if (overlayOut) {
        QImage overlay = small.convertToFormat(QImage::Format_RGB32);
        QPainter painter(&overlay);
        painter.setPen(QPen(QColor(220, 0, 0), 1));
        int rejected = 0;
        for (int index = 1; index < blobs.size() && rejected < 900; ++index) {
            if (acceptedBlob.at(index))
                continue;
            const Blob& blob = blobs.at(index);
            const int blobWidth = blob.x1 - blob.x0 + 1;
            const int blobHeight = blob.y1 - blob.y0 + 1;
            if (blob.area < 8 || blobHeight < 4)
                continue;
            painter.drawRect(QRect(blob.x0, blob.y0, blobWidth, blobHeight));
            ++rejected;
        }
        painter.setPen(QPen(QColor(0, 160, 0), 1));
        for (const Letter& letter : std::as_const(letters))
            painter.drawRect(letter.box);
        painter.setPen(QPen(QColor(0, 80, 255), 2));
        for (const LetterCluster& cluster : std::as_const(result)) {
            painter.drawRect(QRect(int(cluster.box.x() * scale), int(cluster.box.y() * scale),
                                   int(cluster.box.width() * scale),
                                   int(cluster.box.height() * scale)));
        }
        painter.end();
        *overlayOut = overlay;
    }
    return result;
}

// Short lines ("IT", "NO!") are fine; a long token that the dictionary does not
// know is almost always OCR noise from credits or logos.
bool dictionaryLooksLikeText(const QString& text, SpellService& spell, const QString& language)
{
    if (!spell.provider())
        return true;
    int letters = 0;
    int known = 0;
    int longWords = 0;
    const QStringList words = text.split(QRegularExpression(QStringLiteral("[\\s]+")),
                                         Qt::SkipEmptyParts);
    for (const QString& word : words) {
        QString clean;
        for (const QChar& character : word)
            if (character.isLetter())
                clean.append(character);
        letters += clean.size();
        if (clean.size() < 3)
            continue;
        ++longWords;
        if (spell.checkWord(clean, language))
            ++known;
    }
    if (letters <= 6)
        return true;
    return longWords > 0 && known > 0;
}

// A line recovered from a tile: same plausibility rules as the first pass, but it
// may sit anywhere (a tile can contain a whole new bubble).
bool isPlausibleExtraLine(const OcrLine& line, const QVector<OcrLine>& existing,
                          const QRect& region, int medianLineHeight)
{
    if (line.box.isEmpty() || line.box.width() < 6)
        return false;
    int letters = 0;
    for (const QChar& character : line.text)
        letters += character.isLetter() ? 1 : 0;
    if (letters < 2)
        return false;
    if (medianLineHeight > 0
        && (line.box.height() < medianLineHeight * 0.4
            || line.box.height() > medianLineHeight * 2.2))
        return false;
    if (line.box.left() <= region.left() + 2 || line.box.top() <= region.top() + 2
        || line.box.right() >= region.right() - 2 || line.box.bottom() >= region.bottom() - 2)
        return false;

    // OCR garbage from credits and logos: tokens mixing digits with letters and
    // wildly mixed case ("TizANSLATiIONIBY 3zOSTYSCAz").
    const QStringList tokens = line.text.split(QRegularExpression(QStringLiteral("[\\s]+")),
                                               Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        int tokenLetters = 0;
        int tokenChanges = 0;
        bool tokenDigit = false;
        for (int index = 0; index < token.size(); ++index) {
            const QChar current = token.at(index);
            if (current.isDigit()) {
                tokenDigit = true;
                continue;
            }
            if (!current.isLetter())
                continue;
            ++tokenLetters;
            if (index > 0) {
                const QChar previous = token.at(index - 1);
                if (previous.isLetter() && previous.isUpper() != current.isUpper())
                    ++tokenChanges;
            }
        }
        if (tokenDigit && tokenLetters > 0)
            return false;
        if (tokenLetters > 4 && tokenChanges > 2)
            return false;
    }

    const auto normalize = [](const QString& text) {
        QString result;
        for (const QChar& character : text)
            if (character.isLetterOrNumber())
                result.append(character.toLower());
        return result;
    };
    const QString normalized = normalize(line.text);
    for (const OcrLine& other : existing) {
        const QRect intersection = line.box.intersected(other.box);
        if (!intersection.isEmpty()) {
            const int overlap = intersection.width() * intersection.height();
            const int smaller = qMin(line.box.width() * line.box.height(),
                                     other.box.width() * other.box.height());
            if (smaller > 0 && overlap * 100 >= smaller * 40)
                return false;
        }
        if (!normalized.isEmpty() && normalized == normalize(other.text))
            return false;
    }
    return true;
}

struct BlockBuilder {
    QRect box;
    QVector<OcrLine> lines;
    TextBubble bubble;
    bool hasBubble = false;
    int maxLineHeight = 0;
};

// Two touching balloons can share one grown mask, which merges their lines into a
// single block (and often fails to fit). Split a block where the vertical gap
// between consecutive lines is much larger than the normal line pitch.
QVector<BlockBuilder> splitBlockByGap(const BlockBuilder& block)
{
    if (block.lines.size() < 2)
        return { block };
    QVector<OcrLine> sorted = block.lines;
    std::sort(sorted.begin(), sorted.end(), [](const OcrLine& left, const OcrLine& right) {
        if (left.box.top() != right.box.top())
            return left.box.top() < right.box.top();
        return left.box.left() < right.box.left();
    });
    QVector<int> pitches;
    for (int index = 1; index < sorted.size(); ++index)
        pitches.append(sorted.at(index).box.top() - sorted.at(index - 1).box.top());
    if (pitches.isEmpty())
        return { block };
    std::sort(pitches.begin(), pitches.end());
    const int medianPitch = pitches.at(pitches.size() / 2);
    const int medianLineHeight = qMax(6, medianHeight(sorted));
    const int threshold = medianLineHeight * 3;

    QVector<BlockBuilder> parts;
    BlockBuilder current;
    for (int index = 0; index < sorted.size(); ++index) {
        const int pitch = index > 0 ? sorted.at(index).box.top() - sorted.at(index - 1).box.top() : 0;
        if (index > 0 && pitch > threshold && !current.lines.isEmpty()) {
            parts.append(current);
            current = BlockBuilder();
        }
        current.lines.append(sorted.at(index));
        current.maxLineHeight = qMax(current.maxLineHeight, sorted.at(index).height);
    }
    if (!current.lines.isEmpty())
        parts.append(current);
    return parts.size() < 2 ? QVector<BlockBuilder>{ block } : parts;
}


// Groups free (non-bubble) lines into paragraphs by proximity, one column at a
// time so two neighbouring captions do not become one block. OCR boxes are tight
// around glyphs, so the line pitch (top to top) is the stable measure, not the
// gap between boxes.
QVector<BlockBuilder> groupLines(const QVector<OcrLine>& lines, int medianLineHeight)
{
    QVector<OcrLine> sorted = lines;
    std::sort(sorted.begin(), sorted.end(), [](const OcrLine& left, const OcrLine& right) {
        if (left.box.top() != right.box.top())
            return left.box.top() < right.box.top();
        return left.box.left() < right.box.left();
    });

    const int pitchLimit = qMax(16, int(qMax(1, medianLineHeight) * 3.6));
    QVector<BlockBuilder> blocks;
    for (const OcrLine& line : sorted) {
        BlockBuilder* target = nullptr;
        for (int index = int(blocks.size()) - 1; index >= 0 && !target; --index) {
            BlockBuilder& candidate = blocks[index];
            const QRect previous = candidate.box;
            const int overlap = qMin(previous.right(), line.box.right())
                - qMax(previous.left(), line.box.left());
            const int narrowest = qMin(previous.width(), line.box.width());
            if (overlap * 2 <= narrowest)
                continue;
            const int lastTop = candidate.lines.last().box.top();
            const int pitch = line.box.top() - lastTop;
            if (pitch > 0 && pitch <= pitchLimit)
                target = &candidate;
        }
        if (!target) {
            BlockBuilder block;
            block.box = line.box;
            block.lines.append(line);
            block.maxLineHeight = line.height;
            blocks.append(block);
            continue;
        }
        target->box = target->box.united(line.box);
        target->lines.append(line);
        target->maxLineHeight = qMax(target->maxLineHeight, line.height);
    }
    return blocks;
}

// One bubble = one string: line breaks are layout artefacts, and a word broken
// with a hyphen on the previous line is glued back together ("some-" + "thing").
QString joinLineTexts(const QVector<OcrLine>& lines)
{
    QString text;
    for (const OcrLine& line : lines) {
        const QString piece = line.text.trimmed();
        if (piece.isEmpty())
            continue;
        if (text.isEmpty()) {
            text = piece;
            continue;
        }
        const QChar last = text.at(text.size() - 1);
        const bool hyphenated = last == QLatin1Char('-') || last == QChar(0x2010)
            || last == QChar(0x00AD);
        if (hyphenated && piece.at(0).isLower()) {
            text.chop(1);
            text += piece;
            continue;
        }
        text += QLatin1Char(' ');
        text += piece;
    }
    return text.simplified();
}

QRect linesUnion(const QVector<OcrLine>& lines)
{
    QRect box;
    for (const OcrLine& line : lines)
        box = box.isNull() ? line.box : box.united(line.box);
    return box;
}

// A large rectangular caption can hold several paragraphs. Splitting it keeps the
// paragraphs apart and gives each one its own translation, instead of merging
// everything into one long string that has to be shrunk to fit.
QVector<BlockBuilder> splitParagraphs(const BlockBuilder& block, int medianLineHeight)
{
    if (block.hasBubble || block.lines.size() < 4)
        return { block };

    QVector<OcrLine> sorted = block.lines;
    std::sort(sorted.begin(), sorted.end(), [](const OcrLine& left, const OcrLine& right) {
        if (left.box.top() != right.box.top())
            return left.box.top() < right.box.top();
        return left.box.left() < right.box.left();
    });

    const int pitchLimit = qMax(16, int(qMax(1, medianLineHeight) * 3.6));
    const int breakPitch = pitchLimit * 2;

    QVector<BlockBuilder> parts;
    BlockBuilder current;
    for (int index = 0; index < sorted.size(); ++index) {
        const int pitch = index > 0 ? sorted.at(index).box.top() - sorted.at(index - 1).box.top() : 0;
        if (index > 0 && pitch > breakPitch && !current.lines.isEmpty()) {
            parts.append(current);
            current = BlockBuilder();
        }
        current.lines.append(sorted.at(index));
        current.maxLineHeight = qMax(current.maxLineHeight, sorted.at(index).height);
    }
    if (!current.lines.isEmpty())
        parts.append(current);
    if (parts.size() < 2)
        return { block };

    for (BlockBuilder& part : parts)
        part.box = linesUnion(part.lines);
    return parts;
}

bool maskContains(const TextBubble& bubble, const QPoint& point)
{
    if (bubble.fillMask.isNull() || !bubble.box.contains(point))
        return false;
    const QPoint local = point - bubble.box.topLeft();
    return qAlpha(bubble.fillMask.pixel(local)) >= 128;
}

// A grown bubble is only used when its mask actually covers the recognized
// lines. A partial mask would erase only half of the original lettering and draw
// the translation over the rest, so most probe points of every line must land
// inside the shape.
bool maskCoversLines(const TextBubble& bubble, const QVector<OcrLine>& lines)
{
    if (bubble.fillMask.isNull() || lines.isEmpty())
        return false;
    for (const OcrLine& line : lines) {
        if (!bubble.box.contains(line.box))
            return false;
        int inside = 0;
        int probes = 0;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                const QPoint probe(line.box.left() + line.box.width() * column / 2,
                                   line.box.top() + line.box.height() * row / 2);
                ++probes;
                if (maskContains(bubble, probe))
                    ++inside;
            }
        }
        if (inside * 3 < probes * 2)  // less than two thirds covered
            return false;
    }
    return true;
}

// Length budget for LLM connectors: how many characters fit the block at a
// comfortable size. Without it the model happily returns a sentence twice as long
// as the balloon and the typesetter has to shrink it to the minimum.
QString budgetInstruction(const OverlayLine& block)
{
    int widest = 0;
    qint64 area = 0;
    for (const RowSpan& span : block.rows) {
        if (!span.valid())
            continue;
        widest = qMax(widest, span.width());
        area += span.width();
    }
    if (widest <= 0 || area <= 0) {
        widest = block.box.width();
        area = qint64(block.box.width()) * block.box.height();
    }
    if (widest <= 0 || area <= 0)
        return QString();

    const int targetPx = qMax(9, int(std::lround(qMax(8, block.sourceLineHeight) * 0.95)));
    const int maxChars = qBound(8, int(area / (0.62 * targetPx * targetPx)), 600);
    if (maxChars >= 240)
        return QString();  // the balloon is roomy; no need to constrain the model
    const int perLine = qMax(6, int(widest / (0.5 * targetPx)));
    const int lines = qBound(1, int(area / qMax(1, widest)) / qMax(1, int(targetPx * 1.15)), 8);
    return QStringLiteral("Keep the translation concise so it fits a speech balloon: "
                          "at most %1 characters, about %2 characters per line, "
                          "no more than %3 lines. If the source is already short, "
                          "ignore this limit. Do not add explanations or quotes.")
        .arg(maxChars)
        .arg(perLine)
        .arg(lines);
}

OverlayLine makeRectangularBlock(const QRect& unionBox, int lineHeight){
    QRect box = unionBox;
    // OCR boxes are tight, so the padding has to cover ascenders, descenders and
    // the anti-aliased edges of the original lettering; otherwise slivers of it
    // stay visible above and below the translation.
    const int horizontal = qMax(3, lineHeight / 3);
    const int vertical = qMax(3, int(lineHeight * 0.6));
    box = box.adjusted(-horizontal, -vertical, horizontal, vertical);
    const int minHeight = qMax(18, int(lineHeight * 1.9));
    if (box.height() < minHeight) {
        const int missing = minHeight - box.height();
        box.adjust(0, -(missing / 2), 0, missing - missing / 2);
    }

    OverlayLine block;
    block.box = box;
    block.sourceLineHeight = lineHeight;
    return block;
}

}

ImageTranslationPipeline::ImageTranslationPipeline(AppContext* context, QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    m_impl->context = context;
}

ImageTranslationPipeline::~ImageTranslationPipeline() = default;

void ImageTranslationPipeline::cancel()
{
    m_impl->cancelled = true;
}

void ImageTranslationPipeline::start(const QImage& image, const QString& sourceCode,
                                     const QString& targetCode, const QString& providerId)
{
    if (!m_impl->context || !m_impl->context->translation || image.isNull()) {
        emit failed(QStringLiteral("Нет изображения для перевода."));
        return;
    }

    m_impl->image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_impl->sourceCode = sourceCode.isEmpty() ? QStringLiteral("auto") : sourceCode;
    m_impl->targetCode = targetCode;

    // Callers may leave the service empty (CLI, batch without a selection); fall
    // back to the active service so the pipeline works on its own.
    QString provider = providerId;
    if (provider.isEmpty() && m_impl->context->plugins) {
        const QStringList active = m_impl->context->plugins->activeTranslationIds();
        if (!active.isEmpty()) {
            provider = active.first();
        } else {
            const QList<PluginManifest> manifests = m_impl->context->plugins->manifests();
            for (const PluginManifest& manifest : manifests) {
                if (manifest.valid && manifest.capabilities.contains(QStringLiteral("translate"))) {
                    provider = manifest.id;
                    break;
                }
            }
        }
    }
    m_impl->providerId = provider;
    m_impl->blocks.clear();
    m_impl->blockSources.clear();
    m_impl->lineDump.clear();
    m_impl->ocrLines.clear();
    m_impl->recoveryRegions.clear();
    m_impl->recoveryDump.clear();
    m_impl->extraJobs.clear();
    m_impl->extraIndex = 0;
    m_impl->extraStage = 0;
    m_impl->recoveredLines = 0;
    m_impl->recoveryIndex = 0;
    m_impl->bubbles.clear();
    m_impl->translatedText.clear();
    m_impl->sourceText.clear();
    m_impl->index = 0;
    m_impl->bubbleIndex = 0;
    m_impl->cancelled = false;
    m_impl->failed = false;

    m_impl->llmProvider = false;
    if (m_impl->context->plugins && !provider.isEmpty()) {
        if (IPluginProvider* plugin = m_impl->context->plugins->provider(provider))
            m_impl->llmProvider = plugin->manifest().type == QStringLiteral("llm");
    }
    if (m_impl->context->settings) {
        SettingsManager* settings = m_impl->context->settings;
        const QJsonValue confidence =
            settings->value(QStringLiteral("Image.MinOcrConfidence"));
        if (confidence.isDouble())
            m_impl->minOcrConfidence = qBound(0.0, confidence.toDouble(), 1.0);
        m_impl->llmContext = settings->boolValue(QStringLiteral("Image.LlmContext"), true);
        m_impl->lengthBudget = settings->boolValue(QStringLiteral("Image.LengthBudget"), true);
        m_impl->splitParagraphs =
            settings->boolValue(QStringLiteral("Image.SplitParagraphs"), true);
        m_impl->recoverMissedText =
            settings->boolValue(QStringLiteral("Image.RecoverMissedText"), true);
        m_impl->tileLargePages =
            settings->boolValue(QStringLiteral("Image.TileLargePages"), true);
        m_impl->bubblePass = settings->boolValue(QStringLiteral("Image.BubblePass"), false);
        m_impl->balloonSweep =
            settings->boolValue(QStringLiteral("Image.BalloonSweep"), false);
    }

    QString language;
    if (m_impl->context->settings) {
        language = m_impl->context->settings->stringValue(QStringLiteral("Ocr.OcrLanguage"),
                                                          QStringLiteral("eng+rus"));
    }
    // The OCR language follows the source language, but only when the matching
    // model is actually installed; otherwise the configured languages stay.
    if (!m_impl->sourceCode.isEmpty() && m_impl->sourceCode != QStringLiteral("auto")
        && m_impl->context->paths) {
        static const QHash<QString, QString> languages = {
            {QStringLiteral("ja"), QStringLiteral("jpn_vert")},
            {QStringLiteral("zh"), QStringLiteral("chi_sim")},
            {QStringLiteral("ko"), QStringLiteral("kor")},
            {QStringLiteral("ru"), QStringLiteral("rus")},
            {QStringLiteral("en"), QStringLiteral("eng")},
            {QStringLiteral("de"), QStringLiteral("deu")},
            {QStringLiteral("fr"), QStringLiteral("fra")},
            {QStringLiteral("es"), QStringLiteral("spa")}};
        const QString tessdata = QDir(m_impl->context->paths->shippedDir())
                                     .filePath(QStringLiteral("services/ocr/tesseract/tessdata"));
        const auto installed = [&tessdata](const QString& code) {
            return !code.isEmpty()
                && QFile::exists(QDir(tessdata).filePath(code + QStringLiteral(".traineddata")));
        };

        QString mapped = languages.value(m_impl->sourceCode);
        if (mapped == QStringLiteral("jpn_vert") && !installed(mapped))
            mapped = QStringLiteral("jpn");
        if (installed(mapped))
            language = mapped;
    }
    if (language.isEmpty())
        language = QStringLiteral("eng");
    m_impl->ocrLanguage = language;

    bool detectsLines = false;
    QString ocrId;
    if (m_impl->context->plugins && m_impl->context->translation) {
        if (m_impl->context->settings)
            ocrId = m_impl->context->settings->stringValue(QStringLiteral("Ocr.ProviderImages"));
        if (ocrId.isEmpty())
            ocrId = m_impl->context->translation->defaultOcrProviderId();
        if (IPluginProvider* provider = m_impl->context->plugins->provider(ocrId))
            detectsLines = provider->manifest().nativeProvider == QStringLiteral("paddleocr");
    }

    // Three image modes: the selected provider, the plugin-style Tesseract path,
    // or a hybrid that detects with Paddle and re-reads with Tesseract.
    m_impl->hybridMode = ocrId == QStringLiteral("hybrid");
    if (m_impl->hybridMode) {
        m_impl->ocrProviderOverride = QStringLiteral("paddleocr");
        detectsLines = true;  // hybrid starts from a full-page Paddle pass
    }
    if (ocrId == QStringLiteral("tesseract_local")) {
        m_impl->tesseractPath = true;
        startTesseractPass();
        return;
    }

    // Engines with their own line detection read the whole page: nothing is cut at
    // crop edges, and every line is placed into the bubble it really belongs to.
    // Engines without it are fed one bubble at a time.
    if (!detectsLines) {
        m_impl->bubbles = detectTextBubbles(m_impl->image);
        if (!m_impl->bubbles.isEmpty()) {
            recognizeNextBubble();
            return;
        }
    }

    QPointer<ImageTranslationPipeline> self(this);
    m_impl->context->translation->ocrRequestAsync(
        m_impl->image, m_impl->ocrProviderOverride, language,
        [self](const QJsonObject& result) {
            if (!self || self->m_impl->cancelled)
                return;

            const QString error = result.value(QStringLiteral("error")).toString();
            if (!error.isEmpty()) {
                self->m_impl->failed = true;
                emit self->failed(error);
                return;
            }

            const QVector<OcrLine> lines =
                parseOcrLines(result, QPoint(0, 0), self->m_impl->minOcrConfidence);
            self->m_impl->ocrLines = lines;
            for (const OcrLine& line : lines) {
                self->m_impl->lineDump.append(QStringLiteral("%1,%2 %3x%4 %5")
                                                  .arg(line.box.x()).arg(line.box.y())
                                                  .arg(line.box.width()).arg(line.box.height())
                                                  .arg(line.text.left(24)));
            }
            if (lines.isEmpty()) {
                // A dense text column can be invisible to the detector; the
                // plugin-style band pass still reads it.
                const bool paddle = self->m_impl->ocrProviderOverride.isEmpty()
                    || self->m_impl->ocrProviderOverride.compare(
                           QStringLiteral("paddleocr"), Qt::CaseInsensitive) == 0;
                if (!paddle) {
                    self->m_impl->failed = true;
                    emit self->failed(QStringLiteral("На изображении не найден текст."));
                    return;
                }
                self->startDenseColumnPass();
                return;
            } else if (lines.size() <= 1) {
                // Nearly empty detection on a busy page: read the text column
                // itself instead of trusting the whole-page detector.
                self->startDenseColumnPass();
                return;
            }

            BubbleFinder finder(self->m_impl->image);
            self->m_impl->paperPage = finder.paperPage();
            self->m_impl->extraStage = 0;
            self->m_impl->recoveredLines = 0;
            self->m_impl->tileJobs = 0;
            self->m_impl->inkJobs = 0;
            self->m_impl->bubbleJobs = 0;
            self->runExtraStage(0);
        },
        false, QStringLiteral("image"));
}

void ImageTranslationPipeline::runExtraStage(int stage)
{
    if (m_impl->cancelled || m_impl->failed)
        return;
    m_impl->extraStage = stage;
    m_impl->extraJobs.clear();
    m_impl->extraIndex = 0;
    m_impl->stageStartMs = QDateTime::currentMSecsSinceEpoch();

    if (stage == 0) {
        // Large pages: the detector sees the page at 960 px, so a 3000 px scan
        // reaches it at 0.24x. Reading it tile by tile (at most six tiles) keeps
        // each tile near the detector's working scale.
        if (m_impl->tesseractPath)
            m_impl->extraJobs = buildBandJobs(m_impl->image);
        else if (m_impl->tileLargePages)
            m_impl->extraJobs = buildTileJobs(m_impl->image);
        m_impl->tileJobs = m_impl->extraJobs.size();
        if (!m_impl->extraJobs.isEmpty()) {
            processNextExtraJob();
            return;
        }
        runExtraStage(1);
        return;
    }

    if (stage == 1) {
        // Short words (a two-letter "IT") can stay below the detector threshold on
        // a full page: re-read only spots with uncovered text-like ink.
        if (m_impl->recoverMissedText && m_impl->paperPage) {
            const QVector<QRect> regions = findMissedTextRegions(
                m_impl->image, m_impl->ocrLines,
                (m_impl->hybridMode || m_impl->tesseractPath)
                    ? qMax(3, m_impl->maxRecoveryRegions)
                    : m_impl->maxRecoveryRegions);
            for (const QRect& region : regions) {
                ExtraOcrJob job;
                job.stage = 1;
                job.region = region;
                if (m_impl->hybridMode || m_impl->tesseractPath) {
                    const QImage crop = m_impl->image.copy(region);
                    const QImage gray = stretchedGray(grayCopy(crop));
                    job.variants.append(gray);
                    job.variants.append(binarized(gray, 150));
                }
                m_impl->extraJobs.append(job);
            }
            m_impl->recoveryDump = regions;
            m_impl->inkJobs = regions.size();
        }
        if (!m_impl->extraJobs.isEmpty()) {
            processNextExtraJob();
            return;
        }
        runExtraStage(2);
        return;
    }

    if (stage == 2) {
        // Bubble pass: recover stylised lettering by shape (letters enclosed by a
        // light area), then read each block in isolation.
        if ((m_impl->bubblePass || m_impl->tesseractPath || m_impl->hybridMode)
            && m_impl->paperPage) {
            if (!m_impl->balloonsReady) {
                m_impl->balloons = detectTextBubbles(m_impl->image);
                m_impl->balloonsReady = true;
            }
            QImage letterSource = m_impl->image;
            double letterScaleBack = 1.0;
            QVector<OcrLine> letterLines = m_impl->ocrLines;
            if (m_impl->tesseractPath && !m_impl->tesseractImage.isNull()) {
                letterSource = m_impl->tesseractImage;
                letterScaleBack = m_impl->tesseractScale;
                for (OcrLine& line : letterLines) {
                    line.box = QRect(int(std::lround(line.box.x() * letterScaleBack)),
                                     int(std::lround(line.box.y() * letterScaleBack)),
                                     int(std::lround(line.box.width() * letterScaleBack)),
                                     int(std::lround(line.box.height() * letterScaleBack)));
                }
            }
            QImage letterOverlay;
            QVector<LetterCluster> clusters = findLetterClusters(
                letterSource, letterLines, m_impl->balloons, 8,
                &m_impl->letterBlobs, &m_impl->letterClusters, &letterOverlay);
            if (!letterOverlay.isNull() && !m_layoutDumpPath.isEmpty())
                letterOverlay.save(m_layoutDumpPath + QStringLiteral(".blobs.png"));
            if (letterScaleBack > 0.0 && !qFuzzyCompare(letterScaleBack, 1.0)) {
                for (LetterCluster& cluster : clusters)
                    cluster.box = QRect(int(std::lround(cluster.box.x() / letterScaleBack)),
                                        int(std::lround(cluster.box.y() / letterScaleBack)),
                                        int(std::lround(cluster.box.width() / letterScaleBack)),
                                        int(std::lround(cluster.box.height() / letterScaleBack)));
            }
            for (const LetterCluster& cluster : clusters) {
                ExtraOcrJob job;
                job.stage = 2;
                job.region = cluster.box;
                job.textHeight = cluster.height;
                job.psm = cluster.singleLine ? 7 : 6;
                job.variants.append(cluster.light);
                job.variants.append(cluster.bold);
                if (!m_layoutDumpPath.isEmpty()) {
                    QDir clusterDir(m_layoutDumpPath + QStringLiteral(".clusters"));
                    clusterDir.mkpath(QStringLiteral("."));
                    cluster.light.save(clusterDir.filePath(
                        QStringLiteral("cluster_%1_%2_%3.png")
                            .arg(m_impl->extraJobs.size())
                            .arg(cluster.box.x())
                            .arg(cluster.box.y())));
                }
                m_impl->extraJobs.append(job);
            }
            m_impl->bubbleJobs = clusters.size();
        }
        if (!m_impl->extraJobs.isEmpty()) {
            processNextExtraJob();
            return;
        }
        if (m_impl->tesseractPath)
            buildTesseractBlocks();
        else
            buildBlocks();
        return;
    }

    if (stage == 3) {
        // Final polish: every block that looks weak (few dictionary words) is
        // re-read in several binarisations and the best reading wins.
        SpellService spell(m_impl->context->plugins, m_impl->context->settings);
        const QString language = m_impl->sourceCode.isEmpty()
                || m_impl->sourceCode == QStringLiteral("auto")
            ? QStringLiteral("en")
            : m_impl->sourceCode;
        for (int index = 0; index < m_impl->blockSources.size()
                            && m_impl->extraJobs.size() < 6;
             ++index) {
            const QString& text = m_impl->blockSources.at(index);
            int letters = 0;
            int known = 0;
            const QStringList words = text.split(QRegularExpression(QStringLiteral("[\\s]+")),
                                                 Qt::SkipEmptyParts);
            for (const QString& word : words) {
                QString clean;
                for (const QChar& character : word)
                    if (character.isLetter())
                        clean.append(character);
                letters += clean.size();
                if (clean.size() >= 3 && spell.provider()
                    && spell.checkWord(clean, language))
                    ++known;
            }
            if (letters < 4 || known >= 2)
                continue;
            const QRect box = m_impl->blocks.at(index)
                                  .box.adjusted(-6, -6, 6, 6)
                                  .intersected(m_impl->image.rect());
            if (box.width() < 16 || box.height() < 12)
                continue;
            const QImage crop = m_impl->image.copy(box);
            ExtraOcrJob job;
            job.stage = 3;
            job.region = box;
            job.blockIndex = index;
            job.baseScore = readingScore(text, 0.0, spell, language);
            job.baseLetters = letters;
            job.baseKnown = known;
            job.variants = buildReadingVariants(crop);
            m_impl->extraJobs.append(job);
        }
        m_impl->rereadJobs = m_impl->extraJobs.size();
        if (!m_impl->extraJobs.isEmpty()) {
            processNextExtraJob();
            return;
        }
        runExtraStage(4);
        return;
    }

    if (stage == 4) {
        // Balloon sweep: a balloon whose text the OCR missed entirely has no lines
        // and therefore no block. Find such balloons and re-read each of them.
        if (m_impl->balloonSweep && m_impl->paperPage) {
            if (!m_impl->balloonsReady) {
                m_impl->balloons = detectTextBubbles(m_impl->image);
                m_impl->balloonsReady = true;
            }
            const QVector<TextBubble> bubbles = m_impl->balloons;
            m_impl->sweepBubbles = bubbles.size();
            for (const TextBubble& bubble : bubbles) {
                if (!bubble.valid())
                    continue;
                bool covered = false;
                for (const OverlayLine& block : std::as_const(m_impl->blocks)) {
                    const QRect intersection = block.box.intersected(bubble.box);
                    if (!intersection.isEmpty()
                        && intersection.width() * intersection.height() * 100
                            > bubble.box.width() * bubble.box.height() * 25) {
                        covered = true;
                        break;
                    }
                }
                if (covered)
                    continue;
                const QRect cropBox = bubble.box.adjusted(-4, -4, 4, 4)
                                          .intersected(m_impl->image.rect());
                if (cropBox.width() < 24 || cropBox.height() < 20)
                    continue;
                // Render only the balloon interior on white: the dark outline and
                // any artwork outside the mask confuse Tesseract (the plugin's
                // bubble pass does exactly this).
                QImage inside(bubble.box.size(), QImage::Format_RGB32);
                inside.fill(Qt::white);
                const QImage mask = bubble.fillMask;
                for (int y = 0; y < bubble.box.height(); ++y) {
                    const int pageY = bubble.box.top() + y;
                    if (pageY < 0 || pageY >= m_impl->image.height())
                        continue;
                    const QRgb* source =
                        reinterpret_cast<const QRgb*>(m_impl->image.constScanLine(pageY));
                    QRgb* target = reinterpret_cast<QRgb*>(inside.scanLine(y));
                    const QRgb* maskRow = (mask.height() > y)
                        ? reinterpret_cast<const QRgb*>(mask.constScanLine(y))
                        : nullptr;
                    for (int x = 0; x < bubble.box.width(); ++x) {
                        const int pageX = bubble.box.left() + x;
                        if (pageX < 0 || pageX >= m_impl->image.width())
                            continue;
                        if (maskRow && qAlpha(maskRow[qMin(x, mask.width() - 1)]) < 128)
                            continue;
                        target[x] = source[pageX];
                    }
                }
                ExtraOcrJob job;
                job.stage = 4;
                job.region = cropBox;
                job.bubble = bubble;
                job.hasBubble = true;
                job.variants = buildReadingVariants(inside);
                m_impl->extraJobs.append(job);
                if (m_impl->extraJobs.size() >= 8)
                    break;
            }
            m_impl->sweepJobs = m_impl->extraJobs.size();
        }
        if (!m_impl->extraJobs.isEmpty()) {
            processNextExtraJob();
            return;
        }
        finishBlocks();
        return;
    }

    finishBlocks();
}

void ImageTranslationPipeline::mergeParagraphBlocks()
{
    // Dense text columns (a wall of prose next to artwork) come out of the band
    // pass as one block per visual line. Join stacked, equally wide lines into a
    // paragraph so the translation covers the column instead of sprinkling over
    // the source text.
    if (!m_impl->denseTextFallback || m_impl->blocks.size() < 2)
        return;
    const QVector<OverlayLine> blocks = m_impl->blocks;
    const QStringList sources = m_impl->blockSources;
    QVector<int> order(blocks.size());
    for (int index = 0; index < order.size(); ++index)
        order[index] = index;
    std::sort(order.begin(), order.end(), [&blocks](int left, int right) {
        if (blocks.at(left).box.top() != blocks.at(right).box.top())
            return blocks.at(left).box.top() < blocks.at(right).box.top();
        return blocks.at(left).box.left() < blocks.at(right).box.left();
    });

    QVector<OverlayLine> merged;
    QStringList mergedSources;
    for (int index : std::as_const(order)) {
        OverlayLine block = blocks.at(index);
        QString source = sources.value(index);
        if (!merged.isEmpty()) {
            OverlayLine& last = merged.last();
            const QRect a = last.box;
            const QRect b = block.box;
            const int overlapX = qMax(0, qMin(a.right(), b.right()) - qMax(a.left(), b.left()) + 1);
            const int minWidth = qMin(a.width(), b.width());
            const int lineHeight = qMax(6, last.sourceLineHeight);
            const int gapY = b.top() - a.bottom() - 1;
            const bool sameColumn = overlapX >= minWidth * 6 / 10
                || qAbs(a.center().x() - b.center().x()) <= m_impl->image.width() / 3;
            const bool close = gapY >= -lineHeight * 2 && gapY <= lineHeight * 4;
            const bool similar = b.width() >= a.width() * 5 / 10
                && b.width() <= a.width() * 2;
            if (sameColumn && close && similar) {
                last.box = a.united(b);
                last.fillMask = QImage();
                last.rows.clear();
                last.sourceLineHeight = qMax(last.sourceLineHeight, block.sourceLineHeight);
                mergedSources.last() = mergedSources.last() + QLatin1Char(' ') + source;
                continue;
            }
        }
        merged.append(block);
        mergedSources.append(source);
    }
    m_impl->blocks = merged;
    m_impl->blockSources = mergedSources;
}

void ImageTranslationPipeline::startDenseColumnPass()
{
    // A wall of prose next to artwork: the page-wide detector sees almost nothing.
    // Find the large bright region, crop it, upscale and read it as one column.
    const QImage source = m_impl->image.convertToFormat(QImage::Format_RGB32);
    QImage gray(source.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < source.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(source.constScanLine(y));
        uchar* target = gray.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            const QRgb pixel = row[x];
            target[x] = uchar((qRed(pixel) * 299 + qGreen(pixel) * 587 + qBlue(pixel) * 114) / 1000);
        }
    }
    const int smallWidth = qMax(1, source.width() / 4);
    const int smallHeight = qMax(1, source.height() / 4);
    const QImage small = gray.scaled(smallWidth, smallHeight, Qt::IgnoreAspectRatio,
                                     Qt::FastTransformation);
    QVector<uchar> bright(smallWidth * smallHeight, 0);
    for (int y = 0; y < smallHeight; ++y) {
        const uchar* row = small.constScanLine(y);
        for (int x = 0; x < smallWidth; ++x)
            bright[y * smallWidth + x] = row[x] >= 200 ? 1 : 0;
    }
    QVector<uchar> seen(smallWidth * smallHeight, 0);
    QRect bestBox;
    int bestArea = 0;
    for (int start = 0; start < bright.size(); ++start) {
        if (!bright.at(start) || seen.at(start))
            continue;
        QVector<int> stack{ start };
        seen[start] = 1;
        int area = 0;
        int minX = smallWidth;
        int maxX = 0;
        int minY = smallHeight;
        int maxY = 0;
        while (!stack.isEmpty()) {
            const int index = stack.takeLast();
            const int x = index % smallWidth;
            const int y = index / smallWidth;
            ++area;
            minX = qMin(minX, x); maxX = qMax(maxX, x);
            minY = qMin(minY, y); maxY = qMax(maxY, y);
            const int neighbours[4] = { index - 1, index + 1,
                                        index - smallWidth, index + smallWidth };
            for (int k = 0; k < 4; ++k) {
                const int next = neighbours[k];
                if (next < 0 || next >= bright.size() || seen.at(next) || !bright.at(next))
                    continue;
                if (k < 2 && (next / smallWidth) != y)
                    continue;
                seen[next] = 1;
                stack.append(next);
            }
        }
        if (area > bestArea) {
            bestArea = area;
            bestBox = QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
        }
    }
    const int totalSmall = smallWidth * smallHeight;
    QRect column = QRect(bestBox.x() * 4, bestBox.y() * 4,
                               bestBox.width() * 4, bestBox.height() * 4)
                             .intersected(source.rect());
    bool usable = bestArea * 10 >= totalSmall && bestArea * 10 <= totalSmall * 7
        && column.width() < source.width() * 8 / 10
        && column.height() > source.height() / 4;
    if (usable) {
        int dark = 0;
        for (int y = column.top(); y <= column.bottom(); y += 4) {
            const uchar* row = gray.constScanLine(y);
            for (int x = column.left(); x <= column.right(); x += 4)
                dark += row[x] < 128 ? 1 : 0;
        }
        const int samples = qMax(1, (column.width() / 4) * (column.height() / 4));
        usable = dark * 100 >= samples * 2 && dark * 100 <= samples * 40;
    }
    if (!usable || (column.width() > source.width() * 85 / 100
                    && column.height() > source.height() * 85 / 100)) {
        // The bright component swallowed the whole page; score halves instead.
        const QRect candidates[4] = {
            QRect(0, 0, source.width() / 2, source.height()),
            QRect(source.width() / 2, 0, source.width() - source.width() / 2, source.height()),
            QRect(0, 0, source.width(), source.height() / 2),
            QRect(0, source.height() / 2, source.width(), source.height() - source.height() / 2),
        };
        double bestScore = 0.0;
        QRect bestRect;
        for (const QRect& candidate : candidates) {
            const QRect smallRect(candidate.x() / 4, candidate.y() / 4,
                                  qMax(1, candidate.width() / 4), qMax(1, candidate.height() / 4));
            int brightCount = 0;
            int darkCount = 0;
            for (int y = smallRect.top(); y <= smallRect.bottom(); ++y) {
                if (y < 0 || y >= smallHeight)
                    continue;
                const uchar* row = small.constScanLine(y);
                for (int x = smallRect.left(); x <= smallRect.right(); ++x) {
                    if (x < 0 || x >= smallWidth)
                        continue;
                    if (row[x] >= 200)
                        ++brightCount;
                    else if (row[x] < 128)
                        ++darkCount;
                }
            }
            const int samples = qMax(1, smallRect.width() * smallRect.height());
            const double brightRatio = double(brightCount) / samples;
            const double darkRatio = double(darkCount) / samples;
            if (brightRatio < 0.5 || darkRatio < 0.02 || darkRatio > 0.35)
                continue;
            const double score = brightRatio * darkRatio;
            if (score > bestScore) {
                bestScore = score;
                bestRect = candidate;
            }
        }
        if (bestScore > 0.0) {
            column = bestRect;
            usable = true;
        }
    }
    if (usable) {
        // The bright component may stop at the page margin; extend the column
        // while strips still contain dark text on bright paper.
        int misses = 0;
        int x = column.right() + 1;
        while (x + 8 < source.width() && misses < 3) {
            int dark = 0;
            int bright = 0;
            int samples = 0;
            for (int y = column.top(); y <= column.bottom(); y += 4) {
                const uchar* row = gray.constScanLine(y);
                for (int xx = x; xx < qMin(x + 8, source.width()); xx += 2) {
                    ++samples;
                    if (row[xx] < 128)
                        ++dark;
                    else if (row[xx] >= 200)
                        ++bright;
                }
            }
            if (samples > 0 && bright * 10 >= samples * 5 && dark * 100 >= samples) {
                column.setRight(x + 7);
                x += 8;
                misses = 0;
            } else {
                x += 8;
                ++misses;
            }
        }
    }
    if (!usable) {
        m_impl->hybridMode = true;
        m_impl->denseTextFallback = true;
        m_impl->extraStage = 0;
        runExtraStage(0);
        return;
    }
    const QRect cropRect = column.adjusted(-4, -4, 4, 4).intersected(source.rect());
    const QImage crop = source.copy(cropRect);
    const double zoom = qBound(1.0, 1200.0 / qMax(1, qMax(crop.width(), crop.height())), 3.0);
    const QImage scaled = zoom > 1.01
        ? crop.scaled(int(std::lround(crop.width() * zoom)),
                      int(std::lround(crop.height() * zoom)),
                      Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        : crop;
    const QImage prepared = stretchedGray(grayCopy(scaled));
    m_impl->denseColumnRect = cropRect;
    m_impl->denseColumnZoom = zoom;
    QPointer<ImageTranslationPipeline> self(this);
    m_impl->context->translation->ocrRequestAsync(
        prepared, QStringLiteral("tesseract_local"), m_impl->ocrLanguage,
        [self](const QJsonObject& result) {
            if (!self || self->m_impl->cancelled)
                return;
            QVector<OcrLine> lines = parseOcrLines(result, QPoint(0, 0), 0.0);
            QVector<OcrLine> kept;
            const double inverse = self->m_impl->denseColumnZoom > 0.0
                ? 1.0 / self->m_impl->denseColumnZoom : 1.0;
            for (OcrLine line : lines) {
                line.box = QRect(int(std::lround(line.box.x() * inverse)),
                                 int(std::lround(line.box.y() * inverse)),
                                 int(std::lround(line.box.width() * inverse)),
                                 int(std::lround(line.box.height() * inverse)))
                                .translated(self->m_impl->denseColumnRect.topLeft());
                line.height = line.box.height();
                QString lettersOnly;
                for (const QChar& character : line.text)
                    if (character.isLetter())
                        lettersOnly.append(character);
                if (lettersOnly.size() >= 2)
                    kept.append(line);
            }
            if (kept.size() < 3) {
                self->m_impl->hybridMode = true;
                self->m_impl->denseTextFallback = true;
                self->m_impl->extraStage = 0;
                self->runExtraStage(0);
                return;
            }
            std::sort(kept.begin(), kept.end(), [](const OcrLine& left, const OcrLine& right) {
                return left.box.top() < right.box.top();
            });
            QVector<OcrLine> paragraphs;
            QVector<OcrLine> current;
            const auto flush = [&]() {
                if (current.isEmpty())
                    return;
                OcrLine paragraph;
                paragraph.box = current.first().box;
                int letters = 0;
                double weighted = 0.0;
                for (const OcrLine& line : std::as_const(current)) {
                    paragraph.box = paragraph.box.united(line.box);
                    paragraph.text += (paragraph.text.isEmpty() ? QString() : QStringLiteral(" "))
                        + line.text;
                    letters += line.text.size();
                    weighted += line.confidence * line.text.size();
                }
                paragraph.height = current.first().height;
                paragraph.confidence = letters > 0 ? weighted / letters : 0.0;
                paragraphs.append(paragraph);
                current.clear();
            };
            for (const OcrLine& line : std::as_const(kept)) {
                if (!current.isEmpty()) {
                    const int gap = line.box.top() - current.last().box.bottom() - 1;
                    if (gap > current.last().height * 2 || current.size() >= 5)
                        flush();
                }
                current.append(line);
            }
            flush();
            if (paragraphs.isEmpty()) {
                self->m_impl->hybridMode = true;
                self->m_impl->denseTextFallback = true;
                self->m_impl->extraStage = 0;
                self->runExtraStage(0);
                return;
            }
            // Lay the paragraphs out as non-overlapping slots across the column:
            // that erases the whole source column and keeps translations apart.
            int totalLetters = 0;
            for (const OcrLine& paragraph : std::as_const(paragraphs))
                totalLetters += qMax(1, paragraph.text.size());
            const QRect columnBox = self->m_impl->denseColumnRect;
            int y = columnBox.top();
            self->m_impl->blocks.clear();
            self->m_impl->blockSources.clear();
            for (int index = 0; index < paragraphs.size(); ++index) {
                const OcrLine& paragraph = paragraphs.at(index);
                int slot = int(std::lround(double(columnBox.height())
                                           * qMax(1, paragraph.text.size()) / qMax(1, totalLetters)));
                slot = qMax(28, slot);
                if (index + 1 == paragraphs.size())
                    slot = columnBox.bottom() - y + 1;
                slot = qMin(slot, columnBox.bottom() - y + 1);
                if (slot <= 0)
                    break;
                OverlayLine line;
                line.box = QRect(columnBox.left(), y, columnBox.width(), slot);
                line.sourceLineHeight = qMax(8, paragraph.height);
                self->m_impl->blocks.append(line);
                self->m_impl->blockSources.append(paragraph.text);
                y += slot;
                if (y > columnBox.bottom())
                    break;
            }
            if (self->m_impl->blocks.isEmpty()) {
                self->m_impl->hybridMode = true;
                self->m_impl->denseTextFallback = true;
                self->m_impl->extraStage = 0;
                self->runExtraStage(0);
                return;
            }
            self->m_impl->paperPage = false;
            self->m_impl->extraStage = 0;
            self->m_impl->recoveredLines = 0;
            self->finishBlocks();
        },
        true, QStringLiteral("image"), 6);
}

void ImageTranslationPipeline::finishBlocks()
{
    mergeParagraphBlocks();
    applyDictionaryCorrections();
    emit progress(0, int(m_impl->blocks.size()), QString());
    QTimer::singleShot(0, this, [this] { stepTranslate(); });
}

void ImageTranslationPipeline::processNextExtraJob()
{
    if (m_impl->cancelled || m_impl->failed)
        return;

    const qint64 budget = m_impl->extraStage == 2 ? 8000
                                               : (m_impl->extraStage == 1 ? 8000 : 25000);
    if (QDateTime::currentMSecsSinceEpoch() - m_impl->stageStartMs > budget) {
        m_impl->extraJobs.clear();
        m_impl->extraIndex = 0;
        runExtraStage(m_impl->extraStage + 1);
        return;
    }
    if (m_impl->extraIndex >= m_impl->extraJobs.size()) {
        const int stage = m_impl->extraStage;
        m_impl->extraJobs.clear();
        m_impl->extraIndex = 0;
        if (stage == 2) {
            if (m_impl->tesseractPath)
                buildTesseractBlocks();
            else
                buildBlocks();
            return;
        }
        if (stage == 3) {
            runExtraStage(4);
            return;
        }
        runExtraStage(stage + 1);
        return;
    }

    const int jobIndex = m_impl->extraIndex;
    const ExtraOcrJob job = m_impl->extraJobs.at(jobIndex);
    const QImage crop = job.variants.isEmpty()
        ? m_impl->image.copy(job.region)
        : job.variants.at(qMin(job.variantIndex, job.variants.size() - 1));
    // The bubble pass is a Tesseract job: that engine reads isolated lettering
    // best and reports a confidence, exactly like the Firefox translator does.
    QString ocrProvider = m_impl->ocrProviderOverride;
    int psm = 0;
    const bool tesseractAvailable = m_impl->context->plugins
        && m_impl->context->plugins->provider(QStringLiteral("tesseract_local"));
    if ((job.stage == 2 || job.stage == 3 || job.stage == 4) && tesseractAvailable) {
        ocrProvider = QStringLiteral("tesseract_local");
        psm = 6;
    } else if ((m_impl->tesseractPath || m_impl->hybridMode) && tesseractAvailable) {
        // Bands and gap re-reading go to Tesseract in the plugin-style and hybrid
        // modes; Paddle keeps doing full-page detection in hybrid.
        ocrProvider = QStringLiteral("tesseract_local");
        psm = 11;
    }
    if (job.psm > 0)
        psm = job.psm;
    QPointer<ImageTranslationPipeline> self(this);
    m_impl->context->translation->ocrRequestAsync(
        crop, ocrProvider, m_impl->ocrLanguage,
        [self, jobIndex](const QJsonObject& result) {
            if (!self || self->m_impl->cancelled || self->m_impl->failed)
                return;
            const ExtraOcrJob job = self->m_impl->extraJobs.at(jobIndex);
            const QString error = result.value(QStringLiteral("error")).toString();
            if (error.isEmpty()) {
                if (job.stage == 2) {
                    const QVector<OcrLine> parsed =
                        parseOcrLines(result, QPoint(0, 0), 0.0);
                    const QString text = joinLineTexts(parsed).trimmed();
                    double confidence = 0.0;
                    int confidenceCount = 0;
                    for (const OcrLine& line : parsed) {
                        if (line.confidence > 0.0) {
                            confidence += line.confidence;
                            ++confidenceCount;
                        }
                    }
                    if (confidenceCount > 0)
                        confidence /= confidenceCount;
                    const auto normalize = [](const QString& value) {
                        QString result;
                        for (const QChar& character : value)
                            if (character.isLetterOrNumber())
                                result.append(character.toLower());
                        return result;
                    };
                    const QString normalized = normalize(text);
                    // Try the reading against the spelling dictionary: junk from
                    // logos and scratches rarely looks like real words. If it does
                    // not pass, the next (thicker) variant is tried.
                    int textLetters = 0;
                    int textJunk = 0;
                    for (const QChar& character : text) {
                        if (character.isLetter())
                            ++textLetters;
                        else if (!character.isDigit() && !character.isSpace()
                                 && !QStringLiteral(".,!?…:;'\"-—–()[]{}«»/\\|").contains(character))
                            ++textJunk;
                    }
                    bool dictionaryOk = textLetters >= 3 && textJunk == 0;
                    if (dictionaryOk) {
                        SpellService spell(self->m_impl->context->plugins,
                                           self->m_impl->context->settings);
                        if (spell.provider()) {
                            const QString language =
                                self->m_impl->sourceCode.isEmpty()
                                    || self->m_impl->sourceCode == QStringLiteral("auto")
                                ? QStringLiteral("en")
                                : self->m_impl->sourceCode;
                            const QStringList words = text.split(
                                QRegularExpression(QStringLiteral("[\\s]+")),
                                Qt::SkipEmptyParts);
                            int longWords = 0;
                            int known = 0;
                            for (const QString& word : words) {
                                QString clean;
                                for (const QChar& character : word)
                                    if (character.isLetter())
                                        clean.append(character);
                                if (clean.size() < 3)
                                    continue;
                                ++longWords;
                                if (spell.checkWord(clean, language))
                                    ++known;
                            }
                            const QString compact = text;
                            bool nameLike = true;
                            for (const QChar& character : compact)
                                if (!character.isLetter() && !character.isSpace())
                                    nameLike = false;
                            dictionaryOk = known >= 1
                                || (nameLike && textLetters >= 4 && confidence >= 0.65)
                                || (textLetters >= 6 && confidence >= 0.6);
                        }
                    }
                    if (!dictionaryOk && job.variantIndex + 1 < job.variants.size()) {
                        self->m_impl->extraJobs[jobIndex].variantIndex = job.variantIndex + 1;
                        QTimer::singleShot(0, self, [self] { self->processNextExtraJob(); });
                        return;
                    }
                    if (dictionaryOk && text.size() <= 240) {
                        bool duplicate = false;
                        for (const OcrLine& line : std::as_const(self->m_impl->ocrLines)) {
                            if (normalize(line.text) == normalized) {
                                duplicate = true;
                                break;
                            }
                        }
                        if (!duplicate) {
                            OcrLine line;
                            line.box = job.region;
                            line.text = text;
                            line.height = qMax(8, job.textHeight);
                            self->m_impl->ocrLines.append(line);
                            ++self->m_impl->recoveredLines;
                            self->m_impl->lineDump.append(QStringLiteral("%1,%2 %3x%4 %5")
                                                              .arg(line.box.x())
                                                              .arg(line.box.y())
                                                              .arg(line.box.width())
                                                              .arg(line.box.height())
                                                              .arg(line.text.left(24)));
                        }
                    }
                } else if (job.stage == 3) {
                    const QVector<OcrLine> parsed = parseOcrLines(result, QPoint(0, 0), 0.0);
                    const QString text = joinLineTexts(parsed).trimmed();
                    double confidence = 0.0;
                    int confidenceCount = 0;
                    for (const OcrLine& line : parsed) {
                        if (line.confidence > 0.0) {
                            confidence += line.confidence;
                            ++confidenceCount;
                        }
                    }
                    if (confidenceCount > 0)
                        confidence /= confidenceCount;
                    SpellService spell(self->m_impl->context->plugins,
                                       self->m_impl->context->settings);
                    const QString language = self->m_impl->sourceCode.isEmpty()
                            || self->m_impl->sourceCode == QStringLiteral("auto")
                        ? QStringLiteral("en")
                        : self->m_impl->sourceCode;
                    ExtraOcrJob& target = self->m_impl->extraJobs[jobIndex];
                    const double score = readingScore(text, confidence, spell, language);
                    if (score > target.bestScore) {
                        target.bestScore = score;
                        target.bestText = text;
                    }
                    if (target.variantIndex + 1 < target.variants.size()) {
                        ++target.variantIndex;
                        QTimer::singleShot(0, self, [self] { self->processNextExtraJob(); });
                        return;
                    }
                    if (!target.bestText.isEmpty() && target.bestScore > target.baseScore
                        && plausibleRecoveredText(target.bestText)
                        && target.blockIndex >= 0
                        && target.blockIndex < self->m_impl->blockSources.size()) {
                        // Never regress: the new reading must keep at least as many
                        // dictionary words and either add a word or add real letters.
                        const QString reading = target.bestText;
                        int newLetters = 0;
                        int newKnown = 0;
                        const QStringList words = reading.split(
                            QRegularExpression(QStringLiteral("[\\s]+")), Qt::SkipEmptyParts);
                        for (const QString& word : words) {
                            QString clean;
                            for (const QChar& character : word)
                                if (character.isLetter())
                                    clean.append(character);
                            newLetters += clean.size();
                            if (clean.size() >= 3 && spell.provider()
                                && spell.checkWord(clean, language))
                                ++newKnown;
                        }
                        int newJunk = 0;
                        for (const QChar& character : reading) {
                            if (character.isLetterOrNumber() || character.isSpace())
                                continue;
                            if (QStringLiteral(".,!?…:;'\"-—–()[]{}«»").contains(character))
                                continue;
                            ++newJunk;
                        }
                        const bool better = newJunk == 0
                            && (newKnown > target.baseKnown
                                || (newKnown >= target.baseKnown
                                    && newLetters >= target.baseLetters + 3));
                        if (newKnown >= target.baseKnown && newLetters >= target.baseLetters
                            && better) {
                            self->m_impl->blockSources[target.blockIndex] = reading;
                        }
                    }
                } else if (job.stage == 4) {
                    const QVector<OcrLine> parsed = parseOcrLines(result, QPoint(0, 0), 0.0);
                    const QString text = joinLineTexts(parsed).trimmed();
                    double confidence = 0.0;
                    int confidenceCount = 0;
                    for (const OcrLine& line : parsed) {
                        if (line.confidence > 0.0) {
                            confidence += line.confidence;
                            ++confidenceCount;
                        }
                    }
                    if (confidenceCount > 0)
                        confidence /= confidenceCount;
                    SpellService spell(self->m_impl->context->plugins,
                                       self->m_impl->context->settings);
                    const QString language = self->m_impl->sourceCode.isEmpty()
                            || self->m_impl->sourceCode == QStringLiteral("auto")
                        ? QStringLiteral("en")
                        : self->m_impl->sourceCode;
                    ExtraOcrJob& target = self->m_impl->extraJobs[jobIndex];
                    const double score = readingScore(text, confidence, spell, language);
                    if (score > target.bestScore) {
                        target.bestScore = score;
                        target.bestText = text;
                    }
                    if (target.variantIndex + 1 < target.variants.size()) {
                        ++target.variantIndex;
                        QTimer::singleShot(0, self, [self] { self->processNextExtraJob(); });
                        return;
                    }
                    const QString reading = target.bestText;
                    int letters = 0;
                    int known = 0;
                    const QStringList words = reading.split(
                        QRegularExpression(QStringLiteral("[\\s]+")), Qt::SkipEmptyParts);
                    for (const QString& word : words) {
                        QString clean;
                        for (const QChar& character : word)
                            if (character.isLetter())
                                clean.append(character);
                        letters += clean.size();
                        if (clean.size() >= 3 && spell.provider()
                            && spell.checkWord(clean, language))
                            ++known;
                    }
                    const bool accept = !reading.isEmpty() && letters >= 3
                        && plausibleRecoveredText(reading)
                        && (known >= 1 || (confidence >= 0.65 && letters >= 6));
                } else {
                    const QVector<OcrLine> parsed =
                        parseOcrLines(result, job.region.topLeft(), self->m_impl->minOcrConfidence);
                    const int medianLineHeight =
                        qMax(8, medianHeight(self->m_impl->ocrLines));
                    int acceptedCount = 0;
                    for (const OcrLine& line : parsed) {
                        bool accepted = job.stage == 1
                            ? isPlausibleRecoveredLine(line, self->m_impl->ocrLines, job.region,
                                                       medianLineHeight)
                            : isPlausibleExtraLine(line, self->m_impl->ocrLines, job.region,
                                                   medianLineHeight);
                        if (accepted && job.stage == 0) {
                            SpellService spell(self->m_impl->context->plugins,
                                               self->m_impl->context->settings);
                            const QString language =
                                self->m_impl->sourceCode.isEmpty()
                                    || self->m_impl->sourceCode == QStringLiteral("auto")
                                ? QStringLiteral("en")
                                : self->m_impl->sourceCode;
                            accepted = dictionaryLooksLikeText(line.text, spell, language);
                        }
                        if (!accepted)
                            continue;
                        ++acceptedCount;
                        self->m_impl->ocrLines.append(line);
                        ++self->m_impl->recoveredLines;
                        self->m_impl->lineDump.append(QStringLiteral("%1,%2 %3x%4 %5")
                                                          .arg(line.box.x())
                                                          .arg(line.box.y())
                                                          .arg(line.box.width())
                                                          .arg(line.box.height())
                                                          .arg(line.text.left(24)));
                    }
                    if (acceptedCount == 0 && job.variantIndex + 1 < job.variants.size()) {
                        ++self->m_impl->extraJobs[jobIndex].variantIndex;
                        QTimer::singleShot(0, self, [self] { self->processNextExtraJob(); });
                        return;
                    }
                }
            }
            ++self->m_impl->extraIndex;
            QTimer::singleShot(0, self, [self] { self->processNextExtraJob(); });
        },
        false, QStringLiteral("image"), psm);
}

void ImageTranslationPipeline::buildBlocks()
{
    const QVector<OcrLine>& lines = m_impl->ocrLines;
    BubbleFinder finder(m_impl->image);
    QVector<BlockBuilder> blocks;
    QVector<QVector<OcrLine>> groupedLines;
    QVector<TextBubble> groupedBubbles;
    QVector<OcrLine> freeLines;

    for (const OcrLine& line : lines) {
        const TextBubble bubble = finder.around(line.box);
        if (!bubble.valid()) {
            freeLines.append(line);
            continue;
        }
        int group = -1;
        for (int index = 0; index < groupedBubbles.size(); ++index) {
            const QRect& existing = groupedBubbles.at(index).box;
            const QRect& candidate = bubble.box;
            if (qAbs(existing.x() - candidate.x()) <= 4
                && qAbs(existing.y() - candidate.y()) <= 4
                && qAbs(existing.width() - candidate.width()) <= 6
                && qAbs(existing.height() - candidate.height()) <= 6) {
                group = index;
                break;
            }
        }
        if (group < 0) {
            groupedBubbles.append(bubble);
            groupedLines.append(QVector<OcrLine>());
            group = int(groupedBubbles.size()) - 1;
        }
        groupedLines[group].append(line);
    }

    QVector<BlockBuilder> bubbleBlocks;
    for (int index = 0; index < groupedBubbles.size(); ++index) {
        BlockBuilder block;
        block.hasBubble = true;
        block.bubble = groupedBubbles.at(index);
        block.box = block.bubble.box;
        block.lines = groupedLines.at(index);
        std::sort(block.lines.begin(), block.lines.end(),
                  [](const OcrLine& left, const OcrLine& right) {
                      if (left.box.top() != right.box.top())
                          return left.box.top() < right.box.top();
                      return left.box.left() < right.box.left();
                  });
        bubbleBlocks.append(block);
    }

    // Fallback balloon for text whose own outline is broken (low-quality scans):
    // fit an ellipse around the lines, clipped by the paper.
    const auto trySynthetic = [&finder, this](BlockBuilder& block) {
        if (!finder.paperPage() || block.lines.isEmpty())
            return false;
        QVector<QRect> boxes;
        boxes.reserve(block.lines.size());
        for (const OcrLine& line : std::as_const(block.lines))
            boxes.append(line.box);
        const TextBubble bubble = finder.synthetic(boxes);
        if (!bubble.valid() || !maskCoversLines(bubble, block.lines))
            return false;
        block.hasBubble = true;
        block.bubble = bubble;
        block.box = bubble.box;
        return true;
    };

    for (BlockBuilder& original : bubbleBlocks) {
        for (BlockBuilder block : splitBlockByGap(original)) {
            if (isServiceText(joinLineTexts(block.lines)))
                continue;
            const int lineHeight = estimateLineHeight(block.lines);
            if (!block.hasBubble || !maskCoversLines(block.bubble, block.lines)) {
                // Partial mask: try a fitted balloon, then fall back to a rectangle
                // so no original lettering survives.
                block.hasBubble = false;
                if (!trySynthetic(block))
                    block.box = makeRectangularBlock(linesUnion(block.lines), lineHeight).box;
            }
            blocks.append(block);
            if (block.hasBubble)
                m_impl->bubbles.append(block.bubble);  // diagnostics/dump only
        }
    }

    const int pageLineHeight = qMax(8, medianHeight(lines));
    QVector<BlockBuilder> freeBlocks;
    for (const BlockBuilder& grouped : groupLines(freeLines, pageLineHeight))
        freeBlocks.append(splitBlockByGap(grouped));
    for (BlockBuilder& block : freeBlocks) {
        const QVector<BlockBuilder> parts = m_impl->splitParagraphs
            ? splitParagraphs(block, pageLineHeight)
            : QVector<BlockBuilder>{ block };
        for (const BlockBuilder& part : parts) {
            if (isServiceText(joinLineTexts(part.lines)))
                continue;
            const int lineHeight = estimateLineHeight(part.lines);
            BlockBuilder final = part;
            if (!trySynthetic(final))
                final.box = makeRectangularBlock(
                    part.box.isNull() ? linesUnion(part.lines) : part.box, lineHeight).box;
            blocks.append(final);
        }
    }

    for (const BlockBuilder& block : std::as_const(blocks)) {
        const int lineHeight = estimateLineHeight(block.lines);
        OverlayLine overlay;
        overlay.box = block.box;
        overlay.sourceLineHeight = lineHeight;
        if (block.hasBubble) {
            overlay.fillMask = block.bubble.fillMask;
            overlay.rows = block.bubble.area.rows;
        }
        m_impl->blocks.append(overlay);
        m_impl->blockSources.append(joinLineTexts(block.lines));
    }

    if (m_impl->blocks.isEmpty()) {
        m_impl->failed = true;
        emit failed(QStringLiteral("На изображении не найден текст."));
        return;
    }

    runExtraStage(3);
}
void ImageTranslationPipeline::applyDictionaryCorrections()
{
    if (!m_impl->context || !m_impl->context->plugins || !m_impl->context->settings)
        return;
    SpellService spell(m_impl->context->plugins, m_impl->context->settings);
    if (!spell.provider())
        return;
    const QString language = m_impl->sourceCode.isEmpty() || m_impl->sourceCode == QStringLiteral("auto")
        ? QStringLiteral("en")
        : m_impl->sourceCode;
    for (QString& source : m_impl->blockSources)
        source = correctWithDictionary(source, spell, language);
    m_impl->sourceText = m_impl->blockSources.join(QLatin1Char(' '));
}


void ImageTranslationPipeline::startTesseractPass()
{
    // The Firefox translator upscales narrow pages to ~1000 px and feeds Tesseract
    // grayscale/sparse text; without that it misses most of the lettering on art.
    QImage source = m_impl->image.convertToFormat(QImage::Format_RGB32);
    double scale = 1.0;
    if (source.width() < 700)
        scale = qMin(2.0, 1000.0 / source.width());
    const int maxSide = 2600;
    if (qMax(source.width(), source.height()) * scale > maxSide)
        scale = double(maxSide) / qMax(source.width(), source.height());
    QImage prepared = scale != 1.0
        ? source.scaled(int(std::lround(source.width() * scale)),
                        int(std::lround(source.height() * scale)), Qt::IgnoreAspectRatio,
                        Qt::SmoothTransformation)
        : source;
    // Exact luma from the plugin (0.299/0.587/0.114), saved as 8-bit gray:
    // Tesseract's internal binarisation is sensitive to the pixel format.
    QImage gray(prepared.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < prepared.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(prepared.constScanLine(y));
        uchar* target = gray.scanLine(y);
        for (int x = 0; x < prepared.width(); ++x) {
            const QRgb pixel = row[x];
            target[x] = uchar((qRed(pixel) * 299 + qGreen(pixel) * 587 + qBlue(pixel) * 114) / 1000);
        }
    }
    m_impl->tesseractScale = scale;
    m_impl->tesseractImage = gray;

    QPointer<ImageTranslationPipeline> self(this);
    m_impl->context->translation->ocrRequestAsync(
        gray, QStringLiteral("tesseract_local"), m_impl->ocrLanguage,
        [self](const QJsonObject& result) {
            if (!self || self->m_impl->cancelled)
                return;
            const QString error = result.value(QStringLiteral("error")).toString();
            if (!error.isEmpty()) {
                self->m_impl->failed = true;
                emit self->failed(error);
                return;
            }
            const double minConfidence = qMax(0.6, self->m_impl->minOcrConfidence);
            QVector<OcrLine> lines = cleanLinesFromWords(
                parseOcrWords(result), minConfidence, self->m_impl->tesseractImage.size());
            const double inverse =
                self->m_impl->tesseractScale > 0.0 ? 1.0 / self->m_impl->tesseractScale : 1.0;
            for (OcrLine& line : lines) {
                line.box = QRect(int(std::lround(line.box.x() * inverse)),
                                 int(std::lround(line.box.y() * inverse)),
                                 int(std::lround(line.box.width() * inverse)),
                                 int(std::lround(line.box.height() * inverse)));
                line.height = line.box.height();
                self->m_impl->lineDump.append(QStringLiteral("%1,%2 %3x%4 %5")
                                                  .arg(line.box.x())
                                                  .arg(line.box.y())
                                                  .arg(line.box.width())
                                                  .arg(line.box.height())
                                                  .arg(line.text.left(24)));
            }
            self->m_impl->ocrLines = lines;
            if (lines.isEmpty()) {
                self->m_impl->failed = true;
                emit self->failed(QStringLiteral("На изображении не найден текст."));
                return;
            }
            BubbleFinder finder(self->m_impl->image);
            self->m_impl->paperPage = finder.paperPage();
            self->m_impl->extraStage = 0;
            self->m_impl->recoveredLines = 0;
            self->m_impl->tileJobs = 0;
            self->m_impl->inkJobs = 0;
            self->m_impl->bubbleJobs = 0;
            self->runExtraStage(0);
        },
        true, QStringLiteral("image"), 11);
}

void ImageTranslationPipeline::buildTesseractBlocks()
{
    // Grouping must happen at the OCR scale (the plugin works on the upscaled
    // image); blocks are mapped back to page coordinates afterwards.
    const double back = m_impl->tesseractScale > 0.0 ? m_impl->tesseractScale : 1.0;
    QVector<OcrLine> scaledLines = m_impl->ocrLines;
    if (!qFuzzyCompare(back, 1.0)) {
        for (OcrLine& line : scaledLines)
            line.box = QRect(int(std::lround(line.box.x() * back)),
                             int(std::lround(line.box.y() * back)),
                             int(std::lround(line.box.width() * back)),
                             int(std::lround(line.box.height() * back)));
    }
    const QSize groupSize = m_impl->tesseractImage.isNull()
        ? m_impl->image.size() : m_impl->tesseractImage.size();
    const QVector<TesseractGroup> groups = groupTesseractLines(
        scaledLines, groupSize.width(), groupSize.height());
    // Tesseract on manga produces plenty of confident-looking garbage. Keep only
    // blocks that contain at least one real word according to the dictionary.
    SpellService spell(m_impl->context->plugins, m_impl->context->settings);
    const QString language = m_impl->sourceCode.isEmpty()
            || m_impl->sourceCode == QStringLiteral("auto")
        ? QStringLiteral("en")
        : m_impl->sourceCode;
    for (const TesseractGroup& group : groups) {
        int letters = 0;
        int known = 0;
        const QStringList words = group.text.split(QRegularExpression(QStringLiteral("[\\s]+")),
                                                   Qt::SkipEmptyParts);
        for (const QString& word : words) {
            QString clean;
            for (const QChar& character : word)
                if (character.isLetter())
                    clean.append(character);
            letters += clean.size();
            if (clean.size() >= 3 && spell.provider() && spell.checkWord(clean, language))
                ++known;
        }
        if (letters < 3)
            continue;
        // A dictionary hit is the strongest signal, but stylised lettering and
        // names are not in it: allow a confident, plausible reading through.
        // One dictionary word is only enough for a short bubble ("WHAT?"); longer
        // junk like "oust WATCH" needs two real words. This is the plugin's filter.
        const int common = commonWordCount(group.text);
        const bool shortKnown = common == 1 && letters >= 4 && letters <= 6;
        const bool confident = group.confidence >= 0.7 && plausibleRecoveredText(group.text)
            && letters >= 6;
        if (spell.provider() && common < 2 && !shortKnown && !confident)
            continue;
        const int padX = qMax(4, int(group.box.width() * 0.05));
        const int padY = qMax(3, int(group.box.height() * 0.07));
        const QRect box = group.box.adjusted(-padX, -padY, padX, padY)
                              .intersected(m_impl->image.rect());
        OverlayLine line;
        line.box = box;
        line.sourceLineHeight = qMax(8, int(std::lround(group.lineHeight / back)));
        m_impl->blocks.append(line);
        m_impl->blockSources.append(group.text);
    }
    if (m_impl->blocks.isEmpty()) {
        m_impl->failed = true;
        emit failed(QStringLiteral("На изображении не найден текст."));
        return;
    }
    runExtraStage(3);
}

void ImageTranslationPipeline::recognizeNextBubble()
{
    if (m_impl->cancelled || m_impl->failed)
        return;

    if (m_impl->bubbleIndex >= m_impl->bubbles.size()) {
        if (m_impl->blocks.isEmpty()) {
            m_impl->failed = true;
            emit failed(QStringLiteral("На изображении не найден текст."));
            return;
        }
        applyDictionaryCorrections();
        emit progress(0, int(m_impl->blockSources.size()), QString());
        QTimer::singleShot(0, this, [this] { stepTranslate(); });
        return;
    }

    const TextBubble bubble = m_impl->bubbles.at(m_impl->bubbleIndex);
    const QImage crop = m_impl->image.copy(bubble.box);
    QPointer<ImageTranslationPipeline> self(this);
    m_impl->context->translation->ocrRequestAsync(
        crop, QString(), m_impl->ocrLanguage,
        [self, bubble](const QJsonObject& result) {
            if (!self || self->m_impl->cancelled || self->m_impl->failed)
                return;

            const QVector<OcrLine> lines =
                parseOcrLines(result, bubble.box.topLeft(), self->m_impl->minOcrConfidence);
            if (!lines.isEmpty()) {
                OverlayLine overlay;
                overlay.box = bubble.box;
                overlay.sourceLineHeight = estimateLineHeight(lines);
                overlay.fillMask = bubble.fillMask;
                overlay.rows = bubble.area.rows;
                self->m_impl->blocks.append(overlay);
                self->m_impl->blockSources.append(joinLineTexts(lines));
                self->m_impl->sourceText = self->m_impl->sourceText.isEmpty()
                    ? joinLineTexts(lines)
                    : self->m_impl->sourceText + QLatin1Char('\n') + joinLineTexts(lines);
            }

            ++self->m_impl->bubbleIndex;
            QTimer::singleShot(0, self, [self] { self->recognizeNextBubble(); });
        },
        false, QStringLiteral("image"), 6);
}

void ImageTranslationPipeline::stepTranslate()
{
    if (m_impl->cancelled || m_impl->failed)
        return;

    if (m_impl->index >= m_impl->blockSources.size()) {
        finish();
        return;
    }

    const int current = m_impl->index;
    const QString source = m_impl->blockSources.at(current);

    // Scene context and a length budget only make sense for an LLM connector;
    // classic machine translation services would ignore them anyway.
    QJsonObject extra;
    if (m_impl->llmProvider) {
        if (m_impl->llmContext) {
            QStringList context;
            if (current > 0)
                context.append(QStringLiteral("Previous line: ")
                               + m_impl->blockSources.at(current - 1).left(160));
            if (current + 1 < m_impl->blockSources.size())
                context.append(QStringLiteral("Next line: ")
                               + m_impl->blockSources.at(current + 1).left(160));
            if (!context.isEmpty())
                extra.insert(QStringLiteral("context"), context.join(QLatin1Char('\n')));
        }
        if (m_impl->lengthBudget && current < m_impl->blocks.size()) {
            const QString instruction = budgetInstruction(m_impl->blocks.at(current));
            if (!instruction.isEmpty())
                extra.insert(QStringLiteral("instruction"), instruction);
        }
    }

    QPointer<ImageTranslationPipeline> self(this);
    m_impl->context->translation->translateDocumentSegment(
        source, m_impl->sourceCode, m_impl->targetCode, m_impl->providerId,
        [self, current](const QJsonObject& result) {
            if (!self || self->m_impl->cancelled || self->m_impl->failed)
                return;

            const QString error = result.value(QStringLiteral("error")).toString();
            if (!error.isEmpty()) {
                self->m_impl->failed = true;
                emit self->failed(error);
                return;
            }

            const QString translated = result.value(QStringLiteral("text")).toString().trimmed();
            self->m_impl->blocks[current].translated = translated;
            if (!self->m_impl->translatedText.isEmpty())
                self->m_impl->translatedText += QLatin1Char('\n');
            self->m_impl->translatedText += translated;

            ++self->m_impl->index;
            emit self->progress(self->m_impl->index, int(self->m_impl->blockSources.size()),
                                self->m_impl->blockSources.at(current).left(60));
            QTimer::singleShot(0, self, [self] { self->stepTranslate(); });
        },
        extra);
}

void ImageTranslationPipeline::finish()
{
    if (m_impl->cancelled || m_impl->failed)
        return;

    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    OverlayOptions options;
    if (m_impl->context && m_impl->context->settings) {
        SettingsManager* settings = m_impl->context->settings;
        const QString family = settings->stringValue(QStringLiteral("Image.TextFont"));
        if (!family.isEmpty())
            font = QFont(family);
        options.minPixelSize = settings->intValue(QStringLiteral("Image.MinFontPx"), 0);
        options.maxLines = settings->intValue(QStringLiteral("Image.MaxLines"), 8);
        const QJsonValue spacing = settings->value(QStringLiteral("Image.LineSpacing"));
        if (spacing.isDouble())
            options.lineSpacing = spacing.toDouble();
        const QString erase = settings->stringValue(QStringLiteral("Image.EraseMode"));
        if (!erase.isEmpty())
            options.eraseMode = erase;
        options.textOutline =
            settings->boolValue(QStringLiteral("Image.TextOutline"), false);
    }
    if (options.minPixelSize <= 0)
        options.minPixelSize = qMax(7, m_impl->image.width() / 110);

    QVector<OverlayReport> reports;
    const QImage translated = renderOverlay(m_impl->image, m_impl->blocks, font, options,
                                            &reports);

    if (!m_layoutDumpPath.isEmpty()) {
        QJsonObject root;
        root.insert(QStringLiteral("imageWidth"), m_impl->image.width());
        root.insert(QStringLiteral("imageHeight"), m_impl->image.height());
        root.insert(QStringLiteral("minFontPx"), options.minPixelSize);
        root.insert(QStringLiteral("maxLines"), options.maxLines);
        QJsonArray bubbles;
        for (const TextBubble& bubble : std::as_const(m_impl->bubbles)) {
            QJsonObject entry;
            QJsonArray box;
            box.append(bubble.box.x());
            box.append(bubble.box.y());
            box.append(bubble.box.width());
            box.append(bubble.box.height());
            entry.insert(QStringLiteral("box"), box);
            entry.insert(QStringLiteral("widestRow"), bubble.area.widestRow());
            bubbles.append(entry);
        }
        root.insert(QStringLiteral("bubbles"), bubbles);
        QJsonArray rawLines;
        for (const QString& line : std::as_const(m_impl->lineDump))
            rawLines.append(line);
        root.insert(QStringLiteral("ocrLines"), rawLines);
        QJsonArray recoveryBoxes;
        for (const QRect& region : std::as_const(m_impl->recoveryDump)) {
            QJsonArray box;
            box.append(region.x());
            box.append(region.y());
            box.append(region.width());
            box.append(region.height());
            recoveryBoxes.append(box);
        }
        root.insert(QStringLiteral("recoveryRegions"), recoveryBoxes);
        root.insert(QStringLiteral("recoveredLines"), m_impl->recoveredLines);
        root.insert(QStringLiteral("tileJobs"), m_impl->tileJobs);
        root.insert(QStringLiteral("inkJobs"), m_impl->inkJobs);
        root.insert(QStringLiteral("bubbleJobs"), m_impl->bubbleJobs);
        root.insert(QStringLiteral("sweepJobs"), m_impl->sweepJobs);
        root.insert(QStringLiteral("sweepBubbles"), m_impl->sweepBubbles);
        root.insert(QStringLiteral("letterBlobs"), m_impl->letterBlobs);
        root.insert(QStringLiteral("letterClusters"), m_impl->letterClusters);
        QJsonArray blocks;
        for (int index = 0; index < reports.size(); ++index) {
            const OverlayReport& report = reports.at(index);
            QJsonObject entry;
            QJsonArray box;
            box.append(report.box.x());
            box.append(report.box.y());
            box.append(report.box.width());
            box.append(report.box.height());
            entry.insert(QStringLiteral("box"), box);
            entry.insert(QStringLiteral("masked"), report.masked);
            entry.insert(QStringLiteral("fontPx"), report.pixelSize);
            entry.insert(QStringLiteral("lines"), report.lineCount);
            entry.insert(QStringLiteral("sourceLineHeight"), report.sourceLineHeight);
            entry.insert(QStringLiteral("raggedness"), report.raggedness);
            entry.insert(QStringLiteral("overflow"), report.overflow);
            entry.insert(QStringLiteral("fontFamily"), report.fontFamily);
            entry.insert(QStringLiteral("status"), report.status);
            if (index < m_impl->blockSources.size())
                entry.insert(QStringLiteral("source"),
                             m_impl->blockSources.at(index).left(200));
            if (index < m_impl->blocks.size())
                entry.insert(QStringLiteral("translated"),
                             m_impl->blocks.at(index).translated.left(200));
            blocks.append(entry);
        }
        root.insert(QStringLiteral("blocks"), blocks);
        QFile file(m_layoutDumpPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        }

        // Visual diagnostics: every bubble mask tinted over the page.
        QImage overlay = m_impl->image.convertToFormat(QImage::Format_ARGB32);
        QPainter painter(&overlay);
        painter.setRenderHint(QPainter::Antialiasing, false);
        for (const OverlayLine& block : std::as_const(m_impl->blocks)) {
            if (block.fillMask.isNull())
                continue;
            QImage layer(block.box.size(), QImage::Format_ARGB32_Premultiplied);
            layer.fill(QColor(255, 0, 0, 120));
            QPainter layerPainter(&layer);
            layerPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            layerPainter.drawImage(0, 0, block.fillMask);
            layerPainter.end();
            painter.drawImage(block.box.topLeft(), layer);
            painter.setPen(QPen(QColor(0, 120, 255), 2));
            painter.drawRect(block.box);
        }
        painter.end();
        overlay.save(m_layoutDumpPath + QStringLiteral(".masks.png"), "PNG");
    }

    emit finished(translated, m_impl->sourceText, m_impl->translatedText);
}





