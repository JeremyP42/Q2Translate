#pragma once

#include <QJsonObject>
#include <QPixmap>
#include <QWidget>

class AppContext;
class QComboBox;
class QLabel;
class QPushButton;
class QToolButton;

class ScreenCaptureWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ScreenCaptureWindow(AppContext* context, QWidget* parent = nullptr);

    void start();

signals:
    void textRecognized(const QString& text, bool translate);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void updateToolbar();
    void updateStatus();
    void recognize();
    void handleOcrResult(const QJsonObject& result, bool showErrors = true);
    void translateResult();
    void copyResult();
    void saveSelection();
    void executeAutoAction();
    QPixmap selectionPixmap() const;
    QRect normalizedSelection() const;
    void showResultDialog(const QString& text);

    AppContext* m_context = nullptr;
    QPixmap m_screenshot;
    QRect m_selection;
    QPoint m_origin;
    bool m_selecting = false;
    bool m_translateAfterOcr = false;
    QString m_autoAction;
    QString m_resultText;

    QWidget* m_toolbar = nullptr;
    QComboBox* m_provider = nullptr;
    QComboBox* m_language = nullptr;
    QLabel* m_sizeLabel = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_recognizeButton = nullptr;
    QPushButton* m_translateButton = nullptr;
    QPushButton* m_copyButton = nullptr;
    QPushButton* m_saveButton = nullptr;
};
