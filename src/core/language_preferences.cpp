#include "language_preferences.h"
#include "settings_manager.h"

#include <QJsonArray>
#include <QJsonValue>
#include <algorithm>

QSet<int> favoriteLanguageIds(const SettingsManager* settings)
{
    QSet<int> ids;
    if (!settings)
        return ids;
    const QJsonArray pinned = settings->value(QStringLiteral("PinnedLanguages")).toArray();
    for (const QJsonValue& value : pinned)
        ids.insert(value.toInt());
    return ids;
}

QList<LanguageInfo> favoriteLanguageList(const SettingsManager* settings, bool includeAuto)
{
    const QSet<int> favorites = favoriteLanguageIds(settings);
    const bool useAll = favorites.isEmpty();

    QList<LanguageInfo> result;
    for (const LanguageInfo& language : allLanguages()) {
        if (language.id == 1) {
            if (includeAuto)
                result.append(language);
            continue;
        }
        if (useAll || favorites.contains(language.id))
            result.append(language);
    }

    std::sort(result.begin(), result.end(),
              [](const LanguageInfo& left, const LanguageInfo& right) {
        return QString::localeAwareCompare(QString::fromUtf8(left.name),
                                           QString::fromUtf8(right.name)) < 0;
    });
    return result;
}
