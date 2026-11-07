#pragma once

#include <QJsonDocument>
#include <QString>

namespace JsonFileStore {

// Reads and parses a JSON document. Returns false on I/O or parse errors.
bool read(const QString& path, QJsonDocument* document);

// Writes a JSON document atomically (QSaveFile), creating parent directories.
bool write(const QString& path, const QJsonDocument& document);

} // namespace JsonFileStore
