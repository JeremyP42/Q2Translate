#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QJsonObject>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QStringList>

class AppContext;
class PopupWindow;
class ScreenCaptureWindow;
class SpellCheckHighlighter;
class SettingsManager;
class QAction;
class QActionGroup;
class QButtonGroup;
class QComboBox;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QEvent;
class QHBoxLayout;
class QLabel;
class QMenu;
class QProcess;
class QShortcut;
class QShowEvent;
class QPushButton;
class QScrollArea;
class QSplitter;
class QSpacerItem;
class QTimer;
class QLineEdit;
class QTextBrowser;
class QTabBar;
class QTextEdit;
class QToolButton;
class QVBoxLayout;
class QWidget;
class QResizeEvent;
class QMoveEvent;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(AppContext* context, QWidget* parent = nullptr);
    ~MainWindow() override;

    void setSourceText(const QString& text, bool selectAll = false);
    QString sourceText() const;
    void saveState();

public slots:
    void requestQuit();
    void showMainWindow();
    void showOptions();
    void showAbout();
    void showHistory();
    void showDictionary();
    void startOcr();
    void startVoiceInput();
    void showPopup();
    void showPopupText(const QString& text);
    void applyAutomaticDirection(const QString& text);
    void translateSelectionInMain();
    void translateCurrent();
    void speakSelection();
    void replaceSelectionWithTranslation();
    void switchTargetLanguage();
    void focusSource();
    void focusTarget();
    void focusExtra();
    void copyTranslation();
    void clearInput();
    void swapLanguages();
    void translateDocument();
    void translateDocumentFile(const QString& path);
    void batchTranslate(const QStringList& files = QStringList());
    void pasteImage();
    void cleanupImages();
    QString imageSessionDir();
    void translateText(const QString& text);

signals:
    void hiddenToTray();

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onTranslate();
    void onSwapLanguages();
    void onClearAll();
    void onToggleMaximize();
    void onPinToggled(bool checked);
    void onCopySource();
    void onCopyTranslation();
    void showMainMenu();
    void onTranslationStarted(const QString& requestId);
    void onServiceResult(const QString& requestId, const QString& serviceId,
                         const QJsonObject& result);
    void onTranslationFinished(const QString& requestId);
    void maybeAutoSwitchService();
    void restoreBalancerService();
    void onCapabilityResult(const QString& capability, const QString& serviceId,
                            const QJsonObject& result);
    void onSettingsApplied();
    void onHistoryChanged();
    void onHistoryPrevious();
    void onHistoryNext();
    void onHistoryActivated(const QJsonObject& entry);
    void onTextRecognized(const QString& text, bool translate);
    void onInstantTextChanged();
    void setInterfacePreset(const QString& preset);

private:
    void buildUi();
    void buildMainMenu();    QWidget* buildTitleBar();
    QWidget* buildSourceRow();
    QWidget* buildTargetRow();
    QWidget* buildLanguageBar();
    QWidget* buildServicesRow();
    QWidget* buildHistoryBar();
    QWidget* buildStatusBar();
    QWidget* buildEditorSideButtons(bool forSource);
    void loadLanguages();
    void loadState();
    void refreshServices();
    void applyServicesStyle();
    void updateServiceScrollHeight();
    void refreshServiceActions();
    void syncResultTabsForSelection();
    void refreshInternalHotkeys();
    void updateCompactMode();
    void refreshLanguages();
    void refreshHistoryNav();
    void loadHistoryEntry(int index);
    void applyPanelVisibility();
    void applyDensity();
    void applyStatusBar();
    void updateBottomRowCorners();
    void showServiceTooltip();
    void hideServiceTooltip();
    void showSourceContextMenu(const QPoint& position);
    void rebuildWorkspace();
    void applySplitterSizes();
    void scheduleStateSave();
    void setStatusText(const QString& text);
    void handleAdditionalOutput();
    void updateWindowFrameState();
    void refreshFrameStyle();
    void clearResults();
    void showResultForService(const QString& serviceId);
    QStringList checkedServiceIds() const;
    QString sourceLanguageCode() const;
    QString targetLanguageCode() const;
    void openDocument();

    AppContext* m_context = nullptr;
    SettingsManager* m_settings = nullptr;

    QTextEdit* m_sourceEdit = nullptr;
    QTextEdit* m_targetEdit = nullptr;
    // Translated images live in Data/Images/<session>; the tokens in the editors
    // reference them by id, and files without a token are removed.
    QHash<QString, QString> m_imageFiles;
    QSet<QString> m_savedImages;
    QStringList m_pendingImageTokens;
    QString m_imageTokenPrefix;
    QString m_imageSession;
    QComboBox* m_fromCombo = nullptr;
    QComboBox* m_toCombo = nullptr;
    QToolButton* m_clearButton = nullptr;
    QLabel* m_titleLabel = nullptr;
    QHBoxLayout* m_languageBarLayout = nullptr;
    QSpacerItem* m_languageBarSpacer = nullptr;
    QToolButton* m_swapButton = nullptr;
    QSplitter* m_splitter = nullptr;
    QWidget* m_titleBar = nullptr;
    QWidget* m_languageBar = nullptr;
    QWidget* m_servicesRow = nullptr;
    QWidget* m_serviceRibbon = nullptr;
    QScrollArea* m_serviceScroll = nullptr;
    QPushButton* m_serviceBalancerButton = nullptr;
    QWidget* m_historyBar = nullptr;
    QToolButton* m_pinButton = nullptr;
    QToolButton* m_maxButton = nullptr;
    QToolButton* m_gearButton = nullptr;
    QPushButton* m_translateButton = nullptr;
    QTabBar* m_resultTabs = nullptr;
    QPushButton* m_serviceDropdown = nullptr;
    QMenu* m_serviceMenu = nullptr;
    QLabel* m_historyPos = nullptr;
    QPushButton* m_historyPrevious = nullptr;
    QPushButton* m_historyNext = nullptr;
    QVBoxLayout* m_bodyLayout = nullptr;
    QWidget* m_workspace = nullptr;
    QWidget* m_sourceRow = nullptr;
    QWidget* m_targetRow = nullptr;
    QWidget* m_lower = nullptr;
    QWidget* m_sourceSideButtons = nullptr;
    QWidget* m_targetSideButtons = nullptr;
    QProcess* m_voiceProcess = nullptr;
    QWidget* m_statusRow = nullptr;
    QLabel* m_statusLabel = nullptr;    QLabel* m_languageIndicator = nullptr;
    QTimer* m_instantTimer = nullptr;
    QWidget* m_serviceTooltip = nullptr;
    QTimer* m_serviceTooltipTimer = nullptr;
    QPointer<QPushButton> m_serviceTooltipChip;
    QAction* m_statusBarAction = nullptr;
    QActionGroup* m_presetGroup = nullptr;
    SpellCheckHighlighter* m_spellHighlighter = nullptr;

    QMenu* m_mainMenu = nullptr;
    QAction* m_spellAction = nullptr;
    QAction* m_instantAction = nullptr;
    QAction* m_historyAction = nullptr;
    QAction* m_dictionaryAction = nullptr;
    QAction* m_historyNavAction = nullptr;
    QAction* m_langBarAction = nullptr;
    QAction* m_servicesPanelAction = nullptr;
    QActionGroup* m_callGroup = nullptr;
    QButtonGroup* m_serviceGroup = nullptr;

    QHash<QString, QPushButton*> m_serviceButtons;
    QStringList m_serviceButtonOrder;
    QHash<QString, QAction*> m_serviceActions;
    QHash<QString, QString> m_resultTexts;
    QHash<QString, QString> m_resultErrors;
    QHash<QString, QString> m_serviceNames;

    PopupWindow* m_popup = nullptr;
    ScreenCaptureWindow* m_capture = nullptr;
    QString m_currentRequestId;
    QString m_currentResultService;
    QString m_balancerOriginalService;
    bool m_autoSwitchRetried = false;
    bool m_autoSwitchPending = false;
    class QTimer* m_balancerReturnTimer = nullptr;
    bool m_translating = false;
    bool m_waitingForRequest = false;
    bool m_additionalCallDone = false;
    bool m_quitRequested = false;
    int m_historyIndex = -1;
    bool m_historyNavigating = false;
    bool m_sideLayout = false;
    bool m_loadingInterface = false;
    bool m_centerOnFirstShow = false;
    bool m_startMaximized = false;
    QTimer* m_stateSaveTimer = nullptr;
    QElapsedTimer m_lastSelectionTrigger;
    QList<QShortcut*> m_internalShortcuts;
};
