#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

class PathManager;
class SettingsManager;

class HistoryManager : public QObject
{
    Q_OBJECT

public:
    HistoryManager(PathManager* paths, SettingsManager* settings, QObject* parent = nullptr);

    bool load();
    bool save();

    void addEntry(const QString& source, const QString& translation,
                  const QString& from, const QString& to, const QString& service);
    void clear();
    void removeAt(int index);

    const QJsonArray& entries() const { return m_entries; }
    int count() const { return int(m_entries.size()); }
    QJsonObject entryAt(int index) const;
    QString filePath() const;

signals:
    void changed();

private:
    int limit() const;
    PathManager* m_paths = nullptr;
    SettingsManager* m_settings = nullptr;
    QJsonArray m_entries;
};
