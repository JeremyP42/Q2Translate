#pragma once

#include "spell_provider_base.h"

#include <QString>
#include <QStringList>

#include <memory>

// Thin owner around nuspell::Dictionary.  The Nuspell and ICU headers stay in
// this translation unit so the rest of the application does not depend on
// their include layout.
class NuspellEngine : public ISpellEngine
{
public:
    NuspellEngine();
    ~NuspellEngine() override;

    bool load(const QString& affPath, QString* error = nullptr);
    bool isLoaded() const override;
    bool spell(const QString& word) const override;
    QStringList suggest(const QString& word, int limit = 7) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
