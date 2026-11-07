#pragma once

#include <QString>

namespace DocumentReader {

// File dialog filter shared by the "translate document" action.
QString filter();

// True for formats this build can read as text: txt, markdown, html and docx.
bool isSupported(const QString& path);

// Returns UTF-8 text for plain files and extracted paragraph text for DOCX.
// On failure returns an empty string and sets *error when provided.
QString readText(const QString& path, QString* error = nullptr);

} // namespace DocumentReader
