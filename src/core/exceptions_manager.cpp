#include "exceptions_manager.h"

#include "json_file_store.h"
#include "path_manager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

ExceptionsManager::ExceptionsManager(PathManager* paths, QObject* parent)
    : QObject(parent)
    , m_paths(paths)
{
}

bool ExceptionsManager::load()
{
    m_disabled.clear();
    m_enabled.clear();
    m_disabledMode = false;

    const QString path = filePath();
    if (path.isEmpty() || !QFile::exists(path))
        return true;

    QJsonDocument document;
    if (!JsonFileStore::read(path, &document) || !document.isObject())
        return false;

    const QJsonObject root = document.object();
    loadList(root.value(QStringLiteral("Disabled")).toArray(), m_disabled);
    loadList(root.value(QStringLiteral("Enabled")).toArray(), m_enabled);
    m_disabledMode = root.value(QStringLiteral("DisabledMode")).toBool(false);
    return true;
}

bool ExceptionsManager::save()
{
    const QString path = filePath();
    if (path.isEmpty())
        return false;

    auto toArray = [](const QVector<QPair<QString, QString>>& entries) {
        QJsonArray array;
        for (const auto& entry : entries) {
            QJsonArray pair;
            pair.append(entry.first);
            pair.append(entry.second);
            array.append(pair);
        }
        return array;
    };

    QJsonObject root;
    root.insert(QStringLiteral("Disabled"), toArray(m_disabled));
    root.insert(QStringLiteral("Enabled"), toArray(m_enabled));
    root.insert(QStringLiteral("DisabledMode"), m_disabledMode);

    return JsonFileStore::write(path, QJsonDocument(root));
}

bool ExceptionsManager::isExcepted(const QString& processName, const QString& windowClass) const
{
    if (m_disabledMode)
        return !matches(m_disabled, processName, windowClass);
    return matches(m_disabled, processName, windowClass);
}

void ExceptionsManager::setDisabledMode(bool disabled)
{
    if (m_disabledMode == disabled)
        return;
    m_disabledMode = disabled;
    save();
    emit changed();
}

void ExceptionsManager::addEntry(const QString& processName, const QString& windowClass, bool enabled)
{
    const QString process = processName.trimmed();
    const QString window = windowClass.trimmed();
    if (process.isEmpty() && window.isEmpty())
        return;

    auto& target = enabled ? m_enabled : m_disabled;
    for (const auto& entry : target) {
        if (entry.first.compare(process, Qt::CaseInsensitive) == 0
            && entry.second.compare(window, Qt::CaseInsensitive) == 0) {
            return;
        }
    }
    target.append(qMakePair(process, window));
    save();
    emit changed();
}

void ExceptionsManager::removeEntry(int index, bool enabled)
{
    auto& target = enabled ? m_enabled : m_disabled;
    if (index < 0 || index >= target.size())
        return;
    target.removeAt(index);
    save();
    emit changed();
}

void ExceptionsManager::clear()
{
    m_disabled.clear();
    m_enabled.clear();
    save();
    emit changed();
}

QString ExceptionsManager::filePath() const
{
    if (!m_paths)
        return QString();
    return QDir(m_paths->dataDir()).filePath(QStringLiteral("Exceptions.json"));
}

bool ExceptionsManager::matches(const QVector<QPair<QString, QString>>& entries,
                                const QString& processName, const QString& windowClass)
{
    for (const auto& entry : entries) {
        const bool processMatches = entry.first.isEmpty()
            || containsIgnoreCase(entry.first, processName);
        const bool classMatches = entry.second.isEmpty()
            || containsIgnoreCase(entry.second, windowClass);
        if (processMatches && classMatches)
            return true;
    }
    return false;
}

bool ExceptionsManager::containsIgnoreCase(const QString& pattern, const QString& value)
{
    if (pattern.compare(value, Qt::CaseInsensitive) == 0)
        return true;
    return value.contains(pattern, Qt::CaseInsensitive);
}

void ExceptionsManager::loadList(const QJsonArray& array,
                                 QVector<QPair<QString, QString>>& target)
{
    for (const QJsonValue& value : array) {
        if (!value.isArray())
            continue;
        const QJsonArray pair = value.toArray();
        if (pair.isEmpty())
            continue;
        target.append(qMakePair(pair.at(0).toString(), pair.size() > 1 ? pair.at(1).toString() : QString()));
    }
}
