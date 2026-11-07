#pragma once

#include <QFont>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include "modules/image/text_typesetter.h"

// Draws translated text over an image without touching the rest of the artwork.
//
// A block is either bubble-shaped (it carries the bubble's fill mask and the
// per-row text profile) or a plain rectangle. The original lettering is erased
// inside the block, then the translation is typeset against the real profile:
// lines follow the bubble contour instead of being clipped by it.

struct OverlayLine {
    QRect box;
    QString translated;
    // Height of one source text line: it caps the font so a translation is never
    // drawn much larger than the dialogue it replaces.
    int sourceLineHeight = 0;
    // Opaque interior of a speech bubble. Null for plain rectangular blocks.
    QImage fillMask;
    // Text profile aligned with box.topLeft(); empty = rectangular block.
    QVector<RowSpan> rows;
};

struct OverlayOptions {
    int minPixelSize = 10;
    int maxLines = 8;
    qreal lineSpacing = 1.0;
    // "fill" paints a flat background, "smooth" interpolates it between the left
    // and right neighbours of every row.
    QString eraseMode = QStringLiteral("smooth");
    // Draw a contrasting halo around glyphs that sit on busy artwork.
    bool textOutline = false;
};

struct OverlayReport {
    QRect box;
    int pixelSize = 0;
    int lineCount = 0;
    int sourceLineHeight = 0;
    qreal raggedness = 0.0;
    bool overflow = false;
    bool masked = false;
    QString fontFamily;
    QString status;  // "drawn", "empty", "small", "no-area" or "no-fit"
};

// The font must carry the target script; the caller passes the configured text
// font and renderOverlay falls back per block when a glyph is missing.
QImage renderOverlay(const QImage& page, const QVector<OverlayLine>& lines,
                     const QFont& baseFont, const OverlayOptions& options = OverlayOptions(),
                     QVector<OverlayReport>* report = nullptr);
