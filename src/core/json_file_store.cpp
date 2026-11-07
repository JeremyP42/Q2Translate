#include "json_file_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonParseError>
#include <QSaveFile>

namespace JsonFileStore {

bool read(const QString& path, QJsonDocument* document)
{
    if (!document || path.isEmpty())
        return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonParseError error;
    const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError)
        return false;

    *document = parsed;
    return true;
}

bool write(const QString& path, const QJsonDocument& document)
{
    if (path.isEmpty())
        return false;

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    const QByteArray data = document.toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit())
        return false;
    return true;
}

} // namespace JsonFileStore
