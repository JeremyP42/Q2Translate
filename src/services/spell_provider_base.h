#pragma once

#include "provider_interface.h"
#include "spell_provider.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

class PathManager;
class QNetworkAccessManager;

class ISpellEngine
{
public:
    virtual ~ISpellEngine() = default;

    virtual bool isLoaded() const = 0;
    virtual bool spell(const QString& word) const = 0;
    virtual QStringList suggest(const QString& word, int limit = 7) const = 0;
};

// Common implementation for dictionary-backed spelling connectors.  The only
// engine-specific part is createEngine(): Hunspell loads .aff + .dic, Nuspell
// loads .aff and discovers the dictionary itself.
class SpellProviderBase : public IPluginProvider, public ISpellProvider
{
public:
    explicit SpellProviderBase(const PluginManifest& manifest, PathManager* paths,
                               QObject* parent = nullptr);
    ~SpellProviderBase() override;

    const PluginManifest& manifest() const override { return m_manifest; }
    QJsonObject settings() const override { return m_settings; }
    bool updateSettings(const QJsonObject& settings) override;
    QJsonObject defaultSettings() const override { return {}; }
    void executeAsync(const QString& capability, const QJsonObject& params,
                      PluginCallback callback) override;

    QList<SpellLanguage> spellLanguages() const override;
    QString spellDictionaryPath() const override;
    bool checkSpelling(const QString& word, const QString& language,
                       QStringList* suggestions = nullptr) override;
    void installSpellLanguage(const QString& code,
                              std::function<void(bool, const QString&)> done) override;
    void removeSpellLanguage(const QString& code,
                             std::function<void(bool, const QString&)> done) override;

protected:
    virtual ISpellEngine* createEngine(const QString& affPath, const QString& dicPath,
                                       QString* error) = 0;

private:
    QString dictionaryRoot() const;
    QString languageDirectory(const QString& code) const;
    bool languageInstalled(const QString& code) const;
    QString normalizedCode(const QString& code) const;
    QString guessLanguage(const QString& word) const;
    ISpellEngine* engineFor(const QString& code);
    void downloadLanguageFile(const QString& code, const QString& suffix,
                              const QString& targetPath,
                              std::function<void(bool, const QString&)> done);
    void loadSettings();
    void saveSettings();

    PluginManifest m_manifest;
    PathManager* m_paths = nullptr;
    QJsonObject m_settings;
    QNetworkAccessManager* m_network = nullptr;
    QHash<QString, ISpellEngine*> m_engines;
    QString m_writableRoot;
};
