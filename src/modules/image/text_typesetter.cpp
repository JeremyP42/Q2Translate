#include "text_typesetter.h"

#include <QChar>
#include <QFontMetricsF>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr qreal kInfinite = std::numeric_limits<qreal>::max() / 4.0;
constexpr qreal kChunkPenalty = 4000.0;

struct Token {
    QString text;
    qreal width = 0;
    bool glue = false;         // no space between this token and the previous one
    qreal breakPenalty = 0.0;  // cost of starting a line with this token
};

struct Placement {
    int top = 0;
    int height = 0;
    int available = 0;
};

bool isCjk(QChar character)
{
    const ushort value = character.unicode();
    return (value >= 0x3040 && value <= 0x30FF)      // kana
        || (value >= 0x3400 && value <= 0x9FFF)      // CJK ideographs
        || (value >= 0xAC00 && value <= 0xD7AF)      // hangul
        || (value >= 0xF900 && value <= 0xFAFF);
}

// Hyphens are natural break points ("ха-ха-ха", "что-то"): a narrow balloon can
// keep a large font by splitting the word instead of shrinking it. The break has
// a high cost, so it is only used when no single-line layout fits.
constexpr qreal kHyphenBreakPenalty = 5000.0;

bool isHyphen(QChar character)
{
    return character == QLatin1Char('-') || character == QChar(0x2010)
        || character == QChar(0x2011) || character == QChar(0x00AD);
}

QVector<Token> tokenizeWord(const QString& word, const QFontMetricsF& metrics)
{
    QVector<Token> tokens;
    QString segment;
    bool afterHyphen = false;

    const auto flushSegment = [&tokens, &segment, &metrics, &afterHyphen] {
        if (segment.isEmpty())
            return;
        QString run;
        const auto flushRun = [&tokens, &run, &metrics, &afterHyphen] {
            if (run.isEmpty())
                return;
            Token token;
            token.text = run;
            token.width = metrics.horizontalAdvance(run);
            token.glue = afterHyphen;
            token.breakPenalty = afterHyphen ? kHyphenBreakPenalty : 0.0;
            tokens.append(token);
            run.clear();
            afterHyphen = false;
        };
        for (const QChar& character : segment) {
            if (isCjk(character)) {
                flushRun();
                Token token;
                token.text = QString(character);
                token.width = metrics.horizontalAdvance(token.text);
                token.glue = afterHyphen;
                token.breakPenalty = afterHyphen ? kHyphenBreakPenalty : 0.0;
                tokens.append(token);
                afterHyphen = false;
            } else {
                run.append(character);
            }
        }
        flushRun();
        segment.clear();
    };

    for (const QChar& character : word) {
        segment.append(character);
        if (!isHyphen(character))
            continue;
        // Keep the hyphen at the end of the preceding piece.
        flushSegment();
        afterHyphen = true;
    }
    flushSegment();
    return tokens;
}

QVector<Token> tokenize(const QString& text, const QFontMetricsF& metrics)
{
    QVector<Token> tokens;
    const QStringList words = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    tokens.reserve(words.size());
    for (const QString& word : words) {
        const QVector<Token> wordTokens = tokenizeWord(word, metrics);
        for (int index = 0; index < wordTokens.size(); ++index) {
            Token token = wordTokens.at(index);
            token.glue = token.glue || index > 0;
            tokens.append(token);
        }
    }
    return tokens;
}

// Splits words wider than the area into measured chunks so a line break is
// possible inside them. Used only when normal wrapping does not fit.
QVector<Token> chunkTokens(const QVector<Token>& source, const QFontMetricsF& metrics,
                           qreal chunkWidth)
{
    if (chunkWidth < 8)
        return source;

    QVector<Token> result;
    for (const Token& token : source) {
        if (token.width <= chunkWidth) {
            result.append(token);
            continue;
        }
        QString chunk;
        bool first = true;
        for (const QChar& character : token.text) {
            const QString candidate = chunk + character;
            if (!chunk.isEmpty() && metrics.horizontalAdvance(candidate) > chunkWidth) {
                Token piece;
                piece.text = chunk;
                piece.width = metrics.horizontalAdvance(chunk);
                piece.glue = !first;
                piece.breakPenalty = first ? 0.0 : kChunkPenalty;
                result.append(piece);
                chunk.clear();
                first = false;
            }
            chunk.append(character);
        }
        if (!chunk.isEmpty()) {
            Token piece;
            piece.text = chunk;
            piece.width = metrics.horizontalAdvance(chunk);
            piece.glue = !first;
            piece.breakPenalty = first ? 0.0 : kChunkPenalty;
            result.append(piece);
        }
    }
    return result;
}

// Dynamic-programming line breaker: exactly `lineCount` lines, every line must fit
// its own width, cost is the sum of squared slack (balanced lines, no orphans).
bool solveBreaks(const QVector<Token>& tokens, qreal spaceWidth,
                 const QVector<int>& available, int lineCount,
                 QVector<int>* starts, qreal* costOut)
{
    const int count = int(tokens.size());
    if (lineCount <= 0 || count < lineCount)
        return false;

    QVector<qreal> prefix(count + 1, 0.0);
    for (int index = 0; index < count; ++index)
        prefix[index + 1] = prefix[index] + tokens.at(index).width;

    QVector<qreal> previous(count + 1, kInfinite);
    QVector<qreal> current(count + 1, kInfinite);
    QVector<QVector<int>> choice(lineCount, QVector<int>(count + 1, -1));
    previous[0] = 0.0;

    for (int line = 0; line < lineCount; ++line) {
        std::fill(current.begin(), current.end(), kInfinite);
        const qreal limit = qMax(4, available.at(line));
        for (int end = 1; end <= count; ++end) {
            qreal best = kInfinite;
            int bestStart = -1;
            for (int start = end - 1; start >= 0; --start) {
                const qreal width = prefix[end] - prefix[start]
                    + spaceWidth * (end - start - 1);
                if (width > limit)
                    break;
                if (previous[start] >= kInfinite)
                    continue;
                const qreal slack = limit - width;
                const qreal cost = previous[start] + slack * slack
                    + (start > 0 ? tokens.at(start).breakPenalty : 0.0);
                if (cost < best) {
                    best = cost;
                    bestStart = start;
                }
            }
            current[end] = best;
            choice[line][end] = bestStart;
        }
        previous = current;
    }

    if (previous[count] >= kInfinite)
        return false;

    starts->resize(lineCount);
    int end = count;
    for (int line = lineCount - 1; line >= 0; --line) {
        const int start = choice[line][end];
        if (start < 0)
            return false;
        (*starts)[line] = start;
        end = start;
    }
    *costOut = previous[count];
    return end == 0;
}

// Places `lineCount` line slots inside the mask: vertically centred over the
// rows wide enough to hold text, each slot limited by its narrowest row.
bool computePlacements(const TextArea& area, int lineHeight, int lineCount,
                       QVector<Placement>* placements)
{
    const int widest = area.widestRow();
    if (widest < 8)
        return false;

    const int threshold = qMax(8, int(widest * 0.30));
    int usableTop = -1;
    int usableBottom = -1;
    for (int row = 0; row < area.height(); ++row) {
        if (area.widthAt(row) < threshold)
            continue;
        if (usableTop < 0)
            usableTop = row;
        usableBottom = row;
    }
    if (usableTop < 0 || usableBottom <= usableTop)
        return false;

    const int bandHeight = lineHeight * lineCount;
    if (bandHeight > usableBottom - usableTop + 1)
        return false;

    const int bandTop = usableTop + (usableBottom - usableTop + 1 - bandHeight) / 2;
    placements->resize(lineCount);
    for (int line = 0; line < lineCount; ++line) {
        const int rowTop = bandTop + line * lineHeight;
        const int rowBottom = qMin(rowTop + lineHeight - 1, usableBottom);
        int available = widest;
        for (int row = rowTop; row <= rowBottom; ++row)
            available = qMin(available, area.widthAt(row));
        if (available < 12)
            return false;
        (*placements)[line] = Placement{ rowTop, lineHeight, available - 3 };
    }
    return true;
}

QString joinTokens(const QVector<Token>& tokens, int start, int end)
{
    QString text;
    for (int index = start; index < end; ++index) {
        const Token& token = tokens.at(index);
        const bool needsSpace = !text.isEmpty() && !token.glue
            && !isCjk(text.at(text.size() - 1)) && !isCjk(token.text.at(0));
        if (needsSpace)
            text += QLatin1Char(' ');
        text += token.text;
    }
    return text;
}

TypesetResult buildResult(const QVector<Token>& tokens, const QVector<int>& starts,
                          const QVector<Placement>& placements, const TextArea& area,
                          const QFontMetricsF& metrics, int pixelSize, qreal lineSpacing,
                          qreal tracking, qreal cost)
{
    TypesetResult result;
    result.pixelSize = pixelSize;
    result.lineSpacing = lineSpacing;
    result.tracking = tracking;
    result.raggedness = cost;

    const int count = int(tokens.size());
    const qreal ascent = metrics.ascent();
    const int textHeight = int(std::ceil(ascent + metrics.descent()));
    for (int line = 0; line < int(placements.size()); ++line) {
        const int start = starts.at(line);
        const int end = line + 1 < int(starts.size()) ? starts.at(line + 1) : count;

        PlacedLine placed;
        placed.text = joinTokens(tokens, start, end);
        const qreal width = metrics.horizontalAdvance(placed.text);
        const int midRow = qMin(area.height() - 1,
                                placements.at(line).top + placements.at(line).height / 2);
        const RowSpan span = area.span(midRow);
        int left = span.center() - int(width / 2.0);
        if (int(std::ceil(width)) <= span.width())
            left = qMax(span.left, qMin(left, span.right - int(std::ceil(width)) + 1));
        else
            left = span.left;
        placed.left = left;
        placed.right = left + int(std::ceil(width));
        placed.top = placements.at(line).top;
        const int offset = (placements.at(line).height - textHeight) / 2;
        placed.baseline = placed.top + qMax(0, offset) + int(std::lround(ascent));
        result.lines.append(placed);
    }
    return result;
}

// Tries one font size, returning the most balanced layout over all line counts.
bool layoutAtSize(const TextArea& area, const QFont& baseFont, int pixelSize,
                  const TypesetPolicy& policy, qreal tracking, const QVector<Token>& tokens,
                  TypesetResult* best)
{
    QFont font = baseFont;
    font.setPixelSize(pixelSize);
    if (tracking != 1.0)
        font.setLetterSpacing(QFont::PercentageSpacing, 100.0 * tracking);
    const QFontMetricsF metrics(font);
    const qreal spaceWidth = metrics.horizontalAdvance(QLatin1Char(' '));
    const int lineHeight = qMax(pixelSize,
                                int(std::lround(metrics.height() * policy.lineSpacing)));

    bool found = false;
    TypesetResult candidate;
    for (int lines = 1; lines <= policy.maxLines && lines <= int(tokens.size()); ++lines) {
        QVector<Placement> placements;
        if (!computePlacements(area, lineHeight, lines, &placements))
            continue;

        QVector<int> available;
        available.reserve(lines);
        for (const Placement& placement : placements)
            available.append(placement.available);

        QVector<int> starts;
        qreal cost = 0.0;
        if (!solveBreaks(tokens, spaceWidth, available, lines, &starts, &cost))
            continue;

        const TypesetResult layout = buildResult(tokens, starts, placements, area, metrics,
                                                 pixelSize, policy.lineSpacing, tracking, cost);
        if (!found
            || cost / lines < candidate.raggedness / qMax(1, int(candidate.lines.size()))) {
            candidate = layout;
            found = true;
        }
    }

    if (found)
        *best = candidate;
    return found;
}

}

const RowSpan& TextArea::span(int row) const
{
    static const RowSpan empty;
    if (row < 0 || row >= rows.size())
        return empty;
    return rows.at(row);
}

int TextArea::widthAt(int row) const
{
    return span(row).width();
}

int TextArea::centerAt(int row) const
{
    return span(row).center();
}

int TextArea::widestRow() const
{
    int widest = 0;
    for (const RowSpan& row : rows)
        widest = qMax(widest, row.width());
    return widest;
}

TextArea buildTextArea(const QImage& mask, const QRect& box)
{
    TextArea area;
    area.box = box;
    if (mask.isNull() || box.isEmpty())
        return area;

    QImage source = mask;
    if (source.size() != box.size())
        source = source.scaled(box.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (source.format() != QImage::Format_ARGB32_Premultiplied)
        source = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    area.rows.resize(box.height());
    for (int row = 0; row < box.height(); ++row) {
        const QRgb* pixels = reinterpret_cast<const QRgb*>(source.constScanLine(row));
        int left = -1;
        int right = -1;
        for (int column = 0; column < box.width(); ++column) {
            if (qAlpha(pixels[column]) < 128)
                continue;
            if (left < 0)
                left = column;
            right = column;
        }
        if (right - left + 1 >= 8)
            area.rows[row] = RowSpan{ left, right };
    }

    area.valid = area.widestRow() >= 8 && area.height() >= 8;
    return area;
}

TextArea rectangularArea(const QRect& box)
{
    TextArea area;
    area.box = box;
    if (box.isEmpty())
        return area;
    area.rows.resize(box.height());
    for (int row = 0; row < box.height(); ++row)
        area.rows[row] = RowSpan{ 0, box.width() - 1 };
    area.valid = box.width() >= 8 && box.height() >= 8;
    return area;
}

TypesetResult typesetText(const QString& text, const TextArea& area, const QFont& baseFont,
                          const TypesetPolicy& policy)
{
    TypesetResult empty;
    const QString normalized = text.simplified();
    if (normalized.isEmpty() || area.isEmpty())
        return empty;

    const int widest = area.widestRow();
    // The longest chunk the breaker cannot split: hyphens and CJK characters are
    // break points, so "ха-ха-ха" must not cap the font at its full length.
    int longestWord = 1;
    const QStringList words = normalized.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& word : words) {
        int run = 0;
        for (const QChar& character : word) {
            if (isHyphen(character) || isCjk(character)) {
                longestWord = qMax(longestWord, qMax(1, run));
                run = 0;
                if (isCjk(character))
                    longestWord = qMax(longestWord, 1);
                continue;
            }
            ++run;
        }
        longestWord = qMax(longestWord, qMax(1, run));
    }

    // Largest size that could conceivably fit: even the longest word has to fit
    // the widest row. 0.45 em is a safe lower bound for an average glyph width.
    int upperBound = int(widest / (0.45 * longestWord)) + 2;
    upperBound = qMin(upperBound, area.height() * 9 / 10);
    upperBound = qMin(upperBound, 160);
    if (policy.maxPixelSize > 0)
        upperBound = qMin(upperBound, policy.maxPixelSize);

    const int minPixelSize = qMax(4, policy.minPixelSize);
    upperBound = qMax(upperBound, minPixelSize);

    for (int pixelSize = upperBound; pixelSize >= minPixelSize; --pixelSize) {
        QFont font = baseFont;
        font.setPixelSize(pixelSize);
        const QFontMetricsF metrics(font);
        const QVector<Token> tokens = tokenize(normalized, metrics);
        if (tokens.isEmpty())
            return empty;
        TypesetResult best;
        if (layoutAtSize(area, baseFont, pixelSize, policy, 1.0, tokens, &best))
            return best;
    }

    // Slight negative tracking buys up to ~6% of line width before the font has
    // to shrink; it is invisible at these sizes.
    for (const qreal tracking : { 0.97, 0.94 }) {
        QFont tracked = baseFont;
        tracked.setPixelSize(minPixelSize);
        tracked.setLetterSpacing(QFont::PercentageSpacing, 100.0 * tracking);
        const QFontMetricsF metrics(tracked);
        const QVector<Token> tokens = tokenize(normalized, metrics);
        TypesetResult best;
        if (layoutAtSize(area, baseFont, minPixelSize, policy, tracking, tokens, &best))
            return best;
    }

    // Last resort: break inside long words so the text still stays inside the
    // contour. Marked as overflow so the pipeline can log it.
    {
        QFont emergency = baseFont;
        emergency.setPixelSize(minPixelSize);
        const QFontMetricsF metrics(emergency);
        const QVector<Token> tokens = tokenize(normalized, metrics);
        const QVector<Token> chunked = chunkTokens(tokens, metrics, widest * 0.92);
        if (!chunked.isEmpty()) {
            TypesetResult best;
            if (layoutAtSize(area, baseFont, minPixelSize, policy, 1.0, chunked, &best)) {
                best.overflow = true;
                return best;
            }
        }
    }

    return empty;
}
