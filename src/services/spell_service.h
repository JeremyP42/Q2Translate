#pragma once

#include <QString>
#include <QStringList>

class PluginManager;
class SettingsManager;
class ISpellProvider;

// Chooses the spelling connector configured in the settings and mediates
// between the text editor and that connector.
class SpellService
{
public:
    SpellService(PluginManager* plugins, SettingsManager* settings);
    ~SpellService() = default;

    QString providerId() const;
    ISpellProvider* provider() const;
    QStringList availableProviderIds() const;

    bool checkWord(const QString& word, const QString& language,
                   QStringList* suggestions = nullptr) const;
    QStringList suggestions(const QString& word, const QString& language,
                            int limit = 7) const;
    QString resolveLanguage(const QString& word, const QString& language) const;

private:
    PluginManager* m_plugins = nullptr;
    SettingsManager* m_settings = nullptr;
};
