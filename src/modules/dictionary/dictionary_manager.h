#pragma once

#include "xdxf_dictionary.h"
#include "services/provider_interface.h"

#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVector>

#include <functional>

class PathManager;
class SettingsManager;
class PluginManager;

class DictionaryManager : public QObject
{
    Q_OBJECT

public:
    DictionaryManager(PathManager* paths, SettingsManager* settings,
                      PluginManager* plugins, QObject* parent = nullptr);

    QStringList localPaths() const;
    bool addLocalDictionary(const QString& path);
    void removeLocalDictionary(const QString& path);

    void searchLocalAsync(const QString& word, bool exactMatch,
                          std::function<void(const QVector<DictionaryEntry>&)> callback);
    void searchOnline(const QString& word, const QString& from, const QString& to,
                      const QStringList& providerIds,
                      std::function<void(const QString&, const QJsonObject&)> callback,
                      bool exact = false);
    QList<PluginManifest> onlineDictionaries() const;

    QStringList history() const { return m_history; }
    void addHistory(const QString& word);

    QString historyPath() const;

private:
    void loadHistory();
    void saveHistory();

    PathManager* m_paths = nullptr;
    SettingsManager* m_settings = nullptr;
    PluginManager* m_plugins = nullptr;
    QStringList m_history;
};
