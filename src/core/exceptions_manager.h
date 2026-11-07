#pragma once

#include <QObject>
#include <QPair>
#include <QString>
#include <QVector>

class PathManager;
class QJsonArray;

class ExceptionsManager : public QObject
{
    Q_OBJECT

public:
    explicit ExceptionsManager(PathManager* paths, QObject* parent = nullptr);

    bool load();
    bool save();

    bool isExcepted(const QString& processName, const QString& windowClass) const;
    bool disabledMode() const { return m_disabledMode; }
    void setDisabledMode(bool disabled);

    QVector<QPair<QString, QString>> disabledEntries() const { return m_disabled; }

    void addEntry(const QString& processName, const QString& windowClass, bool enabled);
    void removeEntry(int index, bool enabled);
    void clear();

    QString filePath() const;

signals:
    void changed();

private:
    static bool matches(const QVector<QPair<QString, QString>>& entries,
                        const QString& processName, const QString& windowClass);
    static bool containsIgnoreCase(const QString& pattern, const QString& value);
    void loadList(const QJsonArray& array, QVector<QPair<QString, QString>>& target);

    PathManager* m_paths = nullptr;
    QVector<QPair<QString, QString>> m_disabled;
    QVector<QPair<QString, QString>> m_enabled;
    bool m_disabledMode = false;
};

