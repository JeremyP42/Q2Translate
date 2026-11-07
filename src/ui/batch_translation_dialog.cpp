#include "batch_translation_dialog.h"

#include "core/app_context.h"
#include "core/settings_manager.h"
#include "services/plugin_manager.h"
#include "core/translation_manager.h"
#include "core/webp_image.h"
#include "modules/document/document_reader.h"
#include "modules/document/document_translation_job.h"
#include "modules/image/image_translation_pipeline.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStyle>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

bool isImageFile(const QString& path)
{
    static const QStringList suffixes = {
        QStringLiteral("png"),  QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"),  QStringLiteral("webp"), QStringLiteral("tif"),
        QStringLiteral("tiff")};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

// Muted status colours: grey for queued files, blue while a file is processed,
// green when it is saved, red for failures.
QString statusColor(int status)
{
    switch (status) {
    case 1: return QStringLiteral("#3B6FB6");  // working - muted blue
    case 2: return QStringLiteral("#2F7D4F");  // done - muted green
    case 3: return QStringLiteral("#9A3B3B");  // error - muted red
    default: return QStringLiteral("#6B7280"); // pending - grey
    }
}

}

BatchTranslationDialog::BatchTranslationDialog(AppContext* context, const QString& sourceCode,
                                               const QString& targetCode,
                                               const QString& providerId,
                                               const QStringList& files, QWidget* parent)
    : QDialog(parent)
    , m_context(context)
    , m_sourceCode(sourceCode)
    , m_targetCode(targetCode)
    , m_providerId(providerId)
{
    setWindowTitle(QStringLiteral("Пакетный перевод документов"));
    setAcceptDrops(true);
    resize(760, 460);

    auto* layout = new QVBoxLayout(this);

    auto* hint = new QLabel(
        QStringLiteral("Перетащите файлы в список или добавьте их через проводник.\n"
                       "Файлы обрабатываются по одному и сохраняются рядом с исходными."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_table = new QTableWidget(0, 3, this);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->horizontalHeader()->setVisible(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(38);
    m_table->setColumnWidth(1, 118);
    m_table->setColumnWidth(2, 46);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    layout->addWidget(m_table, 1);

    auto* buttons = new QHBoxLayout;
    m_addButton = new QPushButton(QStringLiteral("Добавить файлы…"), this);
    buttons->addWidget(m_addButton);
    buttons->addStretch(1);
    m_runButton = new QPushButton(QStringLiteral("Запустить перевод"), this);
    m_closeButton = new QPushButton(QStringLiteral("Отмена"), this);
    buttons->addWidget(m_runButton);
    buttons->addWidget(m_closeButton);
    layout->addLayout(buttons);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    connect(m_addButton, &QPushButton::clicked, this, &BatchTranslationDialog::addFiles);
    connect(m_runButton, &QPushButton::clicked, this, &BatchTranslationDialog::startQueue);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);

    if (!files.isEmpty())
        addPaths(files);
}

QString BatchTranslationDialog::outputFor(const QString& path) const
{
    const QFileInfo info(path);
    if (isImageFile(path)) {
        return info.dir().filePath(
            QStringLiteral("%1.%2.png").arg(info.completeBaseName(), m_targetCode));
    }
    const QString suffix = info.suffix().toLower();
    const QString extension = (suffix == QStringLiteral("djvu") || suffix == QStringLiteral("djv"))
        ? QStringLiteral("pdf")
        : suffix;
    return info.dir().filePath(
        QStringLiteral("%1.%2.%3").arg(info.completeBaseName(), m_targetCode, extension));
}

void BatchTranslationDialog::addFiles()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Файлы для перевода"), QString(),
        QStringLiteral("Все поддерживаемые (*.pdf *.djvu *.djv *.docx *.pptx *.fb2 *.epub "
                       "*.xliff *.xlf *.ts *.json *.xml *.yaml *.yml *.srt *.vtt *.ass *.ssa "
                       "*.csv *.ini *.md *.txt *.html *.htm *.png *.jpg *.jpeg *.bmp *.webp "
                       "*.tif *.tiff);;Все файлы (*.*)"));
    addPaths(paths);
}

void BatchTranslationDialog::addPaths(const QStringList& paths)
{
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (!info.isFile())
            continue;
        if (!DocumentReader::isSupported(path) && !isImageFile(path)) {
            m_status->setText(QStringLiteral("Пропущен неподдерживаемый файл: %1").arg(info.fileName()));
            continue;
        }
        addFileRow(info.absoluteFilePath());
    }
    m_status->setText(QStringLiteral("Файлов в списке: %1").arg(m_table->rowCount()));
}

void BatchTranslationDialog::addFileRow(const QString& path)
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 0) && m_table->item(row, 0)->data(Qt::UserRole).toString() == path)
            return;
    }

    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto* name = new QTableWidgetItem(QFileInfo(path).fileName());
    name->setData(Qt::UserRole, path);
    name->setToolTip(path);
    m_table->setItem(row, 0, name);

    setRowStatus(row, FileStatus::Pending);

    const QString rowPath = path;
    auto* remove = new QPushButton(this);
    remove->setIcon(QApplication::style()->standardIcon(QStyle::SP_TrashIcon));
    remove->setToolTip(QStringLiteral("Удалить из списка"));
    remove->setFlat(true);
    remove->setFixedSize(30, 30);
    remove->setCursor(Qt::PointingHandCursor);
    // The row is identified by its file path: rows shift when entries above are
    // removed, and pointer comparisons against cell widgets proved fragile.
    connect(remove, &QPushButton::clicked, this, [this, rowPath] {
        removeByPath(rowPath);
    });

    auto* holder = new QWidget(this);
    auto* holderLayout = new QHBoxLayout(holder);
    holderLayout->setContentsMargins(0, 0, 0, 0);
    holderLayout->addWidget(remove, 0, Qt::AlignCenter);
    m_table->setCellWidget(row, 2, holder);
}

void BatchTranslationDialog::removeByPath(const QString& path)
{
    if (m_running)
        return;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto* item = m_table->item(row, 0);
        if (item && item->data(Qt::UserRole).toString() == path) {
            m_table->removeRow(row);
            m_status->setText(QStringLiteral("Файлов в списке: %1").arg(m_table->rowCount()));
            return;
        }
    }
}

void BatchTranslationDialog::removeRow(int row)
{
    if (m_running || row < 0 || row >= m_table->rowCount())
        return;
    m_table->removeRow(row);
    m_status->setText(QStringLiteral("Файлов в списке: %1").arg(m_table->rowCount()));
}

void BatchTranslationDialog::clearRows()
{
    m_table->setRowCount(0);
}

void BatchTranslationDialog::setRowStatus(int row, FileStatus status)
{
    QString text;
    switch (status) {
    case FileStatus::Working: text = QStringLiteral("в работе"); break;
    case FileStatus::Done: text = QStringLiteral("готово"); break;
    case FileStatus::Error: text = QStringLiteral("ошибка"); break;
    default: text = QStringLiteral("ожидание"); break;
    }

    auto* pill = new QLabel(text, this);
    pill->setAlignment(Qt::AlignCenter);
    pill->setStyleSheet(QStringLiteral(
        "QLabel { background: %1; color: white; border-radius: 10px;"
        " padding: 3px 10px; font-size: 12px; }").arg(statusColor(int(status))));
    pill->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    auto* holder = new QWidget(this);
    auto* holderLayout = new QHBoxLayout(holder);
    holderLayout->setContentsMargins(4, 0, 4, 0);
    holderLayout->addWidget(pill, 0, Qt::AlignVCenter | Qt::AlignLeft);
    holderLayout->addStretch(1);
    m_table->setCellWidget(row, 1, holder);
}

void BatchTranslationDialog::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void BatchTranslationDialog::dropEvent(QDropEvent* event)
{
    if (m_running)
        return;
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    addPaths(paths);
    event->acceptProposedAction();
}

void BatchTranslationDialog::reject()
{
    m_cancelled = true;
    if (m_job) {
        m_job->cancel();
        m_job->deleteLater();
        m_job = nullptr;
    }
    if (m_image) {
        m_image->cancel();
        m_image->deleteLater();
        m_image = nullptr;
    }
    QDialog::reject();
}

void BatchTranslationDialog::startQueue()
{
    if (m_running)
        return;
    if (m_table->rowCount() == 0) {
        m_status->setText(QStringLiteral("Добавьте файлы для обработки."));
        return;
    }
    m_cancelled = false;
    m_index = 0;
    for (int row = 0; row < m_table->rowCount(); ++row)
        setRowStatus(row, FileStatus::Pending);
    setRunning(true);
    startNext();
}

void BatchTranslationDialog::setRunning(bool running)
{
    m_running = running;
    m_table->setEnabled(!running);
    m_addButton->setEnabled(!running);
    m_runButton->setEnabled(!running);
    m_closeButton->setText(running ? QStringLiteral("Отмена") : QStringLiteral("Закрыть"));
}

void BatchTranslationDialog::startNext()
{
    if (m_cancelled) {
        finishQueue();
        return;
    }
    if (m_index >= m_table->rowCount()) {
        m_status->setText(QStringLiteral("Готово. Обработано файлов: %1").arg(m_table->rowCount()));
        setRunning(false);
        return;
    }

    const int row = m_index;
    auto* nameItem = m_table->item(row, 0);
    const QString path = nameItem ? nameItem->data(Qt::UserRole).toString() : QString();
    const QString display = nameItem ? nameItem->text() : QString();
    const QString output = outputFor(path);
    const QFileInfo info(path);

    m_status->setText(QStringLiteral("Обработка %1 из %2: %3")
                          .arg(m_index + 1)
                          .arg(m_table->rowCount())
                          .arg(info.fileName()));
    setRowStatus(row, FileStatus::Working);

    if (isImageFile(path)) {
        QImage image = loadImageFile(path);
        if (image.isNull()) {
            setRowStatus(row, FileStatus::Error);
            ++m_index;
            startNext();
            return;
        }

        m_image = new ImageTranslationPipeline(m_context, this);
        connect(m_image, &ImageTranslationPipeline::finished, this,
                [this, row, output, display](const QImage& translated, const QString&,
                                              const QString&) {
            setRowStatus(row, translated.save(output, "PNG") ? FileStatus::Done
                                                              : FileStatus::Error);
            m_image->deleteLater();
            m_image = nullptr;
            ++m_index;
            startNext();
        });
        connect(m_image, &ImageTranslationPipeline::failed, this,
                [this, row, display](const QString& message) {
            setRowStatus(row, FileStatus::Error);
            if (auto* nameItem = m_table->item(row, 0))
                nameItem->setToolTip(message);
            m_image->deleteLater();
            m_image = nullptr;
            ++m_index;
            startNext();
        });
        m_image->start(image, m_sourceCode, m_targetCode, m_providerId);
        return;
    }

    auto* job = new DocumentTranslationJob(m_context, this);
    m_job = job;
    connect(job, &DocumentTranslationJob::progress, this,
            [this, info](int completed, int total, const QString& current) {
        if (total <= 0)
            return;
        m_status->setText(QStringLiteral("%1: %2 из %3%4")
                              .arg(info.fileName())
                              .arg(completed)
                              .arg(total)
                              .arg(current.isEmpty() ? QString() : QStringLiteral(" · ") + current));
    });
    connect(job, &DocumentTranslationJob::finished, this,
            [this, row](const QString&) {
        setRowStatus(row, FileStatus::Done);
        m_job->deleteLater();
        m_job = nullptr;
        ++m_index;
        startNext();
    });
    connect(job, &DocumentTranslationJob::failed, this,
            [this, row](const QString& message) {
        setRowStatus(row, FileStatus::Error);
        if (auto* nameItem = m_table->item(row, 0))
            nameItem->setToolTip(message);
        m_job->deleteLater();
        m_job = nullptr;
        ++m_index;
        startNext();
    });
    // Per-file connector: PDF and DJVU may use their own translation service.
    QString providerId = m_providerId;
    if (m_context && m_context->settings) {
        const QString suffix = QFileInfo(path).suffix().toLower();
        QString key = QStringLiteral("ServiceProviders.fileTranslate");
        if (suffix == QStringLiteral("pdf"))
            key = QStringLiteral("ServiceProviders.translatePdf");
        else if (suffix == QStringLiteral("djvu") || suffix == QStringLiteral("djv"))
            key = QStringLiteral("ServiceProviders.translateDjvu");
        QString preferred = m_context->settings->stringValue(key);
        if (preferred.isEmpty())
            preferred = m_context->settings->stringValue(QStringLiteral("ServiceProviders.fileTranslate"));
        if (!preferred.isEmpty() && m_context->plugins
            && m_context->plugins->provider(preferred))
            providerId = preferred;
    }
    job->start(path, output, m_sourceCode, m_targetCode, providerId);
}

void BatchTranslationDialog::finishQueue()
{
    setRunning(false);
    m_status->setText(QStringLiteral("Обработка отменена."));
    reject();
}
