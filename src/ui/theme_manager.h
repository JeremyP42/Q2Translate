#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

namespace ThemeManager {

QStringList themeNames();
bool isDark(const QString& themeName);
bool apply(const QString& themeName);
QString styleSheet(const QString& themeName);

// Re-reads Themes/General/*.json so edits made while the application is running are
// visible the next time the settings dialog is opened.
void reload();

// Token value (camelCase, without '$') of the given theme.
// Empty themeName means the active theme (set by apply()/styleSheet()).
// Unknown token or theme: qWarning + invalid QColor.
QColor color(const QString& token, const QString& themeName = QString());
QString activeTheme();

}
