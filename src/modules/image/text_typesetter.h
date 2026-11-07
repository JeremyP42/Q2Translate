#pragma once

#include <QFont>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

// Mask-aware typesetting for translated comic text.
//
// A speech bubble is not a rectangle: the usable width changes from row to row.
// The old renderer fitted text into the bubble's bounding box and then clipped it
// with the bubble mask, so letters were cut by the contour or shrunk to an
// unreadable size. This module lays text out against the real per-row profile:
// every line gets the narrowest span of the rows it covers, and the font size is
// the largest one for which a valid line break exists.

struct RowSpan {
    int left = 0;
    int right = -1;  // inclusive

    bool valid() const { return right >= left; }
    int width() const { return valid() ? right - left + 1 : 0; }
    int center() const { return (left + right) / 2; }
};

// Usable text area: the block box plus the span of every row inside it.
struct TextArea {
    QRect box;
    QVector<RowSpan> rows;  // size == box.height()
    bool valid = false;

    int height() const { return int(rows.size()); }
    const RowSpan& span(int row) const;
    int widthAt(int row) const;
    int centerAt(int row) const;
    int widestRow() const;
    bool isEmpty() const { return !valid || rows.isEmpty(); }
};

// Builds the profile from an opaque mask (white = inside). Rows are filled from
// the first to the last opaque pixel, so letter-shaped holes in the mask do not
// become holes in the text area.
TextArea buildTextArea(const QImage& mask, const QRect& box);

// A rectangular area for text that does not sit in a bubble.
TextArea rectangularArea(const QRect& box);

struct TypesetPolicy {
    int minPixelSize = 10;
    int maxPixelSize = 0;  // 0 = derive from the area and the text
    int maxLines = 8;
    qreal lineSpacing = 1.0;  // multiplier of the font's natural line height
};

struct PlacedLine {
    QString text;
    int left = 0;
    int right = 0;
    int top = 0;       // relative to TextArea::box
    int baseline = 0;  // relative to TextArea::box
};

struct TypesetResult {
    QVector<PlacedLine> lines;
    int pixelSize = 0;
    qreal lineSpacing = 1.0;
    qreal tracking = 1.0;  // letter spacing factor (1.0 = normal)
    qreal raggedness = 0.0;
    bool overflow = false;
};

// Fits the text into the area: largest pixel size, balanced line breaks.
TypesetResult typesetText(const QString& text, const TextArea& area, const QFont& baseFont,
                          const TypesetPolicy& policy);
