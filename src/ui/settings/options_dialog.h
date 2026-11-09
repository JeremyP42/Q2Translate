#pragma once

#include <QDialog>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QVariant>
#include <QPair>

#include "core/language_catalog.h"

class AppContext;
class IPluginProvider;
struct PluginManifest;
class PopupWindow;
class QButtonGroup;
class QNetworkAccessManager;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class MultiSelectComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QGridLayout;
class QPushButton;
class QProgressBar;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTableWidgetItem;
class QTextBrowser;
class QTimer;
class QVBoxLayout;
class QHBoxLayout;
class QKeySequenceEdit;
class QScrollArea;
class QWidget;

class OptionsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit OptionsDialog(AppContext* context, QWidget* parent = nullptr);
    ~OptionsDialog() override;

    void reject() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void settingsApplied();
    void popupPreviewOpenMainRequested(const QString& text);

private:
    struct Field {
        QWidget* widget = nullptr;
        QString key;
        int kind = 0;
        int scale = 1;
    };

    void buildUi();
    QVBoxLayout* addPage(const QString& title, const QString& id, bool sub = false,
                         bool scrollable = true);
    QCheckBox* addCheck(QVBoxLayout* layout, const QString& key, const QString& label,
                        const QString& hint = QString());
    QComboBox* addCombo(QVBoxLayout* layout, const QString& key, const QString& label,
                        const QList<QPair<QString, QVariant>>& options,
                        const QString& hint = QString(), int columns = 0);
    QSpinBox* addNumber(QVBoxLayout* layout, const QString& key, const QString& label,
                        int minimum, int maximum, int scale = 1,
                        const QString& suffix = QString(), const QString& hint = QString());
    QDoubleSpinBox* addDouble(QVBoxLayout* layout, const QString& key, const QString& label,
                              double minimum, double maximum, double step,
                              const QString& suffix = QString(),
                              const QString& hint = QString());
    QLineEdit* addLine(QVBoxLayout* layout, const QString& key, const QString& label,
                       bool password = false, const QString& hint = QString());
    QLabel* addHint(QVBoxLayout* layout, const QString& text);
    QPair<QVBoxLayout*, QVBoxLayout*> addTwoColumns(QVBoxLayout* layout, int spacing = 12);
    QWidget* fieldWidget(const QString& key, int kind = -1) const;

    void buildGeneral(QVBoxLayout* layout);
    void buildAppearance(QVBoxLayout* layout);
    void buildServices(QVBoxLayout* layout);
    void buildBehavior(QVBoxLayout* layout);
    void buildLanguages(QVBoxLayout* layout);
    void buildLayout(QVBoxLayout* layout);
    void buildPopups(QVBoxLayout* layout);
    void buildHotkeys(QVBoxLayout* layout);
    void buildPlugins(QVBoxLayout* layout);
    void showConnectorDetails(const QString& id);
    void updateConnectorCard(const PluginManifest& manifest);
    void rebuildConnectorForm(IPluginProvider* provider);
    QWidget* createConnectorField(const QJsonObject& field, const QJsonObject& values);
    void commitConnectorSettings();
    void applyConnectorSettings();
    void applyBalancerPage();
    void refreshConnectorModels(const QJsonObject& field, QComboBox* combo,
                                QPushButton* button, bool notify);
    void buildNetwork(QVBoxLayout* layout);
    void buildExceptions(QVBoxLayout* layout);
    void buildAdvanced(QVBoxLayout* layout);
    void buildUpdates(QVBoxLayout* layout);

    void loadAll();
    void loadGeneric();
    void applyAll();
    void applyLiveSettings();
    void scheduleLiveApply();
    void connectLiveApply();
    void applyAppearanceSettings();
    QString appearanceKey() const;
    bool hasPendingChanges() const;
    void restoreInitialSettings();
    void applyGeneric();
    void applyLanguages();
    void applyServices();
    void applyHotkeys();
    void applyStartup();
    void resetDefaults();
    void updateDependencies();
    void refreshServiceTables();
    void refreshOrderTable();
    void fitTableHeight(QTableWidget* table);
    void refreshLanguageLists();
    void rebuildLanguageCombos();
    QList<LanguageInfo> selectedFavoriteLanguages() const;
    void refreshExceptions();
    void refreshThemeSchedule();
    void applyThemeSchedule();
    void addThemeScheduleEntry();
    void addThemeScheduleRow(const QJsonObject& entry);
    bool promptThemeScheduleEntry(QJsonObject& entry);
    void showPopupPreview();
    void refreshOcrLanguages();
    void browseOcrLanguages();
    void browseSpellLanguages();
    void refreshSpellProviders();
    void refreshServiceCombos();
    void loadPopupStyleFontSizes();
    void applyPopupStyleFontSizes();
    void refreshHotkeyPlaceholders();
    bool validateHotkeys() const;
    void captureUserHotkeys();
    void applyUserHotkeys();
    void captureHotkeySnapshot();    bool hotkeysMatchSnapshot() const;
    bool internalHotkeysMatchPreset(const QString& preset) const;
    bool hotkeysMatchDefaults() const;
    void updateSpellControls();
    void applyDensity();
    void testNetwork();
    QWidget* createGroup(QVBoxLayout* page, const QString& title, QVBoxLayout** groupLayout);

    AppContext* m_context = nullptr;
    QStackedWidget* m_pages = nullptr;
    QVBoxLayout* m_navLayout = nullptr;
    QButtonGroup* m_navGroup = nullptr;
    QList<Field> m_fields;

    QComboBox* m_fromCombo = nullptr;
    QComboBox* m_toCombo = nullptr;
    QListWidget* m_rulesList = nullptr;
    QList<QPair<QCheckBox*, QString>> m_balancerFallbackChecks;
    QSpinBox* m_connectorTimeoutSpin = nullptr;
    QList<QPair<QSpinBox*, QString>> m_balancerTimeoutSpins;
    QComboBox* m_ruleFrom = nullptr;
    QComboBox* m_ruleTo = nullptr;
    QHash<int, QCheckBox*> m_pinnedChecks;

    QStringList m_serviceOrder;
    QTableWidget* m_orderTable = nullptr;
    QComboBox* m_ocrProvider = nullptr;
    MultiSelectComboBox* m_ocrLanguage = nullptr;
    QLineEdit* m_saveImagePath = nullptr;
    QPushButton* m_ocrLanguagesButton = nullptr;
    QCheckBox* m_spellCheck = nullptr;
    QComboBox* m_spellProvider = nullptr;
    QPushButton* m_spellLanguagesButton = nullptr;
    QSpinBox* m_popupSourceFontSize = nullptr;
    QSpinBox* m_popupTargetFontSize = nullptr;

    QTableWidget* m_hotkeyTable = nullptr;
    QTableWidget* m_scheduleTable = nullptr;
    QHash<QString, QKeySequenceEdit*> m_hotkeyEdits;
    QHash<QString, QKeySequenceEdit*> m_internalHotkeyEdits;
    QHash<QString, QKeySequence> m_hotkeySnapshotInternal;
    QHash<QString, QKeySequence> m_hotkeySnapshotGlobal;
    QComboBox* m_hotkeyPreset = nullptr;
    QKeySequenceEdit* m_lastHotkeyEditor = nullptr;
    QJsonObject m_userHotkeysInternal;
    QJsonObject m_userHotkeysGlobal;
    QString m_activeHotkeyPreset = QStringLiteral("user");

    QLabel* m_pluginDetails = nullptr;
    QWidget* m_connectorFormHost = nullptr;
    QScrollArea* m_connectorScroll = nullptr;
    QHash<QString, QWidget*> m_connectorFields;
    QHash<QString, QJsonObject> m_connectorFieldDefs;
    QHash<QString, QComboBox*> m_connectorModelCombos;
    QHash<QString, QPushButton*> m_connectorModelButtons;
    QHash<QString, QJsonObject> m_connectorSettingsCache;
    QString m_connectorProviderId;
    QNetworkAccessManager* m_connectorNetwork = nullptr;
    PopupWindow* m_popupPreview = nullptr;
    QJsonObject m_initialSettings;
    QHash<QString, QJsonObject> m_initialProviderSettings;
    QTimer* m_liveApplyTimer = nullptr;
    bool m_liveApplyEnabled = false;
    bool m_restoringSettings = false;
    QString m_appliedAppearanceKey;

    QRadioButton* m_exceptionsModeOff = nullptr;
    QRadioButton* m_exceptionsModeOn = nullptr;
    QTableWidget* m_exceptionsTable = nullptr;
    QLineEdit* m_exceptionProcess = nullptr;
    QLineEdit* m_exceptionClass = nullptr;

    QLabel* m_networkStatus = nullptr;

    QPushButton* m_applyButton = nullptr;
    bool m_loadingHotkeys = false;
};
