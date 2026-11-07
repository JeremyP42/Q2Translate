#pragma once

#include <QImage>
#include <QRect>
#include <QVector>

#include "modules/image/text_typesetter.h"

// A speech bubble found on the page: bounding box, solid interior mask used to
// erase the original lettering, and the eroded profile used for typesetting.
struct TextBubble {
    QRect box;
    QImage fillMask;
    TextArea area;

    bool valid() const { return !fillMask.isNull() && !area.isEmpty(); }
};

// Grows bubbles from recognized text. A block of text is a bubble member when the
// bright area around it is closed by a dark outline and is flat enough to be a
// balloon — a face, a shaded panel or the page background does not qualify.
class BubbleFinder
{
public:
    explicit BubbleFinder(const QImage& page);

    bool isValid() const { return !m_gray.isNull(); }

    // Bubble around a text box, or an invalid bubble when the text sits on the
    // artwork or on the page background.
    TextBubble around(const QRect& textBox) const;

    int brightThreshold() const { return m_threshold; }

    // Manga-like page: flat white paper with dark ink, not a photo or a screenshot.
    bool paperPage() const { return m_paper; }

    // Fallback balloon for text whose own outline is broken: an ellipse fitted
    // around the recognized lines and clipped by the bright paper around them.
    TextBubble synthetic(const QVector<QRect>& lineBoxes) const;

private:
    TextBubble grow(const QRect& textBox, int threshold) const;
    int localThreshold(const QRect& textBox) const;

    QImage m_gray;
    int m_threshold = 230;
    bool m_paper = false;
};

// Bright enclosed regions that contain dark text. Used to seed per-bubble OCR for
// engines without their own line detection.
QVector<TextBubble> detectTextBubbles(const QImage& page, int maxBubbles = 60);
