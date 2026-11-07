#include "nuspell_engine.h"

#include <nuspell/dictionary.hxx>

#include <filesystem>
#include <string>
#include <vector>

struct NuspellEngine::Impl
{
    nuspell::Dictionary dictionary;
    bool loaded = false;
};

NuspellEngine::NuspellEngine()
    : m_impl(std::make_unique<Impl>())
{
}

NuspellEngine::~NuspellEngine() = default;

bool NuspellEngine::load(const QString& affPath, QString* error)
{
    try {
#ifdef Q_OS_WIN
        const std::filesystem::path path(affPath.toStdWString());
#else
        const std::filesystem::path path(affPath.toStdString());
#endif
        m_impl->dictionary.load_aff_dic(path);
        m_impl->loaded = true;
        return true;
    } catch (const std::exception& exception) {
        m_impl->loaded = false;
        if (error)
            *error = QString::fromUtf8(exception.what());
        return false;
    }
}

bool NuspellEngine::isLoaded() const
{
    return m_impl->loaded;
}

bool NuspellEngine::spell(const QString& word) const
{
    if (!m_impl->loaded || word.isEmpty())
        return true;
    try {
        return m_impl->dictionary.spell(word.toStdString());
    } catch (...) {
        return true;
    }
}

QStringList NuspellEngine::suggest(const QString& word, int limit) const
{
    QStringList result;
    if (!m_impl->loaded || word.isEmpty() || limit <= 0)
        return result;
    try {
        std::vector<std::string> suggestions;
        m_impl->dictionary.suggest(word.toStdString(), suggestions);
        for (const std::string& suggestion : suggestions) {
            result.append(QString::fromStdString(suggestion));
            if (result.size() >= limit)
                break;
        }
    } catch (...) {
        result.clear();
    }
    return result;
}
