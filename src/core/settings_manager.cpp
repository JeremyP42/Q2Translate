#include "settings_manager.h"

#include "path_manager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStringList>

SettingsManager::SettingsManager(PathManager* paths, QObject* parent)
    : QObject(parent)
    , m_paths(paths)
{
    if (m_paths) {
        m_defaultsPath = m_paths->settingsDefaultPath();
        m_userPath = m_paths->userSettingsPath();
    }
}

bool SettingsManager::load(QString* error)
{
    m_lastError.clear();
    m_settings = QJsonObject();

    if (!m_paths) {
        m_lastError = QStringLiteral("PathManager не инициализирован.");
        if (error)
            *error = m_lastError;
        return false;
    }

    m_defaultsPath = m_paths->settingsDefaultPath();
    m_userPath = m_paths->userSettingsPath();

    QString defaultsError;
    const QJsonObject defaults = readJson(m_defaultsPath, &defaultsError);
    if (!defaultsError.isEmpty()) {
        m_lastError = QStringLiteral("Не удалось прочитать %1: %2")
                          .arg(m_defaultsPath, defaultsError);
        if (error)
            *error = m_lastError;
        return false;
    }

    if (QFileInfo::exists(m_userPath)) {
        QString userError;
        const QJsonObject user = readJson(m_userPath, &userError);
        if (!userError.isEmpty()) {
            m_lastError = QStringLiteral("Не удалось прочитать %1: %2")
                              .arg(m_userPath, userError);
            if (error)
                *error = m_lastError;
            return false;
        }
        m_settings = user;
        mergeDefaults(m_settings, defaults);
    } else {
        m_settings = defaults;
        if (!save(error))
            return false;
    }

    emit changed();
    return true;
}

bool SettingsManager::save(QString* error)
{
    if (!m_autosaveEnabled)
        return true;
    if (m_userPath.isEmpty()) {
        m_lastError = QStringLiteral("Путь к пользовательским настройкам не задан.");
        if (error)
            *error = m_lastError;
        return false;
    }

    QDir().mkpath(QFileInfo(m_userPath).absolutePath());

    QSaveFile file(m_userPath);
    if (!file.open(QIODevice::WriteOnly)) {
        m_lastError = QStringLiteral("Не удалось открыть %1: %2")
                          .arg(m_userPath, file.errorString());
        if (error)
            *error = m_lastError;
        return false;
    }

    const QByteArray data = QJsonDocument(m_settings).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        m_lastError = QStringLiteral("Не удалось записать %1: %2")
                          .arg(m_userPath, file.errorString());
        file.cancelWriting();
        if (error)
            *error = m_lastError;
        return false;
    }

    if (!file.commit()) {
        m_lastError = QStringLiteral("Не удалось сохранить %1: %2")
                          .arg(m_userPath, file.errorString());
        if (error)
            *error = m_lastError;
        return false;
    }

    return true;
}

QJsonValue SettingsManager::value(const QString& keyPath, const QJsonValue& fallback) const
{
    const QJsonValue found = findValue(m_settings, keyPath);
    return found.isUndefined() ? fallback : found;
}

QString SettingsManager::stringValue(const QString& keyPath, const QString& fallback) const
{
    return value(keyPath).toString(fallback);
}

int SettingsManager::intValue(const QString& keyPath, int fallback) const
{
    return value(keyPath).toInt(fallback);
}

bool SettingsManager::boolValue(const QString& keyPath, bool fallback) const
{
    return value(keyPath).toBool(fallback);
}

void SettingsManager::setValue(const QString& keyPath, const QJsonValue& value)
{
    if (keyPath.isEmpty())
        return;
    insertValue(m_settings, keyPath, value);
    emit changed();
}

void SettingsManager::setRoot(const QJsonObject& root)
{
    m_settings = root;
    emit changed();
}

QJsonObject SettingsManager::readJson(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return QJsonObject();
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = parseError.errorString();
        return QJsonObject();
    }

    if (!document.isObject()) {
        if (error)
            *error = QStringLiteral("корневой элемент JSON должен быть объектом");
        return QJsonObject();
    }

    return document.object();
}

void SettingsManager::mergeDefaults(QJsonObject& target, const QJsonObject& defaults)
{
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
        const QJsonValue existing = target.value(it.key());
        if (existing.isUndefined() || existing.isNull()) {
            target.insert(it.key(), it.value());
        } else if (existing.isObject() && it.value().isObject()) {
            QJsonObject child = existing.toObject();
            mergeDefaults(child, it.value().toObject());
            target.insert(it.key(), child);
        }
    }
}

QJsonValue SettingsManager::findValue(const QJsonObject& object, const QString& keyPath)
{
    QJsonValue current = QJsonValue(object);
    const QStringList parts = keyPath.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (!current.isObject())
            return QJsonValue();
        current = current.toObject().value(part);
        if (current.isUndefined())
            return QJsonValue();
    }
    return current;
}

namespace {
void insertNested(QJsonObject& target, const QStringList& parts, int index, const QJsonValue& value)
{
    if (index == parts.size() - 1) {
        target.insert(parts.at(index), value);
        return;
    }

    QJsonObject child = target.value(parts.at(index)).toObject();
    insertNested(child, parts, index + 1, value);
    target.insert(parts.at(index), child);
}
}

void SettingsManager::insertValue(QJsonObject& object, const QString& keyPath, const QJsonValue& value)
{
    const QStringList parts = keyPath.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return;

    insertNested(object, parts, 0, value);
}
