#pragma once

#include <QWidget>

#include "popup_style.h"

class AppContext;
class QGraphicsOpacityEffect;
class QLabel;
class QTextBrowser;
class QTimer;
class QToolButton;
class QVBoxLayout;

class PopupWindow : public QWidget
{
    Q_OBJECT

public:
    explicit PopupWindow(AppContext* context, QWidget* parent = nullptr);
    ~PopupWindow() override;

    void showTranslation(const QString& text, const QString& fromCode, const QString& toCode);
    void showResult(const QString& source, const QString& translation);

signals:
    void openMainRequested(const QString& text);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyAppearance();
    void rebuild();
    void updateAutoSize();
    void startHideTimer();
    void copyTranslation();
    void resetRequest();
    void setSourceText(const QString& text);
    QString sourceText() const;
    QString headerText() const;

    AppContext* m_context = nullptr;
    QWidget* m_frame = nullptr;
    QVBoxLayout* m_rootLayout = nullptr;
    QLabel* m_source = nullptr;
    QTextBrowser* m_sourceBrowser = nullptr;
    QTextBrowser* m_translation = nullptr;
    QLabel* m_status = nullptr;
    QToolButton* m_copyButton = nullptr;
    QToolButton* m_openButton = nullptr;
    QToolButton* m_speakButton = nullptr;
    QToolButton* m_closeButton = nullptr;
    QLabel* m_header = nullptr;
    QWidget* m_actions = nullptr;
    QGraphicsOpacityEffect* m_actionsOpacity = nullptr;
    QTimer* m_hideTimer = nullptr;
    PopupStyle m_style;
    QString m_requestId;
    QString m_text;
    QString m_fromCode;
    QString m_toCode;
    bool m_waiting = false;
    bool m_success = false;
    bool m_outsideFilter = false;
};
