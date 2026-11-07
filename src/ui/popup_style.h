#pragma once

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QString>

struct PopupStyle {
    QString id;
    QString name;
    QJsonObject raw;
    bool valid = false;
};

// Styles live in <application>/Themes/Popup/*.json, one file per style.
QList<PopupStyle> popupStyles();
void reloadPopupStyles();
PopupStyle popupStyle(const QString& id);

// A colour entry is either a theme token string or an object:
// { "token": "surfaceAlt", "darken": 0.12 } / { "token": "border", "lighten": 0.08 }
// with optional "alpha" (0..1).
QColor popupColor(const QJsonValue& spec, const QString& fallbackToken,
                  const QColor& fallback = QColor());
