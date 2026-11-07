#include "image_deskew.h"

#include <QPainter>
#include <QVector>

#include "image_deskew.h"

#include <cmath>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Angles beyond this range are rare for captures and cost accuracy on the
// projection profile, so the search stays narrow.
constexpr double kMaxAngle = 10.0;
constexpr int kSampleWidth = 600;
constexpr int kMinInkPixels = 200;
constexpr int kInkThreshold = 140;
constexpr double kMinUsefulAngle = 1.0;

// Projection profile score: text lines become sharp ridges in the horizontal
// sum when the rows follow the baseline, so the angle with the highest variance
// is the angle that straightens the page.
void fillProfile(const QVector<quint8>& ink, int width, int height, double degrees,
                 QVector<int>& profile)
{
    const double tan = std::tan(degrees * kPi / 180.0);
    const double center = width / 2.0;
    for (int& value : profile)
        value = 0;

    for (int y = 0; y < height; ++y) {
        const int rowBase = y * width;
        for (int x = 0; x < width; ++x) {
            if (ink[rowBase + x] == 0)
                continue;
            const int shifted = y + int(std::lround((x - center) * tan)) + width;
            if (shifted >= 0 && shifted < profile.size())
                ++profile[shifted];
        }
    }
}

double profileScore(const QVector<int>& profile)
{
    double sum = 0.0;
    double sumSquares = 0.0;
    for (const int value : profile) {
        sum += value;
        sumSquares += double(value) * value;
    }
    const double mean = sum / profile.size();
    return sumSquares / profile.size() - mean * mean;
}

}

double estimateSkewAngle(const QImage& image)
{
    if (image.isNull())
        return 0.0;

    QImage sample = image.width() > kSampleWidth
                        ? image.scaledToWidth(kSampleWidth, Qt::SmoothTransformation)
                        : image;
    sample = sample.convertToFormat(QImage::Format_Grayscale8);

    const int width = sample.width();
    const int height = sample.height();
    if (width < 32 || height < 32)
        return 0.0;

    QVector<quint8> ink(width * height);
    int inkCount = 0;
    for (int y = 0; y < height; ++y) {
        const uchar* line = sample.constScanLine(y);
        for (int x = 0; x < width; ++x) {
            if (line[x] < kInkThreshold) {
                ink[y * width + x] = 1;
                ++inkCount;
            }
        }
    }
    if (inkCount < kMinInkPixels)
        return 0.0;

    QVector<int> profile(height + 2 * width);

    double best = 0.0;
    double bestScore = -1.0;
    for (double angle = -kMaxAngle; angle <= kMaxAngle + 1e-9; angle += 1.0) {
        fillProfile(ink, width, height, angle, profile);
        const double score = profileScore(profile);
        if (score > bestScore) {
            bestScore = score;
            best = angle;
        }
    }
    for (double angle = best - 1.0; angle <= best + 1.0 + 1e-9; angle += 0.25) {
        fillProfile(ink, width, height, angle, profile);
        const double score = profileScore(profile);
        if (score > bestScore) {
            bestScore = score;
            best = angle;
        }
    }

    return std::abs(best) < kMinUsefulAngle ? 0.0 : best;
}

QImage deskewImage(const QImage& image, double degrees)
{
    if (image.isNull() || std::abs(degrees) < kMinUsefulAngle)
        return image;

    const double radians = degrees * kPi / 180.0;
    const double cosinus = std::abs(std::cos(radians));
    const double sinus = std::abs(std::sin(radians));
    const double width = image.width();
    const double height = image.height();
    const int canvasWidth = int(std::ceil(width * cosinus + height * sinus));
    const int canvasHeight = int(std::ceil(width * sinus + height * cosinus));

    QImage canvas(canvasWidth, canvasHeight, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::white);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.translate(canvasWidth / 2.0, canvasHeight / 2.0);
    painter.rotate(degrees);
    painter.translate(-width / 2.0, -height / 2.0);
    painter.drawImage(QPointF(0, 0), image);
    painter.end();

    return canvas;
}

QImage deskewImageCropped(const QImage& image, double degrees)
{
    if (image.isNull() || std::abs(degrees) < kMinUsefulAngle)
        return image;

    QImage canvas(image.size(), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::white);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.translate(canvas.width() / 2.0, canvas.height() / 2.0);
    painter.rotate(degrees);
    painter.translate(-image.width() / 2.0, -image.height() / 2.0);
    painter.drawImage(QPointF(0, 0), image);
    painter.end();

    return canvas;
}
