#include "popup_window.h"

#include "core/app_context.h"
#include "core/settings_manager.h"
#include "core/translation_manager.h"
#include "icon_factory.h"
#include "platform/win/speech_manager.h"
#include "services/tts_service.h"
#include "services/plugin_manager.h"
#include "theme_manager.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QCursor>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QFont>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cmath>

namespace {

QString cssColor(const QColor& color)
{
    if (!color.isValid())
        return QStringLiteral("transparent");
    if (color.alpha() >= 255)
        return color.name();
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(QString::number(color.alphaF(), 'f', 3));
}

QList<int> paddingOf(const QJsonObject& object, const QList<int>& fallback)
{
    const QJsonArray array = object.value(QStringLiteral("padding")).toArray();
    if (array.size() != 4)
        return fallback;
    QList<int> result;
    for (const QJsonValue& value : array)
        result.append(value.toInt());
    return result;
}

void applyLineHeight(QTextDocument* document, qreal lineHeight)
{
    if (!document || lineHeight <= 0.0)
        return;
    QTextCursor cursor(document);
    cursor.select(QTextCursor::Document);
    QTextBlockFormat format;
    format.setLineHeight(lineHeight * 100.0, QTextBlockFormat::ProportionalHeight);
    cursor.mergeBlockFormat(format);
}

PopupStyle fallbackStyle()
{
    static const char* json = R"({
        "id":"classic","name":"Классика","width":380,"radius":6,
        "background":{"token":"surface"},"border":{"token":"border"},
        "fontFamily":"Tahoma",
        "header":{"visible":false},
        "source":{"fontSize":11,"lineHeight":1.4,"color":{"token":"muted"},
                  "background":{"token":"surfaceAlt"},"padding":[8,28,7,12],
                  "maxHeight":46,"borderBottom":true},
        "translation":{"fontSize":14,"lineHeight":1.4,"color":{"token":"text"},
                       "padding":[10,12,10,12],"maxHeight":126},
        "actions":{"placement":"bottom","padding":[4,8,6,8],"gap":2,
                   "buttonWidth":28,"buttonHeight":26,"background":{"token":"surfaceAlt"},
                   "borderTop":true,"textButton":true,"textLabel":"Перевод"},
        "twoColumns":false,"close":{"size":28}
    })";
    PopupStyle style;
    style.id = QStringLiteral("classic");
    style.name = QStringLiteral("Классика");
    style.raw = QJsonDocument::fromJson(QByteArray(json)).object();
    style.valid = true;
    return style;
}

#ifdef Q_OS_WIN
HHOOK g_mouseHook = nullptr;
PopupWindow* g_mousePopup = nullptr;

LRESULT CALLBACK popupMouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_mousePopup
        && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN
            || wParam == WM_MBUTTONDOWN)) {
        const auto* mouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        RECT rect;
        if (GetWindowRect(reinterpret_cast<HWND>(g_mousePopup->winId()), &rect)) {
            const bool inside = mouse->pt.x >= rect.left && mouse->pt.x <= rect.right
                && mouse->pt.y >= rect.top && mouse->pt.y <= rect.bottom;
            if (!inside)
                QMetaObject::invokeMethod(g_mousePopup, "hide", Qt::QueuedConnection);
        }
    }
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
}
#endif

} // namespace

PopupWindow::PopupWindow(AppContext* context, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , m_context(context)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setObjectName(QStringLiteral("PopupWindow"));
    setWindowTitle(QStringLiteral("Q2Translate"));

    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    connect(m_hideTimer, &QTimer::timeout, this, &QWidget::hide);

    if (m_context && m_context->translation) {
        connect(m_context->translation, &TranslationManager::translationStarted, this,
                [this](const QString& requestId) {
                    if (m_waiting && m_requestId.isEmpty())
                        m_requestId = requestId;
                });
        connect(m_context->translation, &TranslationManager::serviceResult, this,
                [this](const QString& requestId, const QString&, const QJsonObject& result) {
                    if (!m_waiting || requestId != m_requestId || !m_translation)
                        return;
                    const QString error = result.value(QStringLiteral("error")).toString();
                    const QString text = result.value(QStringLiteral("text")).toString();
                    if (!text.isEmpty()) {
                        m_translation->setPlainText(text);
                        m_success = true;
                        updateAutoSize();
                    } else if (!error.isEmpty() && !m_success) {
                        m_translation->setPlainText(error);
                        updateAutoSize();
                    }
                });
        connect(m_context->translation, &TranslationManager::translationFinished, this,
                [this](const QString& requestId) {
                    if (requestId != m_requestId)
                        return;
                    m_waiting = false;
                    if (m_success) {
                        if (m_status)
                            m_status->clear();
                    } else {
                        if (m_status)
                            m_status->setText(QStringLiteral("Ошибка"));
                        if (m_translation && m_translation->toPlainText().isEmpty())
                            m_translation->setPlainText(
                                QStringLiteral("Не удалось получить перевод."));
                    }
                    startHideTimer();
                });
    }

    applyAppearance();
    hide();
}

PopupWindow::~PopupWindow()
{
#ifdef Q_OS_WIN
    if (g_mousePopup == this) {
        if (g_mouseHook) {
            UnhookWindowsHookEx(g_mouseHook);
            g_mouseHook = nullptr;
        }
        g_mousePopup = nullptr;
    }
#endif
}

void PopupWindow::applyAppearance()
{
    if (!m_context || !m_context->settings)
        return;
    const QString styleId = m_context->settings->stringValue(
        QStringLiteral("Appearance.PopupStyle"), QStringLiteral("compact"));
    m_style = popupStyle(styleId);
    if (!m_style.valid)
        m_style = fallbackStyle();
    const int transparency =
        m_context->settings->intValue(QStringLiteral("Appearance.Transparency"), 5);
    setWindowOpacity(1.0 - qBound(0, transparency, 95) / 100.0);
    rebuild();
}

void PopupWindow::rebuild()
{
    if (m_frame) {
        m_frame->hide();
        m_rootLayout->removeWidget(m_frame);
        m_frame->deleteLater();
        m_frame = nullptr;
    }
    m_source = nullptr;
    m_sourceBrowser = nullptr;
    m_translation = nullptr;
    m_status = nullptr;
    m_copyButton = nullptr;
    m_speakButton = nullptr;
    m_openButton = nullptr;
    m_closeButton = nullptr;
    m_header = nullptr;
    m_actions = nullptr;
    m_actionsOpacity = nullptr;

    const QJsonObject raw = m_style.raw;
    const int width = raw.value(QStringLiteral("width")).toInt(380);
    const int radius = raw.value(QStringLiteral("radius")).toInt(8);
    const int borderWidth = raw.value(QStringLiteral("borderWidth")).toInt(1);
    const QString family = raw.value(QStringLiteral("fontFamily"))
                               .toString(QStringLiteral("Segoe UI"));
    const QJsonObject sourceConfig = raw.value(QStringLiteral("source")).toObject();
    const QJsonObject translationConfig = raw.value(QStringLiteral("translation")).toObject();
    const QJsonObject headerConfig = raw.value(QStringLiteral("header")).toObject();
    const QJsonObject actionsConfig = raw.value(QStringLiteral("actions")).toObject();
    const QJsonObject closeConfig = raw.value(QStringLiteral("close")).toObject();
    const bool twoColumns = raw.value(QStringLiteral("twoColumns")).toBool(false);
    const bool headerVisible = headerConfig.value(QStringLiteral("visible")).toBool(false);
    const bool actionsInHeader =
        actionsConfig.value(QStringLiteral("placement")).toString() == QStringLiteral("header");

    const QColor background = popupColor(raw.value(QStringLiteral("background")),
                                         QStringLiteral("surface"));
    const QColor border = popupColor(raw.value(QStringLiteral("border")),
                                     QStringLiteral("border"));
    const QColor textColor = popupColor(translationConfig.value(QStringLiteral("color")),
                                        QStringLiteral("text"));
    const QColor mutedColor = popupColor(sourceConfig.value(QStringLiteral("color")),
                                         QStringLiteral("muted"));
    const QColor headerColor = popupColor(headerConfig.value(QStringLiteral("color")),
                                          QStringLiteral("muted"));
    const QColor actionBackground = popupColor(actionsConfig.value(QStringLiteral("background")),
                                               QStringLiteral("surfaceAlt"),
                                               QColor(Qt::transparent));
    QColor hoverColor = textColor;
    hoverColor.setAlphaF(0.10);
    const QColor accent = ThemeManager::color(QStringLiteral("accent"));

    const int closeSize = closeConfig.value(QStringLiteral("size")).toInt(28);
    const int sourceFont = sourceConfig.value(QStringLiteral("fontSize")).toInt(12);
    const int translationFont = translationConfig.value(QStringLiteral("fontSize")).toInt(14);
    const bool sourceItalic = sourceConfig.value(QStringLiteral("italic")).toBool(false);
    const int sourceMax = sourceConfig.value(QStringLiteral("maxHeight")).toInt(0);
    const int translationMax =
        translationConfig.value(QStringLiteral("maxHeight")).toInt(0);
    const int headerWeight =
        headerConfig.value(QStringLiteral("fontWeight")).toInt(600);
    const bool headerUppercase =
        headerConfig.value(QStringLiteral("uppercase")).toBool(false);

    m_frame = new QWidget(this);
    m_frame->setObjectName(QStringLiteral("PopupFrame"));
    m_frame->setAttribute(Qt::WA_StyledBackground, true);
    m_rootLayout->addWidget(m_frame);

    auto* layout = new QVBoxLayout(m_frame);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QString sheet = QStringLiteral(
        "QWidget#PopupFrame{background:%1;border:%2px solid %3;border-radius:%4px;}")
        .arg(cssColor(background), QString::number(borderWidth), cssColor(border),
             QString::number(radius));
    sheet += QStringLiteral(
        "QToolButton#PopupButton{background:transparent;border:0;border-radius:6px;}"
        "QToolButton#PopupButton:hover{background:%1;}").arg(cssColor(hoverColor));
    sheet += QStringLiteral(
        "QLabel#PopupStatus{color:%1;font-family:'%2';font-size:10px;"
        "background:transparent;border:0;}").arg(cssColor(mutedColor), family);

    const auto createAction = [&](const IconFactory::Glyph glyph, const QString& tooltip,
                                  const QSize& size) {
        auto* button = new QToolButton(m_frame);
        button->setObjectName(QStringLiteral("PopupButton"));
        button->setIcon(IconFactory::icon(glyph, textColor, 15));
        button->setIconSize(QSize(16, 16));
        button->setFixedSize(size);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(tooltip);
        button->setAccessibleName(tooltip);
        return button;
    };

    if (headerVisible) {
        auto* headerHost = new QWidget(m_frame);
        headerHost->setObjectName(QStringLiteral("PopupHeader"));
        auto* headerLayout = new QHBoxLayout(headerHost);
        const QList<int> padding = paddingOf(headerConfig, {7, 14, 7, 14});
        headerLayout->setContentsMargins(padding[3], padding[0],
                                         padding[1] + closeSize + 4, padding[2]);
        headerLayout->setSpacing(actionsInHeader ? 2 : 6);

        m_header = new QLabel(headerUppercase ? headerText().toUpper() : headerText(), headerHost);
        m_header->setObjectName(QStringLiteral("PopupHeaderLabel"));
        QFont headerFont = m_header->font();
        headerFont.setFamily(family);
        headerFont.setPixelSize(headerConfig.value(QStringLiteral("fontSize")).toInt(11));
        headerFont.setWeight(static_cast<QFont::Weight>(qBound(1, headerWeight, 1000)));
        headerFont.setLetterSpacing(
            QFont::AbsoluteSpacing,
            headerConfig.value(QStringLiteral("letterSpacing")).toDouble(0.0));
        m_header->setFont(headerFont);
        headerLayout->addWidget(m_header);
        headerLayout->addStretch(1);

        if (actionsInHeader) {
            m_actions = new QWidget(headerHost);
            auto* actionsLayout = new QHBoxLayout(m_actions);
            actionsLayout->setContentsMargins(0, 0, 0, 0);
            actionsLayout->setSpacing(
                actionsConfig.value(QStringLiteral("gap")).toInt(2));
            const QSize buttonSize(actionsConfig.value(QStringLiteral("buttonWidth")).toInt(26),
                                   actionsConfig.value(QStringLiteral("buttonHeight")).toInt(24));
            m_copyButton = createAction(IconFactory::Glyph::Copy,
                                        QStringLiteral("Копировать перевод"), buttonSize);
            m_speakButton = createAction(IconFactory::Glyph::Speaker,
                                         QStringLiteral("Озвучить перевод"), buttonSize);
            m_openButton = createAction(IconFactory::Glyph::Globe,
                                        QStringLiteral("Перейти в главное окно"), buttonSize);
            actionsLayout->addWidget(m_copyButton);
            actionsLayout->addWidget(m_speakButton);
            actionsLayout->addWidget(m_openButton);
            headerLayout->addWidget(m_actions);
        }

        layout->addWidget(headerHost);
        sheet += QStringLiteral("QLabel#PopupHeaderLabel{color:%1;background:transparent;}"
                                "QWidget#PopupHeader{background:transparent;border:0;%2}")
                     .arg(cssColor(headerColor),
                          headerConfig.value(QStringLiteral("borderBottom")).toBool(false)
                              ? QStringLiteral("border-bottom:1px solid %1;").arg(cssColor(border))
                              : QString());
    }

    auto* sourceHost = new QWidget(m_frame);
    sourceHost->setObjectName(QStringLiteral("PopupSourceHost"));
    auto* sourceLayout = new QVBoxLayout(sourceHost);
    const QList<int> sourcePadding = paddingOf(sourceConfig, {8, 12, 8, 12});
    int sourceRight = sourcePadding[1];
    if (!twoColumns && !headerVisible)
        sourceRight = qMax(sourceRight, closeSize + 6);
    sourceLayout->setContentsMargins(sourcePadding[3], sourcePadding[0], sourceRight,
                                     sourcePadding[2]);
    sourceLayout->setSpacing(0);

    const bool sourceHasBackground =
        sourceConfig.value(QStringLiteral("background")).isObject();
    if (sourceHasBackground) {
        const QColor sourceBackground =
            popupColor(sourceConfig.value(QStringLiteral("background")),
                       QStringLiteral("surfaceAlt"));
        QString sourceRule =
            QStringLiteral("QWidget#PopupSourceHost{background:%1;%2}")
                .arg(cssColor(sourceBackground),
                     sourceConfig.value(QStringLiteral("borderBottom")).toBool(false)
                         ? QStringLiteral("border-bottom:1px solid %1;").arg(cssColor(border))
                         : QString());
        sheet += sourceRule;
    }
    if (twoColumns && sourceConfig.value(QStringLiteral("borderRight")).toBool(false)) {
        sheet += QStringLiteral("QWidget#PopupSourceHost{border-right:1px solid %1;}")
                     .arg(cssColor(border));
    }

    if (twoColumns) {
        m_sourceBrowser = new QTextBrowser(sourceHost);
        m_sourceBrowser->setObjectName(QStringLiteral("PopupSourceText"));
        m_sourceBrowser->setFrameShape(QFrame::NoFrame);
        m_sourceBrowser->setOpenLinks(false);
        m_sourceBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_sourceBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_sourceBrowser->setLineWrapMode(QTextEdit::WidgetWidth);
        m_sourceBrowser->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        m_sourceBrowser->document()->setDocumentMargin(0);
        if (sourceMax > 0)
            m_sourceBrowser->setMaximumHeight(sourceMax);
        sourceLayout->addWidget(m_sourceBrowser);
        sheet += QStringLiteral(
            "QTextBrowser#PopupSourceText{background:transparent;border:0;color:%1;"
            "font-family:'%2';font-size:%3px;}")
            .arg(cssColor(mutedColor), family, QString::number(sourceFont));
    } else {
        m_source = new QLabel(sourceHost);
        m_source->setObjectName(QStringLiteral("PopupSource"));
        m_source->setWordWrap(true);
        m_source->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_source->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        if (sourceMax > 0)
            m_source->setMaximumHeight(sourceMax);
        sourceLayout->addWidget(m_source);
        sheet += QStringLiteral(
            "QLabel#PopupSource{background:transparent;border:0;color:%1;"
            "font-family:'%2';font-size:%3px;%4}")
            .arg(cssColor(mutedColor), family, QString::number(sourceFont),
                 sourceItalic ? QStringLiteral("font-style:italic;") : QString());
    }

    auto* translationHost = new QWidget(m_frame);
    translationHost->setObjectName(QStringLiteral("PopupTranslationHost"));
    auto* translationLayout = new QVBoxLayout(translationHost);
    const QList<int> translationPadding = paddingOf(translationConfig, {10, 12, 10, 12});
    translationLayout->setContentsMargins(translationPadding[3], translationPadding[0],
                                          translationPadding[1], translationPadding[2]);
    translationLayout->setSpacing(0);

    m_translation = new QTextBrowser(translationHost);
    m_translation->setObjectName(QStringLiteral("PopupTranslation"));
    m_translation->setFrameShape(QFrame::NoFrame);
    m_translation->setOpenLinks(false);
    m_translation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_translation->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_translation->setLineWrapMode(QTextEdit::WidgetWidth);
    m_translation->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_translation->setMinimumHeight(18);
    m_translation->document()->setDocumentMargin(0);
    if (translationMax > 0)
        m_translation->setMaximumHeight(translationMax);
    translationLayout->addWidget(m_translation);
    sheet += QStringLiteral(
        "QTextBrowser#PopupTranslation{background:transparent;border:0;color:%1;"
        "font-family:'%2';font-size:%3px;}")
        .arg(cssColor(textColor), family, QString::number(translationFont));

    if (twoColumns) {
        auto* gridHost = new QWidget(m_frame);
        auto* grid = new QGridLayout(gridHost);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(0);
        grid->addWidget(sourceHost, 0, 0);
        grid->addWidget(translationHost, 0, 1);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        layout->addWidget(gridHost);

        const int columnWidth =
            qMax(120, (width - sourcePadding[1] - sourcePadding[3]
                       - translationPadding[1] - translationPadding[3]) / 2 - 10);
        for (QTextBrowser* browser : {m_sourceBrowser, m_translation}) {
            if (!browser)
                continue;
            browser->setLineWrapMode(QTextEdit::FixedPixelWidth);
            browser->setLineWrapColumnOrWidth(columnWidth);
        }
    } else {
        layout->addWidget(sourceHost);
        layout->addWidget(translationHost);
    }

    if (!actionsInHeader) {
        m_actions = new QWidget(m_frame);
        const bool bottom = actionsConfig.value(QStringLiteral("placement")).toString()
                            != QStringLiteral("inline");
        m_actions->setObjectName(bottom ? QStringLiteral("PopupActions")
                                        : QStringLiteral("PopupActionsInline"));
        auto* actionsLayout = new QHBoxLayout(m_actions);
        const QList<int> actionsPadding = paddingOf(actionsConfig, {4, 8, 6, 8});
        actionsLayout->setContentsMargins(actionsPadding[3], actionsPadding[0],
                                          actionsPadding[1], actionsPadding[2]);
        actionsLayout->setSpacing(actionsConfig.value(QStringLiteral("gap")).toInt(2));

        const QSize buttonSize(actionsConfig.value(QStringLiteral("buttonWidth")).toInt(28),
                               actionsConfig.value(QStringLiteral("buttonHeight")).toInt(26));
        m_copyButton = createAction(IconFactory::Glyph::Copy,
                                    QStringLiteral("Копировать перевод"), buttonSize);
        m_speakButton = createAction(IconFactory::Glyph::Speaker,
                                     QStringLiteral("Озвучить перевод"), buttonSize);
        m_openButton = createAction(IconFactory::Glyph::Globe,
                                    QStringLiteral("Перейти в главное окно"), buttonSize);
        actionsLayout->addWidget(m_copyButton);
        actionsLayout->addWidget(m_speakButton);
        actionsLayout->addWidget(m_openButton);
        actionsLayout->addStretch(1);

        m_status = new QLabel(m_actions);
        m_status->setObjectName(QStringLiteral("PopupStatus"));
        actionsLayout->addWidget(m_status);

        if (actionsConfig.value(QStringLiteral("textButton")).toBool(false)) {
            auto* openText = new QPushButton(
                actionsConfig.value(QStringLiteral("textLabel"))
                    .toString(QStringLiteral("Перевод")),
                m_actions);
            openText->setObjectName(QStringLiteral("PopupOpenText"));
            openText->setCursor(Qt::PointingHandCursor);
            openText->setFlat(true);
            connect(openText, &QPushButton::clicked, this, [this] {
                emit openMainRequested(m_text.isEmpty() ? sourceText() : m_text);
                hide();
            });
            actionsLayout->addWidget(openText);
            sheet += QStringLiteral(
                "QPushButton#PopupOpenText{background:transparent;border:0;color:%1;"
                "font-family:'%2';font-size:12px;padding:0 4px;}"
                "QPushButton#PopupOpenText:hover{color:%3;}")
                .arg(cssColor(textColor), family, cssColor(accent));
        }

        if (bottom) {
            if (actionsConfig.value(QStringLiteral("background")).isObject()) {
                sheet += QStringLiteral("QWidget#PopupActions{background:%1;%2}")
                             .arg(cssColor(actionBackground),
                                  actionsConfig.value(QStringLiteral("borderTop")).toBool(false)
                                      ? QStringLiteral("border-top:1px solid %1;")
                                            .arg(cssColor(border))
                                      : QString());
            } else if (actionsConfig.value(QStringLiteral("borderTop")).toBool(false)) {
                sheet += QStringLiteral(
                    "QWidget#PopupActions{background:transparent;border-top:1px solid %1;}")
                             .arg(cssColor(border));
            }
        } else {
            sheet += QStringLiteral("QWidget#PopupActionsInline{background:transparent;border:0;}");
        }

        const qreal opacity = actionsConfig.value(QStringLiteral("opacity")).toDouble(1.0);
        const bool reveal = actionsConfig.value(QStringLiteral("revealOnHover")).toBool(false);
        if (reveal || opacity < 1.0) {
            m_actionsOpacity = new QGraphicsOpacityEffect(m_actions);
            m_actionsOpacity->setOpacity(qBound(0.0, opacity, 1.0));
            m_actions->setGraphicsEffect(m_actionsOpacity);
        }
        layout->addWidget(m_actions);
    }

    m_closeButton = new QToolButton(m_frame);
    m_closeButton->setObjectName(QStringLiteral("PopupButton"));
    m_closeButton->setIcon(IconFactory::icon(IconFactory::Glyph::Close, textColor, 14));
    m_closeButton->setIconSize(QSize(16, 16));
    m_closeButton->setFixedSize(closeSize, closeSize);
    m_closeButton->setCursor(Qt::PointingHandCursor);
    m_closeButton->setToolTip(QStringLiteral("Закрыть (Esc)"));
    m_closeButton->setAccessibleName(QStringLiteral("Закрыть (Esc)"));
    connect(m_closeButton, &QToolButton::clicked, this, &QWidget::hide);
    m_closeButton->raise();

    m_frame->setStyleSheet(sheet);

    if (m_copyButton)
        connect(m_copyButton, &QToolButton::clicked, this, &PopupWindow::copyTranslation);
    if (m_speakButton) {
        connect(m_speakButton, &QToolButton::clicked, this, [this] {
            if (!m_translation)
                return;
            TtsService::speak(
                m_context, m_translation->toPlainText(),
                m_toCode.isEmpty() ? QStringLiteral("en") : m_toCode,
                m_context && m_context->settings
                    && m_context->settings->boolValue(
                        QStringLiteral("Advanced.EnableSlowerListening"), false));
        });
    }
    if (m_openButton) {
        connect(m_openButton, &QToolButton::clicked, this, [this] {
            emit openMainRequested(m_text.isEmpty() ? sourceText() : m_text);
            hide();
        });
    }

    const bool showIcons = !m_context || !m_context->settings
        || m_context->settings->boolValue(QStringLiteral("Appearance.PopupIcons"), true);
    for (QToolButton* button : {m_copyButton, m_speakButton, m_openButton}) {
        if (button)
            button->setVisible(showIcons);
    }

    applyLineHeight(m_translation ? m_translation->document() : nullptr,
                    translationConfig.value(QStringLiteral("lineHeight")).toDouble(1.4));
    if (m_sourceBrowser) {
        applyLineHeight(m_sourceBrowser->document(),
                        sourceConfig.value(QStringLiteral("lineHeight")).toDouble(1.4));
    }
}

void PopupWindow::setSourceText(const QString& text)
{
    if (m_source)
        m_source->setText(text);
    if (m_sourceBrowser)
        m_sourceBrowser->setPlainText(text);
}

QString PopupWindow::sourceText() const
{
    if (m_source)
        return m_source->text();
    if (m_sourceBrowser)
        return m_sourceBrowser->toPlainText();
    return m_text;
}

QString PopupWindow::headerText() const
{
    const auto code = [](QString value) {
        value = value.trimmed().toUpper();
        if (value.isEmpty())
            return QStringLiteral("AUTO");
        return value;
    };
    return QStringLiteral("%1 → %2").arg(code(m_fromCode), code(m_toCode));
}

void PopupWindow::showResult(const QString& source, const QString& translation)
{
    if (m_fromCode.isEmpty())
        m_fromCode = m_context && m_context->settings
            ? m_context->settings->stringValue(QStringLiteral("General.LanguageFrom"),
                                               QStringLiteral("en"))
            : QStringLiteral("en");
    if (m_toCode.isEmpty())
        m_toCode = m_context && m_context->settings
            ? m_context->settings->stringValue(QStringLiteral("General.LanguageTo"),
                                               QStringLiteral("ru"))
            : QStringLiteral("ru");
    applyAppearance();
    resetRequest();
    setSourceText(source);
    if (m_translation)
        m_translation->setPlainText(translation);
    if (m_status)
        m_status->clear();
    m_text = source;
    updateAutoSize();
    show();
    raise();
    const QPoint cursor = QCursor::pos();
    QRect available = QGuiApplication::primaryScreen()
        ? QGuiApplication::primaryScreen()->availableGeometry() : QRect(0, 0, 1280, 720);
    if (QScreen* screen = QGuiApplication::screenAt(cursor))
        available = screen->availableGeometry();
    QPoint target;
    if (!m_context || !m_context->settings
        || m_context->settings->boolValue(QStringLiteral("Appearance.PopupNearCursor"), true)) {
        target = cursor + QPoint(18, 18);
    } else {
        target = available.center() - QPoint(width() / 2, height() / 2);
    }
    target.setX(qBound(available.left(), target.x(), available.right() - width()));
    target.setY(qBound(available.top(), target.y(), available.bottom() - height()));
    move(target);
    startHideTimer();
}

void PopupWindow::showTranslation(const QString& text, const QString& fromCode,
                                  const QString& toCode)
{
    if (text.trimmed().isEmpty())
        return;

    m_fromCode = fromCode;
    m_toCode = toCode;
    applyAppearance();
    resetRequest();
    m_text = text;
    setSourceText(text);
    if (m_translation)
        m_translation->clear();
    if (m_status)
        m_status->clear();
    m_waiting = true;
    m_success = false;

    if (m_context && m_context->settings) {
        setAttribute(Qt::WA_ShowWithoutActivating,
                     !m_context->settings->boolValue(QStringLiteral("Appearance.PopupAutoFocus"),
                                                     true));
    }
    updateAutoSize();
    show();
    raise();
    const QPoint cursor = QCursor::pos();
    QRect available = QGuiApplication::primaryScreen()
        ? QGuiApplication::primaryScreen()->availableGeometry() : QRect(0, 0, 1280, 720);
    if (QScreen* screen = QGuiApplication::screenAt(cursor))
        available = screen->availableGeometry();
    QPoint target;
    if (!m_context || !m_context->settings
        || m_context->settings->boolValue(QStringLiteral("Appearance.PopupNearCursor"), true)) {
        target = cursor + QPoint(18, 18);
    } else {
        target = available.center() - QPoint(width() / 2, height() / 2);
    }
    target.setX(qBound(available.left(), target.x(), available.right() - width()));
    target.setY(qBound(available.top(), target.y(), available.bottom() - height()));
    move(target);

    if (m_context && m_context->translation) {
        m_context->translation->translate(
            text, fromCode, toCode,
            m_context->plugins ? m_context->plugins->activeTranslationIds() : QStringList());
    } else {
        m_waiting = false;
        if (m_translation)
            m_translation->setPlainText(QStringLiteral("Ядро перевода недоступно."));
    }
    startHideTimer();
}

void PopupWindow::resetRequest()
{
    m_requestId.clear();
    m_waiting = false;
    m_success = false;
    m_hideTimer->stop();
}

void PopupWindow::updateAutoSize()
{
    if (!m_context || !m_context->settings || !m_frame)
        return;
    const bool autoSize =
        m_context->settings->boolValue(QStringLiteral("Appearance.PopupAutoSize"), true);
    const QJsonObject translationConfig =
        m_style.raw.value(QStringLiteral("translation")).toObject();
    const QJsonObject sourceConfig = m_style.raw.value(QStringLiteral("source")).toObject();
    const int width = m_style.raw.value(QStringLiteral("width")).toInt(380);
    setFixedWidth(width);
    const QList<int> translationPadding = paddingOf(translationConfig, {10, 12, 10, 12});
    const int textWidth =
        qMax(80, width - translationPadding[1] - translationPadding[3] - 16);
    const bool twoColumns = m_style.raw.value(QStringLiteral("twoColumns")).toBool(false);
    int columnTextWidth = textWidth;
    if (twoColumns) {
        const QList<int> sourcePadding = paddingOf(sourceConfig, {11, 14, 11, 14});
        columnTextWidth = qMax(120, (width - sourcePadding[1] - sourcePadding[3]
                                     - translationPadding[1] - translationPadding[3]) / 2 - 10);
    }

    int translationHeight = 0;
    if (m_translation) {
        m_translation->document()->setTextWidth(columnTextWidth);
        applyLineHeight(m_translation->document(),
                        translationConfig.value(QStringLiteral("lineHeight")).toDouble(1.4));
        translationHeight =
            qMax(18, int(std::ceil(m_translation->document()->size().height())) + 2);
        const int maxHeight = translationConfig.value(QStringLiteral("maxHeight")).toInt(0);
        if (!autoSize)
            translationHeight = 140;
        if (maxHeight > 0)
            translationHeight = qMin(translationHeight, maxHeight);
    }

    int sourceHeight = 0;
    if (m_sourceBrowser) {
        m_sourceBrowser->document()->setTextWidth(columnTextWidth);
        applyLineHeight(m_sourceBrowser->document(),
                        sourceConfig.value(QStringLiteral("lineHeight")).toDouble(1.4));
        sourceHeight = qMax(18, int(std::ceil(m_sourceBrowser->document()->size().height())) + 2);
        const int maxHeight = sourceConfig.value(QStringLiteral("maxHeight")).toInt(0);
        if (maxHeight > 0)
            sourceHeight = qMin(sourceHeight, maxHeight);
    }

    if (m_translation)
        m_translation->setFixedHeight(translationHeight);
    if (m_sourceBrowser) {
        const int maxHeight = sourceConfig.value(QStringLiteral("maxHeight")).toInt(0);
        int height = qMax(sourceHeight, translationHeight);
        if (maxHeight > 0)
            height = qMin(height, maxHeight);
        m_sourceBrowser->setFixedHeight(height);
        if (m_translation)
            m_translation->setFixedHeight(height);
    }

    if (QLayout* frameLayout = m_frame->layout())
        frameLayout->activate();
    adjustSize();
}

void PopupWindow::startHideTimer()
{
    if (!m_context || !m_context->settings)
        return;
    const int seconds = m_context->settings->intValue(QStringLiteral("Appearance.PopupTimeout"), 12);
    m_hideTimer->stop();
    if (seconds > 0)
        m_hideTimer->start(seconds * 1000);
}

void PopupWindow::copyTranslation()
{
    if (!m_translation)
        return;
    QApplication::clipboard()->setText(m_translation->toPlainText());
    if (m_status) {
        m_status->setText(QStringLiteral("Скопировано"));
        QTimer::singleShot(1500, m_status, [status = m_status] { status->clear(); });
    }
}

void PopupWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && windowHandle())
        windowHandle()->startSystemMove();
    QWidget::mousePressEvent(event);
}

void PopupWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        hide();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PopupWindow::enterEvent(QEnterEvent* event)
{
    m_hideTimer->stop();
    if (m_actionsOpacity)
        m_actionsOpacity->setOpacity(1.0);
    QWidget::enterEvent(event);
}

void PopupWindow::leaveEvent(QEvent* event)
{
    startHideTimer();
    if (m_actionsOpacity && m_style.valid) {
        const QJsonObject actions = m_style.raw.value(QStringLiteral("actions")).toObject();
        if (actions.value(QStringLiteral("revealOnHover")).toBool(false))
            m_actionsOpacity->setOpacity(
                qBound(0.0, actions.value(QStringLiteral("opacity")).toDouble(0.35), 1.0));
    }
    QWidget::leaveEvent(event);
}

void PopupWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_closeButton && m_frame) {
        m_closeButton->move(m_frame->width() - m_closeButton->width() - 6, 6);
        m_closeButton->raise();
    }
}

void PopupWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!m_outsideFilter) {
        qApp->installEventFilter(this);
        m_outsideFilter = true;
    }
#ifdef Q_OS_WIN
    if (!g_mouseHook && m_context && m_context->settings
        && m_context->settings->boolValue(
            QStringLiteral("Appearance.PopupCloseOnOutsideClick"), true)) {
        g_mousePopup = this;
        g_mouseHook =
            SetWindowsHookExW(WH_MOUSE_LL, popupMouseProc, GetModuleHandleW(nullptr), 0);
    }
#endif
}

void PopupWindow::hideEvent(QHideEvent* event)
{
    if (m_outsideFilter) {
        qApp->removeEventFilter(this);
        m_outsideFilter = false;
    }
#ifdef Q_OS_WIN
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
        g_mousePopup = nullptr;
    }
#endif
    QWidget::hideEvent(event);
}

bool PopupWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget && widget->window() != this)
            hide();
    }
    return QWidget::eventFilter(watched, event);
}
