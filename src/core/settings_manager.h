#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

class PathManager;

class SettingsManager : public QObject
{
    Q_OBJECT

public:
    explicit SettingsManager(PathManager* paths, QObject* parent = nullptr);

    bool load(QString* error = nullptr);
    bool save(QString* error = nullptr);

    // When disabled, save() becomes a no-op.  OptionsDialog uses this to keep
    // live previews in memory until the user presses Apply/OK.
    void setAutosaveEnabled(bool enabled) { m_autosaveEnabled = enabled; }

    const QJsonObject& root() const { return m_settings; }
    QJsonValue value(const QString& keyPath, const QJsonValue& fallback = QJsonValue()) const;
    QString stringValue(const QString& keyPath, const QString& fallback = QString()) const;
    int intValue(const QString& keyPath, int fallback = 0) const;
    bool boolValue(const QString& keyPath, bool fallback = false) const;

    void setValue(const QString& keyPath, const QJsonValue& value);
    void setRoot(const QJsonObject& root);

    QString userSettingsPath() const { return m_userPath; }

signals:
    void changed();

private:
    static QJsonObject readJson(const QString& path, QString* error);
    static void mergeDefaults(QJsonObject& target, const QJsonObject& defaults);
    static QJsonValue findValue(const QJsonObject& object, const QString& keyPath);
    static void insertValue(QJsonObject& object, const QString& keyPath, const QJsonValue& value);

    PathManager* m_paths = nullptr;
    QJsonObject m_settings;
    QString m_defaultsPath;
    QString m_userPath;
    QString m_lastError;
    bool m_autosaveEnabled = true;
};
