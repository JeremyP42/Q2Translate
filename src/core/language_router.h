#pragma once

#include <QHash>
#include <QString>

class SettingsManager;

class LanguageRouter
{
public:
    explicit LanguageRouter(SettingsManager* settings);

    void reload();
    QString resolveTarget(const QString& sourceCode) const;
    QString fallbackTarget() const;
    static QString detectLanguage(const QString& text);

private:
    SettingsManager* m_settings = nullptr;
    QHash<QString, QString> m_rules;
    QString m_fallback;
};
