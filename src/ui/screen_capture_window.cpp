#include "screen_capture_window.h"
#include "down_combo_box.h"

#include "core/app_context.h"
#include "core/settings_manager.h"
#include "core/translation_manager.h"
#include "theme_manager.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QImage>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

ScreenCaptureWindow::ScreenCaptureWindow(AppContext* context, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , m_context(context)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);

    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(QStringLiteral("CaptureToolbar"));
    m_toolbar->setAttribute(Qt::WA_StyledBackground, true);

    // intentionally always-dark: readability over arbitrary captured content.
    // Token values come from the "Fusion Dark" theme, independent of the active theme.
    {
        const auto token = [](const char* name) {
            const QColor color = ThemeManager::color(QString::fromLatin1(name),
                                                     QStringLiteral("Fusion Dark"));
            if (!color.isValid()) {
                qWarning("screen_capture_window: token %s is not available in Fusion Dark", name);
                return QString();
            }
            return color.name();
        };
        const QString surface = token("surface");
        const QString border = token("border");
        const QString text = token("text");
        const QString surfaceAlt = token("surfaceAlt");
        const QString borderStrong = token("borderStrong");
        const QString accentSoft = token("accentSoft");
        const QString accent = token("accent");
        if (!surface.isEmpty() && !border.isEmpty() && !text.isEmpty() && !surfaceAlt.isEmpty()
            && !borderStrong.isEmpty() && !accentSoft.isEmpty() && !accent.isEmpty()) {
            m_toolbar->setStyleSheet(QStringLiteral(
                "QWidget#CaptureToolbar { background:%1; border:1px solid %2; border-radius:10px; }"
                "QWidget#CaptureToolbar QLabel { color:%3; }"
                "QWidget#CaptureToolbar QPushButton { background:%4; color:%3; border:1px solid %5; border-radius:7px; padding:5px 10px; }"
                "QWidget#CaptureToolbar QPushButton:hover { border-color:%7; background:%6; }"
                "QWidget#CaptureToolbar QComboBox { background:%4; color:%3; border:1px solid %5; border-radius:7px; padding:4px 8px; }")
                .arg(surface, border, text, surfaceAlt, borderStrong, accentSoft, accent));
        }
    }

    auto* toolbarLayout = new QHBoxLayout(m_toolbar);
    toolbarLayout->setContentsMargins(12, 8, 12, 8);
    toolbarLayout->setSpacing(6);

    m_provider = new DownComboBox(m_toolbar);
    if (m_context && m_context->translation) {
        for (const PluginManifest& manifest : m_context->translation->ocrServices())
            m_provider->addItem(manifest.name, manifest.id);
        if (const int index = m_provider->findData(m_context->translation->defaultOcrProviderId());
            index >= 0) {
            m_provider->setCurrentIndex(index);
        }
    }
    toolbarLayout->addWidget(m_provider);

    m_language = new DownComboBox(m_toolbar);
    m_language->setEditable(true);
    toolbarLayout->addWidget(m_language);

    m_recognizeButton = new QPushButton(QStringLiteral("Распознать"), m_toolbar);
    m_translateButton = new QPushButton(QStringLiteral("Перевести"), m_toolbar);
    m_copyButton = new QPushButton(QStringLiteral("Копировать"), m_toolbar);
    m_saveButton = new QPushButton(QStringLiteral("Сохранить"), m_toolbar);
    toolbarLayout->addWidget(m_recognizeButton);
    toolbarLayout->addWidget(m_translateButton);
    toolbarLayout->addWidget(m_copyButton);
    toolbarLayout->addWidget(m_saveButton);

    toolbarLayout->addWidget(new QLabel(QStringLiteral("|"), m_toolbar));
    m_sizeLabel = new QLabel(QStringLiteral("0 × 0"), m_toolbar);
    toolbarLayout->addWidget(m_sizeLabel);
    m_status = new QLabel(QStringLiteral("Выделите область"), m_toolbar);
    toolbarLayout->addWidget(m_status);

    auto* selectAll = new QPushButton(QStringLiteral("Выделить всё"), m_toolbar);
    auto* clear = new QPushButton(QStringLiteral("Сбросить"), m_toolbar);
    auto* close = new QPushButton(QStringLiteral("Закрыть"), m_toolbar);
    toolbarLayout->addWidget(selectAll);
    toolbarLayout->addWidget(clear);
    toolbarLayout->addWidget(close);
    m_toolbar->adjustSize();

    auto refreshLanguages = [this] {
        const QString provider = m_provider->currentData().toString();
        const QString current = m_language->currentText();
        m_language->clear();
        if (m_context && m_context->translation) {
            for (const QString& language : m_context->translation->ocrLanguages(provider))
                m_language->addItem(language);
        }
        if (!current.isEmpty())
            m_language->setCurrentText(current);
        else if (m_context && m_context->settings)
            m_language->setCurrentText(
                m_context->settings->stringValue(QStringLiteral("Ocr.OcrLanguage"),
                                                 QStringLiteral("eng")));

        int widest = 0;
        const QFontMetrics metrics(m_language->font());
        for (int i = 0; i < m_language->count(); ++i) {
            widest = qMax(widest, metrics.horizontalAdvance(m_language->itemText(i)));
        }
        m_language->setMinimumWidth(qMax(120, int((widest + 56) * 1.3)));
    };
    connect(m_provider, &QComboBox::currentIndexChanged, this, refreshLanguages);
    refreshLanguages();

    connect(m_recognizeButton, &QPushButton::clicked, this, &ScreenCaptureWindow::recognize);
    connect(m_translateButton, &QPushButton::clicked, this, &ScreenCaptureWindow::translateResult);
    connect(m_copyButton, &QPushButton::clicked, this, &ScreenCaptureWindow::copyResult);
    connect(m_saveButton, &QPushButton::clicked, this, &ScreenCaptureWindow::saveSelection);
    connect(selectAll, &QPushButton::clicked, this, [this] {
        m_selection = QRect(QPoint(0, 0), size());
        update();
        updateStatus();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        m_selection = QRect();
        m_resultText.clear();
        update();
        updateStatus();
    });
    connect(close, &QPushButton::clicked, this, &QWidget::hide);

    if (m_context && m_context->translation) {
        connect(m_context->translation, &TranslationManager::ocrResult,
            this, [this](const QString&, const QJsonObject& result) {
                const QString error = result.value(QStringLiteral("error")).toString();
                const QString text = result.value(QStringLiteral("text")).toString();
                const QString markdown = result.value(QStringLiteral("table_markdown")).toString();
                if (!error.isEmpty()) {
                    m_status->setText(QStringLiteral("Ошибка OCR"));
                    QMessageBox::warning(this, QStringLiteral("OCR"), error);
                } else {
                    const bool table = !markdown.isEmpty();
                    QString cleaned = table ? markdown : text;
                    if (!table && m_context && m_context->settings
                        && m_context->settings->boolValue(QStringLiteral("Advanced.RemoveLineBreaks"), false)) {
                        cleaned.replace(QLatin1Char('\n'), QLatin1Char(' '));
                        cleaned = cleaned.simplified();
                    }
                    m_resultText = cleaned;
                    m_status->setText(table
                                          ? QStringLiteral("Распознана таблица: %1 строк").arg(markdown.count(QLatin1Char('\n')) + 1)
                                          : QStringLiteral("Распознано: %1 символов").arg(cleaned.size()));
                    if (!m_autoAction.isEmpty()) {
                        const QString action = m_autoAction;
                        m_autoAction.clear();
                        m_translateAfterOcr = false;
                        emit textRecognized(cleaned,
                                            action == QStringLiteral("translate"));
                        hide();
                        updateToolbar();
                        return;
                    }
                    showResultDialog(cleaned);
                }
                updateToolbar();
            });
    }

    updateToolbar();
}

void ScreenCaptureWindow::start()
{
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen) {
        QMessageBox::warning(this, QStringLiteral("Захват экрана"),
                             QStringLiteral("Не удалось получить экран."));
        return;
    }

    m_screenshot = screen->grabWindow(0);
    m_selection = QRect();
    m_resultText.clear();
    m_autoAction.clear();
    m_translateAfterOcr = false;
    m_origin = QPoint();
    m_selecting = false;
    setMouseTracking(true);
    setGeometry(screen->geometry());
    const QString mode = m_context && m_context->settings
        ? m_context->settings->stringValue(QStringLiteral("Advanced.ScreenCaptureMode"),
                                           QStringLiteral("auto"))
        : QStringLiteral("auto");
    if (m_toolbar)
        m_toolbar->setVisible(mode != QStringLiteral("auto"));
    // A normal window covering the screen is more reliable than showFullScreen:
    // it avoids the fullscreen transition that can swallow the first drag.
    show();
    raise();
    activateWindow();
    setFocus();
#ifdef Q_OS_WIN
    HWND windowHandle = reinterpret_cast<HWND>(winId());
    ShowWindow(windowHandle, SW_SHOW);
    SetForegroundWindow(windowHandle);
    SetActiveWindow(windowHandle);
#endif
    QTimer::singleShot(0, this, [this] {
        raise();
        activateWindow();
        setFocus();
    });
    updateToolbar();
    updateStatus();
    update();
}

void ScreenCaptureWindow::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0, 0, 0));
    if (m_screenshot.isNull())
        return;

    painter.drawPixmap(rect(), m_screenshot);
    const QRect selection = normalizedSelection();
    painter.fillRect(rect(), QColor(0, 0, 0, 130));
    if (!selection.isEmpty()) {
        const QRect sourceRect(
            int(selection.x() * m_screenshot.width() / qreal(qMax(1, width()))),
            int(selection.y() * m_screenshot.height() / qreal(qMax(1, height()))),
            int(selection.width() * m_screenshot.width() / qreal(qMax(1, width()))),
            int(selection.height() * m_screenshot.height() / qreal(qMax(1, height()))));
        painter.drawPixmap(selection, m_screenshot, sourceRect);
        painter.setPen(QPen(QColor(77, 163, 255), 2));
        painter.drawRect(selection.adjusted(0, 0, -1, -1));
        painter.setPen(QColor(255, 255, 255));
        painter.setBrush(QColor(27, 33, 41, 210));
        const QString sizeText = QStringLiteral("%1 × %2")
                                     .arg(selection.width())
                                     .arg(selection.height());
        const QRect labelRect(selection.left() + 6, selection.top() + 6, 88, 22);
        painter.drawRoundedRect(labelRect, 4, 4);
        painter.drawText(labelRect, Qt::AlignCenter, sizeText);
    }
}

void ScreenCaptureWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_toolbar) {
        m_toolbar->adjustSize();
        m_toolbar->move((width() - m_toolbar->width()) / 2, 12);
        m_toolbar->raise();
    }
}

void ScreenCaptureWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::RightButton) {
        hide();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    m_origin = event->position().toPoint();
    m_selection = QRect(m_origin, m_origin);
    m_selecting = true;
    update();
}

void ScreenCaptureWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_selecting)
        return;
    const QPoint current = event->position().toPoint();
    m_selection = QRect(m_origin, current).normalized().intersected(rect());
    updateStatus();
    update();
}

void ScreenCaptureWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_selecting)
        return;
    m_selecting = false;
    m_selection = normalizedSelection();
    updateStatus();
    update();

    const QString mode = m_context && m_context->settings
        ? m_context->settings->stringValue(QStringLiteral("Advanced.ScreenCaptureMode"),
                                           QStringLiteral("auto"))
        : QStringLiteral("auto");
    if (mode == QStringLiteral("auto") && !m_selection.isEmpty())
        QTimer::singleShot(120, this, &ScreenCaptureWindow::executeAutoAction);
}

void ScreenCaptureWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        hide();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        recognize();
        return;
    }
    QWidget::keyPressEvent(event);
}

QRect ScreenCaptureWindow::normalizedSelection() const
{
    return m_selection.normalized().intersected(rect());
}

void ScreenCaptureWindow::updateToolbar()
{
    const bool hasSelection = !normalizedSelection().isEmpty();
    if (m_recognizeButton)
        m_recognizeButton->setEnabled(hasSelection && m_context && m_context->translation);
    if (m_translateButton)
        m_translateButton->setEnabled(hasSelection && m_context && m_context->translation);
    if (m_copyButton)
        m_copyButton->setEnabled(hasSelection || !m_resultText.isEmpty());
    if (m_saveButton)
        m_saveButton->setEnabled(hasSelection && !m_screenshot.isNull());
}

void ScreenCaptureWindow::updateStatus()
{
    const QRect selection = normalizedSelection();
    if (m_sizeLabel)
        m_sizeLabel->setText(QStringLiteral("%1 × %2").arg(selection.width()).arg(selection.height()));
    updateToolbar();
    update();
}

void ScreenCaptureWindow::recognize()
{
    if (!m_context || !m_context->translation)
        return;
    const QRect selection = normalizedSelection();
    if (selection.isEmpty()) {
        m_status->setText(QStringLiteral("Сначала выделите область"));
        return;
    }

    const qreal scaleX = m_screenshot.width() / qreal(qMax(1, width()));
    const qreal scaleY = m_screenshot.height() / qreal(qMax(1, height()));
    const QRect sourceRect(
        int(selection.x() * scaleX), int(selection.y() * scaleY),
        qMax(1, int(selection.width() * scaleX)), qMax(1, int(selection.height() * scaleY)));
    const QImage image = m_screenshot.toImage().copy(sourceRect.intersected(m_screenshot.rect()));

    m_status->setText(QStringLiteral("Распознавание…"));
    m_resultText.clear();
    updateToolbar();
    m_context->translation->ocrAsync(image, m_provider->currentData().toString(),
                                     m_language->currentText());
}

void ScreenCaptureWindow::translateResult()
{
    if (!m_resultText.isEmpty()) {
        emit textRecognized(m_resultText, true);
        hide();
        return;
    }
    if (!normalizedSelection().isEmpty() && m_context && m_context->translation) {
        m_autoAction = QStringLiteral("translate");
        recognize();
    }
}

void ScreenCaptureWindow::copyResult()
{
    if (!m_resultText.isEmpty()) {
        QApplication::clipboard()->setText(m_resultText);
        return;
    }
    const QPixmap image = selectionPixmap();
    if (!image.isNull())
        QApplication::clipboard()->setPixmap(image);
}

QPixmap ScreenCaptureWindow::selectionPixmap() const
{
    const QRect selection = normalizedSelection();
    if (selection.isEmpty() || m_screenshot.isNull())
        return QPixmap();
    const qreal scaleX = m_screenshot.width() / qreal(qMax(1, width()));
    const qreal scaleY = m_screenshot.height() / qreal(qMax(1, height()));
    const QRect sourceRect(
        int(selection.x() * scaleX), int(selection.y() * scaleY),
        qMax(1, int(selection.width() * scaleX)), qMax(1, int(selection.height() * scaleY)));
    return m_screenshot.copy(sourceRect.intersected(m_screenshot.rect()));
}

void ScreenCaptureWindow::executeAutoAction()
{
    if (normalizedSelection().isEmpty())
        return;
    const QString action = m_context && m_context->settings
        ? m_context->settings->stringValue(QStringLiteral("Advanced.ScreenCaptureAction"),
                                           QStringLiteral("translate"))
        : QStringLiteral("translate");
    if (action == QStringLiteral("copy")) {
        const QPixmap image = selectionPixmap();
        if (!image.isNull())
            QApplication::clipboard()->setPixmap(image);
        hide();
        return;
    }
    if (action == QStringLiteral("save")) {
        saveSelection();
        hide();
        return;
    }
    m_autoAction = action == QStringLiteral("translate") ? QStringLiteral("translate")
                                                         : QStringLiteral("recognize");
    recognize();
}

void ScreenCaptureWindow::saveSelection()
{
    const QRect selection = normalizedSelection();
    if (selection.isEmpty())
        return;

    QString directory;
    if (m_context && m_context->settings)
        directory = m_context->settings->stringValue(QStringLiteral("Ocr.SaveImagePath"));
    if (directory.isEmpty())
        directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Сохранить снимок"),
        QDir(directory).filePath(QStringLiteral("qtranslate_capture.png")),
        QStringLiteral("PNG (*.png);;JPEG (*.jpg)"));
    if (path.isEmpty())
        return;

    const qreal scaleX = m_screenshot.width() / qreal(qMax(1, width()));
    const qreal scaleY = m_screenshot.height() / qreal(qMax(1, height()));
    const QRect sourceRect(
        int(selection.x() * scaleX), int(selection.y() * scaleY),
        qMax(1, int(selection.width() * scaleX)), qMax(1, int(selection.height() * scaleY)));
    m_screenshot.copy(sourceRect.intersected(m_screenshot.rect())).save(path);
}

void ScreenCaptureWindow::showResultDialog(const QString& text)
{
    if (text.trimmed().isEmpty())
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Результат OCR"));
    dialog.resize(620, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* editor = new QPlainTextEdit(&dialog);
    editor->setPlainText(text);
    editor->setReadOnly(false);
    layout->addWidget(editor, 1);

    auto* buttons = new QDialogButtonBox(&dialog);
    auto* translate = buttons->addButton(QStringLiteral("Перевести"), QDialogButtonBox::ActionRole);
    auto* copy = buttons->addButton(QStringLiteral("Копировать"), QDialogButtonBox::ActionRole);
    auto* save = buttons->addButton(QStringLiteral("Сохранить текст"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    connect(copy, &QPushButton::clicked, &dialog, [editor] {
        QApplication::clipboard()->setText(editor->toPlainText());
    });
    connect(save, &QPushButton::clicked, &dialog, [&dialog, editor] {
        const QString path = QFileDialog::getSaveFileName(
            &dialog, QStringLiteral("Сохранить текст"), QString(),
            QStringLiteral("Текст (*.txt)"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
            file.write(editor->toPlainText().toUtf8());
    });
    connect(translate, &QPushButton::clicked, &dialog, [this, editor, &dialog] {
        m_resultText = editor->toPlainText();
        emit textRecognized(m_resultText, true);
        dialog.accept();
        hide();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialog.exec();
    m_resultText = editor->toPlainText();
    updateToolbar();
}
