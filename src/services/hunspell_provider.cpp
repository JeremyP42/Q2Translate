#include "hunspell_provider.h"

#include "hunspell_engine.h"

HunspellProvider::HunspellProvider(const PluginManifest& manifest, PathManager* paths,
                                   QObject* parent)
    : SpellProviderBase(manifest, paths, parent)
{
}

ISpellEngine* HunspellProvider::createEngine(const QString& affPath, const QString& dicPath,
                                             QString* error)
{
    auto* engine = new HunspellEngine;
    if (!engine->load(affPath, dicPath, error)) {
        delete engine;
        return nullptr;
    }
    return engine;
}
