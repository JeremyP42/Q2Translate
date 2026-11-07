#include "language_catalog.h"

namespace {

const LanguageInfo kLanguages[] = {
    {1, "auto", "Автоопределение"},
    {2, "af", "Африкаанс"},
    {3, "az", "Азербайджанский"},
    {4, "sq", "Албанский"},
    {5, "ar", "Арабский"},
    {6, "hy", "Армянский"},
    {7, "eu", "Баскский"},
    {8, "be", "Белорусский"},
    {9, "bg", "Болгарский"},
    {10, "ca", "Каталанский"},
    {11, "zh-CN", "Китайский (упрощ.)"},
    {12, "zh-TW", "Китайский (традиц.)"},
    {13, "hr", "Хорватский"},
    {14, "cs", "Чешский"},
    {15, "da", "Датский"},
    {16, "nl", "Нидерландский"},
    {17, "en", "Английский"},
    {18, "et", "Эстонский"},
    {19, "fi", "Финский"},
    {20, "fil", "Филиппинский"},
    {21, "fr", "Французский"},
    {22, "gl", "Галисийский"},
    {23, "de", "Немецкий"},
    {24, "el", "Греческий"},
    {25, "ht", "Гаитянский креольский"},
    {26, "he", "Иврит"},
    {27, "hi", "Хинди"},
    {28, "hu", "Венгерский"},
    {29, "is", "Исландский"},
    {30, "id", "Индонезийский"},
    {31, "it", "Итальянский"},
    {32, "ga", "Ирландский"},
    {33, "ja", "Японский"},
    {34, "ka", "Грузинский"},
    {35, "ko", "Корейский"},
    {36, "lv", "Латышский"},
    {37, "lt", "Литовский"},
    {38, "mk", "Македонский"},
    {39, "ms", "Малайский"},
    {40, "mt", "Мальтийский"},
    {41, "no", "Норвежский"},
    {42, "fa", "Персидский"},
    {43, "pl", "Польский"},
    {44, "pt", "Португальский"},
    {45, "ro", "Румынский"},
    {46, "ru", "Русский"},
    {47, "sr", "Сербский"},
    {48, "sk", "Словацкий"},
    {49, "sl", "Словенский"},
    {50, "es", "Испанский"},
    {51, "sw", "Суахили"},
    {52, "sv", "Шведский"},
    {53, "th", "Тайский"},
    {54, "tr", "Турецкий"},
    {55, "uk", "Украинский"},
    {56, "ur", "Урду"},
    {57, "vi", "Вьетнамский"},
    {58, "cy", "Валлийский"},
    {59, "yi", "Идиш"},
    {60, "eo", "Эсперанто"},
    {61, "hmn", "Хмонг"},
    {62, "la", "Латынь"},
    {63, "lo", "Лаосский"},
    {64, "kk", "Казахский"},
    {65, "uz", "Узбекский"},
    {66, "si", "Сингальский"},
    {67, "tg", "Таджикский"},
    {68, "te", "Телугу"},
    {69, "km", "Кхмерский"},
    {70, "mn", "Монгольский"},
    {71, "kn", "Каннада"},
    {72, "ta", "Тамильский"},
    {73, "mr", "Маратхи"},
    {74, "bn", "Бенгальский"},
    {75, "tt", "Татарский"}
};

}

const QVector<LanguageInfo>& allLanguages()
{
    static const QVector<LanguageInfo> languages = [] {
        QVector<LanguageInfo> result;
        result.reserve(int(sizeof(kLanguages) / sizeof(kLanguages[0])));
        for (const LanguageInfo& language : kLanguages)
            result.append(language);
        return result;
    }();
    return languages;
}

QString languageName(int id)
{
    for (const LanguageInfo& language : allLanguages()) {
        if (language.id == id)
            return QString::fromUtf8(language.name);
    }
    return QString();
}

QString languageCode(int id)
{
    for (const LanguageInfo& language : allLanguages()) {
        if (language.id == id)
            return QString::fromLatin1(language.code);
    }
    return QStringLiteral("auto");
}

QString languageNameByCode(const QString& code)
{
    const QString normalized = normalizeLanguageCode(code);
    for (const LanguageInfo& language : allLanguages()) {
        if (QString::fromLatin1(language.code).compare(normalized, Qt::CaseInsensitive) == 0)
            return QString::fromUtf8(language.name);
    }
    return code;
}

int languageId(const QString& codeOrName)
{
    const QString value = codeOrName.trimmed();
    if (value.isEmpty())
        return 1;

    bool ok = false;
    const int numeric = value.toInt(&ok);
    if (ok) {
        for (const LanguageInfo& language : allLanguages()) {
            if (language.id == numeric)
                return numeric;
        }
    }

    const QString normalized = normalizeLanguageCode(value);
    for (const LanguageInfo& language : allLanguages()) {
        if (QString::fromLatin1(language.code).compare(normalized, Qt::CaseInsensitive) == 0)
            return language.id;
        if (QString::fromUtf8(language.name).compare(value, Qt::CaseInsensitive) == 0)
            return language.id;
    }
    return 1;
}

QString normalizeLanguageCode(const QString& codeOrName)
{
    const QString value = codeOrName.trimmed();
    if (value.isEmpty())
        return QStringLiteral("auto");

    const QString lower = value.toLower();
    if (lower == QStringLiteral("auto") || lower == QStringLiteral("auto-detect"))
        return QStringLiteral("auto");
    if (lower == QStringLiteral("zh") || lower == QStringLiteral("zh-cn") || lower == QStringLiteral("zh_hans"))
        return QStringLiteral("zh-CN");
    if (lower == QStringLiteral("zh-tw") || lower == QStringLiteral("zh_hant"))
        return QStringLiteral("zh-TW");
    if (lower == QStringLiteral("iw"))
        return QStringLiteral("he");
    if (lower == QStringLiteral("in"))
        return QStringLiteral("id");
    if (lower == QStringLiteral("nb") || lower == QStringLiteral("nn"))
        return QStringLiteral("no");
    if (lower == QStringLiteral("tl"))
        return QStringLiteral("fil");

    for (const LanguageInfo& language : allLanguages()) {
        if (QString::fromLatin1(language.code).compare(value, Qt::CaseInsensitive) == 0)
            return QString::fromLatin1(language.code);
        if (QString::fromUtf8(language.name).compare(value, Qt::CaseInsensitive) == 0)
            return QString::fromLatin1(language.code);
    }
    return value;
}

