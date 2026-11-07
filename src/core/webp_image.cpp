#include "webp_image.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QVector>

#include <cstdint>

namespace {

using WebPGetInfoFunc = int (*)(const uint8_t*, size_t, int*, int*);
using WebPDecodeRGBAFunc = uint8_t* (*)(const uint8_t*, size_t, int*, int*);
using WebPFreeFunc = void (*)(void*);

QImage decodeWebP(const QString& path)
{
    static QLibrary library;
    static bool resolved = false;
    static WebPGetInfoFunc getInfo = nullptr;
    static WebPDecodeRGBAFunc decode = nullptr;
    static WebPFreeFunc release = nullptr;

    if (!resolved) {
        resolved = true;
        const QString appDir = QCoreApplication::applicationDirPath();
        const QStringList candidates = {
            QDir(appDir).filePath(QStringLiteral("plugins/libwebpdecoder.dll")),
            QDir(appDir).filePath(QStringLiteral("libwebpdecoder.dll"))};
        for (const QString& candidate : candidates) {
            if (!QFileInfo::exists(candidate))
                continue;
            library.setFileName(candidate);
            if (!library.load())
                continue;
            getInfo = reinterpret_cast<WebPGetInfoFunc>(library.resolve("WebPGetInfo"));
            decode = reinterpret_cast<WebPDecodeRGBAFunc>(library.resolve("WebPDecodeRGBA"));
            release = reinterpret_cast<WebPFreeFunc>(library.resolve("WebPFree"));
            if (getInfo && decode && release)
                break;
        }
    }

    if (!getInfo || !decode || !release)
        return {};

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty())
        return {};

    int width = 0;
    int height = 0;
    if (!getInfo(reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size()),
                 &width, &height)
        || width <= 0 || height <= 0) {
        return {};
    }

    uint8_t* pixels = decode(reinterpret_cast<const uint8_t*>(bytes.constData()),
                             size_t(bytes.size()), &width, &height);
    if (!pixels)
        return {};

    QImage image(pixels, width, height, width * 4, QImage::Format_RGBA8888);
    const QImage copy = image.copy();
    release(pixels);
    return copy;
}

}

QImage loadImageFile(const QString& path)
{
    QImage image(path);
    if (!image.isNull())
        return image;

    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("webp"))
        return decodeWebP(path);
    return image;
}
