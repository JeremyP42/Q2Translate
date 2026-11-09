#include "dictionary_manager.h"

#include "core/path_manager.h"
#include "core/settings_manager.h"
#include "services/plugin_manager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QSaveFile>
#include <QtConcurrent>

#include <utility>

DictionaryManager::DictionaryManager(PathManager* paths, SettingsManager* settings,
                                     PluginManager* plugins, QObject* parent)
    : QObject(parent)
    , m_paths(paths)
    , m_settings(settings)
    , m_plugins(plugins)
{
    loadHistory();
}

QStringList DictionaryManager::localPaths() const
{
    QStringList result;
    QSet<QString> seen;

    if (m_settings) {
        const QJsonArray values = m_settings->value(QStringLiteral("OfflineDictionaries")).toArray();
        for (const QJsonValue& value : values) {
            QString path;
            if (value.isString())
                path = value.toString();
            else if (value.isObject())
                path = value.toObject().value(QStringLiteral("path")).toString();
            if (!path.isEmpty() && !seen.contains(path) && QFileInfo::exists(path)) {
                seen.insert(path);
                result.append(path);
            }
        }
    }

    return result;
}

bool DictionaryManager::addLocalDictionary(const QString& path)
{
    if (!m_settings || !QFileInfo::exists(path))
        return false;
    QJsonArray values = m_settings->value(QStringLiteral("OfflineDictionaries")).toArray();
    for (const QJsonValue& value : values) {
        if (value.toString() == path || value.toObject().value(QStringLiteral("path")).toString() == path)
            return true;
    }
    values.append(path);
    m_settings->setValue(QStringLiteral("OfflineDictionaries"), values);
    m_settings->save();
    return true;
}

void DictionaryManager::removeLocalDictionary(const QString& path)
{
    if (!m_settings)
        return;
    QJsonArray values = m_settings->value(QStringLiteral("OfflineDictionaries")).toArray();
    QJsonArray updated;
    for (const QJsonValue& value : values) {
        const QString current = value.isString()
            ? value.toString()
            : value.toObject().value(QStringLiteral("path")).toString();
        if (current != path)
            updated.append(value);
    }
    m_settings->setValue(QStringLiteral("OfflineDictionaries"), updated);
    m_settings->save();
}

void DictionaryManager::searchLocalAsync(const QString& word, bool exactMatch,
                                         std::function<void(const QVector<DictionaryEntry>&)> callback)
{
    const QStringList paths = localPaths();
    auto future = QtConcurrent::run([paths, word, exactMatch] {
        QVector<DictionaryEntry> result;
        for (const QString& path : paths) {
            XdxfDictionary dictionary(path);
            QString error;
            if (!dictionary.open(&error))
                continue;
            result += dictionary.search(word, exactMatch);
        }
        return result;
    });

    auto* watcher = new QFutureWatcher<QVector<DictionaryEntry>>(this);
    connect(watcher, &QFutureWatcher<QVector<DictionaryEntry>>::finished, this,
            [watcher, callback = std::move(callback)] {
                const QVector<DictionaryEntry> result = watcher->result();
                watcher->deleteLater();
                if (callback)
                    callback(result);
            });
    watcher->setFuture(future);
}

void DictionaryManager::searchOnline(const QString& word, const QString& from, const QString& to,
                                     const QStringList& providerIds,
                                     std::function<void(const QString&, const QJsonObject&)> callback,
                                     bool exact)
{
    if (!m_plugins || providerIds.isEmpty())
        return;

    QJsonObject params;
    params.insert(QStringLiteral("word"), word);
    params.insert(QStringLiteral("text"), word);
    params.insert(QStringLiteral("from"), from);
    params.insert(QStringLiteral("to"), to);
    params.insert(QStringLiteral("exact"), exact);

    for (IPluginProvider* provider : m_plugins->providersFor(QStringLiteral("dictionary"))) {
        if (!providerIds.contains(provider->id()))
            continue;
        const QString name = provider->manifest().name;
        provider->executeAsync(QStringLiteral("dictionary"), params,
            [name, callback](const QJsonObject& result) {
                if (callback)
                    callback(name, result);
            });
    }
}

QList<PluginManifest> DictionaryManager::onlineDictionaries() const
{
    QList<PluginManifest> result;
    if (!m_plugins)
        return result;
    for (const PluginManifest& manifest : m_plugins->manifests()) {
        if (manifest.valid
            && manifest.capabilities.contains(QStringLiteral("dictionary"))
            && m_plugins->provider(manifest.id)
            && !m_plugins->isDisabled(manifest.id)) {
            result.append(manifest);
        }
    }
    return result;
}

void DictionaryManager::addHistory(const QString& word)
{
    const QString trimmed = word.trimmed();
    if (trimmed.isEmpty())
        return;
    m_history.removeAll(trimmed);
    m_history.prepend(trimmed);
    while (m_history.size() > 30)
        m_history.removeLast();
    saveHistory();
}

QString DictionaryManager::historyPath() const
{
    if (!m_paths)
        return QString();
    return QDir(m_paths->dataDir()).filePath(QStringLiteral("DictionaryHistory.json"));
}

void DictionaryManager::loadHistory()
{
    m_history.clear();
    const QString path = historyPath();
    if (path.isEmpty() || !QFile::exists(path))
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError)
        return;
    const QJsonArray array = document.isArray()
        ? document.array()
        : document.object().value(QStringLiteral("History")).toArray();
    for (const QJsonValue& value : array) {
        const QString word = value.toString();
        if (!word.isEmpty())
            m_history.append(word);
    }
}

void DictionaryManager::saveHistory()
{
    const QString path = historyPath();
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    const QByteArray data = QJsonDocument(QJsonArray::fromStringList(m_history))
                                .toJson(QJsonDocument::Indented);
    if (file.write(data) == data.size())
        file.commit();
}
