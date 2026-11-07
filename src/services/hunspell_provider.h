#pragma once

#include "spell_provider_base.h"

class HunspellProvider : public SpellProviderBase
{
    Q_OBJECT

public:
    explicit HunspellProvider(const PluginManifest& manifest, PathManager* paths,
                              QObject* parent = nullptr);

protected:
    ISpellEngine* createEngine(const QString& affPath, const QString& dicPath,
                               QString* error) override;
};
