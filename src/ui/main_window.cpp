#include "main_window.h"
#include "down_combo_box.h"

#include "core/app_context.h"
#include "core/exceptions_manager.h"
#include "core/history_manager.h"
#include "core/language_catalog.h"
#include "core/language_preferences.h"
#include "core/language_router.h"
#include "core/localization.h"
#include "core/path_manager.h"
#include "core/hotkey_utils.h"
#include "core/settings_manager.h"
#include "core/translation_manager.h"
#include "dictionary_window.h"
#include "batch_translation_dialog.h"
#include "history_window.h"
#include "icon_factory.h"
#include "modules/document/document_reader.h"
#include "modules/document/document_translation_job.h"
#include "modules/image/image_translation_pipeline.h"
#include "theme_manager.h"
#include "platform/win/clipboard_manager.h"
#include "platform/win/global_hotkey_manager.h"
#include "platform/win/speech_manager.h"
#include "services/tts_service.h"
#include "popup_window.h"
#include "screen_capture_window.h"
#include "spell_check_highlighter.h"
#include "services/plugin_manager.h"
#include "settings/options_dialog.h"

#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QMoveEvent>
#include <QComboBox>
#include <QEventLoop>
#include <QFile>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QSysInfo>
#include <QDesktopServices>
#include <QDir>
#include <QKeyEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QFrame>
#include <QJsonArray>
#include <QJsonValue>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QPalette>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QShowEvent>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStyle>
#include <QSplitter>
#include <QSplitterHandle>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextEdit>
#include <QTimer>
#include <QProcess>
#include <QRegularExpression>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <QtMath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#endif

namespace {

QColor mutedUiColor()
{
    return QApplication::palette().color(QPalette::PlaceholderText);
}

class FlowLayout final : public QLayout
{
public:
    explicit FlowLayout(QWidget* parent = nullptr, int margin = 0,
                        int horizontalSpacing = 6, int verticalSpacing = 6)
        : QLayout(parent)
        , m_horizontalSpacing(horizontalSpacing)
        , m_verticalSpacing(verticalSpacing)
    {
        setContentsMargins(margin, margin, margin, margin);
    }

    ~FlowLayout() override
    {
        while (QLayoutItem* item = takeAt(0))
            delete item;
    }

    void addItem(QLayoutItem* item) override { m_items.append(item); }
    void addWidget(QWidget* widget) { addItem(new QWidgetItem(widget)); }
    int count() const override { return int(m_items.size()); }
    QLayoutItem* itemAt(int index) const override
    {
        return (index >= 0 && index < m_items.size()) ? m_items.at(index) : nullptr;
    }
    QLayoutItem* takeAt(int index) override
    {
        if (index < 0 || index >= m_items.size())
            return nullptr;
        return m_items.takeAt(index);
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return doLayout(QRect(0, 0, width, 0), true); }
    void setGeometry(const QRect& rect) override
    {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override
    {
        QSize size;
        for (QLayoutItem* item : m_items)
            size = size.expandedTo(item->minimumSize());
        const QMargins margins = contentsMargins();
        return size + QSize(margins.left() + margins.right(),
                            margins.top() + margins.bottom());
    }

private:
    int doLayout(const QRect& rect, bool testOnly) const
    {
        const QMargins margins = contentsMargins();
        const QRect effective = rect.adjusted(margins.left(), margins.top(),
                                              -margins.right(), -margins.bottom());
        int x = effective.x();
        int y = effective.y();
        int lineHeight = 0;
        for (QLayoutItem* item : m_items) {
            const QSize hint = item->sizeHint();
            int nextX = x + hint.width() + m_horizontalSpacing;
            if (nextX - m_horizontalSpacing > effective.right() && lineHeight > 0) {
                x = effective.x();
                y += lineHeight + m_verticalSpacing;
                nextX = x + hint.width() + m_horizontalSpacing;
                lineHeight = 0;
            }
            if (!testOnly)
                item->setGeometry(QRect(QPoint(x, y), hint));
            x = nextX;
            lineHeight = qMax(lineHeight, hint.height());
        }
        return y + lineHeight - rect.y() + margins.bottom();
    }

    QList<QLayoutItem*> m_items;
    int m_horizontalSpacing;
    int m_verticalSpacing;
};

class LanguageCombo final : public DownComboBox
{
public:
    explicit LanguageCombo(QWidget* parent = nullptr)
        : DownComboBox(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setFixedWidth(142);
        setMaxVisibleItems(18);
    }
};

// Custom tooltip for the service chips: QToolTip brings a rectangular native
// drop shadow that shows a square corner behind the rounded panel. The panel is
// painted by hand, so the rounded shape never depends on how Qt styles a
// translucent top-level window.
class ServiceTooltip final : public QWidget
{
public:
    explicit ServiceTooltip(QWidget* parent = nullptr)
        : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        setObjectName(QStringLiteral("ServiceTooltip"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
    }

    void setText(const QString& text)
    {
        m_text = text;
        const QFontMetrics metrics(tooltipFont());
        setFixedSize(metrics.horizontalAdvance(text) + 16, metrics.height() + 8);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(ThemeManager::color(QStringLiteral("borderStrong")), 1));
        painter.setBrush(ThemeManager::color(QStringLiteral("surfaceAlt")));
        painter.drawRoundedRect(box, 6, 6);
        painter.setFont(tooltipFont());
        painter.setPen(ThemeManager::color(QStringLiteral("text")));
        painter.drawText(rect(), Qt::AlignCenter, m_text);
    }

private:
    QFont tooltipFont() const
    {
        QFont font = this->font();
        font.setPixelSize(12);
        return font;
    }

    QString m_text;
};

class GrippySplitterHandle final : public QSplitterHandle
{
public:
    GrippySplitterHandle(Qt::Orientation orientation, QSplitter* parent)
        : QSplitterHandle(orientation, parent)
    {
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Midlight));
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawLine(0, 0, width(), 0);
        painter.drawLine(0, height() - 1, width(), height() - 1);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::PlaceholderText));
        painter.drawRoundedRect(QRectF((width() - 34.0) / 2.0, (height() - 3.0) / 2.0, 34.0, 3.0),
                                1.5, 1.5);
    }
};

class MainSplitter final : public QSplitter
{
public:
    explicit MainSplitter(QWidget* parent = nullptr)
        : QSplitter(Qt::Vertical, parent)
    {
    }

protected:
    QSplitterHandle* createHandle() override
    {
        return new GrippySplitterHandle(orientation(), this);
    }
};

void setPlaceholderColor(QTextEdit* edit)
{
    QPalette palette = edit->palette();
    palette.setColor(QPalette::PlaceholderText,
                     QApplication::palette().color(QPalette::PlaceholderText));
    edit->setPalette(palette);
}

QToolButton* makeIconButton(const IconFactory::Glyph glyph, const QString& accessibleName,
                            QWidget* parent)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("SideButton"));
    button->setIcon(IconFactory::icon(glyph, mutedUiColor(), 15));
    button->setIconSize(QSize(16, 16));
    button->setFixedSize(30, 30);
    button->setCursor(Qt::PointingHandCursor);
    button->setAccessibleName(accessibleName);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QString currentLanguageCode(SettingsManager* settings, const QString& key, const QString& fallback)
{
    if (!settings)
        return fallback;
    const QJsonValue value = settings->value(key);
    if (value.isDouble())
        return languageCode(value.toInt());
    const QString normalized = normalizeLanguageCode(value.toString());
    return normalized.isEmpty() ? fallback : normalized;
}

}

MainWindow::MainWindow(AppContext* context, QWidget* parent)
    : QMainWindow(parent)
    , m_context(context)
    , m_settings(context ? context->settings : nullptr)
{
    setObjectName(QStringLiteral("MainWindow"));
    setWindowTitle(QStringLiteral("Q2Translate"));
    setWindowIcon(IconFactory::appIcon());
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMinimumSize(350, 320);
    resize(500, 400);

    m_stateSaveTimer = new QTimer(this);
    m_stateSaveTimer->setSingleShot(true);
    m_stateSaveTimer->setInterval(600);
    connect(m_stateSaveTimer, &QTimer::timeout, this, [this] {
        saveState();
        if (m_settings)
            m_settings->save();
    });

    buildUi();
    buildMainMenu();
    setAcceptDrops(true);
    if (m_sourceEdit)
        m_sourceEdit->setAcceptDrops(false);
    if (m_targetEdit)
        m_targetEdit->setAcceptDrops(false);

    loadLanguages();
    loadState();
    updateWindowFrameState();

    if (m_context && m_context->translation) {
        connect(m_context->translation, &TranslationManager::translationStarted,
                this, &MainWindow::onTranslationStarted);
        connect(m_context->translation, &TranslationManager::serviceResult,
                this, &MainWindow::onServiceResult);
        connect(m_context->translation, &TranslationManager::translationFinished,
                this, &MainWindow::onTranslationFinished);
        connect(m_context->translation, &TranslationManager::capabilityResult,
                this, &MainWindow::onCapabilityResult);
    }
    if (m_context && m_context->history)
        connect(m_context->history, &HistoryManager::changed, this, &MainWindow::onHistoryChanged);
}

MainWindow::~MainWindow()
{
    if (m_settings) {
        saveState();
        m_settings->save();
    }
}

void MainWindow::buildUi()
{
    auto* frame = new QWidget(this);
    frame->setObjectName(QStringLiteral("WindowFrame"));
    frame->setAttribute(Qt::WA_StyledBackground, true);
    setCentralWidget(frame);

    auto* outer = new QVBoxLayout(frame);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_titleBar = buildTitleBar();
    outer->addWidget(m_titleBar);

    auto* body = new QWidget(frame);
    body->setObjectName(QStringLiteral("WindowBody"));
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    m_bodyLayout = bodyLayout;
    m_sourceRow = buildSourceRow();
    m_targetRow = buildTargetRow();
    m_languageBar = buildLanguageBar();
    rebuildWorkspace();

    m_servicesRow = buildServicesRow();
    bodyLayout->addWidget(m_servicesRow, 0);

    m_historyBar = buildHistoryBar();
    m_historyBar->hide();
    bodyLayout->addWidget(m_historyBar, 0);

    m_statusRow = buildStatusBar();
    bodyLayout->addWidget(m_statusRow, 0);

    outer->addWidget(body, 1);

    m_instantTimer = new QTimer(this);
    m_instantTimer->setSingleShot(true);
    m_instantTimer->setInterval(900);
    connect(m_instantTimer, &QTimer::timeout, this, &MainWindow::onTranslate);

    m_serviceTooltipTimer = new QTimer(this);
    m_serviceTooltipTimer->setSingleShot(true);
    m_serviceTooltipTimer->setInterval(600);
    connect(m_serviceTooltipTimer, &QTimer::timeout, this, &MainWindow::showServiceTooltip);
    connect(m_sourceEdit, &QTextEdit::textChanged, this, &MainWindow::onInstantTextChanged);

    m_spellHighlighter = new SpellCheckHighlighter(
        m_sourceEdit->document(), m_context ? m_context->plugins : nullptr, m_settings);
    m_spellHighlighter->setLanguage(sourceLanguageCode());
    m_spellHighlighter->setEnabled(
        m_settings && m_settings->boolValue(QStringLiteral("General.SpellChecking"), true)
            && m_settings->boolValue(QStringLiteral("ServiceProviders.spellOn"), true));
    m_sourceEdit->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sourceEdit, &QTextEdit::customContextMenuRequested,
            this, &MainWindow::showSourceContextMenu);
    connect(m_fromCombo, &QComboBox::currentIndexChanged, this, [this] {
        if (m_spellHighlighter)
            m_spellHighlighter->setLanguage(sourceLanguageCode());
    });
}

QWidget* MainWindow::buildTitleBar()
{
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("TitleBar"));
    bar->setFixedHeight(40);
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->installEventFilter(this);

    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 0, 8, 0);
    layout->setSpacing(12);

    auto* logo = new QLabel(bar);
    logo->setObjectName(QStringLiteral("TitleLogo"));
    logo->setPixmap(IconFactory::appIcon().pixmap(18, 18));
    logo->setFixedSize(18, 18);
    layout->addWidget(logo);

    m_titleLabel = new QLabel(QStringLiteral("Q2Translate"), bar);
    m_titleLabel->setObjectName(QStringLiteral("TitleText"));
    layout->addWidget(m_titleLabel);
    layout->addStretch(1);

    m_pinButton = new QToolButton(bar);
    m_pinButton->setObjectName(QStringLiteral("WinButton"));
    m_pinButton->setIcon(IconFactory::icon(IconFactory::Glyph::Pin,
                                           mutedUiColor(), 15));
    m_pinButton->setIconSize(QSize(16, 16));
    m_pinButton->setFixedSize(36, 28);
    m_pinButton->setCheckable(true);
    m_pinButton->setCursor(Qt::PointingHandCursor);
    m_pinButton->setFocusPolicy(Qt::NoFocus);
    connect(m_pinButton, &QToolButton::toggled, this, &MainWindow::onPinToggled);
    layout->addWidget(m_pinButton);

    m_gearButton = new QToolButton(bar);
    m_gearButton->setObjectName(QStringLiteral("WinButton"));
    m_gearButton->setIcon(IconFactory::icon(IconFactory::Glyph::Gear,
                                            mutedUiColor(), 15));
    m_gearButton->setIconSize(QSize(16, 16));
    m_gearButton->setFixedSize(36, 28);
    m_gearButton->setCursor(Qt::PointingHandCursor);
    m_gearButton->setFocusPolicy(Qt::NoFocus);
    connect(m_gearButton, &QToolButton::clicked, this, &MainWindow::showMainMenu);
    layout->addWidget(m_gearButton);

    auto* windowButtons = new QWidget(bar);
    auto* windowButtonLayout = new QHBoxLayout(windowButtons);
    windowButtonLayout->setContentsMargins(0, 0, 0, 0);
    windowButtonLayout->setSpacing(2);

    auto* minButton = new QToolButton(windowButtons);
    minButton->setObjectName(QStringLiteral("WinButton"));
    minButton->setText(QString::fromUtf8("─"));
    minButton->setFixedSize(36, 28);
    minButton->setCursor(Qt::PointingHandCursor);
    minButton->setFocusPolicy(Qt::NoFocus);
    connect(minButton, &QToolButton::clicked, this, &QWidget::showMinimized);
    windowButtonLayout->addWidget(minButton);

    m_maxButton = new QToolButton(windowButtons);
    m_maxButton->setObjectName(QStringLiteral("WinButton"));
    m_maxButton->setText(QString::fromUtf8("□"));
    m_maxButton->setFixedSize(36, 28);
    m_maxButton->setCursor(Qt::PointingHandCursor);
    m_maxButton->setFocusPolicy(Qt::NoFocus);
    connect(m_maxButton, &QToolButton::clicked, this, &MainWindow::onToggleMaximize);
    windowButtonLayout->addWidget(m_maxButton);

    auto* closeButton = new QToolButton(windowButtons);
    closeButton->setObjectName(QStringLiteral("WinClose"));
    closeButton->setText(QString::fromUtf8("✕"));
    closeButton->setFixedSize(36, 28);
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setFocusPolicy(Qt::NoFocus);
    connect(closeButton, &QToolButton::clicked, this, &QWidget::close);
    windowButtonLayout->addWidget(closeButton);

    layout->addWidget(windowButtons);
    return bar;
}

QWidget* MainWindow::buildSourceRow()
{
    auto* row = new QWidget(this);
    row->setObjectName(QStringLiteral("EditorRow"));
    row->setMinimumHeight(22);
    row->setMinimumWidth(200);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* pane = new QWidget(row);
    pane->setObjectName(QStringLiteral("EditorPane"));
    auto* paneLayout = new QVBoxLayout(pane);
    paneLayout->setContentsMargins(0, 0, 0, 0);
    paneLayout->setSpacing(0);

    m_sourceEdit = new QTextEdit(pane);
    m_sourceEdit->setObjectName(QStringLiteral("SourceEdit"));
    m_sourceEdit->setAcceptRichText(false);
    m_sourceEdit->setMinimumHeight(22);
    m_sourceEdit->setPlaceholderText(
        QStringLiteral("Введите текст для перевода, перетащите файл или нажмите кнопку микрофона…"));
    setPlaceholderColor(m_sourceEdit);
    m_sourceEdit->document()->setDocumentMargin(0);
    m_sourceEdit->setTabChangesFocus(true);
    paneLayout->addWidget(m_sourceEdit);
    layout->addWidget(pane, 1);
    m_sourceSideButtons = buildEditorSideButtons(true);
    layout->addWidget(m_sourceSideButtons, 0);
    return row;
}

QWidget* MainWindow::buildTargetRow()
{
    auto* row = new QWidget(this);
    row->setObjectName(QStringLiteral("EditorRow"));
    row->setMinimumHeight(22);
    row->setMinimumWidth(200);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* pane = new QWidget(row);
    pane->setObjectName(QStringLiteral("EditorPane"));
    auto* paneLayout = new QVBoxLayout(pane);
    paneLayout->setContentsMargins(0, 0, 0, 0);
    paneLayout->setSpacing(0);

    m_resultTabs = new QTabBar(pane);
    m_resultTabs->setObjectName(QStringLiteral("ResultTabs"));
    m_resultTabs->setExpanding(false);
    m_resultTabs->setDrawBase(false);
    m_resultTabs->setFocusPolicy(Qt::NoFocus);
    m_resultTabs->hide();
    paneLayout->addWidget(m_resultTabs);

    m_targetEdit = new QTextEdit(pane);
    m_targetEdit->setObjectName(QStringLiteral("TargetEdit"));
    m_targetEdit->setAcceptRichText(false);
    m_targetEdit->setReadOnly(true);
    m_targetEdit->setMinimumHeight(22);
    m_targetEdit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_targetEdit->setPlaceholderText(
        QStringLiteral("Выберите сервис внизу и нажмите «Перевести».\n"
                       "Клик по сервису — выбрать один, Ctrl+клик — несколько (появятся вкладки).\n"
                       "Полный список — «Настройки → Сервисы»."));
    setPlaceholderColor(m_targetEdit);
    m_targetEdit->document()->setDocumentMargin(0);
    paneLayout->addWidget(m_targetEdit);
    layout->addWidget(pane, 1);
    m_targetSideButtons = buildEditorSideButtons(false);
    layout->addWidget(m_targetSideButtons, 0);

    // Images pasted from the clipboard and placeholder tokens are handled by the
    // window itself: paste opens the image pipeline, a click on a token opens the
    // file, and removing a token releases the file.
    m_sourceEdit->installEventFilter(this);
    m_targetEdit->installEventFilter(this);
    m_sourceEdit->viewport()->installEventFilter(this);
    m_targetEdit->viewport()->installEventFilter(this);
    connect(m_sourceEdit, &QTextEdit::textChanged, this, &MainWindow::cleanupImages);
    connect(m_targetEdit, &QTextEdit::textChanged, this, &MainWindow::cleanupImages);

    connect(m_resultTabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index < 0)
            return;
        showResultForService(m_resultTabs->tabData(index).toString());
    });

    return row;
}

QWidget* MainWindow::buildLanguageBar()
{
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("LanguageBar"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(40);

    auto* layout = new QHBoxLayout(bar);
    m_languageBarLayout = layout;
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(8);

    m_clearButton = new QToolButton(bar);
    m_clearButton->setObjectName(QStringLiteral("BarButton"));
    m_clearButton->setIcon(IconFactory::icon(IconFactory::Glyph::Trash,
                                           mutedUiColor(), 15));
    m_clearButton->setIconSize(QSize(16, 16));
    m_clearButton->setFixedSize(30, 30);
    m_clearButton->setCursor(Qt::PointingHandCursor);
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    connect(m_clearButton, &QToolButton::clicked, this, &MainWindow::onClearAll);
    layout->addWidget(m_clearButton);

    m_fromCombo = new LanguageCombo(bar);
    layout->addWidget(m_fromCombo);

    m_swapButton = new QToolButton(bar);
    m_swapButton->setObjectName(QStringLiteral("BarButton"));
    m_swapButton->setIcon(IconFactory::icon(IconFactory::Glyph::Swap,
                                          mutedUiColor(), 15));
    m_swapButton->setIconSize(QSize(16, 16));
    m_swapButton->setFixedSize(30, 30);
    m_swapButton->setCursor(Qt::PointingHandCursor);
    m_swapButton->setFocusPolicy(Qt::NoFocus);
    connect(m_swapButton, &QToolButton::clicked, this, &MainWindow::onSwapLanguages);
    layout->addWidget(m_swapButton);

    m_toCombo = new LanguageCombo(bar);
    layout->addWidget(m_toCombo);
    m_languageBarSpacer = new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum);
    layout->addSpacerItem(m_languageBarSpacer);

    m_translateButton = new QPushButton(QStringLiteral("Перевести"), bar);
    m_translateButton->setObjectName(QStringLiteral("TranslateButton"));
    m_translateButton->setAccessibleName(QStringLiteral("Перевести"));
    m_translateButton->setIcon(IconFactory::icon(
        IconFactory::Glyph::Globe,
        ThemeManager::color(QStringLiteral("onAccent")),
        ThemeManager::color(QStringLiteral("onAccent")),
        ThemeManager::color(QStringLiteral("accentSoft")),
        14));
    m_translateButton->setIconSize(QSize(16, 16));
    m_translateButton->setMinimumHeight(30);
    // The button may shrink in a narrow bar but must never stretch: text,
    // padding and the accent fill stay at their natural width.
    m_translateButton->setSizePolicy(
        QSizePolicy::Maximum, m_translateButton->sizePolicy().verticalPolicy());
    m_translateButton->setCursor(Qt::PointingHandCursor);
    connect(m_translateButton, &QPushButton::clicked, this, &MainWindow::onTranslate);
    layout->addWidget(m_translateButton);

    auto updateLanguageIndicator = [this] {
        if (!m_languageIndicator)
            return;
        const QString from = m_fromCombo ? m_fromCombo->currentData().toString() : QString();
        const QString to = m_toCombo ? m_toCombo->currentData().toString() : QString();
        m_languageIndicator->setText(QStringLiteral("%1 → %2")
                                         .arg(languageNameByCode(from),
                                              languageNameByCode(to)));
    };
    connect(m_fromCombo, &QComboBox::currentIndexChanged, this, updateLanguageIndicator);
    connect(m_toCombo, &QComboBox::currentIndexChanged, this, updateLanguageIndicator);
    updateLanguageIndicator();
    return bar;
}

QWidget* MainWindow::buildServicesRow()
{
    auto* row = new QWidget(this);
    row->setObjectName(QStringLiteral("ServicesRow"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    row->setMinimumHeight(40);
    row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(8);

    m_serviceScroll = new QScrollArea(row);
    m_serviceScroll->setObjectName(QStringLiteral("ServiceScroll"));
    m_serviceScroll->setFrameShape(QFrame::NoFrame);
    m_serviceScroll->setWidgetResizable(true);
    m_serviceScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_serviceScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_serviceScroll->setFixedHeight(28);
    m_serviceScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_serviceScroll->horizontalScrollBar(), &QScrollBar::rangeChanged, this,
            [this](int, int) { updateServiceScrollHeight(); });

    m_serviceRibbon = new QWidget(m_serviceScroll);
    m_serviceRibbon->setObjectName(QStringLiteral("ServiceRibbon"));
    m_serviceRibbon->setMinimumHeight(28);
    m_serviceRibbon->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    auto* ribbonLayout = new QHBoxLayout(m_serviceRibbon);
    ribbonLayout->setContentsMargins(0, 0, 0, 0);
    ribbonLayout->setSpacing(6);
    ribbonLayout->setSizeConstraint(QLayout::SetMinimumSize);
    m_serviceRibbon->setLayout(ribbonLayout);
    m_serviceScroll->setWidget(m_serviceRibbon);
    layout->addWidget(m_serviceScroll, 1);

    m_serviceDropdown = new QPushButton(row);
    m_serviceDropdown->setObjectName(QStringLiteral("ServiceDropdown"));
    m_serviceDropdown->setCursor(Qt::PointingHandCursor);
    m_serviceMenu = new QMenu(m_serviceDropdown);
    m_serviceDropdown->setMenu(m_serviceMenu);
    m_serviceDropdown->setMinimumWidth(220);
    m_serviceDropdown->hide();
    layout->addWidget(m_serviceDropdown, 0);
    return row;
}

QWidget* MainWindow::buildHistoryBar()
{
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("HistoryBar"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(40);

    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(8);

    m_historyPrevious = new QPushButton(QString::fromUtf8("◄"), bar);
    m_historyPrevious->setObjectName(QStringLiteral("SmallButton"));
    m_historyPrevious->setFocusPolicy(Qt::NoFocus);
    connect(m_historyPrevious, &QPushButton::clicked, this, &MainWindow::onHistoryPrevious);
    layout->addWidget(m_historyPrevious);

    m_historyNext = new QPushButton(QString::fromUtf8("►"), bar);
    m_historyNext->setObjectName(QStringLiteral("SmallButton"));
    m_historyNext->setFocusPolicy(Qt::NoFocus);
    connect(m_historyNext, &QPushButton::clicked, this, &MainWindow::onHistoryNext);
    layout->addWidget(m_historyNext);

    m_historyPos = new QLabel(QStringLiteral("— / 0"), bar);
    m_historyPos->setObjectName(QStringLiteral("MiniNote"));
    layout->addWidget(m_historyPos);
    layout->addStretch(1);

    auto* note = new QLabel(QStringLiteral("Панель навигации по истории (Layout.ShowHistoryNav)"), bar);
    note->setObjectName(QStringLiteral("MiniNote"));
    layout->addWidget(note);
    return bar;
}

QWidget* MainWindow::buildStatusBar()
{
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("StatusBar"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(12, 4, 12, 4);
    layout->setSpacing(8);

    auto* dot = new QLabel(QStringLiteral("●"), bar);
    dot->setObjectName(QStringLiteral("StatusDot"));
    layout->addWidget(dot);
    m_statusLabel = new QLabel(QStringLiteral("Готово"), bar);
    m_statusLabel->setObjectName(QStringLiteral("StatusText"));
    layout->addWidget(m_statusLabel);
    layout->addStretch(1);
    m_languageIndicator = new QLabel(bar);
    m_languageIndicator->setObjectName(QStringLiteral("MiniNote"));
    layout->addWidget(m_languageIndicator);
    return bar;
}

void MainWindow::updateCompactMode()
{
    // The side buttons follow their own setting; compact mode must not hide
    // them behind the user's back.
    const bool showSideButtons =
        !m_settings
        || m_settings->boolValue(QStringLiteral("Layout.ShowEditorSideButtons"), false);
    if (m_sourceSideButtons)
        m_sourceSideButtons->setVisible(showSideButtons);
    if (m_targetSideButtons)
        m_targetSideButtons->setVisible(showSideButtons);

    const bool narrow = width() < 440;
    const bool compactBar = width() < 560;
    if (m_clearButton)
        m_clearButton->setVisible(!narrow);
    if (m_swapButton)
        m_swapButton->setVisible(true);
    const int comboWidth = width() < 380 ? 100
        : (width() < 440 ? 126 : (compactBar ? 134 : 142));
    if (m_fromCombo)
        m_fromCombo->setFixedWidth(comboWidth);
    if (m_toCombo)
        m_toCombo->setFixedWidth(comboWidth);

    if (m_languageBarLayout)
        m_languageBarLayout->setSpacing(compactBar ? 4 : 7);

    if (m_translateButton) {
        m_translateButton->setIcon(IconFactory::icon(
            IconFactory::Glyph::Globe,
            ThemeManager::color(QStringLiteral("onAccent")),
            ThemeManager::color(QStringLiteral("onAccent")),
            ThemeManager::color(QStringLiteral("accentSoft")),
            14));
        const bool iconOnly = width() < 495;
        if (iconOnly) {
            m_translateButton->setText(QString());
            m_translateButton->setFixedWidth(44);
        } else {
            m_translateButton->setText(QStringLiteral("Перевести"));
            m_translateButton->setMinimumWidth(0);
            m_translateButton->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        const bool compactPadding = !iconOnly && width() < 540;
        if (m_translateButton->property("compact").toBool() != compactPadding) {
            m_translateButton->setProperty("compact", compactPadding);
            m_translateButton->style()->unpolish(m_translateButton);
            m_translateButton->style()->polish(m_translateButton);
            m_translateButton->update();
        }
    }
    if (m_titleLabel) {
        m_titleLabel->setText(QStringLiteral("Q2Translate"));
    }
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    updateCompactMode();
    scheduleStateSave();
}

void MainWindow::moveEvent(QMoveEvent* event)
{
    QMainWindow::moveEvent(event);
    scheduleStateSave();
}

void MainWindow::scheduleStateSave()
{
    if (!m_stateSaveTimer || !isVisible() || m_loadingInterface)
        return;
    m_stateSaveTimer->start();
}

void MainWindow::setStatusText(const QString& text)
{
    if (m_statusLabel)
        m_statusLabel->setText(text);
}

void MainWindow::applyStatusBar()
{
    if (!m_settings)
        return;
    if (m_statusRow)
        m_statusRow->setVisible(m_settings->boolValue(QStringLiteral("Layout.ShowStatusBar"), true));
    if (m_languageIndicator)
        m_languageIndicator->setVisible(
            m_settings->boolValue(QStringLiteral("Advanced.LayoutIndicator"), false));
    updateBottomRowCorners();
}

void MainWindow::updateBottomRowCorners()
{
    // Children are not clipped by the rounded WindowFrame, so the last visible
    // row must round its bottom corners itself; otherwise it paints a square
    // corner over the window shape.
    QWidget* bottom = nullptr;
    if (!isMaximized()) {
        // isVisible() is false before the window is shown, so use the explicit
        // hidden state of each row instead.
        if (m_statusRow && !m_statusRow->isHidden())
            bottom = m_statusRow;
        else if (m_historyBar && !m_historyBar->isHidden())
            bottom = m_historyBar;
        else if (m_servicesRow && !m_servicesRow->isHidden())
            bottom = m_servicesRow;
    }
    for (QWidget* row : {m_servicesRow, m_historyBar, m_statusRow}) {
        if (!row)
            continue;
        const bool enabled = row == bottom;
        if (row->property("bottomRow").toBool() == enabled)
            continue;
        row->setProperty("bottomRow", enabled);
        row->style()->unpolish(row);
        row->style()->polish(row);
    }
}
void MainWindow::rebuildWorkspace()
{
    if (!m_bodyLayout)
        return;

    if (m_workspace) {
        if (m_sourceRow)
            m_sourceRow->setParent(nullptr);
        if (m_targetRow)
            m_targetRow->setParent(nullptr);
        if (m_languageBar)
            m_languageBar->setParent(nullptr);
        m_bodyLayout->removeWidget(m_workspace);
        delete m_workspace;
        m_workspace = nullptr;
        m_splitter = nullptr;
        m_lower = nullptr;
    }

    const QString preset = m_settings
        ? m_settings->stringValue(QStringLiteral("Layout.InterfacePreset"), QStringLiteral("classic"))
        : QStringLiteral("classic");
    m_sideLayout = preset == QStringLiteral("side") || preset == QStringLiteral("compare");

    m_workspace = new QWidget(m_bodyLayout->parentWidget());
    auto* workspaceLayout = new QVBoxLayout(m_workspace);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(0);

    if (m_sideLayout) {
        m_splitter = new MainSplitter(m_workspace);
        m_splitter->setOrientation(Qt::Horizontal);
        m_splitter->setChildrenCollapsible(false);
        m_splitter->setHandleWidth(7);
        m_splitter->addWidget(m_sourceRow);
        m_splitter->addWidget(m_targetRow);
        m_splitter->setStretchFactor(0, 1);
        m_splitter->setStretchFactor(1, 1);
        workspaceLayout->addWidget(m_splitter, 1);
        workspaceLayout->addWidget(m_languageBar, 0);
    } else {
        m_lower = new QWidget(m_workspace);
        m_lower->setMinimumHeight(68);
        auto* lowerLayout = new QVBoxLayout(m_lower);
        lowerLayout->setContentsMargins(0, 0, 0, 0);
        lowerLayout->setSpacing(0);
        lowerLayout->addWidget(m_languageBar);
        lowerLayout->addWidget(m_targetRow, 1);

        m_splitter = new MainSplitter(m_workspace);
        m_splitter->setChildrenCollapsible(false);
        m_splitter->setHandleWidth(7);
        m_splitter->addWidget(m_sourceRow);
        m_splitter->addWidget(m_lower);
        m_splitter->setStretchFactor(0, 0);
        m_splitter->setStretchFactor(1, 1);
        workspaceLayout->addWidget(m_splitter, 1);
    }

    m_bodyLayout->insertWidget(0, m_workspace, 1);
    m_sourceRow->show();
    m_targetRow->show();
    m_languageBar->setVisible(
        !m_settings || m_settings->boolValue(QStringLiteral("Layout.ShowLangBar"), true));
    connect(m_splitter, &QSplitter::splitterMoved, this, [this] { scheduleStateSave(); });
    QTimer::singleShot(0, this, &MainWindow::applySplitterSizes);
}

void MainWindow::applySplitterSizes()
{
    if (!m_splitter)
        return;
    if (m_sideLayout) {
        const int total = m_splitter->width();
        if (total <= 0)
            return;
        const int saved = m_settings
            ? m_settings->intValue(QStringLiteral("Layout.SideSplitterPos"), total / 2)
            : total / 2;
        const int sourceWidth = qBound(200, saved, qMax(200, total - 200));
        m_splitter->setSizes(QList<int>{sourceWidth, qMax(200, total - sourceWidth)});
    } else {
        const int total = m_splitter->height();
        if (total <= 0)
            return;
        const int saved = m_settings
            ? m_settings->intValue(QStringLiteral("General.MidSplitterPos"), 118)
            : 118;
        const int sourceHeight = qBound(22, saved, qMax(22, total - 68));
        m_splitter->setSizes(QList<int>{sourceHeight, qMax(68, total - sourceHeight)});
    }
}

void MainWindow::setInterfacePreset(const QString& preset)
{
    if (!m_settings)
        return;
    if (preset == QStringLiteral("compare") && m_context && m_context->plugins
        && m_context->translation) {
        QStringList ids;
        for (const PluginManifest& manifest : m_context->translation->translationServices())
            ids.append(manifest.id);
        m_context->plugins->setActiveServices(ids);
        refreshServices();
    }
    // Do not persist settings from live previews: only a real preset change
    // should write to disk.  Otherwise every 350 ms preview save would commit
    // unrelated edits before the user presses Apply/OK.
    if (m_settings->stringValue(QStringLiteral("Layout.InterfacePreset"))
        != preset) {
        m_settings->setValue(QStringLiteral("Layout.InterfacePreset"), preset);
        m_settings->save();
    }
    rebuildWorkspace();
    applyServicesStyle();
    applyPanelVisibility();
    if (m_presetGroup) {
        for (QAction* action : m_presetGroup->actions()) {
            if (action->data().toString() == preset) {
                const QSignalBlocker blocker(action);
                action->setChecked(true);
                break;
            }
        }
    }
}
QWidget* MainWindow::buildEditorSideButtons(bool forSource)
{
    auto* host = new QWidget(this);
    host->setObjectName(QStringLiteral("SideButtons"));
    host->setAttribute(Qt::WA_StyledBackground, true);
    host->setFixedWidth(43);
    host->setMinimumHeight(0);
    host->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);

    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(6, 8, 6, 8);
    layout->setSpacing(6);

    if (forSource) {
        auto* copy = makeIconButton(IconFactory::Glyph::Copy,
                                    QStringLiteral("Скопировать текст"), host);
        connect(copy, &QToolButton::clicked, this, &MainWindow::onCopySource);
        layout->addWidget(copy);
        auto* speakSource = makeIconButton(IconFactory::Glyph::Speaker,
                                           QStringLiteral("Озвучить текст (ID 1028)"), host);
        connect(speakSource, &QToolButton::clicked, this, [this] {
            if (m_settings && !m_settings->boolValue(
                    QStringLiteral("ServiceProviders.ttsOn"), true)) {
                setStatusText(QStringLiteral("Озвучивание отключено в настройках"));
                return;
            }
            TtsService::speak(
                m_context, m_sourceEdit->toPlainText(), sourceLanguageCode(),
                m_settings && m_settings->boolValue(
                    QStringLiteral("Advanced.EnableSlowerListening"), false));
        });
        layout->addWidget(speakSource);
        auto* mic = makeIconButton(IconFactory::Glyph::Microphone,
                                   QStringLiteral("Надиктовать с микрофона"), host);
        connect(mic, &QToolButton::clicked, this, &MainWindow::startVoiceInput);
        layout->addWidget(mic);
    } else {
        auto* copy = makeIconButton(IconFactory::Glyph::Copy,
                                    QStringLiteral("Скопировать перевод"), host);
        connect(copy, &QToolButton::clicked, this, &MainWindow::onCopyTranslation);
        layout->addWidget(copy);
        auto* speakTarget = makeIconButton(IconFactory::Glyph::Speaker,
                                           QStringLiteral("Озвучить перевод (ID 1027)"), host);
        connect(speakTarget, &QToolButton::clicked, this, [this] {
            if (m_settings && !m_settings->boolValue(
                    QStringLiteral("ServiceProviders.ttsOn"), true)) {
                setStatusText(QStringLiteral("Озвучивание отключено в настройках"));
                return;
            }
            TtsService::speak(
                m_context, m_targetEdit->toPlainText(), targetLanguageCode(),
                m_settings && m_settings->boolValue(
                    QStringLiteral("Advanced.EnableSlowerListening"), false));
        });
        layout->addWidget(speakTarget);
    }
    layout->addStretch(1);
    return host;
}

void MainWindow::showAbout()
{

        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("О программе"));
        dialog.setModal(true);
        dialog.setMinimumWidth(500);
        auto* root = new QVBoxLayout(&dialog);
        root->setContentsMargins(18, 16, 18, 14);
        root->setSpacing(10);

        auto* logoRow = new QHBoxLayout();
        auto* logo = new QLabel(&dialog);
        logo->setPixmap(IconFactory::appIcon().pixmap(64, 64));
        logo->setFixedSize(64, 64);
        logoRow->addStretch(1);
        logoRow->addWidget(logo);
        logoRow->addStretch(1);
        root->addLayout(logoRow);

        auto* name = new QLabel(QStringLiteral("Q2Translate"), &dialog);
        QFont nameFont = name->font();
        nameFont.setPointSize(nameFont.pointSize() + 6);
        nameFont.setBold(true);
        name->setFont(nameFont);
        name->setAlignment(Qt::AlignCenter);
        root->addWidget(name);

        auto* tagline = new QLabel(
            QStringLiteral("Портативный переводчик для Windows"), &dialog);
        tagline->setObjectName(QStringLiteral("FieldHint"));
        tagline->setAlignment(Qt::AlignCenter);
        root->addWidget(tagline);

        auto* info = new QGridLayout();
        info->setHorizontalSpacing(12);
        info->setVerticalSpacing(4);
        const auto addInfo = [this, &dialog, info](int row, const QString& caption,
                                                   const QString& value) {
            auto* key = new QLabel(caption, &dialog);
            key->setObjectName(QStringLiteral("FieldHint"));
            auto* val = new QLabel(value, &dialog);
            val->setTextInteractionFlags(Qt::TextSelectableByMouse);
            info->addWidget(key, row, 0, Qt::AlignTop);
            info->addWidget(val, row, 1);
        };
        addInfo(0, QStringLiteral("Версия"),
                QApplication::applicationVersion());
        addInfo(1, QStringLiteral("Qt"), QString::fromLatin1(qVersion()));
        addInfo(2, QStringLiteral("Стандарт"), QStringLiteral("C++20"));
        addInfo(3, QStringLiteral("Лицензия"), QStringLiteral("MIT"));
        root->addLayout(info);

        auto* features = new QLabel(
            QStringLiteral(
                "• Перевод текста, документов и изображений\n"
                "• LLM, словари и история переводов\n"
                "• OCR: Windows OCR, Tesseract, PaddleOCR (ONNX)\n"
                "• Горячие клавиши, трей, прокси и офлайн-режим"),
            &dialog);
        features->setWordWrap(true);
        features->setObjectName(QStringLiteral("FieldHint"));
        root->addWidget(features);

        auto* copyright = new QLabel(
            QStringLiteral("© 2026 Q2Translate contributors · MIT License"), &dialog);
        copyright->setObjectName(QStringLiteral("FieldHint"));
        root->addWidget(copyright);

        auto* buttons = new QHBoxLayout();
        auto* close = new QPushButton(QStringLiteral("OK"), &dialog);
        close->setDefault(true);
        buttons->addStretch(1);
        buttons->addWidget(close);
        root->addLayout(buttons);

        connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
        dialog.exec();
}

void MainWindow::buildMainMenu()
{
    m_mainMenu = new QMenu(this);
    m_mainMenu->setObjectName(QStringLiteral("MainMenu"));

    m_spellAction = m_mainMenu->addAction(QStringLiteral("Проверка орфографии"));
    m_spellAction->setCheckable(true);
    connect(m_spellAction, &QAction::toggled, this, [this](bool checked) {
        if (m_settings)
            m_settings->setValue(QStringLiteral("General.SpellChecking"), checked);
        if (m_spellHighlighter)
            m_spellHighlighter->setEnabled(
                checked && m_settings
                && m_settings->boolValue(QStringLiteral("ServiceProviders.spellOn"), true));
    });

    m_instantAction = m_mainMenu->addAction(QStringLiteral("Мгновенный перевод"));
    m_instantAction->setCheckable(true);
    connect(m_instantAction, &QAction::toggled, this, [this](bool checked) {
        if (m_settings)
            m_settings->setValue(QStringLiteral("General.InstantTranslation"), checked);
    });

    auto* callMenu = m_mainMenu->addMenu(QStringLiteral("Дополнительный вызов"));
    m_callGroup = new QActionGroup(this);
    m_callGroup->setExclusive(true);
    const auto addCall = [this, callMenu](const QString& text, const QString& value) {
        QAction* action = callMenu->addAction(text);
        action->setCheckable(true);
        action->setData(value);
        m_callGroup->addAction(action);
        return action;
    };
    addCall(QStringLiteral("Нет"), QStringLiteral("none"))->setChecked(true);
    addCall(QStringLiteral("Обратный перевод"), QStringLiteral("back"));
    addCall(QStringLiteral("Саммаризация (кратко)"), QStringLiteral("summ"));
    addCall(QStringLiteral("Рерайт (перефразирование)"), QStringLiteral("rewr"));
    connect(m_callGroup, &QActionGroup::triggered, this, [this](QAction* action) {
        if (action && m_settings)
            m_settings->setValue(QStringLiteral("General.AdditionalCall"), action->data().toString());
    });

    auto* optionsMenu = m_mainMenu->addMenu(QStringLiteral("Опции"));
    auto* presetsMenu = optionsMenu->addMenu(QStringLiteral("Пресеты интерфейса"));
    m_presetGroup = new QActionGroup(this);
    m_presetGroup->setExclusive(true);
    const auto addPreset = [this, presetsMenu](const QString& title, const QString& value) {
        QAction* action = presetsMenu->addAction(title);
        action->setCheckable(true);
        action->setData(value);
        m_presetGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, value] {
            setInterfacePreset(value);
        });
        return action;
    };
    addPreset(QStringLiteral("Классический"), QStringLiteral("classic"));
    addPreset(QStringLiteral("Бок обок"), QStringLiteral("side"));
    addPreset(QStringLiteral("Сравнение"), QStringLiteral("compare"));

    m_historyNavAction = optionsMenu->addAction(QStringLiteral("Показать панель истории"));
    m_historyNavAction->setCheckable(true);
    connect(m_historyNavAction, &QAction::toggled, this, [this](bool checked) {
        m_historyBar->setVisible(checked);
        if (m_settings)
            m_settings->setValue(QStringLiteral("Layout.ShowHistoryNav"), checked);
    });

    m_langBarAction = optionsMenu->addAction(QStringLiteral("Показать панель выбора языка"));
    m_langBarAction->setCheckable(true);
    connect(m_langBarAction, &QAction::toggled, this, [this](bool checked) {
        m_languageBar->setVisible(checked);
        if (m_settings)
            m_settings->setValue(QStringLiteral("Layout.ShowLangBar"), checked);
    });

    m_servicesPanelAction = optionsMenu->addAction(QStringLiteral("Показать панель сервисов"));
    m_servicesPanelAction->setCheckable(true);
    connect(m_servicesPanelAction, &QAction::toggled, this, [this](bool checked) {
        if (m_settings)
            m_settings->setValue(QStringLiteral("Layout.ShowServicesPanel"), checked);
        applyServicesStyle();
    });

    m_statusBarAction = optionsMenu->addAction(QStringLiteral("Показать строку состояния"));
    m_statusBarAction->setCheckable(true);
    connect(m_statusBarAction, &QAction::toggled, this, [this](bool checked) {
        if (m_settings)
            m_settings->setValue(QStringLiteral("Layout.ShowStatusBar"), checked);
        applyStatusBar();
    });

    m_mainMenu->addSeparator();

    m_dictionaryAction = m_mainMenu->addAction(QStringLiteral("Словарь"));
    connect(m_dictionaryAction, &QAction::triggered, this, &MainWindow::showDictionary);
    m_historyAction = m_mainMenu->addAction(QStringLiteral("История переводов…"));
    connect(m_historyAction, &QAction::triggered, this, &MainWindow::showHistory);
    auto* ocr = m_mainMenu->addAction(QStringLiteral("Распознавание текста (OCR)"));
    connect(ocr, &QAction::triggered, this, &MainWindow::startOcr);
    auto* document = m_mainMenu->addAction(QStringLiteral("Перевести документ…"));
    connect(document, &QAction::triggered, this, &MainWindow::openDocument);
    auto* batch = m_mainMenu->addAction(QStringLiteral("Пакетный перевод документов…"));
    connect(batch, &QAction::triggered, this, [this] { batchTranslate(); });
    auto* settingsAction = m_mainMenu->addAction(QStringLiteral("Настройки…"));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::showOptions);

    m_mainMenu->addSeparator();

    auto* helpMenu = m_mainMenu->addMenu(QStringLiteral("Справка"));
    auto* aboutAction = helpMenu->addAction(QStringLiteral("О программе"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
    auto* exitAction = m_mainMenu->addAction(QStringLiteral("Выход"));
    connect(exitAction, &QAction::triggered, this, &MainWindow::requestQuit);
}
void MainWindow::loadLanguages()
{
    if (!m_fromCombo || !m_toCombo)
        return;
    m_fromCombo->clear();
    m_toCombo->clear();

    QList<LanguageInfo> languages = favoriteLanguageList(m_settings, true);
    for (const LanguageInfo& language : languages) {
        const QString code = QString::fromLatin1(language.code);
        const QString display = QString::fromUtf8(language.name);
        m_fromCombo->addItem(display, code);
        m_toCombo->addItem(display, code);
    }
    const QString from = currentLanguageCode(m_settings, QStringLiteral("General.LanguageFrom"),
                                             QStringLiteral("en"));
    const QString to = currentLanguageCode(m_settings, QStringLiteral("General.LanguageTo"),
                                           QStringLiteral("ru"));
    if (m_fromCombo->findData(from) < 0)
        m_fromCombo->addItem(languageNameByCode(from), from);
    if (m_toCombo->findData(to) < 0)
        m_toCombo->addItem(languageNameByCode(to), to);
    if (const int index = m_fromCombo->findData(from); index >= 0)
        m_fromCombo->setCurrentIndex(index);
    if (const int index = m_toCombo->findData(to); index >= 0)
        m_toCombo->setCurrentIndex(index);
}

void MainWindow::refreshLanguages()
{
    loadLanguages();
}

void MainWindow::loadState()
{
    if (!m_settings)
        return;

    m_loadingInterface = true;
    const QString geometry = m_settings->stringValue(QStringLiteral("General.WindowMainGeometry"));
    if (!geometry.isEmpty()) {
        restoreGeometry(QByteArray::fromBase64(geometry.toLatin1()));
        m_startMaximized = m_settings->boolValue(QStringLiteral("General.WindowMainMaximized"),
                                                 false);
    } else {
        m_centerOnFirstShow = true;
    }

    const bool spell = m_settings->boolValue(QStringLiteral("General.SpellChecking"), true);
    const bool instant = m_settings->boolValue(QStringLiteral("General.InstantTranslation"), true);
    m_spellAction->setChecked(spell);
    m_instantAction->setChecked(instant);

    const QString call = m_settings->stringValue(QStringLiteral("General.AdditionalCall"),
                                                 QStringLiteral("none"));
    for (QAction* action : m_callGroup->actions()) {
        if (action->data().toString() == call) {
            action->setChecked(true);
            break;
        }
    }

    const bool showLangBar = m_settings->boolValue(QStringLiteral("Layout.ShowLangBar"), true);
    const bool showServices = m_settings->boolValue(QStringLiteral("Layout.ShowServicesPanel"), true);
    const bool showHistory = m_settings->boolValue(QStringLiteral("Layout.ShowHistoryNav"), false);
    const bool showStatus = m_settings->boolValue(QStringLiteral("Layout.ShowStatusBar"), true);
    m_langBarAction->setChecked(showLangBar);
    m_servicesPanelAction->setChecked(showServices);
    m_historyNavAction->setChecked(showHistory);
    if (m_statusBarAction)
        m_statusBarAction->setChecked(showStatus);
    if (m_historyAction)
        m_historyAction->setVisible(
            m_settings->boolValue(QStringLiteral("General.EnableHistory"), false));
    if (m_dictionaryAction)
        m_dictionaryAction->setVisible(
            m_settings->boolValue(QStringLiteral("ServiceProviders.dictOn"), false));
    if (m_callGroup) {
        const bool summOn =
            m_settings->boolValue(QStringLiteral("ServiceProviders.summOn"), false);
        const bool rewrOn =
            m_settings->boolValue(QStringLiteral("ServiceProviders.rewrOn"), false);
        for (QAction* action : m_callGroup->actions()) {
            const QString callType = action->data().toString();
            if (callType == QStringLiteral("summ"))
                action->setEnabled(summOn);
            else if (callType == QStringLiteral("rewr"))
                action->setEnabled(rewrOn);
        }
    }
    m_languageBar->setVisible(showLangBar);
    m_servicesRow->setVisible(showServices);
    m_historyBar->setVisible(showHistory);

    refreshServices();
    setInterfacePreset(m_settings->stringValue(QStringLiteral("Layout.InterfacePreset"),
                                               QStringLiteral("classic")));
    applyStatusBar();
    applyServicesStyle();
    applyPanelVisibility();
    updateCompactMode();
    refreshHistoryNav();

    if (m_settings->boolValue(QStringLiteral("Contents.SaveOnExit"), true)) {
        const QString source = m_settings->stringValue(QStringLiteral("Contents.EditSource"));
        const QString translation = m_settings->stringValue(QStringLiteral("Contents.EditTranslation"));
        if (!source.isEmpty()) {
            const QSignalBlocker blocker(m_sourceEdit);
            m_sourceEdit->setPlainText(source);
        }
        if (!translation.isEmpty())
            m_targetEdit->setPlainText(translation);
    }
    if (m_spellHighlighter) {
        m_spellHighlighter->setLanguage(sourceLanguageCode());
        m_spellHighlighter->setEnabled(
            m_settings->boolValue(QStringLiteral("General.SpellChecking"), true)
            && m_settings->boolValue(QStringLiteral("ServiceProviders.spellOn"), true));
    }
    if (m_instantTimer) {
        m_instantTimer->setInterval(900);
    }
    refreshInternalHotkeys();
    m_loadingInterface = false;
}

void MainWindow::refreshServices()
{
    if (!m_serviceRibbon || !m_context || !m_context->translation)
        return;

    hideServiceTooltip();

    if (m_serviceBalancerButton) {
        if (m_servicesRow && m_servicesRow->layout())
            m_servicesRow->layout()->removeWidget(m_serviceBalancerButton);
        m_serviceBalancerButton->deleteLater();
        m_serviceBalancerButton = nullptr;
    }

    if (QLayout* layout = m_serviceRibbon->layout()) {
        while (QLayoutItem* item = layout->takeAt(0)) {
            if (QWidget* widget = item->widget())
                widget->deleteLater();
            delete item;
        }
    }
    if (m_serviceGroup) {
        delete m_serviceGroup;
        m_serviceGroup = nullptr;
    }
    m_serviceGroup = new QButtonGroup(this);
    m_serviceGroup->setExclusive(false);
    m_serviceButtons.clear();
    m_serviceButtonOrder.clear();
    m_serviceActions.clear();
    m_serviceNames.clear();
    if (m_serviceMenu)
        m_serviceMenu->clear();

    QList<PluginManifest> services;
    if (m_context->plugins) {
        for (const PluginManifest& manifest : m_context->plugins->manifests()) {
            if (manifest.valid
                && manifest.capabilities.contains(QStringLiteral("translate"))
                && !m_context->plugins->isDisabled(manifest.id)) {
                services.append(manifest);
            }
        }
    }
    QStringList active = m_context->plugins
        ? m_context->plugins->activeTranslationIds() : QStringList();
    if (active.isEmpty() && !services.isEmpty())
        active.append(services.first().id);

    for (const PluginManifest& manifest : services) {
        const bool isBalancer = manifest.id == QStringLiteral("balancer");
        auto* chip = new QPushButton(isBalancer ? static_cast<QWidget*>(m_servicesRow)
                                                : static_cast<QWidget*>(m_serviceRibbon));
        chip->setObjectName(QStringLiteral("ServiceChip"));
        chip->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        chip->setCheckable(true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFocusPolicy(Qt::NoFocus);
        chip->setText(manifest.name);
        chip->setProperty("serviceId", manifest.id);
        QIcon icon;
        if (!manifest.iconPath.isEmpty() && QFile::exists(manifest.iconPath))
            icon = QIcon(manifest.iconPath);
        if (icon.isNull()) {
            const QColor color = QColor::fromHsv(qHash(manifest.id) % 360, 160, 190);
            icon = IconFactory::serviceIcon(manifest.name, color, 18);
        }
        chip->setIcon(icon);
        chip->setIconSize(QSize(16, 16));
        chip->setToolTip(manifest.name);
        chip->setChecked(active.contains(manifest.id));
        m_serviceGroup->addButton(chip);
        m_serviceButtons.insert(manifest.id, chip);
        m_serviceButtonOrder.append(manifest.id);
        m_serviceNames.insert(manifest.id, manifest.name);
        if (isBalancer) {
            m_serviceBalancerButton = chip;
            if (auto* rowLayout = qobject_cast<QHBoxLayout*>(m_servicesRow->layout()))
                rowLayout->insertWidget(rowLayout->count() - 1, chip, 0, Qt::AlignVCenter);
        } else if (auto* ribbonLayout = qobject_cast<QHBoxLayout*>(m_serviceRibbon->layout())) {
            ribbonLayout->addWidget(chip);
        }
        chip->installEventFilter(this);
        chip->show();

        if (m_serviceMenu) {
            QAction* action = m_serviceMenu->addAction(icon, manifest.name);
            action->setCheckable(true);
            action->setChecked(active.contains(manifest.id));
            action->setData(manifest.id);
            m_serviceActions.insert(manifest.id, action);
            connect(action, &QAction::triggered, this, [this, id = manifest.id] {
                const bool ctrl = QApplication::keyboardModifiers() & Qt::ControlModifier;
                QAction* current = m_serviceActions.value(id, nullptr);
                if (!current)
                    return;
                if (!ctrl) {
                    for (QAction* candidate : std::as_const(m_serviceActions)) {
                        if (candidate != current)
                            candidate->setChecked(false);
                    }
                    current->setChecked(true);
                } else if (std::none_of(m_serviceActions.constBegin(), m_serviceActions.constEnd(),
                                        [](QAction* candidate) { return candidate->isChecked(); })) {
                    current->setChecked(true);
                }
                for (auto it = m_serviceActions.constBegin(); it != m_serviceActions.constEnd(); ++it) {
                    if (QPushButton* button = m_serviceButtons.value(it.key()))
                        button->setChecked(it.value()->isChecked());
                }
                if (checkedServiceIds().isEmpty() && current)
                    current->setChecked(true);
                if (m_context->plugins)
                    m_context->plugins->setActiveServices(checkedServiceIds());
                refreshServiceActions();
            });
        }

        connect(chip, &QPushButton::clicked, this, [this, chip] {
            const bool ctrl = QApplication::keyboardModifiers() & Qt::ControlModifier;
            if (!ctrl) {
                for (auto it = m_serviceButtons.constBegin(); it != m_serviceButtons.constEnd(); ++it) {
                    if (it.value() != chip)
                        it.value()->setChecked(false);
                }
                chip->setChecked(true);
            } else if (checkedServiceIds().isEmpty()) {
                chip->setChecked(true);
            }

            QStringList ids = checkedServiceIds();
            if (ids.isEmpty()) {
                chip->setChecked(true);
                ids = checkedServiceIds();
            }
            for (auto it = m_serviceActions.constBegin(); it != m_serviceActions.constEnd(); ++it) {
                if (QPushButton* button = m_serviceButtons.value(it.key()))
                    it.value()->setChecked(button->isChecked());
            }
            if (m_context->plugins)
                m_context->plugins->setActiveServices(ids);
            if (m_settings) {
                const QString chipId = chip->property("serviceId").toString();
                if (chip->isChecked()) {
                    // Remember the service the user is translating with, but do
                    // not reorder the active list: the panel order comes from
                    // ServicesOrder.
                    m_settings->setValue(QStringLiteral("PrimaryTranslator"), chipId);
                    m_settings->save();
                } else if (m_settings->stringValue(QStringLiteral("PrimaryTranslator")) == chipId
                           && !ids.isEmpty()) {
                    m_settings->setValue(QStringLiteral("PrimaryTranslator"), ids.first());
                    m_settings->save();
                }
            }
            refreshServiceActions();
        });
    }

    if (auto* ribbonLayout = qobject_cast<QHBoxLayout*>(m_serviceRibbon->layout()))
        ribbonLayout->addStretch(1);

    if (m_serviceButtons.isEmpty()) {
        auto* empty = new QLabel(QStringLiteral("Сервисы перевода не найдены"), m_serviceRibbon);
        empty->setObjectName(QStringLiteral("ServiceNote"));
        if (auto* ribbonLayout = qobject_cast<QHBoxLayout*>(m_serviceRibbon->layout()))
            ribbonLayout->addWidget(empty);
        empty->show();
        if (m_serviceDropdown) {
            m_serviceDropdown->setText(QStringLiteral("Нет сервисов"));
            m_serviceDropdown->setEnabled(false);
        }
    }

    applyServicesStyle();
    refreshServiceActions();
    if (m_serviceRibbon) {
        m_serviceRibbon->layout()->invalidate();
        m_serviceRibbon->updateGeometry();
        m_serviceRibbon->show();
    }
    updateServiceScrollHeight();
    if (m_servicesRow) {
        m_servicesRow->updateGeometry();
        if (m_servicesRow->layout())
            m_servicesRow->layout()->activate();
    }
    QTimer::singleShot(0, this, [this] {
        if (!m_serviceRibbon)
            return;
        m_serviceRibbon->layout()->invalidate();
        m_serviceRibbon->updateGeometry();
        m_serviceRibbon->show();
        m_serviceRibbon->update();
        if (m_servicesRow && m_servicesRow->layout())
            m_servicesRow->layout()->activate();
    });
}

void MainWindow::refreshServiceActions()
{
    if (!m_context || !m_context->plugins)
        return;
    QStringList checked;
    for (auto it = m_serviceActions.constBegin(); it != m_serviceActions.constEnd(); ++it) {
        if (QPushButton* button = m_serviceButtons.value(it.key())) {
            const QSignalBlocker blocker(it.value());
            it.value()->setChecked(button->isChecked());
            if (button->isChecked())
                checked.append(it.key());
        }
    }

    if (!m_serviceDropdown)
        return;
    if (checked.isEmpty()) {
        m_serviceDropdown->setText(QStringLiteral("Сервисы"));
        m_serviceDropdown->setIcon(QIcon());
    } else if (checked.size() == 1) {
        m_serviceDropdown->setText(m_serviceNames.value(checked.first(), checked.first()));
        if (QPushButton* button = m_serviceButtons.value(checked.first()))
            m_serviceDropdown->setIcon(button->icon());
    } else {
        m_serviceDropdown->setText(QStringLiteral("Сервисов: %1").arg(checked.size()));
        if (QPushButton* button = m_serviceButtons.value(checked.first()))
            m_serviceDropdown->setIcon(button->icon());
    }
    syncResultTabsForSelection();
}

void MainWindow::syncResultTabsForSelection()
{
    if (!m_resultTabs)
        return;
    const QStringList ids = checkedServiceIds();
    m_resultTexts.clear();
    m_resultErrors.clear();
    m_currentResultService.clear();
    while (m_resultTabs->count() > 0)
        m_resultTabs->removeTab(0);
    m_resultTabs->hide();

    if (ids.size() <= 1)
        return;

    m_resultTabs->blockSignals(true);
    for (const QString& id : ids) {
        const int index = m_resultTabs->addTab(m_serviceNames.value(id, id));
        m_resultTabs->setTabData(index, id);
    }
    m_resultTabs->blockSignals(false);
    m_resultTabs->show();
    m_resultTabs->setCurrentIndex(0);
    if (m_targetEdit)
        m_targetEdit->setPlainText(QStringLiteral("Перевод…"));
}

void MainWindow::updateServiceScrollHeight()
{
    if (!m_serviceScroll || !m_serviceRibbon)
        return;
    const int contentHeight = m_serviceRibbon->sizeHint().height();
    QScrollBar* bar = m_serviceScroll->horizontalScrollBar();
    const int extra = (bar && bar->maximum() > bar->minimum())
        ? bar->sizeHint().height() : 0;
    m_serviceScroll->setFixedHeight(contentHeight + extra);
}

void MainWindow::applyServicesStyle()
{
    if (!m_servicesRow || !m_settings)
        return;
    const bool showPanel = m_settings->boolValue(QStringLiteral("Layout.ShowServicesPanel"), true);
    const QString style = m_settings->stringValue(QStringLiteral("Layout.ServicesListStyle"),
                                                  QStringLiteral("ribbon"));
    if (!showPanel || style == QStringLiteral("hidden")) {
        m_servicesRow->hide();
        updateBottomRowCorners();
        return;
    }
    m_servicesRow->show();
    const bool dropdown = style == QStringLiteral("dropdown");
    if (m_serviceScroll)
        m_serviceScroll->setVisible(!dropdown);
    if (m_serviceBalancerButton)
        m_serviceBalancerButton->setVisible(!dropdown);
    if (m_serviceDropdown)
        m_serviceDropdown->setVisible(dropdown);

    const bool iconsOnly = m_settings->stringValue(
        QStringLiteral("Layout.ClassicView"), QStringLiteral("icons_text"))
        == QStringLiteral("icons");
    for (auto it = m_serviceButtons.constBegin(); it != m_serviceButtons.constEnd(); ++it) {
        const QString name = m_serviceNames.value(it.key(), it.key());
        it.value()->setText(iconsOnly ? QString() : name);
        it.value()->setToolTip(name);
        it.value()->setAccessibleName(name);
        it.value()->setProperty("iconOnly", iconsOnly);
        if (iconsOnly) {
            it.value()->setFixedSize(28, 28);
        } else {
            it.value()->setMinimumSize(0, 0);
            it.value()->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        }
        it.value()->style()->unpolish(it.value());
        it.value()->style()->polish(it.value());
    }
    updateBottomRowCorners();
}
void MainWindow::showSourceContextMenu(const QPoint& position)
{
    if (!m_sourceEdit || !m_spellHighlighter) {
        if (m_sourceEdit) {
            std::unique_ptr<QMenu> menu(m_sourceEdit->createStandardContextMenu());
            if (menu)
                menu->exec(m_sourceEdit->viewport()->mapToGlobal(position));
        }
        return;
    }

    const QTextCursor cursor = m_sourceEdit->cursorForPosition(position);
    const QTextBlock block = cursor.block();
    const QString text = block.text();
    int offset = cursor.positionInBlock();
    const auto isWordCharacter = [](QChar character) {
        return character.isLetter() || character.isMark()
            || character == QLatin1Char('\'') || character == QChar(0x2019)
            || character == QLatin1Char('-');
    };
    int start = offset;
    while (start > 0 && isWordCharacter(text.at(start - 1)))
        --start;
    int end = offset;
    while (end < text.size() && isWordCharacter(text.at(end)))
        ++end;
    while (start < end && !text.at(start).isLetter())
        ++start;
    while (end > start && !text.at(end - 1).isLetter())
        --end;

    const QString word = text.mid(start, end - start);
    const bool misspelled = !word.isEmpty() && m_spellHighlighter->isWordMisspelled(word);

    QMenu menu(m_sourceEdit);
    if (misspelled) {
        QAction* header = menu.addAction(Localization::tr(QStringLiteral("Варианты исправления")));
        header->setEnabled(false);
        const QStringList suggestions = m_spellHighlighter->suggestionsFor(word);
        if (suggestions.isEmpty()) {
            QAction* none = menu.addAction(Localization::tr(QStringLiteral("Нет вариантов")));
            none->setEnabled(false);
        } else {
            for (const QString& suggestion : suggestions) {
                QAction* action = menu.addAction(suggestion);
                const int blockPosition = block.position();
                connect(action, &QAction::triggered, this,
                        [this, blockPosition, start, end, suggestion] {
                    QTextCursor replace(m_sourceEdit->document());
                    replace.setPosition(blockPosition + start);
                    replace.setPosition(blockPosition + end, QTextCursor::KeepAnchor);
                    replace.insertText(suggestion);
                    m_sourceEdit->setTextCursor(replace);
                    m_sourceEdit->setFocus();
                });
            }
        }
        menu.addSeparator();
    }

    std::unique_ptr<QMenu> standard(m_sourceEdit->createStandardContextMenu());
    if (standard) {
        for (QAction* action : standard->actions())
            menu.addAction(action);
    }
    menu.exec(m_sourceEdit->viewport()->mapToGlobal(position));
}

void MainWindow::showServiceTooltip()
{
    QPushButton* chip = m_serviceTooltipChip;
    if (!chip || !chip->isVisible() || chip->toolTip().isEmpty()) {
        hideServiceTooltip();
        return;
    }
    if (!m_serviceTooltip)
        m_serviceTooltip = new ServiceTooltip(this);
    auto* tooltip = static_cast<ServiceTooltip*>(m_serviceTooltip);
    tooltip->setText(chip->toolTip());

    const QPoint anchor = chip->mapToGlobal(QPoint(chip->width() / 2, chip->height()));
    const QRect screen = chip->screen() ? chip->screen()->availableGeometry() : QRect();
    QPoint pos(anchor.x() - tooltip->width() / 2, anchor.y() + 4);
    if (!screen.isNull()) {
        pos.setX(qBound(screen.left() + 4, pos.x(), screen.right() - tooltip->width() - 4));
        if (pos.y() + tooltip->height() > screen.bottom())
            pos.setY(chip->mapToGlobal(QPoint(0, 0)).y() - tooltip->height() - 4);
    }
    tooltip->move(pos);
    tooltip->show();
}

void MainWindow::hideServiceTooltip()
{
    if (m_serviceTooltipTimer)
        m_serviceTooltipTimer->stop();
    if (m_serviceTooltip)
        m_serviceTooltip->hide();
}

QStringList MainWindow::checkedServiceIds() const
{
    // Persisted order first: the first entry is the service used for a plain
    // single translation, so the user's latest choice survives a restart.
    QStringList result;
    if (m_context && m_context->plugins) {
        for (const QString& id : m_context->plugins->activeTranslationIds()) {
            if (QPushButton* chip = m_serviceButtons.value(id); chip && chip->isChecked())
                result.append(id);
        }
    }
    for (const QString& id : m_serviceButtonOrder) {
        if (result.contains(id))
            continue;
        if (QPushButton* chip = m_serviceButtons.value(id); chip && chip->isChecked())
            result.append(id);
    }
    return result;
}

void MainWindow::onTranslate()
{
    if (m_translating || !m_context || !m_context->translation)
        return;
    if (!m_autoSwitchPending)
        m_autoSwitchRetried = false;
    m_autoSwitchPending = false;
    QString text = m_sourceEdit->toPlainText().trimmed();
    if (text.isEmpty()) {
        m_sourceEdit->setFocus();
        return;
    }

    // Image placeholders are UI markers, not text: they must never reach a service.
    m_pendingImageTokens.clear();
    m_imageTokenPrefix.clear();    if (text.contains(QStringLiteral("⟦"))) {
        QStringList kept;
        const QStringList lines = text.split(QLatin1Char('\n'));
        for (const QString& line : lines) {
            const QString trimmed = line.trimmed();
            const QString prefix = QStringLiteral("⟦image:");
            if (trimmed.startsWith(prefix) && trimmed.endsWith(QStringLiteral("⟧"))) {
                const QString id = trimmed.mid(prefix.size(), trimmed.size() - prefix.size() - 1);
                m_pendingImageTokens.append(QStringLiteral("⟦translate:%1⟧").arg(id));
                continue;
            }
            if (trimmed.startsWith(QStringLiteral("⟦translate:")))
                continue;
            kept.append(line);
        }
        text = kept.join(QLatin1Char('\n')).trimmed();
        m_imageTokenPrefix = m_pendingImageTokens.join(QLatin1Char('\n'));
        if (text.isEmpty())
            return;
    }

    QStringList ids = checkedServiceIds();
    if (ids.isEmpty() && m_context->plugins)
        ids = m_context->plugins->activeTranslationIds();
    if (ids.isEmpty()) {
        m_targetEdit->setPlainText(QStringLiteral("Не найден ни один сервис перевода.\n"
                                                  "Проверьте папку services и настройки."));
        return;
    }

    if (m_instantTimer)
        m_instantTimer->stop();
    m_translating = true;
    m_waitingForRequest = true;
    m_additionalCallDone = false;
    m_translateButton->setEnabled(false);
    clearResults();
    refreshServiceActions();
    m_targetEdit->setPlainText(QStringLiteral("Перевод…"));
    setStatusText(QStringLiteral("Перевод…"));
    QString from = sourceLanguageCode();
    if (m_settings
        && m_settings->boolValue(QStringLiteral("General.AlwaysDetectLanguage"), false)) {
        from = QStringLiteral("auto");
        const QString detected = LanguageRouter::detectLanguage(text);
        if (!detected.isEmpty()) {
            if (m_fromCombo) {
                int index = m_fromCombo->findData(detected);
                if (index < 0) {
                    m_fromCombo->addItem(languageNameByCode(detected), detected);
                    index = m_fromCombo->findData(detected);
                }
                if (index >= 0)
                    m_fromCombo->setCurrentIndex(index);
            }
            if (m_languageIndicator) {
                m_languageIndicator->setText(
                    QStringLiteral("%1 → %2")
                        .arg(languageNameByCode(detected), languageNameByCode(targetLanguageCode())));
            }
        }
    }
    m_context->translation->translate(text, from, targetLanguageCode(), ids);
}

void MainWindow::onTranslationStarted(const QString& requestId)
{
    if (!m_waitingForRequest)
        return;
    m_waitingForRequest = false;
    m_currentRequestId = requestId;
}

void MainWindow::maybeAutoSwitchService()
{
    if (!m_settings || !m_context || !m_context->plugins)
        return;
    if (!m_settings->boolValue(QStringLiteral("Balancer.AutoSwitch"), false))
        return;
    if (m_autoSwitchRetried)
        return;
    const bool failed = m_currentResultService.isEmpty()
        || !m_resultErrors.value(m_currentResultService).isEmpty();
    if (!failed)
        return;

    QStringList order = m_context->plugins->activeTranslationIds();
    if (order.isEmpty())
        order = checkedServiceIds();
    if (order.size() < 2)
        return;
    const QString current =
        m_currentResultService.isEmpty() ? order.first() : m_currentResultService;
    int index = order.indexOf(current);
    if (index < 0)
        index = 0;
    const QString next = order.at((index + 1) % order.size());
    if (next.isEmpty() || next == current)
        return;

    if (m_balancerOriginalService.isEmpty())
        m_balancerOriginalService = current;
    m_autoSwitchRetried = true;
    m_autoSwitchPending = true;

    for (auto it = m_serviceActions.begin(); it != m_serviceActions.end(); ++it)
        it.value()->setChecked(it.key() == next);
    for (auto it = m_serviceButtons.begin(); it != m_serviceButtons.end(); ++it)
        it.value()->setChecked(it.key() == next);
    m_context->plugins->setActiveServices({ next });
    m_settings->setValue(QStringLiteral("PrimaryTranslator"), next);
    m_settings->save();
    refreshServiceActions();
    setStatusText(QStringLiteral("Коннектор недоступен — переключаюсь на %1")
                      .arg(m_serviceNames.value(next, next)));
    QTimer::singleShot(0, this, &MainWindow::onTranslate);

    if (m_settings->boolValue(QStringLiteral("Balancer.ReturnToPrimary"), true)) {
        const int minutes =
            qMax(1, m_settings->intValue(QStringLiteral("Balancer.ReturnAfterMinutes"), 10));
        if (!m_balancerReturnTimer) {
            m_balancerReturnTimer = new QTimer(this);
            m_balancerReturnTimer->setSingleShot(true);
            connect(m_balancerReturnTimer, &QTimer::timeout, this,
                    &MainWindow::restoreBalancerService);
        }
        m_balancerReturnTimer->start(minutes * 60 * 1000);
    }
}

void MainWindow::restoreBalancerService()
{
    if (m_balancerOriginalService.isEmpty() || !m_context || !m_context->plugins)
        return;
    const QString original = m_balancerOriginalService;
    m_balancerOriginalService.clear();
    for (auto it = m_serviceActions.begin(); it != m_serviceActions.end(); ++it)
        it.value()->setChecked(it.key() == original);
    for (auto it = m_serviceButtons.begin(); it != m_serviceButtons.end(); ++it)
        it.value()->setChecked(it.key() == original);
    m_context->plugins->setActiveServices({ original });
    if (m_settings) {
        m_settings->setValue(QStringLiteral("PrimaryTranslator"), original);
        m_settings->save();
    }
    refreshServiceActions();
}

void MainWindow::onServiceResult(const QString& requestId, const QString& serviceId,
                                 const QJsonObject& result)
{
    if (requestId != m_currentRequestId)
        return;
    QString text = result.value(QStringLiteral("text")).toString();
    const QString error = result.value(QStringLiteral("error")).toString();
    // Keep image placeholders in every service result, otherwise switching tabs or
    // a second pass would drop them from the translated block.
    if (!m_imageTokenPrefix.isEmpty() && !text.isEmpty())
        text = m_imageTokenPrefix + QLatin1Char('\n') + text;
    const QString name = result.value(QStringLiteral("service_name"))
                             .toString(m_serviceNames.value(serviceId, serviceId));
    m_resultTexts.insert(serviceId, text);
    m_resultErrors.insert(serviceId, error);
    if (!name.isEmpty())
        m_serviceNames.insert(serviceId, name);

    const QString detected = result.value(QStringLiteral("detected_lang")).toString();
    if (!detected.isEmpty() && m_settings
        && m_settings->boolValue(QStringLiteral("General.AlwaysDetectLanguage"), false)) {
        if (m_fromCombo) {
            int index = m_fromCombo->findData(detected);
            if (index < 0) {
                m_fromCombo->addItem(languageNameByCode(detected), detected);
                index = m_fromCombo->findData(detected);
            }
            if (index >= 0 && m_fromCombo->currentIndex() != index)
                m_fromCombo->setCurrentIndex(index);
        }
        if (m_languageIndicator) {
            m_languageIndicator->setText(QStringLiteral("%1 → %2")
                .arg(languageNameByCode(detected), languageNameByCode(targetLanguageCode())));
        }
    }

    int tabIndex = -1;
    for (int index = 0; index < m_resultTabs->count(); ++index) {
        if (m_resultTabs->tabData(index).toString() == serviceId) {
            tabIndex = index;
            break;
        }
    }
    if (tabIndex < 0) {
        m_resultTabs->blockSignals(true);
        tabIndex = m_resultTabs->addTab(name);
        m_resultTabs->setTabData(tabIndex, serviceId);
        m_resultTabs->blockSignals(false);
    }

    if (!text.isEmpty()) {
        if (m_currentResultService.isEmpty()
            || !m_resultErrors.value(m_currentResultService).isEmpty()) {
            m_currentResultService = serviceId;
            m_resultTabs->setCurrentIndex(tabIndex);
            showResultForService(serviceId);
        }
    } else if (m_currentResultService.isEmpty() && !error.isEmpty()) {
        m_currentResultService = serviceId;
        m_resultTabs->setCurrentIndex(tabIndex);
        showResultForService(serviceId);
    }

    if (m_resultTabs->count() > 1)
        m_resultTabs->show();
}

void MainWindow::onTranslationFinished(const QString& requestId)
{
    if (requestId != m_currentRequestId)
        return;
    m_translating = false;
    if (m_translateButton)
        m_translateButton->setEnabled(true);
    setStatusText(m_currentResultService.isEmpty() ? QStringLiteral("Ошибка перевода")
                                                    : QStringLiteral("Готово"));

    if (m_resultTabs->count() > 1) {
        m_resultTabs->show();
    } else {
        m_resultTabs->hide();
        if (m_resultTabs->count() == 1 && m_currentResultService.isEmpty())
            m_currentResultService = m_resultTabs->tabData(0).toString();
    }

    if (m_currentResultService.isEmpty()) {
        if (!m_resultTabs->count())
            m_targetEdit->setPlainText(QStringLiteral("Не удалось получить перевод."));
    } else {
        showResultForService(m_currentResultService);
        handleAdditionalOutput();

        if (!m_additionalCallDone && m_context && m_context->plugins && m_settings) {
            const QString additional = m_settings->stringValue(QStringLiteral("General.AdditionalCall"),
                                                               QStringLiteral("none"));
            bool additionalEnabled = true;
            if (additional == QStringLiteral("summ"))
                additionalEnabled =
                    m_settings->boolValue(QStringLiteral("ServiceProviders.summOn"), true);
            else if (additional == QStringLiteral("rewr"))
                additionalEnabled =
                    m_settings->boolValue(QStringLiteral("ServiceProviders.rewrOn"), true);
            const QString translatedText = m_resultTexts.value(m_currentResultService);
            if (additionalEnabled && !additional.isEmpty() && additional != QStringLiteral("none")
                && !translatedText.isEmpty()) {
                QJsonObject params;
                QString capability;
                if (additional == QStringLiteral("back")) {
                    capability = QStringLiteral("translate");
                    params.insert(QStringLiteral("text"), translatedText);
                    params.insert(QStringLiteral("from"), targetLanguageCode());
                    params.insert(QStringLiteral("to"), sourceLanguageCode() == QStringLiteral("auto")
                                                            ? QStringLiteral("en") : sourceLanguageCode());
                    params.insert(QStringLiteral("from_name"), languageNameByCode(targetLanguageCode()));
                    params.insert(QStringLiteral("to_name"), languageNameByCode(
                        sourceLanguageCode() == QStringLiteral("auto") ? QStringLiteral("en")
                                                                       : sourceLanguageCode()));
                } else if (additional == QStringLiteral("summ")) {
                    capability = QStringLiteral("summarize");
                    params.insert(QStringLiteral("text"), m_sourceEdit->toPlainText());
                    params.insert(QStringLiteral("to"), targetLanguageCode());
                } else if (additional == QStringLiteral("rewr")) {
                    capability = QStringLiteral("paraphrase");
                    params.insert(QStringLiteral("text"), m_sourceEdit->toPlainText());
                    params.insert(QStringLiteral("to"), targetLanguageCode());
                }

                if (!capability.isEmpty()) {
                    const QList<IPluginProvider*> providers =
                        m_context->plugins->providersFor(capability);
                    IPluginProvider* selected = nullptr;
                    if (capability == QStringLiteral("summarize")
                        || capability == QStringLiteral("paraphrase")) {
                        QString preferred;
                        if (capability == QStringLiteral("summarize")) {
                            preferred = m_settings->stringValue(
                                QStringLiteral("ServiceProviders.summ"));
                        } else {
                            preferred = m_settings->stringValue(
                                QStringLiteral("ServiceProviders.rewr"));
                        }
                        if (preferred.isEmpty()) {
                            preferred = m_settings->stringValue(
                                QStringLiteral("General.AdditionalCallProvider"));
                        }
                        if (!preferred.isEmpty()) {
                            for (IPluginProvider* provider : providers) {
                                if (provider->id() == preferred) {
                                    selected = provider;
                                    break;
                                }
                            }
                        }
                    }
                    if (!selected && !providers.isEmpty())
                        selected = providers.first();
                    if (selected) {
                        m_additionalCallDone = true;
                        m_context->translation->executeCapability(capability, params,
                                                                  selected->id());
                    }
                }
            }
        }
    }
    maybeAutoSwitchService();
}

void MainWindow::handleAdditionalOutput()
{
    if (!m_settings)
        return;
    const QString type = m_settings->stringValue(QStringLiteral("General.AdditionalOutputType"),
                                                  QStringLiteral("none"));
    const QString source = m_settings->stringValue(QStringLiteral("General.AdditionalOutputSource"),
                                                    QStringLiteral("translation"));
    const QString text = source == QStringLiteral("input")
        ? m_sourceEdit->toPlainText() : m_targetEdit->toPlainText();
    if (text.trimmed().isEmpty())
        return;

    if (type == QStringLiteral("speak")) {
        if (m_context && m_context->settings
            && m_context->settings->boolValue(QStringLiteral("ServiceProviders.ttsOn"), true)) {
            TtsService::speak(
                m_context, text,
                source == QStringLiteral("input") ? sourceLanguageCode()
                                                  : targetLanguageCode(),
                m_settings->boolValue(QStringLiteral("Advanced.EnableSlowerListening"), false));
        }
    } else if (type == QStringLiteral("copy")) {
        QApplication::clipboard()->setText(text);
        setStatusText(QStringLiteral("Скопировано в буфер"));
    } else if (type == QStringLiteral("popup")) {
        if (!m_popup) {
            m_popup = new PopupWindow(m_context, this);
            connect(m_popup, &PopupWindow::openMainRequested, this, [this](const QString& value) {
                showMainWindow();
                setSourceText(value);
                onTranslate();
            });
        }
        m_popup->showResult(source == QStringLiteral("input") ? m_sourceEdit->toPlainText()
                                                               : m_targetEdit->toPlainText(),
                            m_targetEdit->toPlainText());
    }

    const int copyAction = m_settings->intValue(QStringLiteral("Advanced.CopyAction"), 2);
    if (type != QStringLiteral("copy") && copyAction != 2)
        QApplication::clipboard()->setText(m_targetEdit->toPlainText());
}
void MainWindow::onCapabilityResult(const QString& capability, const QString& serviceId,
                                    const QJsonObject& result)
{
    Q_UNUSED(serviceId);
    if (capability != QStringLiteral("summarize") && capability != QStringLiteral("paraphrase")
        && capability != QStringLiteral("translate")) {
        return;
    }

    QString pseudoId;
    QString title;
    if (capability == QStringLiteral("summarize")) {
        pseudoId = QStringLiteral("__summary");
        title = QStringLiteral("Саммаризация");
    } else if (capability == QStringLiteral("paraphrase")) {
        pseudoId = QStringLiteral("__rewrite");
        title = QStringLiteral("Рерайт");
    } else {
        pseudoId = QStringLiteral("__back_translation");
        title = QStringLiteral("Обратный перевод");
    }

    const QString text = result.value(QStringLiteral("text")).toString();
    const QString error = result.value(QStringLiteral("error")).toString();
    m_resultTexts.insert(pseudoId, text);
    m_resultErrors.insert(pseudoId, error);
    m_serviceNames.insert(pseudoId, title);

    int tabIndex = -1;
    for (int index = 0; index < m_resultTabs->count(); ++index) {
        if (m_resultTabs->tabData(index).toString() == pseudoId) {
            tabIndex = index;
            break;
        }
    }
    if (tabIndex < 0) {
        m_resultTabs->blockSignals(true);
        tabIndex = m_resultTabs->addTab(title);
        m_resultTabs->setTabData(tabIndex, pseudoId);
        m_resultTabs->blockSignals(false);
    }
    m_resultTabs->show();
    m_currentResultService = pseudoId;
    m_resultTabs->setCurrentIndex(tabIndex);
    showResultForService(pseudoId);
}

void MainWindow::showResultForService(const QString& serviceId)
{
    m_currentResultService = serviceId;
    const QString error = m_resultErrors.value(serviceId);
    const QString text = m_resultTexts.value(serviceId);
    if (!text.isEmpty())
        m_targetEdit->setPlainText(text);
    else if (!error.isEmpty())
        m_targetEdit->setPlainText(QStringLiteral("Ошибка: %1").arg(error));
    else
        m_targetEdit->setPlainText(QStringLiteral("Пустой ответ сервиса."));
}

void MainWindow::clearResults()
{
    m_resultTexts.clear();
    m_resultErrors.clear();
    m_currentResultService.clear();
    if (m_resultTabs) {
        while (m_resultTabs->count() > 0)
            m_resultTabs->removeTab(0);
        m_resultTabs->hide();
    }
}

void MainWindow::onSwapLanguages()
{
    if (!m_fromCombo || !m_toCombo)
        return;
    const int fromIndex = m_fromCombo->currentIndex();
    const int toIndex = m_toCombo->currentIndex();
    if (fromIndex < 0 || toIndex < 0)
        return;
    if (m_instantTimer)
        m_instantTimer->stop();
    m_fromCombo->setCurrentIndex(toIndex);
    m_toCombo->setCurrentIndex(fromIndex);
    const QString source = m_sourceEdit->toPlainText();
    const QString target = m_targetEdit->toPlainText();
    m_sourceEdit->setPlainText(target);
    m_targetEdit->setPlainText(source);
    clearResults();
}

void MainWindow::onClearAll()
{
    if (m_instantTimer)
        m_instantTimer->stop();
    m_sourceEdit->clear();
    m_targetEdit->clear();
    clearResults();
    m_sourceEdit->setFocus();
}

void MainWindow::onCopySource()
{
    QApplication::clipboard()->setText(m_sourceEdit->toPlainText());
}

void MainWindow::onCopyTranslation()
{
    QApplication::clipboard()->setText(m_targetEdit->toPlainText());
}

void MainWindow::onToggleMaximize()
{
    if (isMaximized())
        showNormal();
    else
        showMaximized();
}

void MainWindow::onPinToggled(bool checked)
{
    const bool wasVisible = isVisible();
    const bool wasMaximized = isMaximized();
    setWindowFlag(Qt::WindowStaysOnTopHint, checked);
    if (wasVisible) {
        if (wasMaximized)
            showMaximized();
        else
            show();
    }
    m_pinButton->setChecked(checked);
}

void MainWindow::showMainMenu()
{
    if (!m_mainMenu || !m_gearButton)
        return;
    m_mainMenu->exec(m_gearButton->mapToGlobal(QPoint(0, m_gearButton->height() + 4)));
}

void MainWindow::onInstantTextChanged()
{
    // Direction follows the script of the typed text even when instant
    // translation is off.
    if (m_sourceEdit)
        applyAutomaticDirection(m_sourceEdit->toPlainText());
    if (!m_settings || !m_settings->boolValue(QStringLiteral("General.InstantTranslation"), true))
        return;
    if (!m_sourceEdit || m_sourceEdit->toPlainText().trimmed().isEmpty()) {
        if (m_instantTimer)
            m_instantTimer->stop();
        return;
    }
    applyAutomaticDirection(m_sourceEdit->toPlainText());
    if (m_instantTimer)
        m_instantTimer->start();
}

void MainWindow::setSourceText(const QString& text, bool selectAll)
{
    if (m_instantTimer)
        m_instantTimer->stop();
    const QSignalBlocker blocker(m_sourceEdit);
    m_sourceEdit->setPlainText(text);
    if (selectAll) {
        m_sourceEdit->selectAll();
        m_sourceEdit->setFocus(Qt::OtherFocusReason);
    } else {
        m_sourceEdit->moveCursor(QTextCursor::End);
    }
}

QString MainWindow::sourceText() const
{
    return m_sourceEdit ? m_sourceEdit->toPlainText() : QString();
}

QString MainWindow::sourceLanguageCode() const
{
    return m_fromCombo ? m_fromCombo->currentData().toString() : QStringLiteral("auto");
}

QString MainWindow::targetLanguageCode() const
{
    return m_toCombo ? m_toCombo->currentData().toString() : QStringLiteral("ru");
}

void MainWindow::showMainWindow()
{
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::showOptions()
{
    if (!m_context)
        return;
    OptionsDialog dialog(m_context, this);
    connect(&dialog, &OptionsDialog::settingsApplied, this, &MainWindow::onSettingsApplied);
    connect(&dialog, &OptionsDialog::popupPreviewOpenMainRequested, this,
            [this, &dialog](const QString& text) {
                dialog.reject();
                showMainWindow();
                setSourceText(text);
                onTranslate();
            });
    dialog.exec();
}

void MainWindow::showHistory()
{
    if (!m_context)
        return;
    HistoryWindow dialog(m_context, this);
    connect(&dialog, &HistoryWindow::entryActivated, this,
            [this, &dialog](const QJsonObject& entry) {
                onHistoryActivated(entry);
                dialog.accept();
            });
    dialog.exec();
}

void MainWindow::showDictionary()
{
    if (!m_context)
        return;
    DictionaryWindow dialog(m_context, this);
    dialog.exec();
}

void MainWindow::startOcr()
{
    if (m_settings
        && !m_settings->boolValue(QStringLiteral("ServiceProviders.ocrOn"), true)) {
        setStatusText(QStringLiteral("OCR отключён в настройках"));
        return;
    }
    if (m_capture && m_capture->isVisible()) {
        // RegisterHotKey repeats while the key is held; restarting the overlay
        // would clear the selection the user is dragging.
        m_capture->raise();
        m_capture->activateWindow();
        return;
    }
    if (!m_capture) {
        m_capture = new ScreenCaptureWindow(m_context, this);
        connect(m_capture, &ScreenCaptureWindow::textRecognized,
                this, &MainWindow::onTextRecognized);
    }
    m_capture->start();
}

void MainWindow::startVoiceInput()
{
    if (m_voiceProcess && m_voiceProcess->state() != QProcess::NotRunning) {
        setStatusText(QStringLiteral("Распознавание речи уже выполняется…"));
        return;
    }
    if (!m_voiceProcess) {
        m_voiceProcess = new QProcess(this);
        m_voiceProcess->setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments* args) {
                args->flags |= CREATE_NO_WINDOW;
                args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
                args->startupInfo->wShowWindow = SW_HIDE;
            });
        connect(m_voiceProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int, QProcess::ExitStatus) {
                    const QString text =
                        QString::fromUtf8(m_voiceProcess->readAllStandardOutput()).trimmed();
                    if (text.isEmpty()) {
                        setStatusText(QStringLiteral("Речь не распознана"));
                        return;
                    }
                    if (text.startsWith(QStringLiteral("__ERROR__"))) {
                        setStatusText(QStringLiteral("Распознавание речи недоступно"));
                        QMessageBox::information(
                            this, QStringLiteral("Распознавание речи"),
                            QStringLiteral("В Windows не установлен компонент распознавания речи. "
                                           "Добавьте язык и компонент «Распознавание речи» "
                                           "в параметрах Windows."));
                        return;
                    }
                    setSourceText(text);
                    setStatusText(QStringLiteral("Распознано с микрофона"));
                });
    }
    QString culture = m_settings
        ? m_settings->stringValue(QStringLiteral("AutoDetection.LanguageSpeechRecognition"),
                                  QStringLiteral("en-US"))
        : QStringLiteral("en-US");
    culture.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9-]")));
    if (culture.isEmpty())
        culture = QStringLiteral("en-US");
    const QString script = QStringLiteral(
        "[Console]::OutputEncoding=[Text.Encoding]::UTF8; "
        "try { "
        "Add-Type -AssemblyName System.Speech; "
        "$done=$false; "
        "try { "
        "$c=New-Object System.Globalization.CultureInfo('%1'); "
        "$r=New-Object System.Speech.Recognition.SpeechRecognitionEngine -ArgumentList $c; "
        "$r.SetInputToDefaultAudioDevice(); "
        "$r.LoadGrammar((New-Object System.Speech.Recognition.DictationGrammar)); "
        "$e=$r.Recognize([TimeSpan]::FromSeconds(8)); if($e){$e.Text}; $done=$true "
        "} catch {} "
        "if(-not $done){ "
        "$r=New-Object System.Speech.Recognition.SpeechRecognitionEngine; "
        "$r.SetInputToDefaultAudioDevice(); "
        "$r.LoadGrammar((New-Object System.Speech.Recognition.DictationGrammar)); "
        "$e=$r.Recognize([TimeSpan]::FromSeconds(8)); if($e){$e.Text} "
        "} "
        "} catch { Write-Output ('__ERROR__' + $_.Exception.Message) }").arg(culture);
    m_voiceProcess->start(QStringLiteral("powershell.exe"),
                          {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                           QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                           QStringLiteral("-Command"), script});
    setStatusText(QStringLiteral("Слушаю…"));
}

void MainWindow::onTextRecognized(const QString& text, bool translate)
{
    showMainWindow();
    setSourceText(text);
    if (translate)
        onTranslate();
}

void MainWindow::showPopup()
{
    if (!m_context || !m_context->clipboard)
        return;
    QString text = m_context->clipboard->copySelection();
    if (text.isEmpty())
        text = QApplication::clipboard()->text();
    showPopupText(text);
}

void MainWindow::showPopupText(const QString& text)
{
    if (text.trimmed().isEmpty())
        return;

    if (!m_popup) {
        m_popup = new PopupWindow(m_context, this);
        connect(m_popup, &PopupWindow::openMainRequested, this, [this](const QString& value) {
            showMainWindow();
            setSourceText(value);
            onTranslate();
        });
    }
    m_popup->showTranslation(text, sourceLanguageCode(), targetLanguageCode());
}

void MainWindow::translateText(const QString& text)
{
    if (text.trimmed().isEmpty())
        return;
    setSourceText(text);
    onTranslate();
}

void MainWindow::translateCurrent()
{
    showMainWindow();
    onTranslate();
}

void MainWindow::applyAutomaticDirection(const QString& text)
{
    // The user picks text and presses Ctrl twice, or types into the source box:
    // the script of the text decides the direction, following TranslationRules
    // ("Правила перевода"). Without configured rules the manual choice stands.
    if (!m_settings || !m_fromCombo || !m_toCombo)
        return;
    const QJsonValue rules = m_settings->value(QStringLiteral("TranslationRules"));
    if (!rules.isArray() || rules.toArray().isEmpty())
        return;
    int letters = 0;
    for (const QChar& character : text)
        letters += character.isLetter() ? 1 : 0;
    if (letters < 2)
        return;
    LanguageRouter router(m_settings);
    const QString detected = LanguageRouter::detectLanguage(text);
    if (detected.isEmpty() || detected == QStringLiteral("auto"))
        return;
    const QString target = router.resolveTarget(detected);
    if (target.isEmpty() || target == detected)
        return;
    const int fromIndex = m_fromCombo->findData(detected);
    const int toIndex = m_toCombo->findData(target);
    if (fromIndex < 0 || toIndex < 0)
        return;
    if (m_fromCombo->currentIndex() == fromIndex && m_toCombo->currentIndex() == toIndex)
        return;
    const QSignalBlocker blockFrom(m_fromCombo);
    const QSignalBlocker blockTo(m_toCombo);
    m_fromCombo->setCurrentIndex(fromIndex);
    m_toCombo->setCurrentIndex(toIndex);
    if (m_settings) {
        // Settings keep numeric language ids (see settings-default.json).
        m_settings->setValue(QStringLiteral("General.LanguageFrom"), languageId(detected));
        m_settings->setValue(QStringLiteral("General.LanguageTo"), languageId(target));
    }
    setStatusText(QStringLiteral("Направление перевода: %1 → %2")
                      .arg(m_fromCombo->currentText(), m_toCombo->currentText()));
}

void MainWindow::translateSelectionInMain()
{
    if (!m_context || !m_context->clipboard)
        return;
    if (m_lastSelectionTrigger.isValid() && m_lastSelectionTrigger.elapsed() < 700)
        return;
    m_lastSelectionTrigger.start();
    QString text = m_context->clipboard->copySelection();
    if (text.trimmed().isEmpty()) {
        // Double Ctrl without a selection is a shortcut to bring the window up.
        showMainWindow();
        return;
    }
    showMainWindow();
    setSourceText(text);
    applyAutomaticDirection(text);
    onTranslate();
}

void MainWindow::speakSelection()
{
    QString text = m_sourceEdit->textCursor().selectedText();
    if (text.trimmed().isEmpty() && m_context && m_context->clipboard)
        text = m_context->clipboard->copySelection();
    text = text.trimmed();
    if (text.isEmpty())
        return;
    if (m_settings
        && !m_settings->boolValue(QStringLiteral("ServiceProviders.ttsOn"), true)) {
        setStatusText(QStringLiteral("Озвучивание отключено в настройках"));
        return;
    }
    TtsService::speak(m_context, text, sourceLanguageCode(),
                      m_settings
                          && m_settings->boolValue(
                              QStringLiteral("Advanced.EnableSlowerListening"), false));
    setStatusText(QStringLiteral("Озвучивание выделения"));
}

void MainWindow::replaceSelectionWithTranslation()
{
    const QString translation = m_targetEdit->toPlainText().trimmed();
    if (translation.isEmpty() || !m_context || !m_context->clipboard)
        return;
    m_context->clipboard->pasteText(translation);
    setStatusText(QStringLiteral("Перевод вставлен вместо выделения"));
}

void MainWindow::switchTargetLanguage()
{
    if (!m_toCombo || m_toCombo->count() < 2)
        return;
    int index = m_toCombo->currentIndex() + 1;
    if (index >= m_toCombo->count())
        index = 0;
    m_toCombo->setCurrentIndex(index);
    setStatusText(QStringLiteral("Целевой язык: %1").arg(m_toCombo->currentText()));
}

void MainWindow::focusSource()
{
    showMainWindow();
    m_sourceEdit->setFocus();
}

void MainWindow::focusTarget()
{
    showMainWindow();
    m_targetEdit->setFocus();
}

void MainWindow::copyTranslation()
{
    onCopyTranslation();
}

void MainWindow::clearInput()
{
    if (m_instantTimer)
        m_instantTimer->stop();
    if (m_sourceEdit)
        m_sourceEdit->clear();
    setStatusText(QStringLiteral("Поле ввода очищено"));
}

void MainWindow::swapLanguages()
{
    onSwapLanguages();
}

void MainWindow::translateDocument()
{
    openDocument();
}
void MainWindow::focusExtra()
{
    showMainWindow();
    if (m_resultTabs && m_resultTabs->isVisible())
        m_resultTabs->setFocus();
    else
        m_targetEdit->setFocus();
}

void MainWindow::refreshInternalHotkeys()
{
    for (QShortcut* shortcut : std::as_const(m_internalShortcuts))
        shortcut->deleteLater();
    m_internalShortcuts.clear();
    if (!m_settings)
        return;

    struct Definition {
        const char* key;
        std::function<void()> action;
    };
    const Definition definitions[] = {
        {"HotKeyShowMainWindow", [this] { showMainWindow(); }},
        {"HotKeyQuickTranslate", [this] { showPopup(); }},
        {"HotKeyListenText", [this] { speakSelection(); }},
        {"HotKeyTextRecognition", [this] { startOcr(); }},
        {"HotKeyReplaceSelection", [this] { replaceSelectionWithTranslation(); }},
        {"HotKeySwitchTargetLanguage", [this] { switchTargetLanguage(); }},
        {"HotKeyDictionary", [this] { showDictionary(); }},
        {"HotKeyTranslate", [this] { onTranslate(); }},
        {"HotKeyFocusSource", [this] { focusSource(); }},
        {"HotKeyFocusTarget", [this] { focusTarget(); }},
        {"HotKeyFocusExtra", [this] { focusExtra(); }},
        {"HotKeyCopyTranslationText", [this] { copyTranslation(); }},
        {"HotKeyClearInput", [this] { clearInput(); }},
        {"HotKeySwapLanguages", [this] { swapLanguages(); }},
        {"HotKeyOpenSettings", [this] { showOptions(); }},
        {"HotKeyShowHistory", [this] { showHistory(); }},
        {"HotKeyTranslateDocument", [this] { translateDocument(); }}
    };

    for (const Definition& definition : definitions) {
        const int value = m_settings->intValue(
            QStringLiteral("HotKeys.Internal.") + QString::fromLatin1(definition.key),
            HotkeyUtils::defaultInternalHotkey(definition.key));
        if (value <= 0)
            continue;
        const QKeySequence sequence = HotkeyUtils::nativeToSequence(value);
        if (sequence.isEmpty())
            continue;
        auto* shortcut = new QShortcut(sequence, this);
        shortcut->setContext(Qt::WindowShortcut);
        connect(shortcut, &QShortcut::activated, this, [action = definition.action] { action(); });
        m_internalShortcuts.append(shortcut);
    }
}
void MainWindow::openDocument()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Перевести документ"), QString(),
        DocumentReader::filter());
    if (!path.isEmpty())
        translateDocumentFile(path);
}

void MainWindow::translateDocumentFile(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        QMessageBox::warning(this, QStringLiteral("Документ"),
                             QStringLiteral("Файл не найден:\n%1").arg(path));
        return;
    }
    if (!DocumentReader::isSupported(path)) {
        QMessageBox::warning(this, QStringLiteral("Документ"),
                             QStringLiteral("Формат .%1 не поддерживается.\n"
                                            "Поддерживаются DOCX, PPTX, PDF, FB2, EPUB, XLIFF, TS, "
                                            "JSON, XML, Properties, YAML, SRT, VTT, ASS/SSA, CSV, "
                                            "INI, Markdown, TXT и HTML.")
                                 .arg(info.suffix()));
        return;
    }

    QString target = targetLanguageCode();
    if (target.isEmpty() || target == QStringLiteral("auto")) {
        target = m_context && m_context->router
            ? m_context->router->fallbackTarget()
            : QStringLiteral("ru");
    }

    const QString output = info.dir().filePath(
        QStringLiteral("%1.%2.%3").arg(info.completeBaseName(), target, info.suffix()));

    QString providerId;
    // The default connector for file translation is chosen in the settings;
    // fall back to the connector selected in the main window.
    if (m_settings) {
        const QString suffix = info.suffix().toLower();
        QString key = QStringLiteral("ServiceProviders.fileTranslate");
        if (suffix == QStringLiteral("pdf"))
            key = QStringLiteral("ServiceProviders.translatePdf");
        else if (suffix == QStringLiteral("djvu") || suffix == QStringLiteral("djv"))
            key = QStringLiteral("ServiceProviders.translateDjvu");
        QString preferred = m_settings->stringValue(key);
        if (preferred.isEmpty())
            preferred = m_settings->stringValue(QStringLiteral("ServiceProviders.fileTranslate"));
        if (!preferred.isEmpty() && m_context && m_context->plugins) {
            IPluginProvider* candidate = m_context->plugins->provider(preferred);
            if (candidate && candidate->capabilities().contains(QStringLiteral("translate")))
                providerId = preferred;
        }
    }
    if (providerId.isEmpty()) {
        const QStringList selected = checkedServiceIds();
        for (const QString& id : selected) {
            IPluginProvider* provider = m_context && m_context->plugins
                ? m_context->plugins->provider(id)
                : nullptr;
            if (provider && provider->capabilities().contains(QStringLiteral("translate"))) {
                providerId = id;
                break;
            }
        }
    }
    if (providerId.isEmpty() && m_context && m_context->plugins) {
        const QStringList active = m_context->plugins->activeTranslationIds();
        if (!active.isEmpty())
            providerId = active.first();
    }
    if (providerId.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Перевод документа"),
                             QStringLiteral("Не выбран ни один сервис перевода."));
        return;
    }

    auto* progress = new QProgressDialog(
        QStringLiteral("Подготовка документа..."), QStringLiteral("Отмена"), 0, 0, this);
    progress->setWindowTitle(QStringLiteral("Перевод документа"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    progress->show();

    auto* job = new DocumentTranslationJob(m_context, this);
    connect(job, &DocumentTranslationJob::progress, this,
            [progress](int completed, int total, const QString& current) {
        if (!progress)
            return;
        if (progress->maximum() != total)
            progress->setRange(0, total);
        progress->setValue(completed);
        QString label = QStringLiteral("Переведено %1 из %2").arg(completed).arg(total);
        if (!current.isEmpty())
            label += QLatin1Char('\n') + current;
        progress->setLabelText(label);
    });
    connect(job, &DocumentTranslationJob::finished, this,
            [this, progress, job](const QString& savedPath) {
        if (progress) {
            progress->close();
            progress->deleteLater();
        }
        job->deleteLater();
        QMessageBox::information(this, QStringLiteral("Перевод документа"),
                                 QStringLiteral("Перевод сохранён:\n%1").arg(savedPath));
    });
    connect(job, &DocumentTranslationJob::failed, this,
            [this, progress, job](const QString& message) {
        if (progress) {
            progress->close();
            progress->deleteLater();
        }
        job->deleteLater();
        QMessageBox::warning(this, QStringLiteral("Перевод документа"), message);
    });
    connect(progress, &QProgressDialog::canceled, this, [job, progress] {
        job->cancel();
        job->deleteLater();
        progress->close();
        progress->deleteLater();
    });

    job->start(path, output, sourceLanguageCode(), target, providerId);
}

QString MainWindow::imageSessionDir()
{
    if (!m_imageSession.isEmpty())
        return m_imageSession;

    if (!m_context || !m_context->paths)
        return {};

    const QString base = QDir(m_context->paths->dataDir()).filePath(QStringLiteral("Images"));
    m_imageSession = QDir(base).filePath(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")));
    if (!QDir().mkpath(m_imageSession))
        return {};

    // Saved images older than the retention window are removed on the first use of
    // the session folder, so the directory does not grow without a limit.
    int retention = 5;
    if (m_settings)
        retention = m_settings->intValue(QStringLiteral("Ocr.ImageRetentionDays"), 5);
    if (retention <= 0)
        return m_imageSession;

    const QDateTime limit = QDateTime::currentDateTime().addDays(-retention);
    const QDir sessions(base);
    const QStringList names = sessions.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& name : names) {
        const QFileInfo info(sessions.filePath(name));
        if (info.lastModified() >= limit)
            continue;
        QDir(info.absoluteFilePath()).removeRecursively();
    }
    return m_imageSession;
}

void MainWindow::pasteImage()
{
    const QImage image = QApplication::clipboard()->image();
    if (image.isNull() || !m_sourceEdit)
        return;

    const QString dir = imageSessionDir();
    if (dir.isEmpty()) {
        setStatusText(QStringLiteral("Не удалось создать папку для изображений."));
        return;
    }

    const QString id = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString sourcePath = QDir(dir).filePath(id + QStringLiteral("-source.png"));
    if (!image.save(sourcePath, "PNG")) {
        setStatusText(QStringLiteral("Не удалось сохранить изображение."));
        return;
    }
    m_imageFiles.insert(id + QStringLiteral("|source"), sourcePath);

    QString target = targetLanguageCode();
    if (target.isEmpty() || target == QStringLiteral("auto")) {
        target = m_context && m_context->router
            ? m_context->router->fallbackTarget()
            : QStringLiteral("ru");
    }

    QString providerId;
    const QStringList selected = checkedServiceIds();
    for (const QString& serviceId : selected) {
        IPluginProvider* provider = m_context && m_context->plugins
            ? m_context->plugins->provider(serviceId)
            : nullptr;
        if (provider && provider->capabilities().contains(QStringLiteral("translate"))) {
            providerId = serviceId;
            break;
        }
    }
    if (providerId.isEmpty() && m_context && m_context->plugins) {
        const QStringList active = m_context->plugins->activeTranslationIds();
        if (!active.isEmpty())
            providerId = active.first();
    }
    if (providerId.isEmpty()) {
        setStatusText(QStringLiteral("Не выбран сервис перевода для изображения."));
        return;
    }

    setStatusText(QStringLiteral("Распознавание изображения…"));

    auto* pipeline = new ImageTranslationPipeline(m_context, this);
    connect(pipeline, &ImageTranslationPipeline::finished, this,
            [this, pipeline, id, target](const QImage& translated, const QString& sourceText,
                                         const QString& translatedText) {
        const QString folder = imageSessionDir();
        const QString translatedPath = QDir(folder).filePath(
            QStringLiteral("%1-%2.png").arg(id, target));
        if (translated.save(translatedPath, "PNG")) {
            m_imageFiles.insert(id + QStringLiteral("|translate"), translatedPath);
            if (m_settings && m_settings->boolValue(QStringLiteral("Ocr.SaveTranslatedImage"),
                                                    false)) {
                m_savedImages.insert(id);
            }
        }

        const auto appendBlock = [](QTextEdit* edit, const QString& token, const QString& text) {
            QTextCursor cursor(edit->document());
            cursor.movePosition(QTextCursor::End);
            if (!edit->toPlainText().trimmed().isEmpty())
                cursor.insertText(QStringLiteral("\n\n"));
            QString chip = token;
            chip.prepend(QStringLiteral("  "));
            chip.append(QStringLiteral("  "));
            QTextCharFormat format;
            format.setBackground(QColor(219, 234, 254));
            format.setForeground(QColor(30, 64, 124));
            cursor.insertText(chip, format);
            cursor.insertText(QLatin1Char('\n') + text);
        };
        appendBlock(m_sourceEdit, QStringLiteral("⟦image:%1⟧").arg(id), sourceText);
        if (m_targetEdit)
            appendBlock(m_targetEdit, QStringLiteral("⟦translate:%1⟧").arg(id), translatedText);

        setStatusText(QStringLiteral("Изображение переведено."));
        cleanupImages();
        pipeline->deleteLater();
    });
    connect(pipeline, &ImageTranslationPipeline::failed, this,
            [this, pipeline](const QString& message) {
        setStatusText(QStringLiteral("Ошибка распознавания: %1").arg(message));
        pipeline->deleteLater();
    });
    pipeline->start(image, sourceLanguageCode(), target, providerId);
}

void MainWindow::cleanupImages()
{
    if (m_imageFiles.isEmpty())
        return;

    const QString all = (m_sourceEdit ? m_sourceEdit->toPlainText() : QString())
        + (m_targetEdit ? m_targetEdit->toPlainText() : QString());
    if (!all.contains(QStringLiteral("⟦")))
        return;

    static const QRegularExpression token(
        QStringLiteral("[⟦\\[](?:image|translate):([^⟧\\]]+)[⟧\\]]"));
    QSet<QString> referenced;
    auto match = token.globalMatch(all);
    while (match.hasNext())
        referenced.insert(match.next().captured(1));

    for (auto it = m_imageFiles.begin(); it != m_imageFiles.end();) {
        const QString id = it.key().section(QLatin1Char('|'), 0, 0);
        if (!referenced.contains(id) && !m_savedImages.contains(id)) {
            QFile::remove(it.value());
            it = m_imageFiles.erase(it);
        } else {
            ++it;
        }
    }
}

void MainWindow::batchTranslate(const QStringList& files)
{
    QString target = targetLanguageCode();
    if (target.isEmpty() || target == QStringLiteral("auto")) {
        target = m_context && m_context->router
            ? m_context->router->fallbackTarget()
            : QStringLiteral("ru");
    }

    QString providerId;
    // The default connector for file translation is chosen in the settings;
    // fall back to the connector selected in the main window.
    if (m_settings) {
        const QString preferred =
            m_settings->stringValue(QStringLiteral("ServiceProviders.fileTranslate"));
        if (!preferred.isEmpty() && m_context && m_context->plugins) {
            IPluginProvider* candidate = m_context->plugins->provider(preferred);
            if (candidate && candidate->capabilities().contains(QStringLiteral("translate")))
                providerId = preferred;
        }
    }
    if (providerId.isEmpty()) {
        const QStringList selected = checkedServiceIds();
        for (const QString& id : selected) {
            IPluginProvider* provider = m_context && m_context->plugins
                ? m_context->plugins->provider(id)
                : nullptr;
            if (provider && provider->capabilities().contains(QStringLiteral("translate"))) {
                providerId = id;
                break;
            }
        }
    }
    if (providerId.isEmpty() && m_context && m_context->plugins) {
        const QStringList active = m_context->plugins->activeTranslationIds();
        if (!active.isEmpty())
            providerId = active.first();
    }
    if (providerId.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Пакетная обработка"),
                             QStringLiteral("Не выбран ни один сервис перевода."));
        return;
    }

    BatchTranslationDialog dialog(m_context, sourceLanguageCode(), target, providerId, files, this);
    dialog.exec();
}

void MainWindow::onSettingsApplied()
{
    if (m_settings) {
        Localization::load(m_settings->stringValue(QStringLiteral("General.LocaleFoderName"),
                                                  QStringLiteral("Russian")));
        Localization::retranslateAll();
    }
    if (m_context && m_context->hotkeys)
        m_context->hotkeys->reload();
    if (m_context && m_context->plugins)
        m_context->plugins->refreshNetworkConfig();
    refreshLanguages();
    refreshServices();
    if (m_spellAction)
        m_spellAction->setChecked(
            m_settings->boolValue(QStringLiteral("General.SpellChecking"), true));
    if (m_spellHighlighter) {
        m_spellHighlighter->setEnabled(
            m_settings->boolValue(QStringLiteral("General.SpellChecking"), true)
            && m_settings->boolValue(QStringLiteral("ServiceProviders.spellOn"), true));
        m_spellHighlighter->setLanguage(sourceLanguageCode());
        m_spellHighlighter->clearCache();
    }
    if (m_instantAction)
        m_instantAction->setChecked(
            m_settings->boolValue(QStringLiteral("General.InstantTranslation"), true));
    if (m_historyAction)
        m_historyAction->setVisible(
            m_settings->boolValue(QStringLiteral("General.EnableHistory"), false));
    if (m_dictionaryAction)
        m_dictionaryAction->setVisible(
            m_settings->boolValue(QStringLiteral("ServiceProviders.dictOn"), false));
    if (m_callGroup) {
        const bool summOn =
            m_settings->boolValue(QStringLiteral("ServiceProviders.summOn"), false);
        const bool rewrOn =
            m_settings->boolValue(QStringLiteral("ServiceProviders.rewrOn"), false);
        for (QAction* action : m_callGroup->actions()) {
            const QString callType = action->data().toString();
            if (callType == QStringLiteral("summ"))
                action->setEnabled(summOn);
            else if (callType == QStringLiteral("rewr"))
                action->setEnabled(rewrOn);
        }
    }
    const QString call = m_settings->stringValue(QStringLiteral("General.AdditionalCall"),
                                                 QStringLiteral("none"));
    if (m_callGroup) {
        for (QAction* action : m_callGroup->actions()) {
            if (action->data().toString() == call) {
                action->setChecked(true);
                break;
            }
        }
    }
    setInterfacePreset(m_settings->stringValue(QStringLiteral("Layout.InterfacePreset"),
                                               QStringLiteral("classic")));
    applyPanelVisibility();
    applyDensity();
    applyStatusBar();
    applyServicesStyle();
    if (m_capture) {
        m_capture->deleteLater();
        m_capture = nullptr;
    }
    updateWindowFrameState();
    refreshInternalHotkeys();
}

void MainWindow::applyPanelVisibility()
{
    if (!m_settings)
        return;
    const bool showLang = m_settings->boolValue(QStringLiteral("Layout.ShowLangBar"), true);
    const bool showServices = m_settings->boolValue(QStringLiteral("Layout.ShowServicesPanel"), true);
    const bool showHistory = m_settings->boolValue(QStringLiteral("Layout.ShowHistoryNav"), false);
    const bool showStatus = m_settings->boolValue(QStringLiteral("Layout.ShowStatusBar"), true);
    const bool showSideButtons =
        m_settings->boolValue(QStringLiteral("Layout.ShowEditorSideButtons"), false);
    m_languageBar->setVisible(showLang);
    m_servicesRow->setVisible(showServices);
    m_historyBar->setVisible(showHistory);
    if (m_sourceSideButtons)
        m_sourceSideButtons->setVisible(showSideButtons);
    if (m_targetSideButtons)
        m_targetSideButtons->setVisible(showSideButtons);
    if (m_langBarAction)
        m_langBarAction->setChecked(showLang);
    if (m_servicesPanelAction)
        m_servicesPanelAction->setChecked(showServices);
    if (m_historyNavAction)
        m_historyNavAction->setChecked(showHistory);
    if (m_statusBarAction)
        m_statusBarAction->setChecked(showStatus);
    applyStatusBar();
    applyServicesStyle();
    updateBottomRowCorners();
}

void MainWindow::applyDensity()
{
    if (!m_settings)
        return;
    const QString preset = m_settings->stringValue(QStringLiteral("Layout.Preset"),
                                                   QStringLiteral("classic"));
    const bool compact = preset == QStringLiteral("compact");
    const bool minimal = preset == QStringLiteral("minimal");
    const int spacing = minimal ? 4 : (compact ? 6 : 8);
    const int margin = minimal ? 4 : (compact ? 6 : 8);
    if (m_splitter)
        m_splitter->setHandleWidth(minimal ? 4 : (compact ? 6 : 8));
    if (m_languageBar && m_languageBar->layout()) {
        m_languageBar->layout()->setSpacing(spacing);
        m_languageBar->layout()->setContentsMargins(margin, 4, margin, 4);
    }
    if (m_serviceRibbon && m_serviceRibbon->layout())
        m_serviceRibbon->layout()->setSpacing(minimal ? 2 : (compact ? 3 : 4));
}

void MainWindow::onHistoryChanged()
{
    if (!m_historyNavigating)
        m_historyIndex = (m_context && m_context->history && m_context->history->count() > 0) ? 0 : -1;
    refreshHistoryNav();
}

void MainWindow::refreshHistoryNav()
{
    if (!m_context || !m_context->history) {
        m_historyPos->setText(QStringLiteral("— / 0"));
        m_historyPrevious->setEnabled(false);
        m_historyNext->setEnabled(false);
        return;
    }
    const int count = m_context->history->count();
    if (m_historyIndex >= count)
        m_historyIndex = count - 1;
    if (count == 0)
        m_historyIndex = -1;

    m_historyPos->setText(m_historyIndex >= 0
        ? QStringLiteral("%1 / %2").arg(m_historyIndex + 1).arg(count)
        : QStringLiteral("— / 0"));
    m_historyPrevious->setEnabled(m_historyIndex >= 0 && m_historyIndex + 1 < count);
    m_historyNext->setEnabled(m_historyIndex > 0);
}

void MainWindow::onHistoryPrevious()
{
    if (!m_context || !m_context->history)
        return;
    if (m_historyIndex + 1 >= m_context->history->count())
        return;
    m_historyNavigating = true;
    loadHistoryEntry(++m_historyIndex);
    m_historyNavigating = false;
    refreshHistoryNav();
}

void MainWindow::onHistoryNext()
{
    if (m_historyIndex <= 0)
        return;
    m_historyNavigating = true;
    loadHistoryEntry(--m_historyIndex);
    m_historyNavigating = false;
    refreshHistoryNav();
}

void MainWindow::onHistoryActivated(const QJsonObject& entry)
{
    if (entry.isEmpty())
        return;
    const QString timestamp = entry.value(QStringLiteral("timestamp")).toString();
    if (m_context && m_context->history) {
        const QJsonArray entries = m_context->history->entries();
        for (int index = 0; index < entries.size(); ++index) {
            if (entries.at(index).toObject().value(QStringLiteral("timestamp")).toString() == timestamp) {
                m_historyIndex = index;
                break;
            }
        }
    }
    m_historyNavigating = true;
    const QString from = normalizeLanguageCode(entry.value(QStringLiteral("from")).toString());
    const QString to = normalizeLanguageCode(entry.value(QStringLiteral("to")).toString());
    if (const int index = m_fromCombo->findData(from); index >= 0)
        m_fromCombo->setCurrentIndex(index);
    if (const int index = m_toCombo->findData(to); index >= 0)
        m_toCombo->setCurrentIndex(index);
    if (m_instantTimer)
        m_instantTimer->stop();
    {
        const QSignalBlocker blocker(m_sourceEdit);
        m_sourceEdit->setPlainText(entry.value(QStringLiteral("source")).toString());
    }
    m_targetEdit->setPlainText(entry.value(QStringLiteral("translation")).toString());
    clearResults();
    m_historyNavigating = false;
    refreshHistoryNav();
}

void MainWindow::loadHistoryEntry(int index)
{
    if (!m_context || !m_context->history)
        return;
    const QJsonObject entry = m_context->history->entryAt(index);
    if (entry.isEmpty())
        return;
    const QString from = normalizeLanguageCode(entry.value(QStringLiteral("from")).toString());
    const QString to = normalizeLanguageCode(entry.value(QStringLiteral("to")).toString());
    if (const int comboIndex = m_fromCombo->findData(from); comboIndex >= 0)
        m_fromCombo->setCurrentIndex(comboIndex);
    if (const int comboIndex = m_toCombo->findData(to); comboIndex >= 0)
        m_toCombo->setCurrentIndex(comboIndex);
    if (m_instantTimer)
        m_instantTimer->stop();
    {
        const QSignalBlocker blocker(m_sourceEdit);
        m_sourceEdit->setPlainText(entry.value(QStringLiteral("source")).toString());
    }
    m_targetEdit->setPlainText(entry.value(QStringLiteral("translation")).toString());
    clearResults();
}

void MainWindow::saveState()
{
    if (!m_settings)
        return;

    m_settings->setValue(QStringLiteral("General.WindowMainGeometry"),
                         QString::fromLatin1(saveGeometry().toBase64()));
    m_settings->setValue(QStringLiteral("General.WindowMainMaximized"), isMaximized());

    if (m_splitter) {
        const QList<int> sizes = m_splitter->sizes();
        if (!sizes.isEmpty() && sizes.first() > 0) {
            m_settings->setValue(m_sideLayout ? QStringLiteral("Layout.SideSplitterPos")
                                               : QStringLiteral("General.MidSplitterPos"),
                                 sizes.first());
        }
    }
    if (m_fromCombo)
        m_settings->setValue(QStringLiteral("General.LanguageFrom"),
                             m_fromCombo->currentData().toString());
    if (m_toCombo)
        m_settings->setValue(QStringLiteral("General.LanguageTo"),
                             m_toCombo->currentData().toString());
    if (m_spellAction)
        m_settings->setValue(QStringLiteral("General.SpellChecking"), m_spellAction->isChecked());
    if (m_instantAction)
        m_settings->setValue(QStringLiteral("General.InstantTranslation"), m_instantAction->isChecked());
    if (m_callGroup) {
        for (QAction* action : m_callGroup->actions()) {
            if (action->isChecked()) {
                m_settings->setValue(QStringLiteral("General.AdditionalCall"),
                                     action->data().toString());
                break;
            }
        }
    }
    if (m_historyNavAction)
        m_settings->setValue(QStringLiteral("Layout.ShowHistoryNav"), m_historyNavAction->isChecked());
    if (m_langBarAction)
        m_settings->setValue(QStringLiteral("Layout.ShowLangBar"), m_langBarAction->isChecked());
    if (m_servicesPanelAction)
        m_settings->setValue(QStringLiteral("Layout.ShowServicesPanel"),
                             m_servicesPanelAction->isChecked());
    if (m_statusBarAction)
        m_settings->setValue(QStringLiteral("Layout.ShowStatusBar"), m_statusBarAction->isChecked());
    if (m_settings->boolValue(QStringLiteral("Contents.SaveOnExit"), true)) {
        m_settings->setValue(QStringLiteral("Contents.EditSource"), m_sourceEdit->toPlainText());
        m_settings->setValue(QStringLiteral("Contents.EditTranslation"), m_targetEdit->toPlainText());
    }
    if (m_context && m_context->plugins)
        m_context->plugins->setActiveServices(checkedServiceIds());
}

void MainWindow::updateWindowFrameState()
{
    const bool maximized = isMaximized();
    if (m_maxButton) {
        m_maxButton->setText(maximized ? QString::fromUtf8("❐") : QString::fromUtf8("□"));
    }
    refreshFrameStyle();
}

void MainWindow::refreshFrameStyle()
{
    QWidget* frame = centralWidget();
    if (!frame)
        return;
    const bool maximized = isMaximized();
    if (auto* layout = qobject_cast<QVBoxLayout*>(frame->layout())) {
        const int margin = maximized ? 0 : 1;
        layout->setContentsMargins(margin, margin, margin, margin);
    }
    frame->setProperty("maximized", maximized);
    if (m_titleBar)
        m_titleBar->setProperty("maximized", maximized);
    for (QWidget* widget : {frame, m_titleBar}) {
        if (!widget)
            continue;
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    }
    updateBottomRowCorners();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event || !event->mimeData())
        return;
    if (event->mimeData()->hasUrls() || event->mimeData()->hasText()) {
        event->acceptProposedAction();
        setStatusText(QStringLiteral("Отпустите файл или текст для перевода"));
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    if (!event || !event->mimeData())
        return;
    const QMimeData* mime = event->mimeData();
    const bool clear = m_settings
        && m_settings->boolValue(QStringLiteral("General.ClearOnDragDrop"), false);
    if (mime->hasUrls()) {
        for (const QUrl& url : mime->urls()) {
            if (!url.isLocalFile())
                continue;
            const QString path = url.toLocalFile();
            if (!DocumentReader::isSupported(path))
                continue;
            if (clear) {
                m_sourceEdit->clear();
                m_targetEdit->clear();
            }
            event->acceptProposedAction();
            translateDocumentFile(path);
            return;
        }
        setStatusText(QStringLiteral("Формат перетащенного файла не поддерживается"));
        return;
    }
    if (mime->hasText()) {
        if (clear) {
            m_sourceEdit->clear();
            m_targetEdit->clear();
        }
        setSourceText(mime->text());
        event->acceptProposedAction();
        setStatusText(QStringLiteral("Текст вставлен из drag-and-drop"));
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveState();
    if (m_settings)
        m_settings->save();
    if (m_context && m_context->history)
        m_context->history->save();
    if (m_context && m_context->exceptions)
        m_context->exceptions->save();

    const bool closeToTray = m_settings
        ? (m_settings->stringValue(QStringLiteral("Layout.CloseAction"), QStringLiteral("tray"))
               != QStringLiteral("exit"))
        : true;
    if (!m_quitRequested
        && m_settings
        && closeToTray
        && QSystemTrayIcon::isSystemTrayAvailable()) {
        event->ignore();
        hide();
        emit hiddenToTray();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, &MainWindow::applySplitterSizes);
    if (m_startMaximized) {
        m_startMaximized = false;
        QTimer::singleShot(0, this, [this] {
            if (!isMaximized())
                showMaximized();
        });
    }
    if (!m_centerOnFirstShow)
        return;
    m_centerOnFirstShow = false;
    QTimer::singleShot(0, this, [this] {
        if (!isMaximized()) {
            QScreen* current = screen();
            if (!current)
                current = QGuiApplication::primaryScreen();
            if (current) {
                const QRect available = current->availableGeometry();
                move(available.center() - QPoint(width() / 2, height() / 2));
            }
        }
        m_sourceEdit->setFocus(Qt::OtherFocusReason);
    });
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && !isActiveWindow())
        hideServiceTooltip();
    if (event->type() != QEvent::WindowStateChange)
        return;
    updateWindowFrameState();

    // General.MinimizeAction is the current key; the old boolean is kept as a
    // fallback so profiles created before the dropdown keep their behaviour.
    const QString minimizeAction = m_settings
        ? m_settings->stringValue(
              QStringLiteral("General.MinimizeAction"),
              m_settings->boolValue(QStringLiteral("General.MinimizeToTrayOnMinimize"), false)
                  ? QStringLiteral("tray") : QStringLiteral("taskbar"))
        : QStringLiteral("taskbar");

    if (isMinimized()
        && !m_quitRequested
        && minimizeAction == QStringLiteral("tray")
        && QSystemTrayIcon::isSystemTrayAvailable()) {
        QTimer::singleShot(0, this, [this] {
            if (isMinimized()) {
                hide();
                emit hiddenToTray();
            }
        });
    }
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        auto* msg = static_cast<MSG*>(message);
        if (msg->message == WM_GETMINMAXINFO) {
            auto* info = reinterpret_cast<MINMAXINFO*>(msg->lParam);
            MONITORINFO monitorInfo = {};
            monitorInfo.cbSize = sizeof(MONITORINFO);
            const HMONITOR monitor = MonitorFromWindow(reinterpret_cast<HWND>(winId()),
                                                       MONITOR_DEFAULTTONEAREST);
            if (monitor && GetMonitorInfo(monitor, &monitorInfo)) {
                info->ptMaxPosition.x = monitorInfo.rcWork.left - monitorInfo.rcMonitor.left;
                info->ptMaxPosition.y = monitorInfo.rcWork.top - monitorInfo.rcMonitor.top;
                info->ptMaxSize.x = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
                info->ptMaxSize.y = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
                const qreal ratio = devicePixelRatioF();
                info->ptMinTrackSize.x = qRound(minimumWidth() * ratio);
                info->ptMinTrackSize.y = qRound(minimumHeight() * ratio);
                *result = 0;
                return true;
            }
        }
        if (msg->message == WM_NCHITTEST && !isMaximized()) {
            const POINT point = {GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
            RECT windowRect;
            if (GetWindowRect(reinterpret_cast<HWND>(winId()), &windowRect)) {
                const int border = qRound(6.0 * devicePixelRatioF());
                const bool left = point.x >= windowRect.left && point.x < windowRect.left + border;
                const bool right = point.x <= windowRect.right && point.x > windowRect.right - border;
                const bool top = point.y >= windowRect.top && point.y < windowRect.top + border;
                const bool bottom = point.y <= windowRect.bottom && point.y > windowRect.bottom - border;
                if (top && left) *result = HTTOPLEFT;
                else if (top && right) *result = HTTOPRIGHT;
                else if (bottom && left) *result = HTBOTTOMLEFT;
                else if (bottom && right) *result = HTBOTTOMRIGHT;
                else if (left) *result = HTLEFT;
                else if (right) *result = HTRIGHT;
                else if (top) *result = HTTOP;
                else if (bottom) *result = HTBOTTOM;
                else return QMainWindow::nativeEvent(eventType, message, result);
                return true;
            }
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (auto* chip = qobject_cast<QPushButton*>(watched)) {
        if (chip->objectName() == QStringLiteral("ServiceChip")) {
            switch (event->type()) {
            case QEvent::ToolTip:
                // The chip has its own compact tooltip; never show QToolTip.
                return true;
            case QEvent::Enter:
                m_serviceTooltipChip = chip;
                if (m_serviceTooltipTimer)
                    m_serviceTooltipTimer->start();
                break;
            case QEvent::Leave:
                hideServiceTooltip();
                break;
            case QEvent::MouseButtonPress:
                hideServiceTooltip();
                break;
            default:
                break;
            }
        }
    }
    if (watched == m_titleBar) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                onToggleMaximize();
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton && windowHandle()) {
                windowHandle()->startSystemMove();
                return true;
            }
        }
    }
    if (watched == m_sourceEdit || watched == m_targetEdit
        || (m_sourceEdit && watched == m_sourceEdit->viewport())
        || (m_targetEdit && watched == m_targetEdit->viewport())) {
        QTextEdit* edit = nullptr;
        if (m_sourceEdit
            && (watched == m_sourceEdit || watched == m_sourceEdit->viewport())) {
            edit = m_sourceEdit;
        } else if (m_targetEdit
                   && (watched == m_targetEdit || watched == m_targetEdit->viewport())) {
            edit = m_targetEdit;
        }
        if (edit) {
            if (event->type() == QEvent::KeyPress) {
                auto* keyEvent = static_cast<QKeyEvent*>(event);
                const bool paste = (keyEvent->matches(QKeySequence::Paste))
                    || (keyEvent->key() == Qt::Key_Insert && (keyEvent->modifiers() & Qt::ShiftModifier));
                if (paste && watched == m_sourceEdit
                    && !QApplication::clipboard()->image().isNull()) {
                    pasteImage();
                    return true;
                }
            }
            if (event->type() == QEvent::MouseButtonRelease) {
                auto* mouseEvent = static_cast<QMouseEvent*>(event);
                if (mouseEvent->button() == Qt::LeftButton) {
                    const QTextCursor cursor = edit->cursorForPosition(mouseEvent->pos());
                    const QString line = cursor.block().text().trimmed();
                    static const QRegularExpression token(
                        QStringLiteral("^[⟦\\[](image|translate):([^⟧\\]]+)[⟧\\]]$"));
                    const QRegularExpressionMatch match = token.match(line);
                    if (match.hasMatch()) {
                        const QString key = match.captured(2) + QLatin1Char('|')
                            + (match.captured(1) == QStringLiteral("image")
                                   ? QStringLiteral("source")
                                   : QStringLiteral("translate"));
                        if (m_imageFiles.contains(key))
                            QDesktopServices::openUrl(QUrl::fromLocalFile(m_imageFiles.value(key)));
                        return true;
                    }
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::requestQuit()
{
    m_quitRequested = true;
    saveState();
    if (m_settings)
        m_settings->save();
    if (m_context && m_context->history)
        m_context->history->save();
    if (m_context && m_context->exceptions)
        m_context->exceptions->save();
    QCoreApplication::quit();
}
