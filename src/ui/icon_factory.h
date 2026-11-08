#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

namespace IconFactory {

enum class Glyph {
    App,
    Close,
    Minimize,
    Maximize,
    Restore,
    Pin,
    Gear,
    Copy,
    Swap,
    Trash,
    Broom,
    Globe,
    Speaker,
    Microphone,
    ChevronDown,
    Search,
    History,
    Book,
    Wrench,
    Plus,
    Edit,
    Info
};

QIcon icon(Glyph glyph, const QColor& color, int size = 16);
QIcon icon(Glyph glyph, const QColor& normal, const QColor& active,
           const QColor& disabled, int size = 16);
QIcon appIcon();
QIcon serviceIcon(const QString& letter, const QColor& color, int size = 18);
QPixmap pixmap(Glyph glyph, const QColor& color, int size, qreal devicePixelRatio = 1.0);
QPixmap servicePixmap(const QString& letter, const QColor& color, int size,
                      qreal devicePixelRatio = 1.0);

}
