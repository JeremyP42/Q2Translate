#pragma once

#include "provider_interface.h"
#include "spell_provider.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

struct ISpellChecker;
struct ISpellCheckerFactory;

class WindowsSpellProvider : public IPluginProvider, public ISpellProvider
{
    Q_OBJECT

public:
    explicit WindowsSpellProvider(const PluginManifest& manifest, QObject* parent = nullptr);
    ~WindowsSpellProvider() override;

    const PluginManifest& manifest() const override { return m_manifest; }
    QJsonObject settings() const override { return m_settings; }
    bool updateSettings(const QJsonObject& settings) override;
    QJsonObject defaultSettings() const override { return {}; }
    void executeAsync(const QString& capability, const QJsonObject& params,
                      PluginCallback callback) override;

    QList<SpellLanguage> spellLanguages() const override;
    QString spellDictionaryPath() const override { return QString(); }
    bool checkSpelling(const QString& word, const QString& language,
                       QStringList* suggestions = nullptr) override;
    void installSpellLanguage(const QString& code,
                              std::function<void(bool, const QString&)> done) override;
    void removeSpellLanguage(const QString& code,
                             std::function<void(bool, const QString&)> done) override;

private:
    bool ensureFactory();
    bool ensureChecker(const QString& language);
    QString languageTag(const QString& code) const;
    void releaseChecker();

    PluginManifest m_manifest;
    QJsonObject m_settings;
    ISpellCheckerFactory* m_factory = nullptr;
    ISpellChecker* m_checker = nullptr;
    QString m_checkerTag;
    QHash<QString, bool> m_cache;
    bool m_comInitialized = false;
};
