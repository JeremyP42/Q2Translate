#include "language_router.h"

#include "language_catalog.h"
#include "settings_manager.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

LanguageRouter::LanguageRouter(SettingsManager* settings)
    : m_settings(settings)
{
    reload();
}

void LanguageRouter::reload()
{
    m_rules.clear();
    m_fallback = QStringLiteral("ru");

    if (!m_settings)
        return;

    const QString configured = normalizeLanguageCode(
        m_settings->stringValue(QStringLiteral("General.LanguageTo"), QStringLiteral("ru")));
    if (configured != QStringLiteral("auto"))
        m_fallback = configured;

    const QJsonValue translationRules = m_settings->value(QStringLiteral("TranslationRules"));
    if (!translationRules.isArray())
        return;
    for (const QJsonValue& value : translationRules.toArray()) {
        const QJsonArray pair = value.toArray();
        if (pair.size() < 2)
            continue;
        const QString source = languageCode(pair.at(0).toInt());
        const QString target = languageCode(pair.at(1).toInt());
        if (source.isEmpty() || target.isEmpty() || source == target)
            continue;
        if (!m_rules.contains(source))
            m_rules.insert(source, target);
    }
}

QString LanguageRouter::resolveTarget(const QString& sourceCode) const
{
    QString source = normalizeLanguageCode(sourceCode);
    if (source.isEmpty() || source == QStringLiteral("auto")) {
        // An explicit auto rule is the fallback direction when the source
        // language could not be identified.
        const QString autoTarget = m_rules.value(QStringLiteral("auto"));
        if (!autoTarget.isEmpty() && autoTarget != QStringLiteral("auto"))
            return autoTarget;
        source = QStringLiteral("en");
    }

    const QString target = m_rules.value(source);
    if (!target.isEmpty() && target != source)
        return target;

    if (m_fallback != source && m_fallback != QStringLiteral("auto"))
        return m_fallback;

    return source == QStringLiteral("en") ? QStringLiteral("ru") : QStringLiteral("en");
}

QString LanguageRouter::fallbackTarget() const
{
    return m_fallback;
}

QString LanguageRouter::detectLanguage(const QString& text)
{
    int cyrillic = 0;
    int latin = 0;
    int cjk = 0;
    int japanese = 0;
    int korean = 0;
    int arabic = 0;
    int hebrew = 0;
    int greek = 0;

    for (const QChar character : text) {
        const uint code = character.unicode();
        if (code >= 0x0400 && code <= 0x04FF)
            ++cyrillic;
        else if ((code >= 0x0041 && code <= 0x005A) || (code >= 0x0061 && code <= 0x007A)
                 || (code >= 0x00C0 && code <= 0x024F))
            ++latin;
        else if ((code >= 0x3040 && code <= 0x30FF) || (code >= 0x31F0 && code <= 0x31FF))
            ++japanese;
        else if (code >= 0xAC00 && code <= 0xD7AF)
            ++korean;
        else if (code >= 0x4E00 && code <= 0x9FFF)
            ++cjk;
        else if (code >= 0x0600 && code <= 0x06FF)
            ++arabic;
        else if (code >= 0x0590 && code <= 0x05FF)
            ++hebrew;
        else if (code >= 0x0370 && code <= 0x03FF)
            ++greek;
    }

    const int maxValue = qMax(qMax(qMax(cyrillic, latin), qMax(cjk, japanese)),
                              qMax(qMax(korean, arabic), qMax(hebrew, greek)));

    if (maxValue == 0)
        return QStringLiteral("en");
    if (japanese == maxValue)
        return QStringLiteral("ja");
    if (korean == maxValue)
        return QStringLiteral("ko");
    if (cjk == maxValue)
        return QStringLiteral("zh-CN");
    if (cyrillic == maxValue)
        return QStringLiteral("ru");
    if (arabic == maxValue)
        return QStringLiteral("ar");
    if (hebrew == maxValue)
        return QStringLiteral("he");
    if (greek == maxValue)
        return QStringLiteral("el");
    return QStringLiteral("en");
}
