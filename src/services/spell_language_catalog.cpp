#include "spell_language_catalog.h"

namespace {

struct Entry
{
    const char* code;
    const char* name;
};

const Entry kEntries[] = {
    {"en", "English"},
    {"ru", "Russian"},
    {"de", "German"},
    {"fr", "French"},
    {"es", "Spanish"},
    {"it", "Italian"},
    {"pt", "Portuguese"},
    {"nl", "Dutch"},
    {"pl", "Polish"},
    {"uk", "Ukrainian"},
    {"cs", "Czech"},
    {"sk", "Slovak"},
    {"bg", "Bulgarian"},
    {"hr", "Croatian"},
    {"sr", "Serbian"},
    {"sl", "Slovenian"},
    {"ro", "Romanian"},
    {"hu", "Hungarian"},
    {"el", "Greek"},
    {"tr", "Turkish"},
    {"sv", "Swedish"},
    {"da", "Danish"},
    {"nb", "Norwegian Bokmal"},
    {"et", "Estonian"},
    {"lv", "Latvian"},
    {"lt", "Lithuanian"},
    {"ca", "Catalan"},
    {"gl", "Galician"},
    {"eu", "Basque"},
    {"vi", "Vietnamese"},
    {"ko", "Korean"}
};

} // namespace

QList<SpellLanguageEntry> spellLanguageCatalog()
{
    QList<SpellLanguageEntry> result;
    for (const Entry& entry : kEntries) {
        SpellLanguageEntry language;
        language.code = QString::fromLatin1(entry.code);
        language.name = QString::fromUtf8(entry.name);
        result.append(language);
    }
    return result;
}

QString spellLanguageName(const QString& code)
{
    for (const Entry& entry : kEntries) {
        if (code == QString::fromLatin1(entry.code))
            return QString::fromUtf8(entry.name);
    }
    return code;
}

bool isSpellLanguageCode(const QString& code)
{
    for (const Entry& entry : kEntries) {
        if (code == QString::fromLatin1(entry.code))
            return true;
    }
    return false;
}
