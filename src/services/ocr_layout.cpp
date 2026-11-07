#include "ocr_layout.h"

#include <QStringList>

#include <algorithm>

namespace {
constexpr int kMinTableRows = 3;
constexpr int kMinTableColumns = 3;
constexpr int kMaxTableColumns = 8;
constexpr int kMaxWordsPerRow = 60;

struct Cell {
    QString text;
    int left = 0;
    int right = 0;
};

struct Row {
    QVector<Cell> cells;
    int top = 0;
    int bottom = 0;
    int center() const { return (top + bottom) / 2; }
};

QString escapeCell(const QString& text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    return escaped;
}

// Table borders and stray rules come back as words that carry only punctuation
// or as one-pixel lines. They inflate gaps between cells, so drop them first.
bool isNoise(const OcrWord& word)
{
    if (word.box.height() <= 2 || word.box.width() <= 2)
        return true;
    for (const QChar& character : word.text) {
        if (character.isLetterOrNumber())
            return false;
    }
    return true;
}

int medianWordHeight(const QVector<OcrWord>& words)
{
    QVector<int> heights;
    heights.reserve(words.size());
    for (const OcrWord& word : words)
        heights.append(word.box.height());
    if (heights.isEmpty())
        return 0;
    std::sort(heights.begin(), heights.end());
    return heights.at(heights.size() / 2);
}

QVector<Row> buildRows(const QVector<OcrWord>& words, int gapThreshold)
{
    QVector<Row> rows;
    QVector<QVector<OcrWord>> rowWords;

    for (const OcrWord& word : words) {
        int index = -1;
        const int center = word.box.center().y();
        for (int candidate = 0; candidate < rows.size(); ++candidate) {
            if (center >= rows.at(candidate).top && center <= rows.at(candidate).bottom) {
                index = candidate;
                break;
            }
        }
        if (index < 0) {
            rows.append(Row{ {}, word.box.top(), word.box.bottom() });
            rowWords.append(QVector<OcrWord>());
            index = rows.size() - 1;
        }
        rows[index].top = qMin(rows.at(index).top, word.box.top());
        rows[index].bottom = qMax(rows.at(index).bottom, word.box.bottom());
        rowWords[index].append(word);
    }

    // Words arrive sorted by top, which is not left-to-right inside a row; sort
    // each row before splitting it into cells.
    for (int index = 0; index < rows.size(); ++index) {
        QVector<OcrWord>& current = rowWords[index];
        std::sort(current.begin(), current.end(), [](const OcrWord& left, const OcrWord& right) {
            return left.box.left() < right.box.left();
        });
        for (const OcrWord& word : current) {
            if (!rows.at(index).cells.isEmpty()
                && word.box.left() - rows.at(index).cells.last().right < gapThreshold) {
                Cell& last = rows[index].cells.last();
                last.text += QLatin1Char(' ') + word.text;
                last.right = word.box.right();
            } else {
                rows[index].cells.append(Cell{ word.text, word.box.left(), word.box.right() });
            }
        }
    }
    return rows;
}

bool sameColumnLayout(const QVector<Row>& rows, int tolerance)
{
    if (rows.isEmpty())
        return false;
    const int columns = rows.first().cells.size();
    for (const Row& row : rows) {
        if (row.cells.size() != columns)
            return false;
        for (int i = 0; i < columns; ++i) {
            if (std::abs(row.cells.at(i).left - rows.first().cells.at(i).left) > tolerance)
                return false;
        }
    }
    return true;
}

}

namespace {

bool isCjkCharacter(QChar character)
{
    const ushort code = character.unicode();
    return (code >= 0x3040 && code <= 0x30FF)   // kana
        || (code >= 0x3400 && code <= 0x4DBF)   // CJK extension A
        || (code >= 0x4E00 && code <= 0x9FFF)   // CJK unified
        || (code >= 0xAC00 && code <= 0xD7AF)   // Hangul
        || (code >= 0xF900 && code <= 0xFAFF)   // compatibility ideographs
        || (code >= 0xFF00 && code <= 0xFFEF);  // fullwidth forms
}

// Joins two visual lines of the same paragraph. Line breaks inside a paragraph
// are a layout artifact, so they become a space; hyphenated words are glued back
// together, and CJK text gets no space at all.
QString joinProse(const QString& left, const QString& right)
{
    if (left.isEmpty())
        return right;
    if (right.isEmpty())
        return left;

    const QChar last = left.at(left.size() - 1);
    const QChar first = right.at(0);

    if (last == QLatin1Char('-') && left.size() > 1 && left.at(left.size() - 2).isLetter()
        && first.isLetter()) {
        return left.left(left.size() - 1) + right;
    }
    if (isCjkCharacter(last) && isCjkCharacter(first))
        return left + right;
    return left + QLatin1Char(' ') + right;
}

}

OcrDocument parseTesseractTsv(const QString& tsv)
{
    OcrDocument document;

    struct LineGroup {
        int block = 0;
        int paragraph = 0;
        int line = 0;
        QStringList words;
        int left = 0;
        int right = 0;
        int top = 0;
        int bottom = 0;
        int maxGap = 0;
    };
    QVector<LineGroup> groups;

    const QStringList rows = tsv.split(QLatin1Char('\n'));
    for (int index = 1; index < rows.size(); ++index) {
        const QStringList columns = rows.at(index).split(QLatin1Char('\t'));
        if (columns.size() < 12 || columns.at(0) != QLatin1String("5"))
            continue;

        const QString text = columns.at(11).trimmed();
        if (text.isEmpty())
            continue;

        OcrWord word;
        word.text = text;
        word.block = columns.at(2).toInt();
        word.paragraph = columns.at(3).toInt();
        word.line = columns.at(4).toInt();
        word.box = QRect(columns.at(6).toInt(), columns.at(7).toInt(),
                         columns.at(8).toInt(), columns.at(9).toInt());
        word.confidence = qBound(0.0, columns.at(10).toDouble() / 100.0, 1.0);
        document.words.append(word);

        if (groups.isEmpty() || groups.last().block != word.block
            || groups.last().paragraph != word.paragraph || groups.last().line != word.line) {
            LineGroup group;
            group.block = word.block;
            group.paragraph = word.paragraph;
            group.line = word.line;
            group.left = word.box.left();
            group.right = word.box.right();
            group.top = word.box.top();
            group.bottom = word.box.bottom();
            groups.append(group);
        } else {
            // The engine reports one line for text from separate speech bubbles that
            // happen to share a baseline. A wide gap between words marks the border,
            // so the line is split there and each bubble keeps its own text.
            const LineGroup& last = groups.last();
            const int gap = word.box.left() - last.right;
            if (gap > qMax(40, word.box.height() * 3)) {
                LineGroup group;
                group.block = word.block;
                group.paragraph = word.paragraph;
                group.line = word.line;
                group.left = word.box.left();
                group.right = word.box.right();
                group.top = word.box.top();
                group.bottom = word.box.bottom();
                groups.append(group);
            }
        }
        LineGroup& group = groups.last();
        if (!group.words.isEmpty()) {
            const int previousRight = group.right;
            group.maxGap = qMax(group.maxGap, word.box.left() - previousRight);
        }
        group.words.append(text);
        group.left = qMin(group.left, word.box.left());
        group.right = qMax(group.right, word.box.right());
        group.top = qMin(group.top, word.box.top());
        group.bottom = qMax(group.bottom, word.box.bottom());
    }

    // Table cells and sidebars create wide gaps inside a line; flowing prose does not.
    QVector<int> heights;
    heights.reserve(document.words.size());
    for (const OcrWord& word : std::as_const(document.words))
        heights.append(word.box.height());
    std::sort(heights.begin(), heights.end());
    const int medianHeight = heights.isEmpty() ? 12 : heights.at(heights.size() / 2);
    const int gapLimit = qMax(12, medianHeight * 2);

    int previousBlock = groups.isEmpty() ? -1 : groups.first().block;

    for (int index = 0; index < groups.size();) {
        const LineGroup& group = groups.at(index);

        // A paragraph is a run of lines that share the (block, paragraph) pair.
        int last = index;
        while (last + 1 < groups.size() && groups.at(last + 1).block == group.block
               && groups.at(last + 1).paragraph == group.paragraph) {
            ++last;
        }

        // Flowing prose wraps lines to a similar width and has no wide gaps inside
        // them. Tables, forms, code and menus leave such gaps or ragged lines, where
        // breaks carry meaning and must stay.
        int maximumWidth = 0;
        int wideGapLines = 0;
        for (int line = index; line <= last; ++line) {
            maximumWidth = qMax(maximumWidth, groups.at(line).right - groups.at(line).left);
            if (groups.at(line).maxGap > gapLimit)
                ++wideGapLines;
        }

        bool prose = last > index;
        if (wideGapLines * 4 > (last - index + 1))
            prose = false;
        for (int line = index; line <= last && prose; ++line) {
            const int width = groups.at(line).right - groups.at(line).left;
            if (line < last && width * 10 < maximumWidth * 6)
                prose = false;
            if (line > index
                && groups.at(line).top - groups.at(line - 1).bottom > medianHeight * 3 / 2) {
                prose = false;
            }
        }

        QString paragraphText;
        for (int line = index; line <= last; ++line) {
            const QString lineText = groups.at(line).words.join(QLatin1Char(' '));
            if (paragraphText.isEmpty())
                paragraphText = lineText;
            else if (prose)
                paragraphText = joinProse(paragraphText, lineText);
            else
                paragraphText += QLatin1Char('\n') + lineText;
        }

        if (!document.text.isEmpty()) {
            document.text += group.block == previousBlock ? QStringLiteral("\n")
                                                          : QStringLiteral("\n\n");
        }
        document.text += paragraphText;

        previousBlock = group.block;
        index = last + 1;
    }

    return document;
}

QVector<OcrLineBox> ocrLines(const OcrDocument& document)
{
    QVector<OcrLineBox> lines;
    for (const OcrWord& word : document.words) {
        if (lines.isEmpty() || lines.last().block != word.block
            || lines.last().paragraph != word.paragraph || lines.last().line != word.line) {
            OcrLineBox line;
            line.block = word.block;
            line.paragraph = word.paragraph;
            line.line = word.line;
            line.box = word.box;
            line.text = word.text;
            lines.append(line);
        } else {
            OcrLineBox& line = lines.last();
            line.text += QLatin1Char(' ') + word.text;
            line.box = line.box.united(word.box);
            line.confidence = qMax(line.confidence, word.confidence);
        }
    }
    return lines;
}

QString tableMarkdown(const OcrDocument& document)
{
    if (document.words.size() < kMinTableRows * kMinTableColumns)
        return {};

    QVector<OcrWord> words;
    words.reserve(document.words.size());
    for (const OcrWord& word : document.words) {
        if (!isNoise(word))
            words.append(word);
    }
    if (words.size() < kMinTableRows * kMinTableColumns)
        return {};

    std::sort(words.begin(), words.end(), [](const OcrWord& left, const OcrWord& right) {
        if (left.box.top() != right.box.top())
            return left.box.top() < right.box.top();
        return left.box.left() < right.box.left();
    });

    const int medianHeight = medianWordHeight(words);
    if (medianHeight <= 0)
        return {};

    int imageWidth = 0;
    for (const OcrWord& word : words)
        imageWidth = qMax(imageWidth, word.box.right());

    const int gapThreshold = qMax(10, medianHeight * 2);
    const int tolerance = qMax(12, imageWidth / 40);

    const QVector<Row> rows = buildRows(words, gapThreshold);


    QVector<Row> table;
    for (const Row& row : rows) {
        const int columns = row.cells.size();
        if (columns < kMinTableColumns || columns > kMaxTableColumns
            || row.cells.size() > kMaxWordsPerRow) {
            table.clear();
            continue;
        }
        if (!table.isEmpty() && table.last().cells.size() != columns) {
            table.clear();
        }
        table.append(row);
    }

    if (table.size() < kMinTableRows)
        return {};
    if (!sameColumnLayout(table, tolerance))
        return {};

    const int columns = table.first().cells.size();
    QString markdown;
    auto appendRow = [&markdown, columns](const Row& row) {
        markdown += QLatin1Char('|');
        for (int column = 0; column < columns; ++column) {
            markdown += QLatin1Char(' ');
            if (column < row.cells.size())
                markdown += escapeCell(row.cells.at(column).text);
            markdown += QStringLiteral(" |");
        }
        markdown += QLatin1Char('\n');
    };

    appendRow(table.first());
    markdown += QLatin1Char('|');
    for (int column = 0; column < columns; ++column)
        markdown += QStringLiteral(" --- |");
    markdown += QLatin1Char('\n');
    for (int index = 1; index < table.size(); ++index)
        appendRow(table.at(index));

    return markdown.trimmed();
}
