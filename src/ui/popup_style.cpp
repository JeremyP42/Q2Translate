#include "popup_style.h"

#include "theme_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <algorithm>

namespace {

QColor tone(const QColor& base, qreal darken, qreal lighten, qreal alpha)
{
    if (!base.isValid())
        return base;
    qreal red = base.redF();
    qreal green = base.greenF();
    qreal blue = base.blueF();
    if (darken > 0.0) {
        const qreal factor = qBound(0.0, 1.0 - darken, 1.0);
        red *= factor;
        green *= factor;
        blue *= factor;
    }
    if (lighten > 0.0) {
        const qreal factor = qBound(0.0, lighten, 1.0);
        red += (1.0 - red) * factor;
        green += (1.0 - green) * factor;
        blue += (1.0 - blue) * factor;
    }
    QColor result = QColor::fromRgbF(qBound(0.0, red, 1.0), qBound(0.0, green, 1.0),
                                     qBound(0.0, blue, 1.0), qBound(0.0, alpha, 1.0));
    return result;
}

QList<PopupStyle> g_styles;

QList<PopupStyle> loadStyles()
{
    QList<PopupStyle> result;
    const QDir applicationDir(QCoreApplication::applicationDirPath());
    QDir directory(applicationDir.filePath(QStringLiteral("Themes/Popup")));
    if (!directory.exists())
        directory = QDir(applicationDir.filePath(QStringLiteral("PopupStyles")));
    if (!directory.exists())
        return result;

    const QFileInfoList files =
        directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo& info : files) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            continue;
        const QJsonObject object = document.object();
        PopupStyle style;
        style.id = object.value(QStringLiteral("id")).toString(info.completeBaseName());
        style.name = object.value(QStringLiteral("name")).toString(style.id);
        style.raw = object;
        style.valid = !style.id.isEmpty();
        if (style.valid)
            result.append(style);
    }
    std::sort(result.begin(), result.end(), [](const PopupStyle& left, const PopupStyle& right) {
        const int leftOrder = left.raw.value(QStringLiteral("order")).toInt(999);
        const int rightOrder = right.raw.value(QStringLiteral("order")).toInt(999);
        if (leftOrder != rightOrder)
            return leftOrder < rightOrder;
        return left.name.localeAwareCompare(right.name) < 0;
    });
    return result;
}

} // namespace

QList<PopupStyle> popupStyles()
{
    if (g_styles.isEmpty())
        g_styles = loadStyles();
    return g_styles;
}

void reloadPopupStyles()
{
    g_styles.clear();
}

PopupStyle popupStyle(const QString& id)
{
    const QList<PopupStyle> styles = popupStyles();
    for (const PopupStyle& style : styles) {
        if (style.id.compare(id, Qt::CaseInsensitive) == 0)
            return style;
    }
    if (!styles.isEmpty())
        return styles.first();
    return PopupStyle();
}

QColor popupColor(const QJsonValue& spec, const QString& fallbackToken,
                  const QColor& fallback)
{
    QString token = fallbackToken;
    qreal darken = 0.0;
    qreal lighten = 0.0;
    qreal alpha = 1.0;
    if (spec.isString()) {
        token = spec.toString();
    } else if (spec.isObject()) {
        const QJsonObject object = spec.toObject();
        token = object.value(QStringLiteral("token")).toString(fallbackToken);
        darken = object.value(QStringLiteral("darken")).toDouble();
        lighten = object.value(QStringLiteral("lighten")).toDouble();
        alpha = object.value(QStringLiteral("alpha")).toDouble(1.0);
    } else if (fallback.isValid()) {
        return fallback;
    }
    const QColor base = ThemeManager::color(token);
    if (!base.isValid())
        return fallback;
    return tone(base, darken, lighten, alpha);
}
