#include "spell_service.h"

#include "core/settings_manager.h"
#include "plugin_manager.h"
#include "spell_provider.h"

#include <QChar>

SpellService::SpellService(PluginManager* plugins, SettingsManager* settings)
    : m_plugins(plugins)
    , m_settings(settings)
{
}

QStringList SpellService::availableProviderIds() const
{
    QStringList result;
    if (!m_plugins)
        return result;
    for (const PluginManifest& manifest : m_plugins->manifests()) {
        if (manifest.valid && manifest.type == QStringLiteral("spelling")
            && !m_plugins->isDisabled(manifest.id)) {
            result.append(manifest.id);
        }
    }
    return result;
}

QString SpellService::providerId() const
{
    QString configured;
    if (m_settings)
        configured = m_settings->stringValue(QStringLiteral("ServiceProviders.spell"));
    if (configured == QStringLiteral("Windows"))
        configured = QStringLiteral("windows_spell");
    if (configured == QStringLiteral("Hunspell"))
        configured = QStringLiteral("hunspell");

    const QStringList available = availableProviderIds();
    if (available.contains(configured))
        return configured;
    if (available.contains(QStringLiteral("hunspell")))
        return QStringLiteral("hunspell");
    return available.isEmpty() ? QString() : available.first();
}

ISpellProvider* SpellService::provider() const
{
    if (!m_plugins)
        return nullptr;
    const QString id = providerId();
    if (id.isEmpty())
        return nullptr;
    return dynamic_cast<ISpellProvider*>(m_plugins->provider(id));
}

QString SpellService::resolveLanguage(const QString& word, const QString& language) const
{
    QString value = language.trimmed();
    if (!value.isEmpty() && value != QStringLiteral("auto"))
        return value;
    bool cyrillic = false;
    bool greek = false;
    bool hebrew = false;
    bool arabic = false;
    for (const QChar character : word) {
        const ushort code = character.unicode();
        if (code >= 0x0400 && code <= 0x04FF) {
            cyrillic = true;
        } else if (code >= 0x0370 && code <= 0x03FF) {
            greek = true;
        } else if (code >= 0x0590 && code <= 0x05FF) {
            hebrew = true;
        } else if (code >= 0x0600 && code <= 0x06FF) {
            arabic = true;
        }
    }
    if (cyrillic)
        return QStringLiteral("ru");
    if (greek)
        return QStringLiteral("el");
    if (hebrew)
        return QStringLiteral("he");
    if (arabic)
        return QStringLiteral("ar");
    return QStringLiteral("en");
}

bool SpellService::checkWord(const QString& word, const QString& language,
                             QStringList* suggestions) const
{
    if (m_settings && !m_settings->boolValue(QStringLiteral("ServiceProviders.spellOn"), true))
        return true;
    ISpellProvider* spell = provider();
    if (!spell || word.isEmpty())
        return true;
    return spell->checkSpelling(word, resolveLanguage(word, language), suggestions);
}

QStringList SpellService::suggestions(const QString& word, const QString& language,
                                      int limit) const
{
    QStringList result;
    if (limit <= 0)
        return result;
    checkWord(word, language, &result);
    while (result.size() > limit)
        result.removeLast();
    return result;
}
