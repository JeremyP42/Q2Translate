#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class AppContext;
class QTableWidget;
class QPushButton;
class QLabel;
class DocumentTranslationJob;
class ImageTranslationPipeline;

// Translates a list of files one after another. Each file goes through the
// pipeline its extension calls for: office and text formats through the document
// job, images through the image pipeline.
class BatchTranslationDialog : public QDialog
{
    Q_OBJECT

public:
    BatchTranslationDialog(AppContext* context, const QString& sourceCode,
                           const QString& targetCode, const QString& providerId,
                           const QStringList& files = QStringList(),
                           QWidget* parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void reject() override;

private:
    enum class FileStatus { Pending, Working, Done, Error };

    void addFiles();
    void addPaths(const QStringList& paths);
    void addFileRow(const QString& path);
    void removeRow(int row);
    void removeByPath(const QString& path);
    void clearRows();
    void setRowStatus(int row, FileStatus status);
    void startQueue();
    void startNext();
    void finishQueue();
    void setRunning(bool running);
    QString outputFor(const QString& path) const;

    AppContext* m_context = nullptr;
    QString m_sourceCode;
    QString m_targetCode;
    QString m_providerId;

    QTableWidget* m_table = nullptr;
    QPushButton* m_addButton = nullptr;
    QPushButton* m_runButton = nullptr;
    QPushButton* m_closeButton = nullptr;
    QLabel* m_status = nullptr;

    int m_index = 0;
    bool m_running = false;
    bool m_cancelled = false;
    DocumentTranslationJob* m_job = nullptr;
    ImageTranslationPipeline* m_image = nullptr;
};
