#include "bubble_detector.h"

#include <QVector>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kDarkThreshold = 110;
constexpr double kMinAreaRatio = 0.0008;  // of the page
constexpr double kMaxAreaRatio = 0.20;
constexpr double kMinFillRatio = 0.28;
constexpr double kMinDarkRatio = 0.0008;
constexpr double kMaxDarkRatio = 0.45;
constexpr double kMaxFlatness = 32.0;   // std deviation of interior brightness
constexpr double kMinEnclosure = 0.30;  // share of contour next to a dark line

struct Component {
    QRect box;
    int id = 0;
    int area = 0;
};

// Otsu split between ink and paper, clamped to the "paper" range: manga pages have
// white balloons on white paper, and screentone must not become a balloon.
int otsuPaperThreshold(const QImage& gray)
{
    int histogram[256] = { 0 };
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* line = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            ++histogram[line[x]];
    }
    const long long total = static_cast<long long>(gray.width()) * gray.height();
    if (total <= 0)
        return 190;

    double sum = 0;
    for (int value = 0; value < 256; ++value)
        sum += double(value) * histogram[value];

    double sumBackground = 0;
    long long weightBackground = 0;
    double bestVariance = -1;
    int best = 190;
    for (int value = 0; value < 256; ++value) {
        weightBackground += histogram[value];
        if (weightBackground == 0)
            continue;
        const long long weightForeground = total - weightBackground;
        if (weightForeground == 0)
            break;
        sumBackground += double(value) * histogram[value];
        const double meanBackground = sumBackground / weightBackground;
        const double meanForeground = (sum - sumBackground) / weightForeground;
        const double variance = double(weightBackground) * double(weightForeground)
            * (meanBackground - meanForeground) * (meanBackground - meanForeground);
        if (variance > bestVariance) {
            bestVariance = variance;
            best = value;
        }
    }
    return std::clamp(best, 175, 215);
}

QRect clampRect(const QRect& rect, const QSize& size)
{
    return rect.intersected(QRect(QPoint(0, 0), size));
}

// Fills only holes that are enclosed by the shape (the lettering inside a
// balloon), not concavities: a caption box with a bite taken out of it keeps its
// shape, so the artwork in the bite is not painted over.
std::vector<uchar> fillHoles(const std::vector<uchar>& binary, int width, int height)
{
    std::vector<uchar> outside(binary.size(), 0);
    std::vector<int> stack;
    const auto push = [&binary, &outside, &stack, width](int x, int y) {
        const int index = y * width + x;
        if (binary[static_cast<size_t>(index)] || outside[static_cast<size_t>(index)])
            return;
        outside[static_cast<size_t>(index)] = 1;
        stack.push_back(index);
    };
    for (int x = 0; x < width; ++x) {
        push(x, 0);
        push(x, height - 1);
    }
    for (int y = 0; y < height; ++y) {
        push(0, y);
        push(width - 1, y);
    }
    while (!stack.empty()) {
        const int index = stack.back();
        stack.pop_back();
        const int x = index % width;
        const int y = index / width;
        if (x > 0)
            push(x - 1, y);
        if (x + 1 < width)
            push(x + 1, y);
        if (y > 0)
            push(x, y - 1);
        if (y + 1 < height)
            push(x, y + 1);
    }

    std::vector<uchar> solid(binary);
    for (size_t index = 0; index < binary.size(); ++index) {
        if (!binary[index] && !outside[index])
            solid[index] = 1;
    }
    return solid;
}

std::vector<uchar> erodeBinary(const std::vector<uchar>& binary, int width, int height,
                               int radius)
{
    std::vector<uchar> current(binary);
    std::vector<uchar> buffer(binary.size(), 0);
    for (int step = 0; step < radius; ++step) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                uchar value = 1;
                for (int dy = -1; dy <= 1 && value; ++dy) {
                    for (int dx = -1; dx <= 1 && value; ++dx) {
                        const int sx = x + dx;
                        const int sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= width || sy >= height) {
                            value = 0;
                            break;
                        }
                        value = current[static_cast<size_t>(sy) * width + sx];
                    }
                }
                buffer[static_cast<size_t>(y) * width + x] = value;
            }
        }
        current.swap(buffer);
    }
    return current;
}

QImage maskFromBinary(const std::vector<uchar>& binary, int width, int height)
{
    QImage mask(width, height, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    for (int y = 0; y < height; ++y) {
        QRgb* pixels = reinterpret_cast<QRgb*>(mask.scanLine(y));
        for (int x = 0; x < width; ++x) {
            if (binary[static_cast<size_t>(y) * width + x])
                pixels[x] = 0xFFFFFFFF;
        }
    }
    return mask;
}

struct RegionStats {
    int area = 0;
    double mean = 0;
    double deviation = 0;
    double darkRatio = 0;
    double enclosure = 0;
};

// Measures a solid shape: brightness spread, ink share and how much of the
// contour touches a dark line (a balloon outline).
RegionStats analyseRegion(const QImage& gray, const QRect& box, const std::vector<uchar>& solid)
{
    RegionStats stats;
    if (solid.empty() || box.isEmpty())
        return stats;
    const int width = box.width();
    const int height = box.height();
    const int pageWidth = gray.width();
    const int pageHeight = gray.height();

    double sum = 0;
    double sumSquares = 0;
    long long bright = 0;
    int dark = 0;
    int boundary = 0;
    int enclosed = 0;

    for (int y = 0; y < height; ++y) {
        const int imageY = box.top() + y;
        if (imageY < 0 || imageY >= pageHeight)
            continue;
        const uchar* line = gray.constScanLine(imageY);
        for (int x = 0; x < width; ++x) {
            const int imageX = box.left() + x;
            if (imageX < 0 || imageX >= pageWidth)
                continue;
            if (line[imageX] < kDarkThreshold)
                ++dark;
            if (!solid[static_cast<size_t>(y) * width + x])
                continue;
            ++stats.area;
            if (line[imageX] >= kDarkThreshold) {
                sum += line[imageX];
                sumSquares += double(line[imageX]) * line[imageX];
                ++bright;
            }

            bool isBoundary = false;
            for (int dy = -1; dy <= 1 && !isBoundary; ++dy) {
                for (int dx = -1; dx <= 1 && !isBoundary; ++dx) {
                    const int sx = x + dx;
                    const int sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= width || sy >= height)
                        isBoundary = true;
                    else if (!solid[static_cast<size_t>(sy) * width + sx])
                        isBoundary = true;
                }
            }
            if (!isBoundary)
                continue;
            ++boundary;

            bool darkNearby = false;
            for (int dy = -2; dy <= 2 && !darkNearby; ++dy) {
                const int sy = imageY + dy;
                if (sy < 0 || sy >= pageHeight)
                    continue;
                const uchar* probe = gray.constScanLine(sy);
                for (int dx = -2; dx <= 2; ++dx) {
                    const int sx = imageX + dx;
                    if (sx < 0 || sx >= pageWidth)
                        continue;
                    if (probe[sx] < kDarkThreshold) {
                        darkNearby = true;
                        break;
                    }
                }
            }
            if (darkNearby)
                ++enclosed;
        }
    }

    if (bright > 0) {
        stats.mean = sum / double(bright);
        stats.deviation = std::sqrt(qMax(0.0, sumSquares / double(bright) - stats.mean * stats.mean));
    }
    const double boxArea = double(width) * height;
    stats.darkRatio = boxArea > 0 ? double(dark) / boxArea : 0.0;
    stats.enclosure = boundary > 0 ? double(enclosed) / boundary : 0.0;
    return stats;
}

// Builds a bubble from a solid shape. The fill mask keeps the whole interior
// (erasing the lettering down to the outline); the text profile is taken from an
// eroded copy so glyphs never touch the contour.
bool makeBubble(const QImage& gray, const QRect& box, const std::vector<uchar>& solid,
                TextBubble* bubble)
{
    if (solid.empty() || box.isEmpty())
        return false;

    const RegionStats stats = analyseRegion(gray, box, solid);
    if (stats.area <= 0)
        return false;
    const double fillRatio = double(stats.area) / (double(box.width()) * box.height());
    if (fillRatio < kMinFillRatio)
        return false;
    if (stats.deviation > kMaxFlatness)
        return false;
    if (stats.darkRatio < kMinDarkRatio || stats.darkRatio > kMaxDarkRatio)
        return false;
    if (stats.enclosure < kMinEnclosure)
        return false;

    const std::vector<uchar> eroded = erodeBinary(solid, box.width(), box.height(), 2);
    TextArea area = buildTextArea(maskFromBinary(eroded, box.width(), box.height()), box);
    if (area.isEmpty())
        return false;

    bubble->box = box;
    bubble->fillMask = maskFromBinary(solid, box.width(), box.height());
    bubble->area = area;
    return true;
}

}

BubbleFinder::BubbleFinder(const QImage& page)
{
    if (page.isNull())
        return;
    m_gray = page.convertToFormat(QImage::Format_Grayscale8);
    // A balloon is paper-white; the panel behind it is often light grey or a
    // screentone, so the growth threshold has to stay close to white. Otsu alone
    // splits the page into "ink" and "everything else" and lets the growth leak
    // through the anti-aliased outline into the artwork.
    const int otsu = otsuPaperThreshold(m_gray);
    m_threshold = std::clamp(qMax(otsu, 230), 200, 248);

    // Manga pages are flat paper with ink; photos have gradients and mid-tones.
    // Mean standard deviation of 8x8 tiles separates them reliably (manga 23-29,
    // photos 9-17 on the test set).
    const int tile = 8;
    double deviationSum = 0.0;
    long long tiles = 0;
    long long bright = 0;
    long long total = 0;
    for (int y = 0; y + tile <= m_gray.height(); y += tile) {
        for (int x = 0; x + tile <= m_gray.width(); x += tile) {
            double sum = 0;
            double sumSquares = 0;
            for (int dy = 0; dy < tile; ++dy) {
                const uchar* line = m_gray.constScanLine(y + dy);
                for (int dx = 0; dx < tile; ++dx) {
                    const int value = line[x + dx];
                    sum += value;
                    sumSquares += double(value) * value;
                    ++total;
                    if (value >= 220)
                        ++bright;
                }
            }
            const double mean = sum / (tile * tile);
            const double variance = qMax(0.0, sumSquares / (tile * tile) - mean * mean);
            deviationSum += std::sqrt(variance);
            ++tiles;
        }
    }
    const double meanDeviation = tiles > 0 ? deviationSum / double(tiles) : 0.0;
    const double brightRatio = total > 0 ? double(bright) / double(total) : 0.0;
    m_paper = meanDeviation >= 20.0 && brightRatio >= 0.40;
}

TextBubble BubbleFinder::synthetic(const QVector<QRect>& lineBoxes) const
{
    TextBubble result;
    if (!m_paper || !isValid() || lineBoxes.isEmpty())
        return result;

    QRect textBox;
    for (const QRect& box : lineBoxes)
        textBox = textBox.isNull() ? box : textBox.united(box);
    if (textBox.width() < 8 || textBox.height() < 6)
        return result;

    const int centerX = textBox.center().x();
    const int centerY = textBox.center().y();
    const int a = textBox.width() / 2 + int(textBox.width() * 0.18) + 6;
    const int b = textBox.height() / 2 + int(textBox.height() * 0.18) + 8;
    const QRect region = clampRect(QRect(centerX - a, centerY - b, a * 2, b * 2),
                                   m_gray.size());
    if (region.width() < 20 || region.height() < 20)
        return result;

    // Local paper level inside the fitted ellipse.
    long long histogram[256] = { 0 };
    long long total = 0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        const uchar* line = m_gray.constScanLine(y);
        for (int x = region.left(); x <= region.right(); ++x) {
            ++histogram[line[x]];
            ++total;
        }
    }
    if (total == 0)
        return result;
    long long accumulated = 0;
    int paper = 255;
    for (int value = 0; value < 256; ++value) {
        accumulated += histogram[value];
        if (accumulated * 10 >= total * 9) {
            paper = value;
            break;
        }
    }
    const int threshold = std::clamp(paper - 12, 190, 246);

    const int regionWidth = region.width();
    const int regionHeight = region.height();
    std::vector<uchar> binary(static_cast<size_t>(regionWidth) * regionHeight, 0);
    long long brightPixels = 0;
    long long ellipsePixels = 0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        const double normalized = double(y - centerY) / double(qMax(1, b));
        if (std::abs(normalized) >= 1.0)
            continue;
        const int halfWidth = int(a * std::sqrt(1.0 - normalized * normalized));
        const uchar* line = m_gray.constScanLine(y);
        for (int x = centerX - halfWidth; x <= centerX + halfWidth; ++x) {
            if (x < region.left() || x > region.right())
                continue;
            ++ellipsePixels;
            if (line[x] < threshold)
                continue;
            binary[static_cast<size_t>(y - region.top()) * regionWidth
                + (x - region.left())] = 1;
            ++brightPixels;
        }
    }
    if (ellipsePixels == 0 || double(brightPixels) / double(ellipsePixels) < 0.55)
        return result;

    const std::vector<uchar> solid = fillHoles(binary, regionWidth, regionHeight);

    // The fitted shape must still hold every recognized line.
    for (const QRect& box : lineBoxes) {
        int inside = 0;
        int probes = 0;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                const QPoint probe(box.left() + box.width() * column / 2,
                                   box.top() + box.height() * row / 2);
                ++probes;
                if (!region.contains(probe))
                    continue;
                if (solid[static_cast<size_t>(probe.y() - region.top()) * regionWidth
                          + (probe.x() - region.left())])
                    ++inside;
            }
        }
        if (inside * 3 < probes * 2)
            return TextBubble();
    }

    const std::vector<uchar> eroded = erodeBinary(solid, regionWidth, regionHeight, 2);
    TextArea area = buildTextArea(maskFromBinary(eroded, regionWidth, regionHeight), region);
    if (area.isEmpty())
        return result;

    result.box = region;
    result.fillMask = maskFromBinary(solid, regionWidth, regionHeight);
    result.area = area;
    return result;
}

TextBubble BubbleFinder::around(const QRect& textBox) const
{
    const int local = localThreshold(textBox);
    TextBubble bubble = grow(textBox, local);
    if (bubble.valid())
        return bubble;
    const int relaxed = qMax(190, local - 18);
    if (relaxed < local)
        bubble = grow(textBox, relaxed);
    return bubble.valid() ? bubble : TextBubble();
}

// Paper level around the text: the balloon interior is the bright population of
// the local histogram. A page-wide threshold fails on grey-ish scans, where the
// balloon is lighter than the panel but darker than pure white.
int BubbleFinder::localThreshold(const QRect& textBox) const
{
    if (!isValid())
        return m_threshold;
    const QRect area = clampRect(textBox.adjusted(-8, -8, 8, 8), m_gray.size());
    if (area.isEmpty())
        return m_threshold;

    long long histogram[256] = { 0 };
    long long total = 0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const uchar* line = m_gray.constScanLine(y);
        for (int x = area.left(); x <= area.right(); ++x) {
            ++histogram[line[x]];
            ++total;
        }
    }
    if (total == 0)
        return m_threshold;

    long long accumulated = 0;
    int paper = 255;
    for (int value = 0; value < 256; ++value) {
        accumulated += histogram[value];
        if (accumulated * 10 >= total * 9) {  // 90th percentile
            paper = value;
            break;
        }
    }
    return std::clamp(paper - 12, 190, 246);
}

TextBubble BubbleFinder::grow(const QRect& textBox, int threshold) const
{
    TextBubble result;
    if (!isValid() || textBox.isEmpty())
        return result;

    const int width = m_gray.width();
    const int height = m_gray.height();
    const QRect seedBox = clampRect(textBox.adjusted(-3, -3, 3, 3), m_gray.size());
    if (seedBox.isEmpty())
        return result;

    std::vector<uchar> visited(static_cast<size_t>(width) * height, 0);
    std::vector<int> stack;

    const auto isBright = [this, threshold](int x, int y) {
        return m_gray.constScanLine(y)[x] >= threshold;
    };

    for (int y = seedBox.top(); y <= seedBox.bottom(); ++y) {
        for (int x = seedBox.left(); x <= seedBox.right(); ++x) {
            if (!isBright(x, y))
                continue;
            const int index = y * width + x;
            if (visited[static_cast<size_t>(index)])
                continue;
            visited[static_cast<size_t>(index)] = 1;
            stack.push_back(index);
        }
    }
    if (stack.empty())
        return result;

    int area = 0;
    QRect region;
    bool touchesBorder = false;
    while (!stack.empty()) {
        const int index = stack.back();
        stack.pop_back();
        const int x = index % width;
        const int y = index / width;
        ++area;
        region = region.isNull() ? QRect(x, y, 1, 1) : region.united(QRect(x, y, 1, 1));
        if (x == 0 || y == 0 || x == width - 1 || y == height - 1)
            touchesBorder = true;

        const int neighbours[4] = { index - 1, index + 1, index - width, index + width };
        for (int slot = 0; slot < 4; ++slot) {
            const int next = neighbours[slot];
            if (next < 0 || next >= width * height)
                continue;
            if (slot < 2 && (next / width) != y)
                continue;  // horizontal neighbour wrapped to another row
            if (visited[static_cast<size_t>(next)])
                continue;
            const int nx = next % width;
            const int ny = next / width;
            if (!isBright(nx, ny))
                continue;
            visited[static_cast<size_t>(next)] = 1;
            stack.push_back(next);
        }
    }

    if (touchesBorder || area <= 0)
        return result;
    const double pageArea = double(width) * height;
    if (area > pageArea * 0.18)
        return result;
    if (area < double(textBox.width()) * textBox.height() * 1.15)
        return result;

    const int regionWidth = region.width();
    const int regionHeight = region.height();
    std::vector<uchar> binary(static_cast<size_t>(regionWidth) * regionHeight, 0);
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            const int index = y * width + x;
            if (!visited[static_cast<size_t>(index)])
                continue;
            binary[static_cast<size_t>(y - region.top()) * regionWidth + (x - region.left())] = 1;
        }
    }

    const std::vector<uchar> solid = fillHoles(binary, regionWidth, regionHeight);
    TextBubble bubble;
    if (!makeBubble(m_gray, region, solid, &bubble))
        return result;
    if (bubble.area.widestRow() < int(textBox.width() * 0.85))
        return result;
    return bubble;
}

QVector<TextBubble> detectTextBubbles(const QImage& page, int maxBubbles)
{
    QVector<TextBubble> result;
    if (page.isNull())
        return result;

    BubbleFinder finder(page);
    if (!finder.isValid())
        return result;
    const QImage sample = page.convertToFormat(QImage::Format_Grayscale8);
    const int width = sample.width();
    const int height = sample.height();
    if (width < 40 || height < 40)
        return result;

    const int threshold = finder.brightThreshold();
    const int pixelCount = width * height;
    std::vector<uchar> bright(static_cast<size_t>(pixelCount));
    for (int y = 0; y < height; ++y) {
        const uchar* line = sample.constScanLine(y);
        uchar* target = bright.data() + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x)
            target[x] = line[x] >= threshold ? 1 : 0;
    }

    std::vector<int> labels(static_cast<size_t>(pixelCount), 0);
    std::vector<int> stack;
    QVector<Component> components;
    int nextLabel = 0;

    for (int startY = 0; startY < height; ++startY) {
        for (int startX = 0; startX < width; ++startX) {
            const int start = startY * width + startX;
            if (bright[static_cast<size_t>(start)] == 0
                || labels[static_cast<size_t>(start)] != 0)
                continue;

            ++nextLabel;
            Component component;
            component.id = nextLabel;
            component.box = QRect(startX, startY, 1, 1);
            bool touchesBorder = false;
            stack.clear();
            stack.push_back(start);
            labels[static_cast<size_t>(start)] = nextLabel;

            while (!stack.empty()) {
                const int index = stack.back();
                stack.pop_back();
                const int x = index % width;
                const int y = index / width;
                ++component.area;
                if (x < component.box.left())
                    component.box.setLeft(x);
                if (x > component.box.right())
                    component.box.setRight(x);
                if (y < component.box.top())
                    component.box.setTop(y);
                if (y > component.box.bottom())
                    component.box.setBottom(y);
                if (x == 0 || y == 0 || x == width - 1 || y == height - 1)
                    touchesBorder = true;

                const int neighbours[4] = { index - 1, index + 1, index - width, index + width };
                for (int slot = 0; slot < 4; ++slot) {
                    const int next = neighbours[slot];
                    if (next < 0 || next >= pixelCount)
                        continue;
                    if (slot < 2 && (next / width) != y)
                        continue;
                    if (bright[static_cast<size_t>(next)] == 0
                        || labels[static_cast<size_t>(next)] != 0)
                        continue;
                    labels[static_cast<size_t>(next)] = nextLabel;
                    stack.push_back(next);
                }
            }

            if (!touchesBorder)
                components.append(component);
        }
    }

    for (const Component& component : std::as_const(components)) {
        const int boxWidth = component.box.width();
        const int boxHeight = component.box.height();
        if (boxWidth < 20 || boxHeight < 14)
            continue;
        const double areaRatio = double(component.area) / pixelCount;
        if (areaRatio < kMinAreaRatio || areaRatio > kMaxAreaRatio)
            continue;

        std::vector<uchar> binary(static_cast<size_t>(boxWidth) * boxHeight, 0);
        for (int y = component.box.top(); y <= component.box.bottom(); ++y) {
            for (int x = component.box.left(); x <= component.box.right(); ++x) {
                if (labels[static_cast<size_t>(y) * width + x] == component.id)
                    binary[static_cast<size_t>(y - component.box.top()) * boxWidth
                        + (x - component.box.left())] = 1;
            }
        }
        const std::vector<uchar> solid = fillHoles(binary, boxWidth, boxHeight);
        TextBubble bubble;
        if (!makeBubble(sample, component.box, solid, &bubble))
            continue;
        result.append(bubble);
        if (result.size() >= maxBubbles * 2)
            break;
    }

    std::sort(result.begin(), result.end(), [](const TextBubble& left, const TextBubble& right) {
        return left.box.width() * left.box.height() > right.box.width() * right.box.height();
    });

    QVector<TextBubble> unique;
    for (const TextBubble& bubble : std::as_const(result)) {
        bool overlaps = false;
        for (const TextBubble& existing : std::as_const(unique)) {
            const QRect intersection = existing.box.intersected(bubble.box);
            if (!intersection.isEmpty()
                && intersection.width() * intersection.height() * 2
                    > bubble.box.width() * bubble.box.height()) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps)
            unique.append(bubble);
        if (unique.size() >= maxBubbles)
            break;
    }
    return unique;
}


