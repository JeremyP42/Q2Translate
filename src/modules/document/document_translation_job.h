#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <memory>

class AppContext;

// Translates a document segment by segment and writes a translated copy next
// to the source file. DOCX keeps the package and run structure; text formats
// keep their line endings.
class DocumentTranslationJob : public QObject
{
    Q_OBJECT

public:
    explicit DocumentTranslationJob(AppContext* context, QObject* parent = nullptr);
    ~DocumentTranslationJob() override;

    void start(const QString& inputPath, const QString& outputPath,
               const QString& sourceCode, const QString& targetCode,
               const QString& providerId);
    void cancel();

signals:
    void progress(int completed, int total, const QString& current);
    void finished(const QString& outputPath);
    void failed(const QString& message);

private slots:
    void translateNext();
    void recognizeNextPdfPage();

private:
    bool prepareDocx();
    bool preparePlainText();
    bool prepareMarkdown();
    bool prepareHtml();
    bool prepareFb2();
    bool prepareEpub();
    bool preparePdf();
    bool convertDjvuToPdf(const QString& inputPath);
    bool extractDjvuText(const QString& inputPath, QString* textPath);
    bool startPdfOcr();    void appendPdfOcrLines(const QJsonObject& result, int pageIndex, int dpi);
    void beginTranslation();
    bool prepareXliff();
    bool prepareTs();
    bool prepareJson();
    bool prepareXml();
    bool prepareProperties();
    bool prepareYaml();
    bool prepareSrt();
    bool prepareIni();
    bool prepareCsv();
    bool prepareVtt();
    bool prepareAss();
    bool preparePptx();
    bool addOfficePart(const QString& name, const QByteArray& data);
    bool writeText();
    bool writeDocx();
    bool writeFb2();
    bool writeEpub();
    bool writePdf();
    bool writeXmlDocument();
    bool writeTokenText();
    bool writeCsv();
    bool writeYaml();
    bool writePptx();
    bool writeZipWithReplacements();
    void writeOutput();
    void fail(const QString& message);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
