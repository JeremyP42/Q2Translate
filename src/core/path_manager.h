#pragma once

#include <QString>

class PathManager
{
public:
    PathManager() = default;

    bool init();

    const QString& shippedDir() const { return m_shippedDir; }
    const QString& rootDir() const { return m_rootDir; }
    const QString& dataDir() const { return m_dataDir; }

    QString userServicesDir() const;
    QString logDir() const;
    QString settingsDefaultPath() const;
    QString userSettingsPath() const;

private:
    bool ensureDataDirectories() const;

    QString m_applicationDir;
    QString m_shippedDir;
    QString m_rootDir;
    QString m_dataDir;
};
