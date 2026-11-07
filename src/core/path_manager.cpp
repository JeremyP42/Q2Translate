#include "path_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

bool PathManager::init()
{
    m_applicationDir = QDir::cleanPath(QCoreApplication::applicationDirPath());
    m_shippedDir = m_applicationDir;

    const bool portableMarker = QFileInfo::exists(
        QDir(m_applicationDir).filePath(QStringLiteral("portable.txt")));

    if (portableMarker) {
        m_rootDir = m_applicationDir;
    } else {
        // The application was renamed from QTranslate to Q2Translate.  Move the
        // old profile once so existing settings and dictionaries are preserved.
        const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir appDataDir(appData);
        if (appDataDir.cdUp()) {
            const QString newRoot = QDir::cleanPath(
                appDataDir.filePath(QStringLiteral("Q2Translate")));
            const QString oldRoot = QDir::cleanPath(
                appDataDir.filePath(QStringLiteral("QTranslate")));
            if (!QFileInfo::exists(newRoot) && QFileInfo::exists(oldRoot)
                && !QDir().rename(oldRoot, newRoot)) {
                // Renaming is blocked: keep using the old profile rather than
                // silently starting with an empty one.
                m_rootDir = oldRoot;
            } else {
                m_rootDir = newRoot;
            }
        } else {
            m_rootDir = QDir::cleanPath(appData);
        }
    }

    m_dataDir = QDir(m_rootDir).filePath(QStringLiteral("Data"));
    return ensureDataDirectories();
}

QString PathManager::userServicesDir() const
{
    return QDir(m_dataDir).filePath(QStringLiteral("services"));
}

QString PathManager::logDir() const
{
    return QDir(m_dataDir).filePath(QStringLiteral("Logs"));
}

QString PathManager::settingsDefaultPath() const
{
    const QString shipped = QDir(m_shippedDir).filePath(QStringLiteral("settings-default.json"));
    if (QFileInfo::exists(shipped))
        return shipped;
    return QDir(m_rootDir).filePath(QStringLiteral("settings-default.json"));
}

QString PathManager::userSettingsPath() const
{
    return QDir(m_dataDir).filePath(QStringLiteral("settings.json"));
}

bool PathManager::ensureDataDirectories() const
{
    bool ok = QDir().mkpath(m_dataDir);
    ok = QDir().mkpath(userServicesDir()) && ok;
    ok = QDir().mkpath(logDir()) && ok;
    return ok;
}
