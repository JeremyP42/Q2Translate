#include "image_overlay.h"

#include <QColor>
#include <QFontMetricsF>
#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kSampleDepth = 4;

double luminance(const QColor& color)
{
    return 0.299 * color.red() + 0.587 * color.green() + 0.114 * color.blue();
}

// The font must render every script present in the text; otherwise Qt draws the
// empty-box glyph. Resolve the family before typesetting so the metrics used for
// fitting are the metrics used for drawing.
bool coversText(const QFont& font, const QString& text)
{
    ushort probes[6] = { 0, 0, 0, 0, 0, 0 };
    for (const QChar& character : text) {
        const ushort value = character.unicode();
        ushort probe = 0;
        if (value >= 0x0400 && value <= 0x04FF)
            probe = 0x042F;  // Cyrillic
        else if ((value >= 0x3040 && value <= 0x30FF) || (value >= 0x3400 && value <= 0x9FFF))
            probe = 0x4E00;  // CJK
        else if (value >= 0x0590 && value <= 0x05FF)
            probe = 0x05D0;  // Hebrew
        else if (value >= 0x0600 && value <= 0x06FF)
            probe = 0x0645;  // Arabic
        else if (value >= 0x0370 && value <= 0x03FF)
            probe = 0x0391;  // Greek
        else if (value >= 0x0041 && value <= 0x024F)
            probe = 0x0041;  // Latin
        if (probe == 0)
            continue;
        bool known = false;
        for (const ushort existing : probes) {
            if (existing == probe) {
                known = true;
                break;
            }
        }
        if (known)
            continue;
        for (ushort& slot : probes) {
            if (slot == 0) {
                slot = probe;
                break;
            }
        }
    }

    const QFontMetricsF metrics(font);
    for (const ushort probe : probes) {
        if (probe == 0)
            continue;
        if (!metrics.inFont(QChar(probe)))
            return false;
    }
    return true;
}

QFont fontForText(const QFont& base, const QString& text)
{
    // "Comic" is a friendly alias for a hand-lettered manga font: the first
    // installed family wins, and if none covers the text the normal fallbacks run.
    if (base.family() == QLatin1String("Comic")) {
        static const char* kComic[] = { "Comic Neue", "Comic Sans MS", "Segoe Print",
                                        "Comic Relief" };
        for (const char* family : kComic) {
            QFont candidate(QString::fromLatin1(family));
            candidate.setBold(base.bold());
            candidate.setItalic(base.italic());
            if (coversText(candidate, text))
                return candidate;
        }
    }
    if (coversText(base, text))
        return base;
    static const char* kFallbacks[] = { "Segoe UI", "DejaVu Sans", "Noto Sans", "Arial",
                                        "Tahoma", "Microsoft YaHei", "Yu Gothic", "PT Sans" };
    for (const char* family : kFallbacks) {
        QFont candidate(QString::fromLatin1(family));
        if (coversText(candidate, text))
            return candidate;
    }
    return base;
}

// Flat background colour of a bubble: the most common non-dark colour of the
// mask interior. Sampling the mask's own pixels avoids the dark outline.
QColor interiorColor(const QImage& page, const QRect& box, const QImage& mask)
{
    QHash<QRgb, int> counts;
    if (!mask.isNull()) {
        QImage source = mask;
        if (source.size() != box.size())
            source = source.scaled(box.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (source.format() != QImage::Format_ARGB32_Premultiplied)
            source = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        for (int row = 0; row < box.height(); ++row) {
            const QRgb* pixels = reinterpret_cast<const QRgb*>(source.constScanLine(row));
            const int y = box.top() + row;
            if (y < 0 || y >= page.height())
                continue;
            for (int column = 0; column < box.width(); ++column) {
                if (qAlpha(pixels[column]) < 128)
                    continue;
                const QColor color = page.pixelColor(box.left() + column, y);
                if (luminance(color) < 110)
                    continue;
                ++counts[color.rgb() | 0xff000000];
            }
        }
    }

    if (counts.isEmpty()) {
        const QRect ring = box.adjusted(-2, -2, 2, 2).intersected(page.rect());
        for (int y = ring.top(); y <= ring.bottom(); ++y) {
            for (int x = ring.left(); x <= ring.right(); ++x) {
                if (box.contains(x, y))
                    continue;
                const QColor color = page.pixelColor(x, y);
                if (luminance(color) < 110)
                    continue;
                ++counts[color.rgb() | 0xff000000];
            }
        }
    }
    if (counts.isEmpty())
        return QColor(Qt::white);

    QRgb best = Qt::white;
    int bestCount = -1;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() > bestCount) {
            bestCount = it.value();
            best = it.key();
        }
    }
    return QColor(best);
}

// Median colour of up to `kSampleDepth` pixels walking away from the block edge.
QColor edgeColor(const QImage& page, int y, int xStart, int step)
{
    QVector<QColor> colors;
    int x = xStart;
    for (int index = 0; index < kSampleDepth; ++index) {
        if (x < 0 || x >= page.width())
            break;
        colors.append(page.pixelColor(x, y));
        x += step;
    }
    if (colors.isEmpty())
        return QColor();
    std::sort(colors.begin(), colors.end(), [](const QColor& left, const QColor& right) {
        return luminance(left) < luminance(right);
    });
    return colors.at(colors.size() / 2);
}

// Overall colour around a plain block: decides whether the translation is drawn
// black on a light patch or white on a dark one.
QColor averageEdgeColor(const QImage& page, const QRect& box)
{
    const int rows[3] = { box.top(), box.center().y(), box.bottom() };
    int red = 0;
    int green = 0;
    int blue = 0;
    int samples = 0;
    for (const int y : rows) {
        const QColor left = edgeColor(page, y, box.left() - 1, -1);
        const QColor right = edgeColor(page, y, box.right() + 1, 1);
        const QColor colors[2] = { left, right };
        for (const QColor& color : colors) {
            if (!color.isValid())
                continue;
            red += color.red();
            green += color.green();
            blue += color.blue();
            ++samples;
        }
    }
    if (samples == 0)
        return QColor(Qt::white);
    return QColor(red / samples, green / samples, blue / samples);
}

// Colour of the text on top of the erased patch: black on light paper, white on
// dark artwork. Sampled from the rendered background, not from the source, so the
// choice follows the pixels the text will actually sit on. A busy background also
// asks for a contrasting outline around the glyphs.
struct BackgroundSample {
    QColor color = Qt::black;
    bool busy = false;
    int meanLuminance = 128;
};

BackgroundSample sampleBackground(const QImage& image, const QRect& box)
{
    BackgroundSample sample;
    const QRect area = box.intersected(image.rect());
    if (area.isEmpty())
        return sample;

    qint64 sum = 0;
    qint64 sumSquares = 0;
    int count = 0;
    const int stepX = qMax(1, area.width() / 32);
    const int stepY = qMax(1, area.height() / 16);
    for (int y = area.top(); y <= area.bottom(); y += stepY) {
        for (int x = area.left(); x <= area.right(); x += stepX) {
            const int value = qGray(image.pixel(x, y));
            sum += value;
            sumSquares += qint64(value) * value;
            ++count;
        }
    }
    if (count == 0)
        return sample;

    const double mean = double(sum) / count;
    const double variance = qMax(0.0, double(sumSquares) / count - mean * mean);
    sample.color = mean < 128 ? QColor(Qt::white) : QColor(Qt::black);
    sample.meanLuminance = int(mean);
    sample.busy = std::sqrt(variance) > 24.0;
    return sample;
}

// All-caps dialogue is shouting in comic lettering: draw it bold.
bool isShouting(const QString& text)
{
    int letters = 0;
    for (const QChar& character : text) {
        if (!character.isLetter())
            continue;
        ++letters;
        if (character.isLower())
            return false;
    }
    return letters >= 2;
}

void drawTextLine(QPainter& painter, const QPointF& origin, const QString& text,
                  const QColor& color, const QColor& outline)
{
    if (!outline.isValid()) {
        painter.setPen(color);
        painter.drawText(origin, text);
        return;
    }
    QPainterPath path;
    path.addText(origin, painter.font(), text);
    painter.setPen(QPen(outline, 1.6));
    painter.setBrush(color);
    painter.drawPath(path);
    painter.setBrush(Qt::NoBrush);
}

void eraseMasked(QPainter& painter, const QRect& box, const QImage& mask,
                 const QColor& background)
{
    QImage source = mask;
    if (source.size() != box.size())
        source = source.scaled(box.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (source.format() != QImage::Format_ARGB32_Premultiplied)
        source = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QImage patch(box.size(), QImage::Format_ARGB32_Premultiplied);
    patch.fill(background);
    {
        QPainter patchPainter(&patch);
        patchPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        patchPainter.drawImage(0, 0, source);
    }
    painter.drawImage(box.topLeft(), patch);
}

// Reconstructs the background under a text box by solving Laplace's equation
// between the surrounding pixels (harmonic inpainting). It keeps gradients and
// tone changes across the box, unlike a flat fill, without pulling in OpenCV.
QImage inpaintBox(const QImage& page, const QRect& box)
{
    const int width = box.width();
    const int height = box.height();
    if (width <= 0 || height <= 0)
        return QImage();

    // One-pixel border holds the known pixels around the box.
    QImage patch(width + 2, height + 2, QImage::Format_ARGB32_Premultiplied);
    for (int y = -1; y <= height; ++y) {
        for (int x = -1; x <= width; ++x) {
            const int px = qBound(0, box.left() + x, page.width() - 1);
            const int py = qBound(0, box.top() + y, page.height() - 1);
            patch.setPixel(x + 1, y + 1, page.pixel(px, py));
        }
    }

    const int iterations = qBound(40, qMax(width, height) / 2, 160);
    for (int step = 0; step < iterations; ++step) {
        for (int y = 1; y <= height; ++y) {
            for (int x = 1; x <= width; ++x) {
                const QRgb left = patch.pixel(x - 1, y);
                const QRgb right = patch.pixel(x + 1, y);
                const QRgb up = patch.pixel(x, y - 1);
                const QRgb down = patch.pixel(x, y + 1);
                patch.setPixel(x, y,
                               qRgba((qRed(left) + qRed(right) + qRed(up) + qRed(down)) / 4,
                                     (qGreen(left) + qGreen(right) + qGreen(up) + qGreen(down)) / 4,
                                     (qBlue(left) + qBlue(right) + qBlue(up) + qBlue(down)) / 4,
                                     255));
            }
        }
    }
    return patch.copy(1, 1, width, height);
}

// Median colour of a horizontal band (a few samples across the box width).
QColor bandColor(const QImage& page, int y, const QRect& box)
{
    if (y < 0 || y >= page.height())
        return QColor();
    QVector<QColor> colors;
    for (int step = 0; step <= 4; ++step) {
        const int x = box.left() + box.width() * step / 4;
        if (x < 0 || x >= page.width())
            continue;
        colors.append(page.pixelColor(x, y));
    }
    if (colors.isEmpty())
        return QColor();
    std::sort(colors.begin(), colors.end(), [](const QColor& left, const QColor& right) {
        return luminance(left) < luminance(right);
    });
    return colors.at(colors.size() / 2);
}

QColor medianColor(QVector<QColor> colors)
{
    if (colors.isEmpty())
        return QColor();
    std::sort(colors.begin(), colors.end(), [](const QColor& left, const QColor& right) {
        return luminance(left) < luminance(right);
    });
    return colors.at(colors.size() / 2);
}

void eraseRect(QPainter& painter, const QImage& page, const QRect& box, const QString& mode)
{
    // Sample all four sides: a caption on a black panel may have a white page
    // margin on one side, and a horizontal-only gradient then paints a white bar.
    const QColor top = bandColor(page, box.top() - 2, box);
    const QColor bottom = bandColor(page, box.bottom() + 2, box);
    const QColor left = edgeColor(page, box.center().y(), box.left() - 1, -1);
    const QColor right = edgeColor(page, box.center().y(), box.right() + 1, 1);

    QVector<QColor> sides;
    for (const QColor& color : { top, bottom, left, right }) {
        if (color.isValid())
            sides.append(color);
    }
    const QColor fallback = interiorColor(page, box, QImage());
    const QColor base = sides.isEmpty() ? fallback : medianColor(sides);
    const QRect clipped = box.intersected(page.rect());
    if (clipped.isEmpty())
        return;

    QString effective = mode;
    if (effective.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        // A flat background (page margin, black caption panel) is best served by
        // a flat fill; a photo or artwork needs the diffusion fill.
        int minimum = 255;
        int maximum = 0;
        for (const QColor& color : std::as_const(sides)) {
            minimum = qMin(minimum, int(luminance(color)));
            maximum = qMax(maximum, int(luminance(color)));
        }
        const int baseLuminance = int(luminance(base));
        if (maximum - minimum <= 28) {
            effective = QStringLiteral("fill");
        } else if (baseLuminance >= 170 && maximum >= 200) {
            // Bright paper with a soft gradient (scan shading, sky): a flat fill
            // keeps the balloon white, diffusion would drag dark artwork in.
            effective = QStringLiteral("fill");
        } else if (maximum - minimum <= 70) {
            effective = QStringLiteral("smooth");
        } else {
            effective = QStringLiteral("inpaint");
        }
    }

    if (effective.compare(QStringLiteral("inpaint"), Qt::CaseInsensitive) == 0) {
        const QImage patch = inpaintBox(page, box);
        if (!patch.isNull()) {
            painter.drawImage(box.topLeft(), patch);
            return;
        }
    }

    const bool smooth = effective.compare(QStringLiteral("smooth"), Qt::CaseInsensitive) == 0;
    if (!smooth) {
        painter.fillRect(clipped, base);
        return;
    }

    const QColor from = top.isValid() ? top : base;
    const QColor to = bottom.isValid() ? bottom : from;
    // A caption that touches two very different backgrounds (black panel and a
    // white page margin) would otherwise get a visible light bar: use the
    // dominant side colour instead of a steep gradient.
    if (std::abs(int(luminance(from)) - int(luminance(to))) > 70) {
        painter.fillRect(clipped, base);
        return;
    }
    QLinearGradient gradient(QPointF(clipped.left(), clipped.top()),
                             QPointF(clipped.left(), clipped.bottom() + 1));
    gradient.setColorAt(0.0, from);
    gradient.setColorAt(1.0, to);
    painter.fillRect(clipped, gradient);
}

}

QImage renderOverlay(const QImage& page, const QVector<OverlayLine>& lines, const QFont& baseFont,
                     const OverlayOptions& options, QVector<OverlayReport>* report)
{
    if (page.isNull())
        return page;

    QImage result = page.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage source = result;  // sampling colours from the untouched page

    struct Prepared {
        const OverlayLine* line = nullptr;
        TextArea area;
        TypesetResult layout;
        QFont font;
        QColor background = Qt::white;
        QColor textColor = Qt::black;
        QColor outline;  // valid = draw a contrasting halo around the glyphs
        bool masked = false;
    };
    QVector<Prepared> prepared;
    prepared.reserve(lines.size());

    QFont base = baseFont;
    if (base.family().isEmpty())
        base = QFont(QStringLiteral("Segoe UI"));
    base.setBold(false);
    base.setItalic(false);

    // Pass 1: fit every block first. A block that cannot be fitted is skipped
    // entirely, so its original lettering is not erased. One report entry per
    // input line keeps the diagnostics aligned with the pipeline's blocks.
    for (const OverlayLine& line : lines) {
        OverlayReport info;
        info.box = line.box;
        info.sourceLineHeight = line.sourceLineHeight;
        info.masked = !line.fillMask.isNull();

        const auto reportStatus = [report, &info](const char* status) {
            info.status = QString::fromLatin1(status);
            if (report)
                report->append(info);
        };

        if (line.translated.trimmed().isEmpty()) {
            reportStatus("empty");
            continue;
        }
        const QRect box = line.box.intersected(result.rect());
        info.box = box;
        if (box.width() < 8 || box.height() < 8) {
            reportStatus("small");
            continue;
        }

        const bool masked = !line.fillMask.isNull();
        TextArea area;
        if (masked) {
            area = buildTextArea(line.fillMask, box);
        } else {
            area = rectangularArea(box);
            if (!line.rows.isEmpty() && line.rows.size() == box.height()) {
                area.rows = line.rows;
                area.valid = area.widestRow() >= 8 && area.height() >= 8;
            }
        }
        if (area.isEmpty()) {
            reportStatus("no-area");
            continue;
        }

        QFont font = fontForText(base, line.translated);
        if (isShouting(line.translated))
            font.setBold(true);

        TypesetPolicy policy;
        policy.minPixelSize = qMax(6, options.minPixelSize);
        policy.maxLines = qMax(1, options.maxLines);
        policy.lineSpacing = options.lineSpacing > 0.5 ? options.lineSpacing : 1.0;
        // A balloon with one short line of source text still has room for large
        // letters; captions keep a tighter cap so they never outgrow the artwork.
        const qreal capFactor = masked ? 2.5 : 1.9;
        policy.maxPixelSize = line.sourceLineHeight > 0
            ? int(std::lround(line.sourceLineHeight * capFactor))
            : 0;

        const TypesetResult layout = typesetText(line.translated, area, font, policy);
        if (layout.lines.isEmpty()) {
            // Nothing fits: keep the original text instead of a blank box.
            reportStatus("no-fit");
            continue;
        }

        Prepared entry;
        entry.line = &line;
        entry.area = area;
        entry.layout = layout;
        entry.font = font;
        entry.masked = masked;
        entry.background = masked ? interiorColor(source, box, line.fillMask)
                                  : averageEdgeColor(source, box);
        prepared.append(entry);

        info.pixelSize = layout.pixelSize;
        info.lineCount = int(layout.lines.size());
        info.raggedness = layout.raggedness;
        info.overflow = layout.overflow;
        info.fontFamily = entry.font.family();
        reportStatus("drawn");
    }

    // Pass 2: erase the original lettering of every prepared block.
    {
        QPainter eraser(&result);
        eraser.setRenderHint(QPainter::Antialiasing, false);
        for (const Prepared& entry : prepared) {
            if (entry.masked)
                eraseMasked(eraser, entry.area.box, entry.line->fillMask, entry.background);
            else
                eraseRect(eraser, source, entry.area.box, options.eraseMode);
        }
    }

    // Pass 3: draw the fitted translations. The text colour follows the pixels it
    // will actually cover: the band occupied by the placed lines, not the whole box.
    for (Prepared& entry : prepared) {
        QRect band;
        for (const PlacedLine& line : entry.layout.lines) {
            const QRect box(entry.area.box.left() + line.left,
                            entry.area.box.top() + line.top,
                            qMax(1, line.right - line.left),
                            qMax(1, entry.layout.pixelSize));
            band = band.isNull() ? box : band.united(box);
        }
        const BackgroundSample sample = sampleBackground(result, band.isNull() ? entry.area.box : band);
        entry.textColor = sample.color;
        // On a mid-tone background neither black nor white has enough contrast:
        // add a thin halo there (and everywhere the photo is busy, if enabled).
        const bool ambiguous = sample.meanLuminance >= 95 && sample.meanLuminance <= 165;
        const bool wantOutline = options.textOutline ? sample.busy : false;
        entry.outline = !entry.masked && (wantOutline || ambiguous)
            ? (sample.color == QColor(Qt::black) ? QColor(Qt::white) : QColor(Qt::black))
            : QColor();
    }

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    for (const Prepared& entry : prepared) {
        QFont font = entry.font;
        font.setPixelSize(entry.layout.pixelSize);
        if (entry.layout.tracking != 1.0)
            font.setLetterSpacing(QFont::PercentageSpacing, 100.0 * entry.layout.tracking);
        painter.setFont(font);
        for (const PlacedLine& line : entry.layout.lines) {
            drawTextLine(painter,
                         QPointF(entry.area.box.left() + line.left,
                                 entry.area.box.top() + line.baseline),
                         line.text, entry.textColor, entry.outline);
        }
    }
    painter.end();
    return result;
}
