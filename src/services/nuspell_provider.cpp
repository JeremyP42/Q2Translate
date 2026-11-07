#include "nuspell_provider.h"

#include "nuspell_engine.h"

NuspellProvider::NuspellProvider(const PluginManifest& manifest, PathManager* paths,
                                 QObject* parent)
    : SpellProviderBase(manifest, paths, parent)
{
}

ISpellEngine* NuspellProvider::createEngine(const QString& affPath, const QString&,
                                            QString* error)
{
    auto* engine = new NuspellEngine;
    if (!engine->load(affPath, error)) {
        delete engine;
        return nullptr;
    }
    return engine;
}
