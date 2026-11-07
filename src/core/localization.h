#pragma once

#include <QString>
#include <QStringList>

class QApplication;

// Small JSON-backed localization layer.  Locales/<Language>.json contains
// "Language", "LanguageEnglish", "LanguageNative" plus a map of source strings
// to translated strings.
namespace Localization {

QStringList availableLanguages();
QString languageDisplayName(const QString& folder);
QString tr(const QString& source);

// Loads Locales/<Language>.json (legacy Locales/<Language>/lang.json is also
// accepted). "Russian" keeps the original strings.
bool load(const QString& folder);

// Installs the widget tree translator.  Safe to call once after QApplication
// construction and before the first window is created.
void install(QApplication* application);

// Re-applies the current language to all top-level widgets.
void retranslateAll();

} // namespace Localization
