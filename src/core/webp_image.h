#pragma once

#include <QImage>
#include <QString>

// Loads an image file with Qt first and falls back to the bundled libwebp
// decoder when the Qt build has no WebP plugin.
QImage loadImageFile(const QString& path);
