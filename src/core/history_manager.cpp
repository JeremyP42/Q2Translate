#include "history_manager.h"

#include "json_file_store.h"
#include "path_manager.h"
#include "settings_manager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>

HistoryManager::HistoryManager(PathManager* paths, SettingsManager* settings, QObject* parent)
    : QObject(parent)
    , m_paths(paths)
    , m_settings(settings)
{
}

int HistoryManager::limit() const
{
    const int value = m_settings
        ? m_settings->intValue(QStringLiteral("Application.HistoryLimit"), 50) : 50;
    return qBound(10, value, 10000);
}

bool HistoryManager::load()
{
    m_entries = QJsonArray();
    const QString path = filePath();
    if (path.isEmpty() || !QFile::exists(path))
        return true;

    QJsonDocument document;
    if (!JsonFileStore::read(path, &document))
        return false;

    if (document.isArray()) {
        m_entries = document.array();
    } else if (document.isObject()) {
        m_entries = document.object().value(QStringLiteral("History")).toArray();
    }

    while (m_entries.size() > limit())
        m_entries.removeLast();

    return true;
}

bool HistoryManager::save()
{
    const QString path = filePath();
    if (path.isEmpty())
        return false;

    return JsonFileStore::write(path, QJsonDocument(m_entries));
}

void HistoryManager::addEntry(const QString& source, const QString& translation,
                              const QString& from, const QString& to, const QString& service)
{
    if (source.trimmed().isEmpty() || translation.trimmed().isEmpty())
        return;

    if (!m_entries.isEmpty()) {
        const QJsonObject newest = m_entries.first().toObject();
        if (newest.value(QStringLiteral("source")).toString() == source
            && newest.value(QStringLiteral("translation")).toString() == translation
            && newest.value(QStringLiteral("service")).toString() == service) {
            return;
        }
    }

    QJsonObject entry;
    entry.insert(QStringLiteral("source"), source);
    entry.insert(QStringLiteral("translation"), translation);
    entry.insert(QStringLiteral("from"), from);
    entry.insert(QStringLiteral("to"), to);
    entry.insert(QStringLiteral("service"), service);
    entry.insert(QStringLiteral("favorite"), false);
    entry.insert(QStringLiteral("timestamp"),
                 QDateTime::currentDateTime().toString(Qt::ISODate));

    m_entries.prepend(entry);
    while (m_entries.size() > limit())
        m_entries.removeLast();

    save();
    emit changed();
}

void HistoryManager::clear()
{
    if (m_entries.isEmpty())
        return;
    m_entries = QJsonArray();
    save();
    emit changed();
}

void HistoryManager::removeAt(int index)
{
    if (index < 0 || index >= m_entries.size())
        return;
    m_entries.removeAt(index);
    save();
    emit changed();
}

QJsonObject HistoryManager::entryAt(int index) const
{
    if (index < 0 || index >= m_entries.size())
        return QJsonObject();
    return m_entries.at(index).toObject();
}

QString HistoryManager::filePath() const
{
    if (!m_paths)
        return QString();
    return QDir(m_paths->dataDir()).filePath(QStringLiteral("History.json"));
}

