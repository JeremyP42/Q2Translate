#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <functional>

class AppContext;

// Turns one image into a translated image: recognize text blocks, translate each
// block as a whole (a bubble or a paragraph, never a single line), then typeset
// the translations against the real bubble profile so the text stays inside the
// contour at the largest readable size.
class ImageTranslationPipeline : public QObject
{
    Q_OBJECT

public:
    explicit ImageTranslationPipeline(AppContext* context, QObject* parent = nullptr);
    ~ImageTranslationPipeline() override;

    void start(const QImage& image, const QString& sourceCode, const QString& targetCode,
               const QString& providerId = QString());
    void cancel();

    // Optional diagnostics: when set, finish() writes per-block layout numbers
    // (chosen font size, line count, overflow) to this JSON file.
    void setLayoutDumpPath(const QString& path) { m_layoutDumpPath = path; }

signals:
    void progress(int completed, int total, const QString& current);
    void finished(const QImage& translated, const QString& sourceText,
                  const QString& translatedText);
    void failed(const QString& message);

private:
    void recognizeNextBubble();
    void applyDictionaryCorrections();
    void stepTranslate();
    void finish();
    // Builds blocks from m_impl->ocrLines (first pass plus recovered lines).
    void buildBlocks();
    // Plugin-style Tesseract pipeline: upscale, grayscale, sparse-text PSM and
    // geometry grouping instead of bubble masks.
    void startTesseractPass();
    void buildTesseractBlocks();
    // Extra OCR passes that run after the first full-page pass: tiles for large
    // pages, re-reading of missed ink and the geometry-based bubble pass.
    void runExtraStage(int stage);
    void processNextExtraJob();
    void startDenseColumnPass();
    void mergeParagraphBlocks();
    void finishBlocks();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    QString m_layoutDumpPath;
};
