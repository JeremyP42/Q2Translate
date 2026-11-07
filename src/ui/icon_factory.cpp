#include "icon_factory.h"

#include "theme_manager.h"

#include <QFont>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QtMath>

#include <cmath>

namespace {

constexpr qreal kGrid = 24.0;
constexpr qreal kPi = 3.14159265358979323846;

qreal currentDevicePixelRatio()
{
    if (const QScreen* screen = QGuiApplication::primaryScreen())
        return screen->devicePixelRatio();
    return 1.0;
}

QPixmap renderApp(int size, qreal dpr)
{
    const int pixelSize = qMax(1, qRound(size * dpr));
    QPixmap pixmap(pixelSize, pixelSize);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(dpr, dpr);

    const QRectF bounds(0.5, 0.5, size - 1.0, size - 1.0);
    const qreal radius = size * 0.24;
    QLinearGradient gradient(0, 0, size, size);
    gradient.setColorAt(0.0, QColor(QStringLiteral("#2f7cf0")));
    gradient.setColorAt(1.0, QColor(QStringLiteral("#1b4fc0")));

    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRoundedRect(bounds, radius, radius);

    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(qMax(8, qRound(size * 0.40)));
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, QStringLiteral("Q2T"));
    painter.end();

    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

void drawGear(QPainter& painter)
{
    painter.drawEllipse(QPointF(12, 12), 3.2, 3.2);
    for (int i = 0; i < 8; ++i) {
        const qreal angle = i * kPi / 4.0;
        const QPointF inner(12.0 + std::cos(angle) * 4.9, 12.0 + std::sin(angle) * 4.9);
        const QPointF outer(12.0 + std::cos(angle) * 8.5, 12.0 + std::sin(angle) * 8.5);
        painter.drawLine(inner, outer);
    }
}

void drawSpeaker(QPainter& painter)
{
    QPainterPath body;
    body.moveTo(4, 9);
    body.lineTo(8, 9);
    body.lineTo(13, 5);
    body.lineTo(13, 19);
    body.lineTo(8, 15);
    body.lineTo(4, 15);
    body.closeSubpath();
    painter.drawPath(body);
    painter.drawArc(QRectF(10.5, 8.0, 6.0, 8.0), -65 * 16, 130 * 16);
    painter.drawArc(QRectF(8.0, 5.5, 10.5, 13.0), -60 * 16, 120 * 16);
}

void drawMicrophone(QPainter& painter)
{
    painter.drawRoundedRect(QRectF(9, 3, 6, 11), 3, 3);
    painter.drawArc(QRectF(5.5, 6.5, 13, 11), 180 * 16, 180 * 16);
    painter.drawLine(QPointF(12, 18.0), QPointF(12, 21.5));
}

QPixmap renderGlyph(IconFactory::Glyph glyph, const QColor& color, int size, qreal dpr)
{
    if (glyph == IconFactory::Glyph::App)
        return renderApp(size, dpr);

    const int pixelSize = qMax(1, qRound(size * dpr));
    QPixmap pixmap(pixelSize, pixelSize);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(dpr, dpr);
    painter.scale(size / kGrid, size / kGrid);

    QPen pen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (glyph) {
    case IconFactory::Glyph::App:
        break;
    case IconFactory::Glyph::Close:
        painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(6, 6), QPointF(18, 18));
        painter.drawLine(QPointF(18, 6), QPointF(6, 18));
        break;
    case IconFactory::Glyph::Minimize:
        painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(6, 12), QPointF(18, 12));
        break;
    case IconFactory::Glyph::Maximize:
        painter.drawRoundedRect(QRectF(5.5, 5.5, 13, 13), 1.5, 1.5);
        break;
    case IconFactory::Glyph::Restore:
        painter.drawRoundedRect(QRectF(5.5, 8.5, 10, 10), 1.5, 1.5);
        painter.drawRoundedRect(QRectF(8.5, 5.5, 10, 10), 1.5, 1.5);
        break;
    case IconFactory::Glyph::Pin: {
        QPainterPath path;
        path.moveTo(14.5, 3.5);
        path.lineTo(20.5, 9.5);
        path.lineTo(17.9, 10.4);
        path.lineTo(14.5, 13.8);
        path.lineTo(13.9, 17.8);
        path.lineTo(7.1, 11.0);
        path.lineTo(11.1, 10.4);
        path.lineTo(14.5, 7.0);
        path.closeSubpath();
        painter.drawPath(path);
        painter.drawLine(QPointF(9.8, 14.2), QPointF(4.0, 20.0));
        break;
    }
    case IconFactory::Glyph::Gear:
        drawGear(painter);
        break;
    case IconFactory::Glyph::Copy: {
        painter.drawRoundedRect(QRectF(8.5, 8.5, 11, 11), 2, 2);
        QPainterPath path;
        path.moveTo(15.5, 5.5);
        path.lineTo(15.5, 4.5);
        path.quadTo(15.5, 3.5, 14.5, 3.5);
        path.lineTo(7.5, 3.5);
        path.quadTo(6.5, 3.5, 6.5, 4.5);
        path.lineTo(6.5, 13.5);
        path.quadTo(6.5, 14.5, 7.5, 14.5);
        path.lineTo(8.5, 14.5);
        painter.drawPath(path);
        break;
    }
    case IconFactory::Glyph::Swap: {
        painter.drawLine(QPointF(4, 8), QPointF(17, 8));
        QPainterPath first;
        first.moveTo(14, 4.5);
        first.lineTo(17.5, 8);
        first.lineTo(14, 11.5);
        painter.drawPath(first);
        painter.drawLine(QPointF(20, 16), QPointF(7, 16));
        QPainterPath second;
        second.moveTo(10, 12.5);
        second.lineTo(6.5, 16);
        second.lineTo(10, 19.5);
        painter.drawPath(second);
        break;
    }
    case IconFactory::Glyph::Trash: {
        painter.drawLine(QPointF(4.5, 6.5), QPointF(19.5, 6.5));
        painter.drawLine(QPointF(9.5, 6.0), QPointF(9.5, 4.5));
        painter.drawLine(QPointF(9.5, 4.5), QPointF(14.5, 4.5));
        painter.drawLine(QPointF(14.5, 4.5), QPointF(14.5, 6.0));
        QPainterPath body;
        body.moveTo(7, 6.5);
        body.lineTo(7.8, 18.7);
        body.quadTo(8.0, 20.0, 9.5, 20.0);
        body.lineTo(14.5, 20.0);
        body.quadTo(16.0, 20.0, 16.2, 18.7);
        body.lineTo(17.0, 6.5);
        painter.drawPath(body);
        painter.drawLine(QPointF(10, 10.5), QPointF(10, 16.5));
        painter.drawLine(QPointF(14, 10.5), QPointF(14, 16.5));
        break;
    }
    case IconFactory::Glyph::Globe:
        painter.drawEllipse(QPointF(12, 12), 8.5, 8.5);
        painter.drawLine(QPointF(3.5, 12), QPointF(20.5, 12));
        painter.drawEllipse(QPointF(12, 12), 4.2, 8.5);
        break;
    case IconFactory::Glyph::Speaker:
        drawSpeaker(painter);
        break;
    case IconFactory::Glyph::Microphone:
        drawMicrophone(painter);
        break;
    case IconFactory::Glyph::ChevronDown: {
        QPainterPath path;
        path.moveTo(7, 10);
        path.lineTo(12, 15);
        path.lineTo(17, 10);
        painter.drawPath(path);
        break;
    }
    case IconFactory::Glyph::Search:
        painter.drawEllipse(QPointF(11, 11), 6.5, 6.5);
        painter.drawLine(QPointF(16, 16), QPointF(20.5, 20.5));
        break;
    case IconFactory::Glyph::History: {
        painter.drawArc(QRectF(4.5, 4.5, 15, 15), 45 * 16, 315 * 16);
        QPainterPath arrow;
        arrow.moveTo(4.5, 12);
        arrow.lineTo(2.5, 9.8);
        arrow.moveTo(4.5, 12);
        arrow.lineTo(6.8, 10.0);
        painter.drawPath(arrow);
        painter.drawLine(QPointF(12, 8.5), QPointF(12, 12));
        painter.drawLine(QPointF(12, 12), QPointF(14.6, 13.6));
        break;
    }
    case IconFactory::Glyph::Book: {
        QPainterPath path;
        path.moveTo(12, 6.5);
        path.cubicTo(10, 4.8, 7, 4.5, 4, 4.7);
        path.lineTo(4, 18.3);
        path.cubicTo(7, 18.1, 10, 18.4, 12, 20);
        path.cubicTo(14, 18.4, 17, 18.1, 20, 18.3);
        path.lineTo(20, 4.7);
        path.cubicTo(17, 4.5, 14, 4.8, 12, 6.5);
        path.closeSubpath();
        painter.drawPath(path);
        painter.drawLine(QPointF(12, 6.5), QPointF(12, 20));
        break;
    }
    case IconFactory::Glyph::Plus:
        painter.drawLine(QPointF(12, 5), QPointF(12, 19));
        painter.drawLine(QPointF(5, 12), QPointF(19, 12));
        break;
    case IconFactory::Glyph::Edit: {
        QPainterPath path;
        path.moveTo(15.6, 3.4);
        path.lineTo(20.6, 8.4);
        path.lineTo(8.4, 20.6);
        path.lineTo(3.4, 20.6);
        path.lineTo(3.4, 15.6);
        path.closeSubpath();
        painter.drawPath(path);
        painter.drawLine(QPointF(13.6, 5.4), QPointF(18.6, 10.4));
        break;
    }
    case IconFactory::Glyph::Wrench: {
        painter.drawArc(QRectF(12.0, 4.0, 8.0, 8.0), 95 * 16, 250 * 16);
        painter.drawLine(QPointF(15.65, 4.0), QPointF(15.4, 1.9));
        painter.drawLine(QPointF(19.85, 9.0), QPointF(21.9, 9.5));
        painter.drawLine(QPointF(12.5, 10.5), QPointF(4.5, 19.5));
        break;
    }
    case IconFactory::Glyph::Info:
        painter.drawEllipse(QPointF(12, 12), 8.5, 8.5);
        painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(12, 11), QPointF(12, 16));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(12, 8), 1.15, 1.15);
        break;
    }

    painter.end();
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

QPixmap renderService(const QString& letter, const QColor& color, int size, qreal dpr)
{
    const int pixelSize = qMax(1, qRound(size * dpr));
    QPixmap pixmap(pixelSize, pixelSize);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(dpr, dpr);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(0.5, 0.5, size - 1.0, size - 1.0), size * 0.28, size * 0.28);

    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(qMax(7, qRound(size * 0.54)));
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter,
                     letter.isEmpty() ? QStringLiteral("?") : letter.left(1).toUpper());
    painter.end();

    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

}

namespace IconFactory {

QPixmap pixmap(Glyph glyph, const QColor& color, int size, qreal devicePixelRatio)
{
    return renderGlyph(glyph, color, size, devicePixelRatio);
}

QPixmap servicePixmap(const QString& letter, const QColor& color, int size,
                      qreal devicePixelRatio)
{
    return renderService(letter, color, size, devicePixelRatio);
}

QIcon icon(Glyph glyph, const QColor& color, int size)
{
    return icon(glyph, color, ThemeManager::color(QStringLiteral("accent")),
                ThemeManager::color(QStringLiteral("muted")), size);
}

QIcon icon(Glyph glyph, const QColor& normal, const QColor& active,
           const QColor& disabled, int size)
{
    const qreal dpr = currentDevicePixelRatio();
    QIcon result;
    result.addPixmap(renderGlyph(glyph, normal, size, dpr), QIcon::Normal, QIcon::Off);
    result.addPixmap(renderGlyph(glyph, active, size, dpr), QIcon::Active, QIcon::Off);
    result.addPixmap(renderGlyph(glyph, disabled, size, dpr), QIcon::Disabled, QIcon::Off);
    return result;
}

QIcon appIcon()
{
    const qreal dpr = currentDevicePixelRatio();
    QIcon result;
    const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    for (const int size : sizes)
        result.addPixmap(renderApp(size, dpr), QIcon::Normal, QIcon::Off);
    return result;
}

QIcon serviceIcon(const QString& letter, const QColor& color, int size)
{
    const qreal dpr = currentDevicePixelRatio();
    QIcon result;
    result.addPixmap(renderService(letter, color, size, dpr), QIcon::Normal, QIcon::Off);
    return result;
}

}

