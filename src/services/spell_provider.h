#pragma once

#include <QString>
#include <QStringList>
#include <QList>

#include <functional>

// A language offered by a spelling connector.  "downloadable" languages can be
// installed and removed by the application; system languages are managed by
// the operating system.
struct SpellLanguage
{
    QString code;
    QString name;
    bool installed = false;
    bool downloadable = false;
    bool removable = false;
};

// Implemented by native spelling connectors.  The interface is intentionally
// independent from IPluginProvider so the settings UI can talk to a connector
// without knowing how it is implemented.
class ISpellProvider
{
public:
    virtual ~ISpellProvider() = default;

    virtual QList<SpellLanguage> spellLanguages() const = 0;
    virtual QString spellDictionaryPath() const = 0;
    virtual bool checkSpelling(const QString& word, const QString& language,
                               QStringList* suggestions = nullptr) = 0;
    virtual void installSpellLanguage(const QString& code,
                                      std::function<void(bool, const QString&)> done) = 0;
    virtual void removeSpellLanguage(const QString& code,
                                     std::function<void(bool, const QString&)> done) = 0;
};
