#pragma once

#include <QChar>
#include <QRect>
#include <QString>
#include <QVector>

// Structure of a Tesseract run: the plain text the engine renders, plus the
// word boxes it reports with the TSV output config. The boxes let callers keep
// reading order and rebuild tables without guessing from the text alone.

struct OcrWord {
    QString text;
    QRect box;
    double confidence = 0.0;  // 0..1, from the TSV "conf" column
    int block = 0;
    int paragraph = 0;
    int line = 0;
};

struct OcrDocument {
    QString text;
    QVector<OcrWord> words;
};

struct OcrLineBox {
    QString text;
    QRect box;
    double confidence = 0.0;  // weighted average of the line's words
    int block = 0;
    int paragraph = 0;
    int line = 0;
};

// Parses the TSV rows produced by `tesseract <image> stdout tsv`.
OcrDocument parseTesseractTsv(const QString& tsv);

// Groups words into the lines the engine reported, with their bounding boxes.
QVector<OcrLineBox> ocrLines(const OcrDocument& document);

// Rebuilds a grid from word positions and returns a Markdown table, or an
// empty string when the content does not look like a table. Two-column text is
// deliberately ignored: articles and sidebars are not tables.
QString tableMarkdown(const OcrDocument& document);
