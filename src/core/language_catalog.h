#pragma once

#include <QString>
#include <QVector>

struct LanguageInfo {
    int id;
    const char* code;
    const char* name;
};

const QVector<LanguageInfo>& allLanguages();

QString languageName(int id);
QString languageCode(int id);
QString languageNameByCode(const QString& code);
int languageId(const QString& codeOrName);
QString normalizeLanguageCode(const QString& codeOrName);
