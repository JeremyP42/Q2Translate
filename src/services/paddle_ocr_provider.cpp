#define ORT_API_MANUAL_INIT
#ifdef _WIN32
// The ORT header spells the calling convention as _stdcall, which MinGW only
// defines through windows headers; provide it before onnxruntime is included.
#ifndef _stdcall
#define _stdcall __stdcall
#endif
// SAL annotations from sal.h are MSVC-only; MinGW needs them as empty macros.
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#ifndef _In_
#define _In_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#endif
#include "paddle_ocr_provider.h"

#include "ocr_layout.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>
#include <QtConcurrent>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <memory>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

QJsonObject ocrError(const QString& message)
{
    QJsonObject result;
    result.insert(QStringLiteral("error"), message);
    return result;
}

struct Box {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

struct Engine {
    QString error;
    std::unique_ptr<Ort::Session> detector;
    std::unique_ptr<Ort::Session> recognizer;
    std::vector<std::string> characters;
    QString directory;
};

bool initialiseOnnxRuntime(const QString& directory, QString* error)
{
#ifdef Q_OS_WIN
    static bool loaded = false;
    static QString loadedDirectory;
    if (loaded && loadedDirectory == directory)
        return true;

    const QString library = QDir(directory).filePath(QStringLiteral("onnxruntime.dll"));
    if (!QFileInfo::exists(library)) {
        *error = QStringLiteral("onnxruntime.dll не найден: %1").arg(library);
        return false;
    }

    const std::wstring path = QDir::toNativeSeparators(library).toStdWString();
    HMODULE module = LoadLibraryW(path.c_str());
    if (!module) {
        *error = QStringLiteral("Не удалось загрузить onnxruntime.dll");
        return false;
    }

    using GetApiBase = const OrtApiBase* (*)();
    auto getApiBase = reinterpret_cast<GetApiBase>(
        reinterpret_cast<void*>(GetProcAddress(module, "OrtGetApiBase")));
    if (!getApiBase) {
        *error = QStringLiteral("onnxruntime.dll: нет OrtGetApiBase");
        return false;
    }
    Ort::Global<void>::api_ = getApiBase()->GetApi(ORT_API_VERSION);
    loaded = true;
    loadedDirectory = directory;
    return true;
#else
    Q_UNUSED(directory);
    *error = QStringLiteral("PaddleOCR connector поддерживается только в Windows-сборке.");
    return false;
#endif
}

QVector<Box> detectBoxes(Ort::Session& session, const QImage& image)
{
    QVector<Box> boxes;
    const int originalWidth = image.width();
    const int originalHeight = image.height();
    if (originalWidth <= 0 || originalHeight <= 0)
        return boxes;

    const double scale = 960.0 / qMax(originalWidth, originalHeight);
    const int height = qMax(32, int(qRound(originalHeight * scale / 32)) * 32);
    const int width = qMax(32, int(qRound(originalWidth * scale / 32)) * 32);
    const QImage scaled = image.scaled(width, height, Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGB888);

    const float mean[3] = {0.485f, 0.456f, 0.406f};
    const float std[3] = {0.229f, 0.224f, 0.225f};
    std::vector<float> input(size_t(3) * width * height);
    for (int y = 0; y < height; ++y) {
        const uchar* line = scaled.constScanLine(y);
        for (int x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                const float value = line[x * 3 + channel] / 255.0f;
                input[size_t(channel) * width * height + size_t(y) * width + x] =
                    (value - mean[channel]) / std[channel];
            }
        }
    }

    const std::array<int64_t, 4> shape{1, 3, height, width};
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                                        shape.data(), shape.size());
    Ort::AllocatorWithDefaultOptions allocator;
    const auto inputNameHolder = session.GetInputNameAllocated(0, allocator);
    const auto outputNameHolder = session.GetOutputNameAllocated(0, allocator);
    const char* inputName = inputNameHolder.get();
    const char* outputName = outputNameHolder.get();
    auto outputs = session.Run(Ort::RunOptions{nullptr}, &inputName, &tensor, 1, &outputName, 1);

    const auto info = outputs.front().GetTensorTypeAndShapeInfo();
    const std::vector<int64_t> outputShape = info.GetShape();
    const float* data = outputs.front().GetTensorData<float>();
    if (outputShape.size() < 4)
        return boxes;

    const int mapHeight = int(outputShape.at(2));
    const int mapWidth = int(outputShape.at(3));

    std::vector<uchar> mask(size_t(mapHeight) * mapWidth, 0);
    for (int y = 0; y < mapHeight; ++y) {
        for (int x = 0; x < mapWidth; ++x) {
            mask[size_t(y) * mapWidth + x] = data[size_t(y) * mapWidth + x] > 0.3f ? 1 : 0;
        }
    }

    // Connected components give axis-aligned regions; manga bubbles and captions are
    // close enough to boxes that rotated rectangles are not worth the extra code.
    std::vector<int> labels(mask.size(), 0);
    std::vector<int> stack;
    int label = 0;
    const double scaleX = double(originalWidth) / mapWidth;
    const double scaleY = double(originalHeight) / mapHeight;
    for (int y = 0; y < mapHeight; ++y) {
        for (int x = 0; x < mapWidth; ++x) {
            const int index = y * mapWidth + x;
            if (mask[size_t(index)] == 0 || labels[size_t(index)] != 0)
                continue;
            ++label;
            int left = x, right = x, top = y, bottom = y, area = 0;
            stack.clear();
            stack.push_back(index);
            labels[size_t(index)] = label;
            while (!stack.empty()) {
                const int current = stack.back();
                stack.pop_back();
                const int cx = current % mapWidth;
                const int cy = current / mapWidth;
                ++area;
                left = qMin(left, cx); right = qMax(right, cx);
                top = qMin(top, cy); bottom = qMax(bottom, cy);
                const int neighbours[4] = {current - 1, current + 1,
                                           current - mapWidth, current + mapWidth};
                for (int slot = 0; slot < 4; ++slot) {
                    const int next = neighbours[slot];
                    if (next < 0 || next >= mapWidth * mapHeight)
                        continue;
                    if (slot < 2 && (next / mapWidth) != cy)
                        continue;
                    if (mask[size_t(next)] == 0 || labels[size_t(next)] != 0)
                        continue;
                    labels[size_t(next)] = label;
                    stack.push_back(next);
                }
            }
            if (area < 20)
                continue;
            Box box;
            box.left = int(left * scaleX);
            box.top = int(top * scaleY);
            box.width = int((right - left + 1) * scaleX);
            box.height = int((bottom - top + 1) * scaleY);
            if (box.width >= 8 && box.height >= 6)
                boxes.append(box);
        }
    }

    std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) {
        if (qAbs(a.top - b.top) > 12)
            return a.top < b.top;
        return a.left < b.left;
    });
    return boxes;
}

QString recognize(Ort::Session& session, const QImage& image, const Box& box,
                  const std::vector<std::string>& characters)
{
    QRect crop(box.left - 2, box.top - 2, box.width + 4, box.height + 4);
    crop = crop.intersected(image.rect());
    if (crop.isEmpty() || crop.width() < 4 || crop.height() < 4)
        return {};

    const int height = 48;
    const int width = qMax(16, int(qRound(double(crop.width()) * height / crop.height() / 4)) * 4);
    const QImage source = image.copy(crop);
    if (source.isNull())
        return {};
    const QImage scaled = source.scaled(width, height, Qt::IgnoreAspectRatio,
                                        Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGB888);
    if (scaled.isNull() || scaled.width() != width || scaled.height() != height)
        return {};

    std::vector<float> input(size_t(3) * width * height);
    for (int y = 0; y < height; ++y) {
        const uchar* line = scaled.constScanLine(y);
        for (int x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                const float value = line[x * 3 + channel] / 255.0f;
                input[size_t(channel) * width * height + size_t(y) * width + x] =
                    (value - 0.5f) / 0.5f;
            }
        }
    }

    const std::array<int64_t, 4> shape{1, 3, height, width};
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
                                                        shape.data(), shape.size());
    Ort::AllocatorWithDefaultOptions allocator;
    const auto inputNameHolder = session.GetInputNameAllocated(0, allocator);
    const auto outputNameHolder = session.GetOutputNameAllocated(0, allocator);
    const char* inputName = inputNameHolder.get();
    const char* outputName = outputNameHolder.get();
    auto outputs = session.Run(Ort::RunOptions{nullptr}, &inputName, &tensor, 1, &outputName, 1);

    const auto info = outputs.front().GetTensorTypeAndShapeInfo();
    const std::vector<int64_t> outputShape = info.GetShape();
    if (outputShape.size() < 3)
        return {};
    const int steps = int(outputShape.at(1));
    const int classes = int(outputShape.at(2));
    const float* data = outputs.front().GetTensorData<float>();

    QString text;
    int previous = -1;
    for (int step = 0; step < steps; ++step) {
        const float* row = data + size_t(step) * classes;
        int best = 0;
        float bestValue = row[0];
        for (int index = 1; index < classes; ++index) {
            if (row[index] > bestValue) {
                bestValue = row[index];
                best = index;
            }
        }
        if (best != previous && best > 0 && best <= int(characters.size())) {
            text += QString::fromStdString(characters[size_t(best - 1)]);
        }
        previous = best;
    }
    return text.trimmed();
}

QJsonObject runPaddle(const PluginManifest& manifest, const QJsonObject& params)
{
    QString error;
    if (!initialiseOnnxRuntime(manifest.directory, &error))
        return ocrError(error);

    const QString base64 = params.value(QStringLiteral("image_base64")).toString();
    if (base64.isEmpty())
        return ocrError(QStringLiteral("Не передано изображение для OCR"));

    QImage image;
    if (!image.loadFromData(QByteArray::fromBase64(base64.toLatin1())))
        return ocrError(QStringLiteral("Не удалось декодировать изображение"));

    const QString models = QDir(manifest.directory).filePath(QStringLiteral("models/v5"));
    const QString detectorPath = QDir(models).filePath(QStringLiteral("det.onnx"));
    const QString recognizerPath = QDir(models).filePath(QStringLiteral("rec.onnx"));
    const QString dictionaryPath = QDir(models).filePath(QStringLiteral("dict.txt"));
    for (const QString& path : {detectorPath, recognizerPath, dictionaryPath}) {
        if (!QFileInfo::exists(path))
            return ocrError(QStringLiteral("Нет файла модели: %1").arg(path));
    }

    std::vector<std::string> characters;
    {
        QFile dictionary(dictionaryPath);
        if (!dictionary.open(QIODevice::ReadOnly | QIODevice::Text))
            return ocrError(QStringLiteral("Не удалось прочитать словарь символов"));
        while (!dictionary.atEnd()) {
            QString line = QString::fromUtf8(dictionary.readLine());
            line.replace(QLatin1Char('\n'), QString());
            line.replace(QLatin1Char('\r'), QString());
            if (!line.isEmpty())
                characters.push_back(line.toStdString());
        }
        characters.push_back(" ");
    }

    try {
        static Ort::Env environment(ORT_LOGGING_LEVEL_ERROR, "qtranslate-paddle");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(2);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        Ort::Session detector(environment, detectorPath.toStdWString().c_str(), options);
        Ort::Session recognizer(environment, recognizerPath.toStdWString().c_str(), options);

        const QVector<Box> boxes = detectBoxes(detector, image);
        QJsonArray lineArray;
        QStringList texts;
        int index = 0;
        for (const Box& box : boxes) {
            const QString text = recognize(recognizer, image, box, characters);
            if (text.isEmpty())
                continue;
            QJsonObject entry;
            entry.insert(QStringLiteral("text"), text);
            entry.insert(QStringLiteral("left"), box.left);
            entry.insert(QStringLiteral("top"), box.top);
            entry.insert(QStringLiteral("width"), box.width);
            entry.insert(QStringLiteral("height"), box.height);
            entry.insert(QStringLiteral("block"), ++index);
            entry.insert(QStringLiteral("paragraph"), 1);
            entry.insert(QStringLiteral("line"), 1);
            lineArray.append(entry);
            texts.append(text);
        }

        QJsonObject result;
        result.insert(QStringLiteral("text"), texts.join(QLatin1Char('\n')));
        if (!lineArray.isEmpty()) {
            result.insert(QStringLiteral("lines"), lineArray);
            result.insert(QStringLiteral("coordinate_space"), QStringLiteral("image-pixels"));
        }
        result.insert(QStringLiteral("detected_lang"),
                      params.value(QStringLiteral("lang")).toString(QStringLiteral("en")));
        result.insert(QStringLiteral("engine"), QStringLiteral("paddleocr"));
        return result;
    } catch (const std::exception& exception) {
        return ocrError(QString::fromUtf8(exception.what()));
    }
}

}

PaddleOcrProvider::PaddleOcrProvider(const PluginManifest& manifest, QObject* parent)
    : IPluginProvider(parent)
    , m_manifest(manifest)
{
    loadSettings();
}

QJsonObject PaddleOcrProvider::defaultSettings() const
{
    QJsonObject settings;
    for (const QJsonValue& value : m_manifest.settingsSchema) {
        const QJsonObject field = value.toObject();
        const QString key = field.value(QStringLiteral("key")).toString();
        if (key.isEmpty())
            continue;
        settings.insert(key, field.contains(QStringLiteral("default"))
                                 ? field.value(QStringLiteral("default"))
                                 : QJsonValue(QString()));
    }
    return settings;
}

bool PaddleOcrProvider::loadSettings()
{
    m_settings = defaultSettings();
    if (m_manifest.settingsPath.isEmpty())
        return true;
    QFile file(m_manifest.settingsPath);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (document.isObject()) {
            const QJsonObject stored = document.object();
            for (auto it = stored.constBegin(); it != stored.constEnd(); ++it)
                m_settings.insert(it.key(), it.value());
        }
    }
    return true;
}

bool PaddleOcrProvider::saveSettings()
{
    if (m_manifest.settingsPath.isEmpty())
        return false;
    QDir().mkpath(QFileInfo(m_manifest.settingsPath).absolutePath());
    QFile file(m_manifest.settingsPath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(m_settings).toJson(QJsonDocument::Indented));
    return true;
}

bool PaddleOcrProvider::updateSettings(const QJsonObject& settings)
{
    m_settings = settings;
    const bool saved = saveSettings();
    emit settingsChanged();
    return saved;
}

void PaddleOcrProvider::executeAsync(const QString& capability, const QJsonObject& params,
                                     PluginCallback callback)
{
    if (capability != QStringLiteral("ocr")) {
        QJsonObject result;
        result.insert(QStringLiteral("error"),
                      QStringLiteral("Неподдерживаемая возможность: %1").arg(capability));
        if (callback)
            callback(result);
        return;
    }

    const PluginManifest manifest = m_manifest;
    auto future = QtConcurrent::run([manifest, params] { return runPaddle(manifest, params); });

    auto* watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this,
            [watcher, callback = std::move(callback)] {
                const QJsonObject result = watcher->result();
                watcher->deleteLater();
                if (callback)
                    callback(result);
            });
    watcher->setFuture(future);
}