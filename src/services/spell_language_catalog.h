#pragma once

#include <QList>
#include <QString>

struct SpellLanguageEntry
{
    QString code;
    QString name;
};

QList<SpellLanguageEntry> spellLanguageCatalog();
QString spellLanguageName(const QString& code);
bool isSpellLanguageCode(const QString& code);
