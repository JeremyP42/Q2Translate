#include "options_dialog.h"

#include "core/app_context.h"
#include "core/exceptions_manager.h"
#include "core/history_manager.h"
#include "core/language_catalog.h"
#include "core/language_router.h"
#include "core/localization.h"
#include "core/path_manager.h"
#include "core/settings_manager.h"
#include "core/translation_manager.h"
#include "platform/win/global_hotkey_manager.h"
#include "platform/win/window_utils.h"
#include "services/plugin_manager.h"
#include "services/provider_interface.h"
#include "services/spell_provider.h"
#include "services/tts_service.h"
#include "tesseract_language_dialog.h"
#include "spell_language_dialog.h"
#include "ui/icon_factory.h"
#include "ui/theme_manager.h"
#include "ui/theme_schedule.h"
#include "ui/down_combo_box.h"
#include "ui/multi_select_combo_box.h"
#include "ui/popup_window.h"
#include "ui/popup_style.h"
#include "ui/font_tuning.h"
#include "plugin_settings_dialog.h"

#include <QAbstractItemModel>
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QApplication>
#include <QPalette>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QDialogButtonBox>
#include <QFile>
#include <QRegularExpression>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QFont>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeySequenceEdit>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QDoubleSpinBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QTextStream>
#include <QTime>
#include <QTimeEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QSplitter>
#include <QVBoxLayout>
#include <utility>
#include <algorithm>
#include <memory>

namespace {

QString engineComponentRelative(const QString& providerId)
{
    if (providerId == QStringLiteral("tesseract") || providerId == QStringLiteral("tesseract_local"))
        return QStringLiteral("ocr/tesseract");
    if (providerId == QStringLiteral("paddleocr"))
        return QStringLiteral("ocr/paddleocr");
    if (providerId == QStringLiteral("nuspell"))
        return QStringLiteral("spelling/nuspell");
    return QString();
}

QString componentVersionAt(const QString& directory)
{
    QFile file(QDir(directory).filePath(QStringLiteral("manifest.json")));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    const QJsonObject manifest = QJsonDocument::fromJson(file.readAll()).object();
    return manifest.value(QStringLiteral("version")).toString();
}

bool copyComponentTree(const QString& source, const QString& target, QString* error)
{
    QDir sourceDir(source);
    if (!sourceDir.exists()) {
        if (error)
            *error = QStringLiteral("Встроенный пакет не найден: %1").arg(source);
        return false;
    }
    if (!QDir().mkpath(target)) {
        if (error)
            *error = QStringLiteral("Не удалось создать каталог: %1").arg(target);
        return false;
    }
    QDirIterator iterator(source, QDir::Files | QDir::Hidden,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QString relative = sourceDir.relativeFilePath(path);
        const QString destination = QDir(target).filePath(relative);
        QDir().mkpath(QFileInfo(destination).absolutePath());
        if (QFile::exists(destination) && !QFile::remove(destination)) {
            if (error)
                *error = QStringLiteral("Не удалось заменить файл: %1").arg(destination);
            return false;
        }
        if (!QFile::copy(path, destination)) {
            if (error)
                *error = QStringLiteral("Не удалось скопировать файл: %1").arg(relative);
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    return true;
}

struct ComboOption {
    QString label;
    QVariant value;
};

struct PinnedLanguage {
    int id;
    const char* label;
};

const PinnedLanguage kPinned[] = {
    {17, "English (en)"}, {46, "Russian (ru)"}, {11, "Chinese (zh-Hans)"},
    {12, "Chinese (zh-Hant)"}, {27, "Hindi (hi)"}, {50, "Spanish (es)"},
    {21, "French (fr)"}, {5, "Arabic (ar)"}, {74, "Bangla (bn)"}
};

struct HotkeyDefinition {
    const char* key;
    const char* label;
    int internalDefault;
    int globalDefault;
};

const HotkeyDefinition kHotkeys[] = {
    {"HotKeyShowMainWindow", "Показать главное окно", 0, 0},
    {"HotKeyQuickTranslate", "Быстрый перевод", 0, 593},
    {"HotKeyListenText", "Прослушать выделение", 0, 0},
    {"HotKeyTextRecognition", "OCR с экрана", 3153, 3153},
    {"HotKeyReplaceSelection", "Заменить выделение переводом", 0, 0},
    {"HotKeySwitchTargetLanguage", "Переключить целевой язык", 0, 0},
    {"HotKeyDictionary", "Показать словарь", 0, 0},
    {"HotKeyTranslate", "Перевести", 0, 0},
    {"HotKeyFocusSource", "Фокус на панели ввода", 0, 0},
    {"HotKeyFocusTarget", "Фокус на панели вывода", 0, 0},
    {"HotKeyFocusExtra", "Фокус на дополнительной панели вывода", 0, 0},
    {"HotKeyCopyTranslationText", "Копировать перевод", 0, 0},
    {"HotKeyClearInput", "Очистить поле ввода", 0, 0},
    {"HotKeySwapLanguages", "Поменять языки местами", 0, 0},
    {"HotKeyOpenSettings", "Открыть настройки", 0, 0},
    {"HotKeyShowHistory", "История переводов", 0, 0},
    {"HotKeyTranslateDocument", "Перевести документ", 0, 0}
};

struct PresetHotkey {
    const char* key;
    int internal;
};

// (modifiers << 8) | virtualKey; MOD_CONTROL = 2, MOD_SHIFT = 4, MOD_ALT = 1.
const PresetHotkey kClassicHotkeys[] = {
    {"HotKeyQuickTranslate", 593},       // Ctrl+Q
    {"HotKeyListenText", 581},           // Ctrl+E
    {"HotKeyTextRecognition", 585},      // Ctrl+I
    {"HotKeyReplaceSelection", 1620},    // Ctrl+Shift+T
    {"HotKeySwitchTargetLanguage", 588}, // Ctrl+L
    {"HotKeyDictionary", 580},           // Ctrl+D
    {"HotKeyTranslate", 525},            // Ctrl+Enter
    {"HotKeyFocusSource", 305},          // Alt+1
    {"HotKeyFocusTarget", 306},          // Alt+2
    {"HotKeyFocusExtra", 307}            // Alt+3
};

const PresetHotkey kModernHotkeys[] = {
    {"HotKeyQuickTranslate", 1617},      // Ctrl+Shift+Q
    {"HotKeyListenText", 1612},          // Ctrl+Shift+L
    {"HotKeyTextRecognition", 1615},     // Ctrl+Shift+O
    {"HotKeyReplaceSelection", 1618},    // Ctrl+Shift+R
    {"HotKeySwitchTargetLanguage", 588}, // Ctrl+L
    {"HotKeyDictionary", 1604},          // Ctrl+Shift+D
    {"HotKeyTranslate", 525},            // Ctrl+Enter
    {"HotKeyFocusSource", 305},          // Alt+1
    {"HotKeyFocusTarget", 306},          // Alt+2
    {"HotKeyFocusExtra", 307}            // Alt+3
};
QList<QPair<QString, QVariant>> optionsFrom(const QList<ComboOption>& options)
{
    QList<QPair<QString, QVariant>> result;
    for (const ComboOption& option : options)
        result.append(qMakePair(option.label, option.value));
    return result;
}

QList<LanguageInfo> sortedLanguageList()
{
    QList<LanguageInfo> result = allLanguages();
    std::sort(result.begin(), result.end(), [](const LanguageInfo& left, const LanguageInfo& right) {
        return QString::localeAwareCompare(QString::fromUtf8(left.name),
                                           QString::fromUtf8(right.name)) < 0;
    });
    return result;
}


int qtKeyToVk(Qt::Key key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return 0x41 + (key - Qt::Key_A);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return 0x30 + (key - Qt::Key_0);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return 0x70 + (key - Qt::Key_F1);

    switch (key) {
    case Qt::Key_Backspace: return 0x08;
    case Qt::Key_Tab: return 0x09;
    case Qt::Key_Return:
    case Qt::Key_Enter: return 0x0D;
    case Qt::Key_Escape: return 0x1B;
    case Qt::Key_Space: return 0x20;
    case Qt::Key_PageUp: return 0x21;
    case Qt::Key_PageDown: return 0x22;
    case Qt::Key_End: return 0x23;
    case Qt::Key_Home: return 0x24;
    case Qt::Key_Left: return 0x25;
    case Qt::Key_Up: return 0x26;
    case Qt::Key_Right: return 0x27;
    case Qt::Key_Down: return 0x28;
    case Qt::Key_Insert: return 0x2D;
    case Qt::Key_Delete: return 0x2E;
    case Qt::Key_Pause: return 0x13;
    case Qt::Key_Print: return 0x2C;
    case Qt::Key_ScrollLock: return 0x91;
    case Qt::Key_CapsLock: return 0x14;
    case Qt::Key_NumLock: return 0x90;
    case Qt::Key_Plus: return 0xBB;
    case Qt::Key_Minus: return 0xBD;
    case Qt::Key_Comma: return 0xBC;
    case Qt::Key_Period: return 0xBE;
    case Qt::Key_Slash: return 0xBF;
    case Qt::Key_Backslash: return 0xDC;
    case Qt::Key_Semicolon: return 0xBA;
    case Qt::Key_Apostrophe: return 0xDE;
    case Qt::Key_BracketLeft: return 0xDB;
    case Qt::Key_BracketRight: return 0xDD;
    case Qt::Key_QuoteLeft: return 0xC0;
    case Qt::Key_Equal: return 0xBB;
    default:
        if (int(key) > 0 && int(key) < 256)
            return int(key);
        return 0;
    }
}

int sequenceToNative(const QKeySequence& sequence)
{
    if (sequence.isEmpty())
        return 0;
    const QKeyCombination combination = sequence[0];
    const int vk = qtKeyToVk(combination.key());
    if (vk == 0)
        return 0;
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    int mods = 0;
    if (modifiers & Qt::AltModifier)
        mods |= 1;
    if (modifiers & Qt::ControlModifier)
        mods |= 2;
    if (modifiers & Qt::ShiftModifier)
        mods |= 4;
    if (modifiers & Qt::MetaModifier)
        mods |= 8;
    return (mods << 8) | vk;
}

Qt::Key vkToQtKey(int vk)
{
    if (vk >= 0x41 && vk <= 0x5A)
        return Qt::Key(int(Qt::Key_A) + (vk - 0x41));
    if (vk >= 0x30 && vk <= 0x39)
        return Qt::Key(int(Qt::Key_0) + (vk - 0x30));
    if (vk >= 0x70 && vk <= 0x87)
        return Qt::Key(int(Qt::Key_F1) + (vk - 0x70));

    switch (vk) {
    case 0x08: return Qt::Key_Backspace;
    case 0x09: return Qt::Key_Tab;
    case 0x0D: return Qt::Key_Return;
    case 0x1B: return Qt::Key_Escape;
    case 0x20: return Qt::Key_Space;
    case 0x21: return Qt::Key_PageUp;
    case 0x22: return Qt::Key_PageDown;
    case 0x23: return Qt::Key_End;
    case 0x24: return Qt::Key_Home;
    case 0x25: return Qt::Key_Left;
    case 0x26: return Qt::Key_Up;
    case 0x27: return Qt::Key_Right;
    case 0x28: return Qt::Key_Down;
    case 0x2D: return Qt::Key_Insert;
    case 0x2E: return Qt::Key_Delete;
    case 0x13: return Qt::Key_Pause;
    case 0x2C: return Qt::Key_Print;
    case 0x91: return Qt::Key_ScrollLock;
    case 0x14: return Qt::Key_CapsLock;
    case 0x90: return Qt::Key_NumLock;
    case 0xBB: return Qt::Key_Plus;
    case 0xBD: return Qt::Key_Minus;
    case 0xBC: return Qt::Key_Comma;
    case 0xBE: return Qt::Key_Period;
    case 0xBF: return Qt::Key_Slash;
    case 0xDC: return Qt::Key_Backslash;
    case 0xBA: return Qt::Key_Semicolon;
    case 0xDE: return Qt::Key_Apostrophe;
    case 0xDB: return Qt::Key_BracketLeft;
    case 0xDD: return Qt::Key_BracketRight;
    case 0xC0: return Qt::Key_QuoteLeft;
    default: return Qt::Key_unknown;
    }
}

QKeySequence nativeToSequence(int value)
{
    if (value <= 0)
        return QKeySequence();
    const int vk = value & 0xFF;
    const int mods = (value >> 8) & 0xFF;
    const Qt::Key key = vkToQtKey(vk);
    if (key == Qt::Key_unknown)
        return QKeySequence();
    Qt::KeyboardModifiers modifiers;
    if (mods & 1)
        modifiers |= Qt::AltModifier;
    if (mods & 2)
        modifiers |= Qt::ControlModifier;
    if (mods & 4)
        modifiers |= Qt::ShiftModifier;
    if (mods & 8)
        modifiers |= Qt::MetaModifier;
    return QKeySequence(QKeyCombination(modifiers, key));
}

QString languageNameById(int id)
{
    return languageName(id);
}

QString languageSettingValue(SettingsManager* settings, const QString& key,
                             const QString& fallback)
{
    const QJsonValue value = settings->value(key);
    if (value.isDouble())
        return languageCode(value.toInt());
    const QString normalized = normalizeLanguageCode(value.toString());
    return normalized.isEmpty() ? fallback : normalized;
}

QString connectorLink(const PluginManifest& manifest)
{
    const QJsonValue explicitLink = manifest.raw.value(QStringLiteral("link"));
    if (explicitLink.isString()) {
        const QString value = explicitLink.toString().trimmed();
        if (!value.isEmpty())
            return value;
    }

    QFile file(manifest.serviceScript);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("—");
    const QString text = QString::fromUtf8(file.readAll());
    static const QRegularExpression pattern(
        QStringLiteral("https?://[A-Za-z0-9\\.\\-]+(?::\\d+)?(?:/[A-Za-z0-9\\.\\-_/]*)?"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (match.hasMatch())
        return match.captured(0);
    return QStringLiteral("—");
}

}

OptionsDialog::OptionsDialog(AppContext* context, QWidget* parent)
    : QDialog(parent)
    , m_context(context)
{
    setWindowTitle(QStringLiteral("Настройки"));
    resize(1100, 700);
    setMinimumSize(900, 560);
    m_initialSettings = m_context && m_context->settings
        ? m_context->settings->root() : QJsonObject();
    if (m_context && m_context->settings)
        m_context->settings->setAutosaveEnabled(false);
    if (m_context && m_context->plugins) {
        const QList<IPluginProvider*> providers = m_context->plugins->providers();
        for (IPluginProvider* provider : providers) {
            if (provider)
                m_initialProviderSettings.insert(provider->id(), provider->settings());
        }
    }
    m_connectorNetwork = new QNetworkAccessManager(this);
    buildUi();
    m_appliedAppearanceKey = appearanceKey();

    m_liveApplyTimer = new QTimer(this);
    m_liveApplyTimer->setSingleShot(true);
    m_liveApplyTimer->setInterval(350);
    connect(m_liveApplyTimer, &QTimer::timeout, this, &OptionsDialog::applyLiveSettings);
    connectLiveApply();
    m_liveApplyEnabled = true;
}

OptionsDialog::~OptionsDialog()
{
    if (m_context && m_context->settings)
        m_context->settings->setAutosaveEnabled(true);
}

void OptionsDialog::buildUi()
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* sidebar = new QScrollArea(this);
    sidebar->setObjectName(QStringLiteral("OptionsSidebar"));
    sidebar->setWidgetResizable(true);
    sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidebar->setFixedWidth(230);
    sidebar->setFrameShape(QFrame::NoFrame);
    auto* sidebarContent = new QWidget(sidebar);
    sidebarContent->setObjectName(QStringLiteral("OptionsSidebarContent"));
    m_navLayout = new QVBoxLayout(sidebarContent);
    m_navLayout->setContentsMargins(8, 6, 8, 6);
    m_navLayout->setSpacing(0);
    sidebar->setWidget(sidebarContent);
    root->addWidget(sidebar);

    auto* right = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    m_pages = new QStackedWidget(right);
    rightLayout->addWidget(m_pages, 1);

    m_navGroup = new QButtonGroup(this);
    m_navGroup->setExclusive(true);
    connect(m_navGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_pages->setCurrentIndex(id);
    });

    ThemeManager::reload();

    buildGeneral(addPage(QStringLiteral("Общие"), QStringLiteral("general")));
    buildAppearance(addPage(QStringLiteral("Внешний вид"), QStringLiteral("appearance")));
    buildLayout(addPage(QStringLiteral("Расположение"), QStringLiteral("layout"), true));
    buildPopups(addPage(QStringLiteral("Всплывающее окно"), QStringLiteral("popups"), true));
    buildServices(addPage(QStringLiteral("Сервисы"), QStringLiteral("svcpre"), true));
    buildBehavior(addPage(QStringLiteral("Поведение"), QStringLiteral("behavior"), true));
    buildLanguages(addPage(QStringLiteral("Языки"), QStringLiteral("languages"), true));
    buildHotkeys(addPage(QStringLiteral("Горячие клавиши"), QStringLiteral("hotkeys"), true));
    buildPlugins(addPage(QStringLiteral("Коннекторы"), QStringLiteral("plugins"), false, false));
    buildNetwork(addPage(QStringLiteral("Сеть"), QStringLiteral("network")));
    buildExceptions(addPage(QStringLiteral("Исключения"), QStringLiteral("exceptions"), true));
    buildAdvanced(addPage(QStringLiteral("Дополнительно"), QStringLiteral("advanced"), true));
    buildUpdates(addPage(QStringLiteral("О программе"), QStringLiteral("updates"), true));

    auto* footer = new QWidget(right);
    footer->setObjectName(QStringLiteral("OptionsFooter"));
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 8, 12, 8);
    footerLayout->setSpacing(4);
    auto* resetButton = new QPushButton(QStringLiteral("Сбросить по умолчанию"), footer);
    auto* okButton = new QPushButton(QStringLiteral("OK"), footer);
    auto* cancelButton = new QPushButton(QStringLiteral("Отмена"), footer);
    m_applyButton = new QPushButton(QStringLiteral("Сохранить"), footer);
    m_applyButton->setObjectName(QStringLiteral("TranslateButton"));
    footerLayout->addWidget(resetButton);
    footerLayout->addStretch(1);
    footerLayout->addWidget(okButton);
    footerLayout->addWidget(cancelButton);
    footerLayout->addWidget(m_applyButton);
    rightLayout->addWidget(footer);
    root->addWidget(right, 1);

    connect(resetButton, &QPushButton::clicked, this, &OptionsDialog::resetDefaults);
    connect(okButton, &QPushButton::clicked, this, [this] {
        applyAll();
        accept();
    });
    connect(cancelButton, &QPushButton::clicked, this, [this] { reject(); });
    connect(m_applyButton, &QPushButton::clicked, this, [this] {
        applyAll();
    });

    if (QAbstractButton* first = m_navGroup->buttons().value(0)) {
        first->setChecked(true);
        m_pages->setCurrentIndex(0);
    }

    loadAll();

    const QStringList arguments = QCoreApplication::arguments();
    const int pageIndex = arguments.indexOf(QStringLiteral("--options-page"));
    if (pageIndex >= 0 && pageIndex + 1 < arguments.size()) {
        const int index = arguments.at(pageIndex + 1).toInt();
        if (index >= 0 && index < m_pages->count()) {
            if (QAbstractButton* button = m_navGroup->button(index))
                button->setChecked(true);
            m_pages->setCurrentIndex(index);
        }
    }

    const int connectorIndex = arguments.indexOf(QStringLiteral("--connector"));
    if (connectorIndex >= 0 && connectorIndex + 1 < arguments.size()) {
        const QString connectorId = arguments.at(connectorIndex + 1);
        if (auto* tree = findChild<QTreeWidget*>(QStringLiteral("ConnectorTree"))) {
            for (int group = 0; group < tree->topLevelItemCount(); ++group) {
                QTreeWidgetItem* groupItem = tree->topLevelItem(group);
                for (int child = 0; child < groupItem->childCount(); ++child) {
                    QTreeWidgetItem* item = groupItem->child(child);
                    if (item->data(0, Qt::UserRole).toString() != connectorId)
                        continue;
                    groupItem->setExpanded(true);
                    tree->setCurrentItem(item);
                    tree->scrollToItem(item);
                    showConnectorDetails(connectorId);
                    group = tree->topLevelItemCount();
                    break;
                }
            }
        }
    }
}

QVBoxLayout* OptionsDialog::addPage(const QString& title, const QString& id, bool sub,
                                    bool scrollable)
{
    auto* page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("OptionsPage_") + id);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* content = new QWidget(page);
    content->setObjectName(QStringLiteral("OptionsPageContent"));
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);
    auto* heading = new QLabel(title, content);
    heading->setObjectName(QStringLiteral("OptionsHeading"));
    layout->addWidget(heading);
    if (scrollable) {
        auto* scroll = new QScrollArea(page);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFrameShape(QFrame::NoFrame);
        content->setParent(scroll);
        scroll->setWidget(content);
        outer->addWidget(scroll);
    } else {
        outer->addWidget(content, 1);
    }

    const int index = m_pages->addWidget(page);
    auto* button = new QPushButton(title, this);
    button->setObjectName(QStringLiteral("OptionsNavButton"));
    IconFactory::Glyph glyph = IconFactory::Glyph::Info;
    if (id == QStringLiteral("appearance") || id == QStringLiteral("behavior")
        || id == QStringLiteral("advanced"))
        glyph = IconFactory::Glyph::Gear;
    else if (id == QStringLiteral("svcpre") || id == QStringLiteral("languages")
             || id == QStringLiteral("network"))
        glyph = IconFactory::Glyph::Globe;
    else if (id == QStringLiteral("layout"))
        glyph = IconFactory::Glyph::Maximize;
    else if (id == QStringLiteral("popups"))
        glyph = IconFactory::Glyph::Pin;
    else if (id == QStringLiteral("plugins"))
        glyph = IconFactory::Glyph::Wrench;
    else if (id == QStringLiteral("exceptions"))
        glyph = IconFactory::Glyph::Close;
    else if (id == QStringLiteral("updates"))
        glyph = IconFactory::Glyph::ChevronDown;
    button->setIcon(IconFactory::icon(glyph,
                                            QApplication::palette().color(QPalette::PlaceholderText), 15));
    button->setIconSize(QSize(16, 16));
    button->setProperty("sub", sub);
    button->setCheckable(true);
    button->setFlat(true);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    m_navGroup->addButton(button, index);
    m_navLayout->addWidget(button);
    return layout;
}

QWidget* OptionsDialog::createGroup(QVBoxLayout* page, const QString& title,
                                    QVBoxLayout** groupLayout)
{
    auto* group = new QGroupBox(title, this);
    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(4);
    page->addWidget(group);
    if (groupLayout)
        *groupLayout = layout;
    return group;
}

QCheckBox* OptionsDialog::addCheck(QVBoxLayout* layout, const QString& key,
                                   const QString& label, const QString& hint)
{
    auto* check = new QCheckBox(label, this);
    if (!hint.isEmpty())
        check->setToolTip(hint);
    layout->addWidget(check);
    m_fields.append({check, key, 0, 1});
    for (const Field& field : std::as_const(m_fields)) {
        if (field.widget == check || field.key != key || field.kind != 0)
            continue;
        if (auto* other = qobject_cast<QCheckBox*>(field.widget)) {
            connect(check, &QCheckBox::toggled, other, [other](bool checked) {
                const QSignalBlocker blocker(other);
                other->setChecked(checked);
            });
            connect(other, &QCheckBox::toggled, check, [check](bool checked) {
                const QSignalBlocker blocker(check);
                check->setChecked(checked);
            });
        }
    }
    connect(check, &QCheckBox::toggled, this, &OptionsDialog::updateDependencies);
    if (!hint.isEmpty())
        addHint(layout, hint)->setContentsMargins(24, 0, 0, 0);
    return check;
}

QComboBox* OptionsDialog::addCombo(QVBoxLayout* layout, const QString& key,
                                   const QString& label,
                                   const QList<QPair<QString, QVariant>>& options,
                                   const QString& hint, int columns)
{
    auto* row = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);
    auto* caption = new QLabel(label, row);
    caption->setMinimumWidth(160);
    caption->setWordWrap(true);
    rowLayout->addWidget(caption, 0, Qt::AlignVCenter);
    DownComboBox* combo = columns > 1 ? new MultiColumnComboBox(columns, row)
                                      : new DownComboBox(row);
    combo->setMinimumWidth(160);
    for (const auto& option : options)
        combo->addItem(option.first, option.second);
    rowLayout->addWidget(combo, 1);
    layout->addWidget(row);
    m_fields.append({combo, key, 1, 1});
    connect(combo, &QComboBox::currentIndexChanged, this, &OptionsDialog::updateDependencies);
    if (!hint.isEmpty())
        addHint(layout, hint);
    return combo;
}

QPair<QVBoxLayout*, QVBoxLayout*> OptionsDialog::addTwoColumns(QVBoxLayout* layout,
                                                              int spacing)
{
    auto* host = new QWidget(this);
    auto* row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(spacing);
    auto* left = new QVBoxLayout();
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(6);
    auto* right = new QVBoxLayout();
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(6);
    row->addLayout(left, 1);
    row->addLayout(right, 1);
    layout->addWidget(host);
    return qMakePair(left, right);
}

QSpinBox* OptionsDialog::addNumber(QVBoxLayout* layout, const QString& key,
                                   const QString& label, int minimum, int maximum,
                                   int scale, const QString& suffix, const QString& hint)
{
    auto* row = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);
    auto* caption = new QLabel(label, row);
    caption->setMinimumWidth(160);
    caption->setWordWrap(true);
    rowLayout->addWidget(caption, 0, Qt::AlignVCenter);
    auto* spin = new QSpinBox(row);
    spin->setRange(minimum, maximum);
    spin->setSuffix(suffix);
    spin->setMinimumWidth(90);
    rowLayout->addWidget(spin);
    rowLayout->addStretch(1);
    layout->addWidget(row);
    m_fields.append({spin, key, 2, scale});
    if (!hint.isEmpty())
        addHint(layout, hint);
    return spin;
}

QDoubleSpinBox* OptionsDialog::addDouble(QVBoxLayout* layout, const QString& key,
                                         const QString& label, double minimum,
                                         double maximum, double step,
                                         const QString& suffix, const QString& hint)
{
    auto* row = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);
    auto* caption = new QLabel(label, row);
    caption->setMinimumWidth(160);
    caption->setWordWrap(true);
    rowLayout->addWidget(caption, 0, Qt::AlignVCenter);
    auto* spin = new QDoubleSpinBox(row);
    spin->setRange(minimum, maximum);
    spin->setSingleStep(step);
    spin->setDecimals(2);
    spin->setSuffix(suffix);
    spin->setMinimumWidth(90);
    rowLayout->addWidget(spin);
    rowLayout->addStretch(1);
    layout->addWidget(row);
    m_fields.append({spin, key, 5, 1});
    if (!hint.isEmpty())
        addHint(layout, hint);
    return spin;
}

QLineEdit* OptionsDialog::addLine(QVBoxLayout* layout, const QString& key,
                                  const QString& label, bool password, const QString& hint)
{
    auto* row = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);
    auto* caption = new QLabel(label, row);
    caption->setMinimumWidth(160);
    caption->setWordWrap(true);
    rowLayout->addWidget(caption, 0, Qt::AlignVCenter);
    auto* line = new QLineEdit(row);
    line->setMinimumWidth(160);
    if (password)
        line->setEchoMode(QLineEdit::Password);
    rowLayout->addWidget(line, 1);
    layout->addWidget(row);
    m_fields.append({line, key, 3, 1});
    if (!hint.isEmpty())
        addHint(layout, hint);
    return line;
}

QLabel* OptionsDialog::addHint(QVBoxLayout* layout, const QString& text)
{
    auto* label = new QLabel(text, this);
    label->setWordWrap(true);
    label->setObjectName(QStringLiteral("FieldHint"));
    layout->addWidget(label);
    return label;
}

QWidget* OptionsDialog::fieldWidget(const QString& key, int kind) const
{
    for (const Field& field : m_fields) {
        if (field.key == key && (kind < 0 || field.kind == kind))
            return field.widget;
    }
    return nullptr;
}

void OptionsDialog::buildGeneral(QVBoxLayout* layout)
{
    addHint(layout, QStringLiteral(
        "Раздел General: запуск, выделение, история и поведение окна. "
        "Значения сохраняются в Data/settings.json."));

    QVBoxLayout* group = nullptr;
    createGroup(layout, QStringLiteral("Запуск и закрытие"), &group);
    addCheck(group, QStringLiteral("General.RunAtStartup"),
             QStringLiteral("Запускать при входе в систему"));
    addCombo(group, QStringLiteral("General.MainWindowStartupAction"),
             QStringLiteral("Действие при запуске"),
             optionsFrom({
                 {QStringLiteral("Ничего"), 0},
                 {QStringLiteral("Показать главное окно"), 1},
                 {QStringLiteral("Свернуть в трей"), 2}
             }));
    addCombo(group, QStringLiteral("Layout.CloseAction"),
             QStringLiteral("Действие при закрытии"),
             optionsFrom({
                 {QStringLiteral("Сворачивать в трей"), QStringLiteral("tray")},
                 {QStringLiteral("Закрывать"), QStringLiteral("exit")}
             }));
    addCombo(group, QStringLiteral("General.MinimizeAction"),
             QStringLiteral("Действие при сворачивании"),
             optionsFrom({
                 {QStringLiteral("Сворачивать в трей"), QStringLiteral("tray")},
                 {QStringLiteral("Сворачивать в панель задач"), QStringLiteral("taskbar")}
             }));
    addCheck(group, QStringLiteral("Contents.SaveOnExit"),
             QStringLiteral("Сохранять содержимое полей при выходе"));

    createGroup(layout, QStringLiteral("История"), &group);
    addCheck(group, QStringLiteral("General.EnableHistory"),
             QStringLiteral("Включить историю переводов"));
    auto historyColumns = addTwoColumns(group);
    addCheck(historyColumns.first, QStringLiteral("General.ClearHistoryOnExit"),
             QStringLiteral("Очищать историю при выходе"));
    addNumber(historyColumns.second, QStringLiteral("Application.HistoryLimit"),
              QStringLiteral("Лимит записей"), 10, 10000, 1,
              Localization::tr(QStringLiteral(" записей")));

    layout->addStretch(1);
}

void OptionsDialog::buildAppearance(QVBoxLayout* layout)
{
    addHint(layout, QStringLiteral(
        "Шрифты, тема и оформление. Тёмные темы используют тот же "
        "набор компонентов, что и светлые."));

    QVBoxLayout* group = nullptr;
    createGroup(layout, QStringLiteral("Язык"), &group);
    QList<QPair<QString, QVariant>> localeOptions;
    for (const QString& folder : Localization::availableLanguages())
        localeOptions.append(qMakePair(Localization::languageDisplayName(folder), QVariant(folder)));
    addCombo(group, QStringLiteral("General.LocaleFoderName"),
             QStringLiteral("Язык интерфейса"), localeOptions);
    if (auto* localeCombo = qobject_cast<QComboBox*>(
            fieldWidget(QStringLiteral("General.LocaleFoderName"), 1))) {
        localeCombo->setProperty("_loc_skip", true);
    }

    createGroup(layout, QStringLiteral("Тема"), &group);
    QList<QPair<QString, QVariant>> themes;
    for (const QString& theme : ThemeManager::themeNames())
        themes.append(qMakePair(theme, theme));
    auto appearanceColumns = addTwoColumns(group);
    addCombo(appearanceColumns.first, QStringLiteral("Appearance.ThemeName"),
             QStringLiteral("Тема"), themes);
    addCheck(appearanceColumns.second, QStringLiteral("Appearance.SyncWithSystemTheme"),
             QStringLiteral("Синхронизировать с системной светлой/тёмной темой"));

    createGroup(layout, QStringLiteral("Расписание темы"), &group);
    auto* scheduleHolder = new QWidget(this);
    auto* scheduleHolderLayout = new QHBoxLayout(scheduleHolder);
    scheduleHolderLayout->setContentsMargins(0, 0, 0, 0);
    scheduleHolderLayout->setSpacing(6);

    m_scheduleTable = new QTableWidget(scheduleHolder);
    // Global item padding shifts and shrinks cell widgets; with 4px/6px the
    // button hosts become smaller than the buttons and clip them.
    m_scheduleTable->setStyleSheet(
        QStringLiteral("QTableWidget::item { padding: 0px; }"));
    m_scheduleTable->setColumnCount(7);
    m_scheduleTable->setHorizontalHeaderLabels({
        QString(), QStringLiteral("Имя"), QStringLiteral("Начало"), QStringLiteral("Окончание"),
        QStringLiteral("Тема"), QString(), QString()
    });
    m_scheduleTable->setColumnWidth(0, 30);
    m_scheduleTable->setColumnWidth(1, 150);
    m_scheduleTable->setColumnWidth(2, 92);
    m_scheduleTable->setColumnWidth(3, 92);
    m_scheduleTable->setColumnWidth(5, 36);
    m_scheduleTable->setColumnWidth(6, 40);
    m_scheduleTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_scheduleTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_scheduleTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_scheduleTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_scheduleTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_scheduleTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
    m_scheduleTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Fixed);
    m_scheduleTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scheduleTable->setTextElideMode(Qt::ElideRight);
    m_scheduleTable->verticalHeader()->hide();
    m_scheduleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_scheduleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    scheduleHolderLayout->addWidget(m_scheduleTable, 1);

    auto* addSchedule = new QToolButton(scheduleHolder);
    addSchedule->setObjectName(QStringLiteral("IconButton"));
    addSchedule->setIcon(IconFactory::icon(
        IconFactory::Glyph::Plus,
        QApplication::palette().color(QPalette::PlaceholderText), 16));
    addSchedule->setIconSize(QSize(16, 16));
    addSchedule->setFixedSize(32, 32);
    addSchedule->setCursor(Qt::PointingHandCursor);
    addSchedule->setToolTip(QStringLiteral("Добавить интервал"));
    scheduleHolderLayout->addWidget(addSchedule, 0, Qt::AlignTop);
    group->addWidget(scheduleHolder);

    addHint(layout, QStringLiteral(
        "Вне интервалов расписания используется тема, выбранная в блоке «Тема». "
        "Интервал может переходить через полночь."));
    connect(addSchedule, &QToolButton::clicked, this,
            &OptionsDialog::addThemeScheduleEntry);

    createGroup(layout, QStringLiteral("Шрифты"), &group);
    auto fontColumns = addTwoColumns(group);
    addCombo(fontColumns.first, QStringLiteral("Appearance.UiFont"),
             QStringLiteral("Шрифт интерфейса"),
             optionsFrom({
                 {QStringLiteral("Inter"), QStringLiteral("Inter")},
                 {QStringLiteral("Segoe UI"), QStringLiteral("Segoe UI")},
                 {QStringLiteral("Verdana"), QStringLiteral("Verdana")},
                 {QStringLiteral("Tahoma"), QStringLiteral("Tahoma")},
                 {QStringLiteral("Arial"), QStringLiteral("Arial")}
             }));
    addNumber(fontColumns.first, QStringLiteral("Appearance.UiFontSize"),
              QStringLiteral("Размер шрифта интерфейса"), 9, 24, 1,
              QStringLiteral(" pt"));
    addCombo(fontColumns.second, QStringLiteral("Appearance.EditorFont"),
             QStringLiteral("Шрифт редактора"),
             optionsFrom({
                 {QStringLiteral("Rubik"), QStringLiteral("Rubik")},
                 {QStringLiteral("Consolas"), QStringLiteral("Consolas")},
                 {QStringLiteral("Segoe UI"), QStringLiteral("Segoe UI")},
                 {QStringLiteral("Verdana"), QStringLiteral("Verdana")}
             }));
    addNumber(fontColumns.second, QStringLiteral("Appearance.EditorFontSize"),
              QStringLiteral("Размер шрифта редактора"), 9, 28, 1,
              QStringLiteral(" pt"));
    auto fontColumns2 = addTwoColumns(group);
    addCombo(fontColumns2.first, QStringLiteral("Appearance.FallbackFont"),
             QStringLiteral("Запасной шрифт (только PDF)"),
             optionsFrom({
                 {QStringLiteral("Times New Roman"), QStringLiteral("Times New Roman")},
                 {QStringLiteral("Arial"), QStringLiteral("Arial")},
                 {QStringLiteral("Tahoma"), QStringLiteral("Tahoma")},
                 {QStringLiteral("Segoe UI"), QStringLiteral("Segoe UI")}
             }));
    auto fontQuality = addTwoColumns(group);
    addCheck(fontQuality.first, QStringLiteral("Appearance.FontAntialiasing"),
             QStringLiteral("Сглаживание шрифтов"));
    addCombo(fontQuality.second, QStringLiteral("Appearance.FontHinting"),
             QStringLiteral("Хинтинг"),
             optionsFrom({
                 {QStringLiteral("Без хинтинга"), QStringLiteral("none")},
                 {QStringLiteral("Вертикальный"), QStringLiteral("vertical")},
                 {QStringLiteral("Полный"), QStringLiteral("full")}
             }));
    auto* sample = new QLabel(QStringLiteral("Aa Яя 中文 — Q2Translate: быстрый перевод 123"), this);
    sample->setObjectName(QStringLiteral("FontSample"));
    group->addWidget(sample);
    layout->addStretch(1);
}

void OptionsDialog::addThemeScheduleRow(const QJsonObject& entry)
{
    if (!m_scheduleTable)
        return;

    const int row = m_scheduleTable->rowCount();
    m_scheduleTable->insertRow(row);
    m_scheduleTable->setItem(row, 1,
        new QTableWidgetItem(entry.value(QStringLiteral("name")).toString()));
    m_scheduleTable->setItem(row, 2,
        new QTableWidgetItem(entry.value(QStringLiteral("start")).toString()));
    m_scheduleTable->setItem(row, 3,
        new QTableWidgetItem(entry.value(QStringLiteral("end")).toString()));
    m_scheduleTable->setItem(row, 4,
        new QTableWidgetItem(entry.value(QStringLiteral("theme")).toString()));

    // Cell widgets are stretched to the cell rect, so a fixed-size button would
    // stick to the left edge. Host widgets with a centered layout keep the icons
    // (and the enable checkbox) in the middle of their columns.
    const auto makeCenteredHost = [this](QWidget* widget) {
        auto* host = new QWidget(m_scheduleTable);
        auto* hostLayout = new QHBoxLayout(host);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        hostLayout->setSpacing(0);
        hostLayout->addWidget(widget, 0, Qt::AlignCenter);
        return host;
    };

    auto* enabled = new QCheckBox(m_scheduleTable);
    enabled->setChecked(entry.value(QStringLiteral("enabled")).toBool(true));
    enabled->setToolTip(QStringLiteral("Включить интервал"));
    QWidget* enabledHost = makeCenteredHost(enabled);
    m_scheduleTable->setCellWidget(row, 0, enabledHost);

    // A disabled interval keeps its place in the list but is skipped by the
    // scheduler; dim its cells so the state is visible at a glance.
    const auto dimRow = [this, enabledHost](bool checked) {
        int target = -1;
        for (int index = 0; index < m_scheduleTable->rowCount(); ++index) {
            if (m_scheduleTable->cellWidget(index, 0) == enabledHost) {
                target = index;
                break;
            }
        }
        if (target < 0)
            return;
        const QColor color = checked
            ? m_scheduleTable->palette().color(QPalette::Text)
            : m_scheduleTable->palette().color(QPalette::Disabled, QPalette::Text);
        for (int column = 1; column <= 4; ++column) {
            if (QTableWidgetItem* item = m_scheduleTable->item(target, column))
                item->setForeground(color);
        }
    };
    connect(enabled, &QCheckBox::toggled, this, dimRow);
    dimRow(enabled->isChecked());

    auto* edit = new QToolButton(m_scheduleTable);
    edit->setObjectName(QStringLiteral("IconButton"));
    edit->setIcon(IconFactory::icon(
        IconFactory::Glyph::Edit,
        QApplication::palette().color(QPalette::PlaceholderText), 15));
    edit->setIconSize(QSize(16, 16));
    edit->setFixedSize(28, 24);
    edit->setCursor(Qt::PointingHandCursor);
    edit->setToolTip(QStringLiteral("Изменить"));

    auto* remove = new QToolButton(m_scheduleTable);
    remove->setObjectName(QStringLiteral("IconButton"));
    remove->setIcon(IconFactory::icon(
        IconFactory::Glyph::Trash,
        QApplication::palette().color(QPalette::PlaceholderText), 15));
    remove->setIconSize(QSize(16, 16));
    remove->setFixedSize(30, 24);
    remove->setCursor(Qt::PointingHandCursor);
    remove->setToolTip(QStringLiteral("Удалить"));

    QWidget* editHost = makeCenteredHost(edit);
    QWidget* removeHost = makeCenteredHost(remove);

    connect(edit, &QToolButton::clicked, this, [this, editHost] {
        int rowIndex = -1;
        for (int i = 0; i < m_scheduleTable->rowCount(); ++i) {
            if (m_scheduleTable->cellWidget(i, 5) == editHost) {
                rowIndex = i;
                break;
            }
        }
        if (rowIndex < 0)
            return;
        QJsonObject scheduleEntry;
        scheduleEntry.insert(QStringLiteral("name"), m_scheduleTable->item(rowIndex, 1)->text());
        scheduleEntry.insert(QStringLiteral("start"), m_scheduleTable->item(rowIndex, 2)->text());
        scheduleEntry.insert(QStringLiteral("end"), m_scheduleTable->item(rowIndex, 3)->text());
        scheduleEntry.insert(QStringLiteral("theme"), m_scheduleTable->item(rowIndex, 4)->text());
        if (!promptThemeScheduleEntry(scheduleEntry))
            return;
        m_scheduleTable->item(rowIndex, 1)->setText(scheduleEntry.value(QStringLiteral("name")).toString());
        m_scheduleTable->item(rowIndex, 2)->setText(scheduleEntry.value(QStringLiteral("start")).toString());
        m_scheduleTable->item(rowIndex, 3)->setText(scheduleEntry.value(QStringLiteral("end")).toString());
        m_scheduleTable->item(rowIndex, 4)->setText(scheduleEntry.value(QStringLiteral("theme")).toString());
    });
    connect(remove, &QToolButton::clicked, this, [this, removeHost] {
        for (int rowIndex = 0; rowIndex < m_scheduleTable->rowCount(); ++rowIndex) {
            if (m_scheduleTable->cellWidget(rowIndex, 6) == removeHost) {
                m_scheduleTable->removeRow(rowIndex);
                break;
            }
        }
        fitTableHeight(m_scheduleTable);
    });
    m_scheduleTable->setCellWidget(row, 5, editHost);
    m_scheduleTable->setCellWidget(row, 6, removeHost);
}

void OptionsDialog::refreshThemeSchedule()
{
    if (!m_scheduleTable || !m_context || !m_context->settings)
        return;

    m_scheduleTable->setRowCount(0);
    const QJsonArray schedule =
        m_context->settings->value(QStringLiteral("Appearance.ThemeSchedule")).toArray();
    for (const QJsonValue& value : schedule) {
        if (!value.isObject())
            continue;
        addThemeScheduleRow(value.toObject());
    }
    fitTableHeight(m_scheduleTable);
}

void OptionsDialog::applyThemeSchedule()
{
    if (!m_scheduleTable || !m_context || !m_context->settings)
        return;

    QJsonArray schedule;
    for (int row = 0; row < m_scheduleTable->rowCount(); ++row) {
        const QString name = m_scheduleTable->item(row, 1)
                                 ? m_scheduleTable->item(row, 1)->text().trimmed()
                                 : QString();
        const QString start = m_scheduleTable->item(row, 2)
                                  ? m_scheduleTable->item(row, 2)->text().trimmed()
                                  : QString();
        const QString end = m_scheduleTable->item(row, 3)
                                ? m_scheduleTable->item(row, 3)->text().trimmed()
                                : QString();
        const QString theme = m_scheduleTable->item(row, 4)
                                  ? m_scheduleTable->item(row, 4)->text().trimmed()
                                  : QString();
        if (start.isEmpty() || end.isEmpty() || theme.isEmpty())
            continue;
        bool enabled = true;
        if (QWidget* host = m_scheduleTable->cellWidget(row, 0)) {
            if (auto* check = host->findChild<QCheckBox*>())
                enabled = check->isChecked();
        }
        QJsonObject entry;
        entry.insert(QStringLiteral("name"),
                     name.isEmpty() ? QStringLiteral("Интервал") : name);
        entry.insert(QStringLiteral("start"), start);
        entry.insert(QStringLiteral("end"), end);
        entry.insert(QStringLiteral("theme"), theme);
        entry.insert(QStringLiteral("enabled"), enabled);
        schedule.append(entry);
    }
    m_context->settings->setValue(QStringLiteral("Appearance.ThemeSchedule"), schedule);
}

bool OptionsDialog::promptThemeScheduleEntry(QJsonObject& entry)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Интервал расписания"));
    auto* dialogLayout = new QVBoxLayout(&dialog);
    dialogLayout->setSpacing(8);
    auto* form = new QFormLayout();
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);

    const QString entryName = entry.value(QStringLiteral("name")).toString();
    const QTime entryStart = QTime::fromString(
        entry.value(QStringLiteral("start")).toString(), QStringLiteral("HH:mm"));
    const QTime entryEnd = QTime::fromString(
        entry.value(QStringLiteral("end")).toString(), QStringLiteral("HH:mm"));

    auto* nameEdit = new QLineEdit(
        entryName.isEmpty() ? QStringLiteral("Ночь") : entryName, &dialog);
    auto* startEdit = new QTimeEdit(entryStart.isValid() ? entryStart : QTime(21, 0), &dialog);
    auto* endEdit = new QTimeEdit(entryEnd.isValid() ? entryEnd : QTime(8, 0), &dialog);
    startEdit->setDisplayFormat(QStringLiteral("HH:mm"));
    endEdit->setDisplayFormat(QStringLiteral("HH:mm"));
    auto* themeCombo = new DownComboBox(&dialog);
    for (const QString& theme : ThemeManager::themeNames())
        themeCombo->addItem(theme, theme);

    QString preferredTheme = entry.value(QStringLiteral("theme")).toString();
    if (preferredTheme.isEmpty() && m_context && m_context->settings) {
        preferredTheme =
            m_context->settings->stringValue(QStringLiteral("Appearance.ThemeName"));
    }
    int themeIndex = themeCombo->findData(preferredTheme);
    if (themeIndex < 0 || !ThemeManager::isDark(preferredTheme)) {
        for (int i = 0; i < themeCombo->count(); ++i) {
            if (ThemeManager::isDark(themeCombo->itemData(i).toString())) {
                themeIndex = i;
                break;
            }
        }
    }
    if (themeIndex >= 0)
        themeCombo->setCurrentIndex(themeIndex);

    form->addRow(QStringLiteral("Имя"), nameEdit);
    form->addRow(QStringLiteral("Начало"), startEdit);
    form->addRow(QStringLiteral("Окончание"), endEdit);
    form->addRow(QStringLiteral("Тема"), themeCombo);
    dialogLayout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         &dialog);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("OK"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialogLayout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return false;

    entry.insert(QStringLiteral("name"),
                 nameEdit->text().trimmed().isEmpty() ? QStringLiteral("Интервал")
                                                      : nameEdit->text().trimmed());
    entry.insert(QStringLiteral("start"),
                 startEdit->time().toString(QStringLiteral("HH:mm")));
    entry.insert(QStringLiteral("end"),
                 endEdit->time().toString(QStringLiteral("HH:mm")));
    entry.insert(QStringLiteral("theme"), themeCombo->currentData().toString());
    return true;
}

void OptionsDialog::addThemeScheduleEntry()
{
    if (!m_scheduleTable)
        return;

    QJsonObject entry;
    entry.insert(QStringLiteral("name"), QStringLiteral("Ночь"));
    entry.insert(QStringLiteral("start"), QStringLiteral("21:00"));
    entry.insert(QStringLiteral("end"), QStringLiteral("08:00"));
    entry.insert(QStringLiteral("enabled"), true);
    if (!promptThemeScheduleEntry(entry))
        return;
    addThemeScheduleRow(entry);
    fitTableHeight(m_scheduleTable);
}

void OptionsDialog::buildServices(QVBoxLayout* layout)
{
    addHint(layout, QStringLiteral(
        "Список переводчиков в главном окне задаётся в разделе «Коннекторы»: "
        "включённые сервисы появляются на нижней панели, а выбор нужного "
        "делается кликом по нему (Ctrl+клик — несколько сразу)."));

    QVBoxLayout* group = nullptr;
    createGroup(layout, QStringLiteral("Другие сервисы"), &group);
    auto* providersHost = new QWidget(this);
    auto* providersGrid = new QGridLayout(providersHost);
    providersGrid->setContentsMargins(0, 0, 0, 0);
    providersGrid->setHorizontalSpacing(8);
    providersGrid->setVerticalSpacing(4);
    providersGrid->setColumnStretch(4, 1);

    const auto addProviderCheck = [this, providersHost, providersGrid](
                                      int row, const QString& key, const QString& label) {
        auto* check = new QCheckBox(label, providersHost);
        providersGrid->addWidget(check, row, 0);
        m_fields.append({check, key, 0, 1});
        connect(check, &QCheckBox::toggled, this, &OptionsDialog::updateDependencies);
        return check;
    };
    const auto addProviderCombo = [this, providersHost, providersGrid](
                                      int row, int column, const QString& key,
                                      const QList<QPair<QString, QVariant>>& options) {
        auto* combo = new DownComboBox(providersHost);
        combo->setMinimumWidth(160);
        for (const auto& option : options)
            combo->addItem(option.first, option.second);
        providersGrid->addWidget(combo, row, column);
        // The provider names are long; a wider list keeps "PaddleOCR (локальный)"
        // and the hybrid entry readable.
        combo->setMinimumWidth(240);
        if (QAbstractItemView* view = combo->view())
            view->setMinimumWidth(260);
        m_fields.append({combo, key, 1, 1});
        connect(combo, &QComboBox::currentIndexChanged,
                this, &OptionsDialog::updateDependencies);
        return combo;
    };

    const auto manifestOptions = [this](const QString& type) {
        QList<QPair<QString, QVariant>> options;
        if (m_context && m_context->plugins) {
            for (const PluginManifest& manifest : m_context->plugins->manifests()) {
                if (!manifest.valid || manifest.type != type)
                    continue;
                if (m_context->plugins->isDisabled(manifest.id))
                    continue;
                options.append(qMakePair(manifest.name, QVariant(manifest.id)));
            }
        }
        if (options.isEmpty())
            options.append(qMakePair(QStringLiteral("Нет подключённых"), QString()));
        return options;
    };

    addProviderCheck(1, QStringLiteral("ServiceProviders.ttsOn"),
                     QStringLiteral("Озвучивание (TTS)"));
    addProviderCombo(1, 1, QStringLiteral("ServiceProviders.tts"),
                     manifestOptions(QStringLiteral("tts")));

    QList<QPair<QString, QVariant>> ocrOptions;
    if (m_context && m_context->translation) {
        for (const PluginManifest& manifest : m_context->translation->ocrServices())
            ocrOptions.append(qMakePair(manifest.name, QVariant(manifest.id)));
    }
    if (ocrOptions.isEmpty())
        ocrOptions.append(qMakePair(QStringLiteral("System OCR"), QStringLiteral("system")));
    // OCR is configured per file type below, so the old "OCR — использовать"
    // switch and the shared provider combo are no longer needed.
    providersGrid->addWidget(new QLabel(QStringLiteral("Языки OCR"), providersHost), 0, 0);
    m_ocrLanguage = new MultiSelectComboBox(providersHost);
    m_ocrLanguage->setMinimumWidth(220);
    providersGrid->addWidget(m_ocrLanguage, 0, 1);
    m_ocrLanguagesButton = new QPushButton(QStringLiteral("Языки OCR…"), providersHost);
    providersGrid->addWidget(m_ocrLanguagesButton, 0, 2, Qt::AlignLeft);

    connect(m_ocrLanguagesButton, &QPushButton::clicked, this,
            &OptionsDialog::browseOcrLanguages);

    auto* imageOptionsHost = new QWidget(providersHost);
    auto* imageOptions = new QVBoxLayout(imageOptionsHost);
    imageOptions->setContentsMargins(0, 8, 0, 0);
    imageOptions->setSpacing(4);
    // Keeping images and the retention period are one setting: show them on a
    // single row instead of two stacked labelled rows.
    auto* saveRow = new QWidget(imageOptionsHost);
    auto* saveLayout = new QHBoxLayout(saveRow);
    saveLayout->setContentsMargins(0, 0, 0, 0);
    saveLayout->setSpacing(8);
    auto* saveCheck = new QCheckBox(QStringLiteral("Сохранять переведённые изображения"),
                                    saveRow);
    m_fields.append({saveCheck, QStringLiteral("Ocr.SaveTranslatedImage"), 0, 1});
    connect(saveCheck, &QCheckBox::toggled, this, &OptionsDialog::updateDependencies);
    saveLayout->addWidget(saveCheck);
    saveLayout->addSpacing(16);
    saveLayout->addWidget(new QLabel(QStringLiteral("Хранить"), saveRow));
    auto* retention = new QSpinBox(saveRow);
    retention->setRange(0, 3650);
    retention->setSuffix(QStringLiteral(" дн."));
    retention->setMinimumWidth(90);
    saveLayout->addWidget(retention);
    m_fields.append({retention, QStringLiteral("Ocr.ImageRetentionDays"), 2, 1});
    saveLayout->addWidget(new QLabel(QStringLiteral("0 — не удалять автоматически."), saveRow));
    saveLayout->addStretch(1);
    imageOptions->addWidget(saveRow);
    addHint(imageOptions, QStringLiteral(
        "Снятая галочка удаляет файл, как только из полей исчезает плейсхолдер."));
    providersGrid->addWidget(imageOptionsHost, 10, 0, 1, 5);

    // OCR engine per file type: empty means "same as the general OCR field".
    QList<QPair<QString, QVariant>> ocrTypeOptions = ocrOptions;
    // The image row also offers the hybrid mode: Paddle detection plus Tesseract
    // re-reading of the places where Paddle found no text.
    QList<QPair<QString, QVariant>> imageOcrOptions = ocrTypeOptions;
    imageOcrOptions.append(qMakePair(QStringLiteral("Гибрид: Paddle + Tesseract (локально)"),
                                     QVariant(QStringLiteral("hybrid"))));
    QList<QPair<QString, QVariant>> translatorOptions;
    translatorOptions.append(qMakePair(QStringLiteral("Как в главном окне"),
                                       QVariant(QString())));
    QStringList orderedTranslators;
    if (m_context && m_context->settings) {
        for (const QJsonValue& value :
             m_context->settings->value(QStringLiteral("ServicesOrder")).toArray()) {
            const QString id = value.toString();
            if (!id.isEmpty())
                orderedTranslators.append(id);
        }
    }
    if (m_context && m_context->plugins) {
        for (const PluginManifest& manifest : m_context->plugins->manifests()) {
            if (!orderedTranslators.contains(manifest.id))
                orderedTranslators.append(manifest.id);
        }
        // Balancer goes right after "Как в главном окне".
        const int balancerIndex = orderedTranslators.indexOf(QStringLiteral("balancer"));
        if (balancerIndex > 0) {
            orderedTranslators.removeAt(balancerIndex);
            orderedTranslators.prepend(QStringLiteral("balancer"));
        }
        for (const QString& id : std::as_const(orderedTranslators)) {
            IPluginProvider* provider = m_context->plugins->provider(id);
            if (!provider || m_context->plugins->isDisabled(id))
                continue;
            if (!provider->capabilities().contains(QStringLiteral("translate")))
                continue;
            translatorOptions.append(
                qMakePair(provider->manifest().name, QVariant(id)));
        }
    }

    // Every OCR type gets its own OCR engine and its own translation connector,
    // so scans of books and screenshots may use different services.
    const auto addOcrTypeRow = [&](int row, const QString& label, const QString& ocrKey,
                                   const QList<QPair<QString, QVariant>>& engines,
                                   const QString& translatorKey) {
        providersGrid->addWidget(new QLabel(label, providersHost), row, 0);
        if (QComboBox* ocrCombo = addProviderCombo(row, 1, ocrKey, engines)) {
            ocrCombo->setMinimumWidth(210);
            if (m_context && m_context->settings
                && m_context->settings->stringValue(ocrKey).isEmpty()) {
                const QString fallback = ocrKey == QStringLiteral("Ocr.ProviderImages")
                    ? QStringLiteral("paddleocr") : QStringLiteral("tesseract_local");
                const int index = ocrCombo->findData(fallback);
                if (index >= 0)
                    ocrCombo->setCurrentIndex(index);
            }
        }
        providersGrid->addWidget(new QLabel(QStringLiteral("Коннектор"), providersHost), row, 2);
        if (QComboBox* translatorCombo =
                addProviderCombo(row, 3, translatorKey, translatorOptions))
            translatorCombo->setMinimumWidth(190);
    };
    addOcrTypeRow(6, QStringLiteral("OCR для скриншотов"),
                  QStringLiteral("Ocr.ProviderScreen"), ocrTypeOptions,
                  QStringLiteral("ServiceProviders.translateScreen"));
    addOcrTypeRow(7, QStringLiteral("OCR для изображений"),
                  QStringLiteral("Ocr.ProviderImages"), imageOcrOptions,
                  QStringLiteral("ServiceProviders.translateImages"));
    addOcrTypeRow(8, QStringLiteral("OCR для PDF"),
                  QStringLiteral("Ocr.ProviderPdf"), ocrTypeOptions,
                  QStringLiteral("ServiceProviders.translatePdf"));
    addOcrTypeRow(9, QStringLiteral("OCR для DJVU"),
                  QStringLiteral("Ocr.ProviderDjvu"), ocrTypeOptions,
                  QStringLiteral("ServiceProviders.translateDjvu"));

    m_spellCheck = addProviderCheck(2, QStringLiteral("ServiceProviders.spellOn"),
                                    QStringLiteral("Проверка орфографии"));
    m_spellProvider = addProviderCombo(2, 1, QStringLiteral("ServiceProviders.spell"),
                                       manifestOptions(QStringLiteral("spelling")));
    m_spellProvider->setMinimumWidth(170);
    m_spellLanguagesButton = new QPushButton(QStringLiteral("Языки…"), providersHost);
    providersGrid->addWidget(m_spellLanguagesButton, 2, 2);
    connect(m_spellLanguagesButton, &QPushButton::clicked,
            this, &OptionsDialog::browseSpellLanguages);

    addProviderCheck(3, QStringLiteral("ServiceProviders.dictOn"),
                     QStringLiteral("Словарь"));
    addProviderCombo(3, 1, QStringLiteral("ServiceProviders.dict"),
                     manifestOptions(QStringLiteral("dictionaries")));

    addProviderCheck(4, QStringLiteral("ServiceProviders.summOn"),
                     QStringLiteral("Суммаризация"));
    addProviderCombo(4, 1, QStringLiteral("ServiceProviders.summ"),
                     manifestOptions(QStringLiteral("llm")));

    addProviderCheck(5, QStringLiteral("ServiceProviders.rewrOn"),
                     QStringLiteral("Перефразирование"));
    addProviderCombo(5, 1, QStringLiteral("ServiceProviders.rewr"),
                     manifestOptions(QStringLiteral("llm")));

    group->addWidget(providersHost);

    createGroup(layout, QStringLiteral("Изображения"), &group);
    addHint(layout, QStringLiteral(
        "Вёрстка перевода на картинке: шрифт, кегль, стирание оригинала и фильтры OCR."));
    QList<QPair<QString, QVariant>> fontOptions;
    fontOptions.append(qMakePair(QStringLiteral("Системный"), QVariant(QString())));
    fontOptions.append(qMakePair(QStringLiteral("Комикс"), QVariant(QStringLiteral("Comic"))));
    QStringList families = QFontDatabase::families();
    families.sort(Qt::CaseInsensitive);
    for (const QString& family : families)
        fontOptions.append(qMakePair(family, QVariant(family)));
    addCombo(group, QStringLiteral("Image.TextFont"), QStringLiteral("Шрифт перевода"),
             fontOptions,
             QStringLiteral("Пусто — системный шрифт. Проверяется, что шрифт содержит нужный алфавит."),
             3);
    addCombo(group, QStringLiteral("Image.EraseMode"),
             QStringLiteral("Стирание старого текста"),
             QList<QPair<QString, QVariant>>{
                 qMakePair(QStringLiteral("Автоматически"), QVariant(QStringLiteral("auto"))),
                 qMakePair(QStringLiteral("Восстановление фона"),
                           QVariant(QStringLiteral("inpaint"))),
                 qMakePair(QStringLiteral("Плавный градиент"), QVariant(QStringLiteral("smooth"))),
                 qMakePair(QStringLiteral("Плоская заливка"), QVariant(QStringLiteral("fill"))) },
             QStringLiteral("Авто: ровный фон — заливка, фото и рисунок — восстановление."));
    addNumber(group, QStringLiteral("Image.MinFontPx"),
              QStringLiteral("Минимальный кегль"), 0, 60, 1,
              QStringLiteral(" px"),
              QStringLiteral("0 — подобрать от ширины страницы."));
    addNumber(group, QStringLiteral("Image.MaxLines"),
              QStringLiteral("Максимум строк в блоке"), 1, 12);
    addDouble(group, QStringLiteral("Image.LineSpacing"),
              QStringLiteral("Межстрочный интервал"), 0.85, 1.40, 0.05,
              QString(),
              QStringLiteral("1.0 — обычная высота строки шрифта."));
    addDouble(group, QStringLiteral("Image.MinOcrConfidence"),
              QStringLiteral("Порог уверенности OCR"), 0.0, 0.95, 0.05,
              QString(),
              QStringLiteral("Строки с меньшей уверенностью не переводятся. 0 — без фильтра."));
    addCheck(group, QStringLiteral("Image.SplitParagraphs"),
             QStringLiteral("Делить крупные блоки на абзацы"));
    addCheck(group, QStringLiteral("Image.RecoverMissedText"),
             QStringLiteral("Дочитывать пропущенные слова"),
             QStringLiteral("Второй проход OCR по местам, где найден текст без строки: "
                            "короткие слова вроде «IT» иногда пропадают на полной странице."));
    addCheck(group, QStringLiteral("Image.BubblePass"),
             QStringLiteral("Дочитывать стилизованный текст (bubble pass)"),
             QStringLiteral("Поиск букв по форме внутри светлых пузырей: жирный, контурный "
                            "и декоративный леттеринг, который пропускает обычный OCR."));
    addCheck(group, QStringLiteral("Image.TileLargePages"),
             QStringLiteral("Читать большие страницы по фрагментам"),
             QStringLiteral("Для страниц больше 1400 px: нарезка на 2–6 перекрывающихся "
                            "фрагментов, чтобы детектор видел текст крупнее."));
    addCheck(group, QStringLiteral("Image.TextOutline"),
             QStringLiteral("Обводка текста на рисунке"),
             QStringLiteral("Включайте, если перевод лежит поверх сложного рисунка."));
    addCheck(group, QStringLiteral("Image.LlmContext"),
             QStringLiteral("Контекст соседних реплик (LLM)"),
             QStringLiteral("Работает только с LLM-коннекторами."));
    addCheck(group, QStringLiteral("Image.LengthBudget"),
             QStringLiteral("Бюджет длины перевода (LLM)"),
             QStringLiteral("Просит модель уложиться в размер пузыря. Только LLM."));

    createGroup(layout, QStringLiteral("Порядок сервисов"), &group);
    m_orderTable = new QTableWidget(this);
    m_orderTable->setColumnCount(3);
    m_orderTable->setHorizontalHeaderLabels({
        QStringLiteral("Сервис"), QStringLiteral("Возможности"), QString()
    });
    m_orderTable->setColumnWidth(0, 216);
    m_orderTable->setColumnWidth(2, 96);
    m_orderTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_orderTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_orderTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_orderTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_orderTable->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_orderTable->setTextElideMode(Qt::ElideRight);
    m_orderTable->verticalHeader()->hide();
    m_orderTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    group->addWidget(m_orderTable);
    addHint(layout, QStringLiteral("Кнопки ↑/↓ меняют порядок сервисов."));

    layout->addStretch(1);
}

void OptionsDialog::buildBehavior(QVBoxLayout* layout)
{
    addHint(layout, QStringLiteral(
        "Поведение перевода, дополнительный вызов и дополнительный вывод."));

    QVBoxLayout* group = nullptr;
    createGroup(layout, QStringLiteral("Перевод"), &group);
    addCheck(group, QStringLiteral("General.InstantTranslation"),
             QStringLiteral("Мгновенный перевод (по мере ввода)"));
    addCheck(group, QStringLiteral("Advanced.CleanInsertedText"),
             QStringLiteral("Очищать вставляемый текст (из буфера и OCR)"));
    addCheck(group, QStringLiteral("General.SpellChecking"),
             QStringLiteral("Проверка орфографии при вводе"));

    createGroup(layout, QStringLiteral("Дополнительный вызов"), &group);
    auto* callRow = new QWidget(this);
    callRow->setMaximumWidth(460);
    auto* callLayout = new QGridLayout(callRow);
    callLayout->setContentsMargins(0, 0, 0, 0);
    callLayout->setHorizontalSpacing(8);
    callLayout->setColumnStretch(1, 1);
    callLayout->setColumnStretch(3, 1);

    auto* callLabel = new QLabel(QStringLiteral("Действие"), callRow);
    callLayout->addWidget(callLabel, 0, 0);
    auto* callCombo = new DownComboBox(callRow);
    callCombo->setMinimumWidth(100);
    callCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    callCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    callCombo->setMinimumContentsLength(12);
    for (const auto& option : optionsFrom({
             {QStringLiteral("Нет"), QStringLiteral("none")},
             {QStringLiteral("Обратный перевод"), QStringLiteral("back")},
             {QStringLiteral("Суммаризация (кратко)"), QStringLiteral("summ")},
             {QStringLiteral("Реферирование (перефразирование)"), QStringLiteral("rewr")}
         })) {
        callCombo->addItem(option.first, option.second);
    }
    m_fields.append({callCombo, QStringLiteral("General.AdditionalCall"), 1, 1});
    connect(callCombo, &QComboBox::currentIndexChanged,
            this, &OptionsDialog::updateDependencies);
    callLayout->addWidget(callCombo, 0, 1);
    group->addWidget(callRow, 0, Qt::AlignLeft);

    createGroup(layout, QStringLiteral("Дополнительный вывод"), &group);
    auto outputColumns = addTwoColumns(group);
    addCombo(outputColumns.first, QStringLiteral("General.AdditionalOutputType"),
             QStringLiteral("Тип"),
             optionsFrom({
                 {QStringLiteral("Нет"), QStringLiteral("none")},
                 {QStringLiteral("Озвучивание"), QStringLiteral("speak")},
                 {QStringLiteral("Копировать в буфер"), QStringLiteral("copy")},
                 {QStringLiteral("Показать в попапе"), QStringLiteral("popup")}
             }));
    addCombo(outputColumns.second, QStringLiteral("General.AdditionalOutputSource"),
             QStringLiteral("Источник"),
             optionsFrom({
                 {QStringLiteral("Перевод"), QStringLiteral("translation")},
                 {QStringLiteral("Исходный текст"), QStringLiteral("input")}
             }));

    createGroup(layout, QStringLiteral("Общие"), &group);
    auto generalColumns = addTwoColumns(group);
    addCheck(generalColumns.first, QStringLiteral("General.AlwaysDetectLanguage"),
             QStringLiteral("Всегда определять язык"));
    addCheck(generalColumns.second, QStringLiteral("General.ClearOnDragDrop"),
             QStringLiteral("Очищать поле при перетаскивании"));

    layout->addStretch(1);
}

void OptionsDialog::buildLanguages(QVBoxLayout* layout)
{
    addHint(layout, QStringLiteral(
        "Языки интерфейса, исходный/целевой язык, избранные языки "
        "и правила перевода."));

    QVBoxLayout* group = nullptr;
    createGroup(layout, QStringLiteral("Определение языка"), &group);
    addCombo(group, QStringLiteral("General.LanguageDetection"),
             QStringLiteral("Определение языка"),
             optionsFrom({
                 {QStringLiteral("Локально, приложением"), QStringLiteral("local")},
                 {QStringLiteral("Онлайн, используемым сервисом"), QStringLiteral("service")}
             }),
             QStringLiteral("Локально — язык определяется приложением до запроса. "
                            "Онлайн — язык определяет выбранный сервис перевода."));

    createGroup(layout, QStringLiteral("Языки по умолчанию"), &group);
    auto* defaultsRow = new QWidget(this);
    auto* defaultsLayout = new QHBoxLayout(defaultsRow);
    defaultsLayout->setContentsMargins(0, 0, 0, 0);
    defaultsLayout->addWidget(new QLabel(QStringLiteral("Исходный"), defaultsRow));
    m_fromCombo = new DownComboBox(defaultsRow);
    defaultsLayout->addWidget(m_fromCombo);
    defaultsLayout->addWidget(new QLabel(QStringLiteral("Целевой"), defaultsRow));
    m_toCombo = new DownComboBox(defaultsRow);
    defaultsLayout->addWidget(m_toCombo);
    defaultsLayout->addStretch(1);
    group->addWidget(defaultsRow);

    createGroup(layout, QStringLiteral("Избранные языки"), &group);
    auto* pinnedWidget = new QWidget(this);
    auto* pinnedLayout = new QGridLayout(pinnedWidget);
    pinnedLayout->setContentsMargins(0, 0, 0, 0);
    pinnedLayout->setHorizontalSpacing(4);
    pinnedLayout->setVerticalSpacing(2);
    QList<LanguageInfo> pinnedLanguages;
    for (const LanguageInfo& language : sortedLanguageList()) {
        if (language.id != 1)
            pinnedLanguages.append(language);
    }
    const int pinnedColumns = 5;
    const int pinnedRows = qMax(1, (pinnedLanguages.size() + pinnedColumns - 1) / pinnedColumns);
    int pinnedIndex = 0;
    for (const LanguageInfo& language : std::as_const(pinnedLanguages)) {
        auto* check = new QCheckBox(QString::fromUtf8(language.name), pinnedWidget);
        m_pinnedChecks.insert(language.id, check);
        pinnedLayout->addWidget(check, pinnedIndex % pinnedRows, pinnedIndex / pinnedRows);
        connect(check, &QCheckBox::toggled, this, [this] {
            rebuildLanguageCombos();
            scheduleLiveApply();
        });
        ++pinnedIndex;
    }
    group->addWidget(pinnedWidget);

    createGroup(layout, QStringLiteral("Правила перевода"), &group);
    m_rulesList = new QListWidget(this);
    m_rulesList->setMaximumHeight(130);
    group->addWidget(m_rulesList);
    auto* ruleRow = new QWidget(this);
    auto* ruleLayout = new QHBoxLayout(ruleRow);
    ruleLayout->setContentsMargins(0, 0, 0, 0);
    m_ruleFrom = new DownComboBox(ruleRow);
    m_ruleTo = new DownComboBox(ruleRow);
    auto* addRule = new QPushButton(QStringLiteral("+ Добавить"), ruleRow);
    auto* deleteRule = new QPushButton(QStringLiteral("Удалить"), ruleRow);
    ruleLayout->addWidget(m_ruleFrom);
    ruleLayout->addWidget(m_ruleTo);
    ruleLayout->addWidget(addRule);
    ruleLayout->addWidget(deleteRule);
    ruleLayout->addStretch(1);
    group->addWidget(ruleRow);
    connect(addRule, &QPushButton::clicked, this, [this] {
        const int from = m_ruleFrom->currentData().toInt();
        const int to = m_ruleTo->currentData().toInt();
        if (from == to)
            return;
        auto* item = new QListWidgetItem(QStringLiteral("%1 → %2")
                                             .arg(languageNameById(from), languageNameById(to)),
                                         m_rulesList);
        item->setData(Qt::UserRole, QVariantList{from, to});
    });
    connect(deleteRule, &QPushButton::clicked, this, [this] {
        delete m_rulesList->takeItem(m_rulesList->currentRow());
    });

    createGroup(layout, QStringLiteral("Распознавание речи"), &group);
    addCombo(group, QStringLiteral("AutoDetection.LanguageSpeechRecognition"),
             QStringLiteral("Язык распознавания речи"),
             optionsFrom({
                 {QStringLiteral("en-US"), QStringLiteral("en-US")},
                 {QStringLiteral("ru-RU"), QStringLiteral("ru-RU")},
                 {QStringLiteral("cmn-Hans-CN"), QStringLiteral("cmn-Hans-CN")}
             }));
    rebuildLanguageCombos();
    if (m_fromCombo) {
        if (const int index = m_fromCombo->findData(QStringLiteral("en")); index >= 0)
            m_fromCombo->setCurrentIndex(index);
    }
    if (m_toCombo) {
        if (const int index = m_toCombo->findData(QStringLiteral("ru")); index >= 0)
            m_toCombo->setCurrentIndex(index);
    }
    if (m_ruleFrom) {
        if (const int index = m_ruleFrom->findData(17); index >= 0)
            m_ruleFrom->setCurrentIndex(index);
    }
    if (m_ruleTo) {
        if (const int index = m_ruleTo->findData(46); index >= 0)
            m_ruleTo->setCurrentIndex(index);
    }
    layout->addStretch(1);
}

void OptionsDialog::buildLayout(QVBoxLayout* layout)
{
    addHint(layout, QStringLiteral(
        "Макет главного окна и видимость панелей."));

    QVBoxLayout* group = nullptr;
    createGroup(layout, QStringLiteral("Макет интерфейса"), &group);
    auto layoutColumns = addTwoColumns(group);
    addCombo(layoutColumns.first, QStringLiteral("Layout.InterfacePreset"),
             QStringLiteral("Тип интерфейса"),
             optionsFrom({
                 {QStringLiteral("Классический"), QStringLiteral("classic")},
                 {QStringLiteral("Бок обок"), QStringLiteral("side")},
                 {QStringLiteral("Сравнение"), QStringLiteral("compare")}
             }));
    addCombo(layoutColumns.second, QStringLiteral("Layout.Preset"),
             QStringLiteral("Плотность"),
             optionsFrom({
                 {QStringLiteral("Классическая"), QStringLiteral("classic")},
                 {QStringLiteral("Компактная"), QStringLiteral("compact")},
                 {QStringLiteral("Минимальная"), QStringLiteral("minimal")}
             }));
    if (auto* density = qobject_cast<QComboBox*>(
            fieldWidget(QStringLiteral("Layout.Preset"), 1))) {
        connect(density, &QComboBox::currentIndexChanged, this,
                &OptionsDialog::applyDensity);
    }

    createGroup(layout, QStringLiteral("Панели"), &group);
    auto panelColumns = addTwoColumns(group);
    addCheck(panelColumns.first, QStringLiteral("Layout.ShowLangBar"),
             QStringLiteral("Показывать панель выбора языка"));
    addCheck(panelColumns.first, QStringLiteral("Layout.ShowHistoryNav"),
             QStringLiteral("Показывать панель навигации по истории"));
    addCheck(panelColumns.second, QStringLiteral("Layout.ShowStatusBar"),
             QStringLiteral("Показывать строку состояния"));
    addCheck(panelColumns.second, QStringLiteral("Layout.ShowEditorSideButtons"),
             QStringLiteral("Показывать кнопки действий у полей"));

    createGroup(layout, QStringLiteral("Панель сервисов"), &group);
    addCheck(group, QStringLiteral("Layout.ShowServicesPanel"),
             QStringLiteral("Показывать панель сервисов"));
    auto serviceColumns = addTwoColumns(group);
    addCombo(serviceColumns.first, QStringLiteral("Layout.ServicesListStyle"),
             QStringLiteral("Вид панели"),
             optionsFrom({
                 {QStringLiteral("Кнопки на панели"), QStringLiteral("ribbon")},
                 {QStringLiteral("Выпадающее меню"), QStringLiteral("dropdown")},
                 {QStringLiteral("Не отображать"), QStringLiteral("hidden")}
             }));
    addCombo(serviceColumns.second, QStringLiteral("Layout.ClassicView"),
             QStringLiteral("Классический вид"),
             optionsFrom({
                 {QStringLiteral("Только иконки"), QStringLiteral("icons")},
                 {QStringLiteral("Иконки и текст"), QStringLiteral("icons_text")}
             }));

    layout->addStretch(1);
}

void OptionsDialog::buildPopups(QVBoxLayout* layout)
{
    addHint(layout, QStringLiteral(
        "Прозрачность 0% — непрозрачное окно. Таймаут 0 — не скрывать автоматически."));

    QVBoxLayout* group = nullptr;
    createGroup(layout, QStringLiteral("Поведение"), &group);
    addCheck(group, QStringLiteral("Appearance.PopupCloseOnOutsideClick"),
             QStringLiteral("Закрывать всплывающее окно по клику вне его"));

    createGroup(layout, QStringLiteral("Настройки окна"), &group);
    reloadPopupStyles();
    auto* styleRow = new QWidget(this);
    auto* styleLayout = new QHBoxLayout(styleRow);
    styleLayout->setContentsMargins(0, 0, 0, 0);
    styleLayout->setSpacing(8);
    styleLayout->addWidget(new QLabel(QStringLiteral("Стиль плашки"), styleRow));
    auto* styleCombo = new DownComboBox(styleRow);
    styleCombo->setMinimumWidth(220);
    for (const PopupStyle& style : popupStyles())
        styleCombo->addItem(Localization::tr(style.name), style.id);
    if (styleCombo->count() == 0)
        styleCombo->addItem(QStringLiteral("Классика"), QStringLiteral("classic"));
    styleCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    styleCombo->setMinimumContentsLength(18);
    m_fields.append({styleCombo, QStringLiteral("Appearance.PopupStyle"), 1, 1});
    styleLayout->addWidget(styleCombo);
    styleLayout->addStretch(1);
    group->addWidget(styleRow);

    auto popupFlags = addTwoColumns(group);
    addCheck(popupFlags.first, QStringLiteral("Appearance.PopupAutoSize"),
             QStringLiteral("Автоматический размер"));
    addCheck(popupFlags.second, QStringLiteral("Appearance.PopupNearCursor"),
             QStringLiteral("Показывать рядом с курсором"));
    auto popupSizes = addTwoColumns(group);
    addNumber(popupSizes.first, QStringLiteral("Appearance.Transparency"),
              QStringLiteral("Прозрачность"), 0, 95, 1, QStringLiteral(" %"));
    addNumber(popupSizes.second, QStringLiteral("Appearance.PopupTimeout"),
              QStringLiteral("Скрывать через"), 0, 60, 1, Localization::tr(QStringLiteral(" сек")));

    createGroup(layout, QStringLiteral("Настройки внешнего вида"), &group);

    auto* fontRow = new QWidget(this);
    auto* fontLayout = new QHBoxLayout(fontRow);
    fontLayout->setContentsMargins(0, 0, 0, 0);
    fontLayout->setSpacing(8);
    fontLayout->addWidget(new QLabel(QStringLiteral("Размер шрифта:"), fontRow));
    fontLayout->addWidget(new QLabel(QStringLiteral("исходный"), fontRow));
    m_popupSourceFontSize = new QSpinBox(fontRow);
    m_popupSourceFontSize->setRange(8, 48);
    m_popupSourceFontSize->setFixedWidth(96);
    fontLayout->addWidget(m_popupSourceFontSize);
    fontLayout->addWidget(new QLabel(QStringLiteral("целевой"), fontRow));
    m_popupTargetFontSize = new QSpinBox(fontRow);
    m_popupTargetFontSize->setRange(8, 48);
    m_popupTargetFontSize->setFixedWidth(96);
    fontLayout->addWidget(m_popupTargetFontSize);
    fontLayout->addStretch(1);
    group->addWidget(fontRow);
    connect(styleCombo, &QComboBox::currentIndexChanged, this,
            &OptionsDialog::loadPopupStyleFontSizes);
    auto popupChecks = addTwoColumns(group);
    addCheck(popupChecks.first, QStringLiteral("Appearance.PopupIcons"),
             QStringLiteral("Показывать кнопки действий"));
    addCheck(popupChecks.first, QStringLiteral("Appearance.PopupAutoFocus"),
             QStringLiteral("Фокус попапа"));
    layout->addStretch(1);
    auto* previewRow = new QHBoxLayout();
    previewRow->addStretch(1);
    auto* previewButton = new QPushButton(QStringLiteral("Предпросмотр"), this);
    previewButton->setToolTip(QStringLiteral("Открыть всплывающее окно с полным набором кнопок"));
    previewRow->addWidget(previewButton);
    layout->addLayout(previewRow);
    connect(previewButton, &QPushButton::clicked, this, &OptionsDialog::showPopupPreview);
}

void OptionsDialog::showPopupPreview()
{
    if (!m_context)
        return;
    if (!m_popupPreview) {
        m_popupPreview = new PopupWindow(m_context, this);
        connect(m_popupPreview, &PopupWindow::openMainRequested, this,
                [this](const QString& text) {
                    emit popupPreviewOpenMainRequested(text);
                });
    }
    m_popupPreview->showTranslation(
        QStringLiteral("Q2Translate — предпросмотр всплывающего окна. "
                       "Проверьте кнопки копирования, озвучивания и перехода в главное окно."),
        QStringLiteral("ru"), QStringLiteral("en"));
}

void OptionsDialog::refreshHotkeyPlaceholders()
{
    const QString empty = Localization::tr(QStringLiteral("Не задано"));
    const auto apply = [&empty](const QHash<QString, QKeySequenceEdit*>& editors) {
        for (auto it = editors.constBegin(); it != editors.constEnd(); ++it) {
            QKeySequenceEdit* editor = it.value();
            if (!editor || !editor->keySequence().isEmpty())
                continue;
            if (auto* line = editor->findChild<QLineEdit*>())
                line->setPlaceholderText(empty);
        }
    };
    apply(m_internalHotkeyEdits);
    apply(m_hotkeyEdits);
}

void OptionsDialog::captureHotkeySnapshot()
{
    m_hotkeySnapshotInternal.clear();
    m_hotkeySnapshotGlobal.clear();
    for (auto it = m_internalHotkeyEdits.constBegin();
         it != m_internalHotkeyEdits.constEnd(); ++it) {
        if (it.value())
            m_hotkeySnapshotInternal.insert(it.key(), it.value()->keySequence());
    }
    for (auto it = m_hotkeyEdits.constBegin(); it != m_hotkeyEdits.constEnd(); ++it) {
        if (it.value())
            m_hotkeySnapshotGlobal.insert(it.key(), it.value()->keySequence());
    }
}

bool OptionsDialog::hotkeysMatchSnapshot() const
{
    for (auto it = m_internalHotkeyEdits.constBegin();
         it != m_internalHotkeyEdits.constEnd(); ++it) {
        if (!it.value())
            continue;
        if (m_hotkeySnapshotInternal.value(it.key()) != it.value()->keySequence())
            return false;
    }
    for (auto it = m_hotkeyEdits.constBegin(); it != m_hotkeyEdits.constEnd(); ++it) {
        if (!it.value())
            continue;
        if (m_hotkeySnapshotGlobal.value(it.key()) != it.value()->keySequence())
            return false;
    }
    return true;
}

bool OptionsDialog::internalHotkeysMatchPreset(const QString& preset) const
{
    const bool classic = preset == QStringLiteral("classic");
    if (!classic && preset != QStringLiteral("modern"))
        return false;
    const PresetHotkey* table = classic ? kClassicHotkeys : kModernHotkeys;
    const int count = classic ? int(sizeof(kClassicHotkeys) / sizeof(kClassicHotkeys[0]))
                              : int(sizeof(kModernHotkeys) / sizeof(kModernHotkeys[0]));
    for (int index = 0; index < count; ++index) {
        const QString key = QString::fromLatin1(table[index].key);
        QKeySequenceEdit* editor = m_internalHotkeyEdits.value(key);
        if (editor && editor->keySequence() != nativeToSequence(table[index].internal))
            return false;
    }
    return true;
}

bool OptionsDialog::hotkeysMatchDefaults() const
{
    for (const HotkeyDefinition& definition : kHotkeys) {
        const QString key = QString::fromLatin1(definition.key);
        if (QKeySequenceEdit* editor = m_internalHotkeyEdits.value(key)) {
            if (editor->keySequence() != nativeToSequence(definition.internalDefault))
                return false;
        }
        if (QKeySequenceEdit* editor = m_hotkeyEdits.value(key)) {
            if (editor->keySequence() != nativeToSequence(definition.globalDefault))
                return false;
        }
    }
    return true;
}

void OptionsDialog::buildHotkeys(QVBoxLayout* layout)
{
    addHint(layout, QStringLiteral(
        "Сочетания регистрируются через RegisterHotKey. Формат значения: "
        "(modifiers << 8) | virtualKey; 0 — не задано."));

    QVBoxLayout* group = nullptr;
    createGroup(layout, QStringLiteral("Глобальные горячие клавиши"), &group);
    auto hotkeyColumns = addTwoColumns(group);
    addCheck(hotkeyColumns.first, QStringLiteral("HotKeys.EnableHotKeys"),
             QStringLiteral("Включить глобальные горячие клавиши"));
    addCheck(hotkeyColumns.second, QStringLiteral("HotKeys.DoubleCtrlShowMain"),
             QStringLiteral("Двойной Ctrl переводит выделенный текст"));

    createGroup(layout, QStringLiteral("Профиль клавиш"), &group);
    auto* presetRow = new QWidget(this);
    auto* presetLayout = new QHBoxLayout(presetRow);
    presetLayout->setContentsMargins(0, 0, 0, 0);
    presetLayout->addWidget(new QLabel(QStringLiteral("Набор"), presetRow));
    m_hotkeyPreset = new DownComboBox(presetRow);
    m_hotkeyPreset->addItem(QStringLiteral("По умолчанию"), QStringLiteral("default"));
    m_hotkeyPreset->addItem(QStringLiteral("Классический (Q2Translate)"),
                            QStringLiteral("classic"));
    m_hotkeyPreset->addItem(QStringLiteral("Современный"), QStringLiteral("modern"));
    m_hotkeyPreset->addItem(QStringLiteral("Пользовательский"), QStringLiteral("user"));
    presetLayout->addWidget(m_hotkeyPreset);
    presetLayout->addStretch(1);
    group->addWidget(presetRow);

    createGroup(layout, QStringLiteral("Назначения"), &group);
    auto* hotkeyTools = new QWidget(this);
    auto* hotkeyToolsLayout = new QHBoxLayout(hotkeyTools);
    hotkeyToolsLayout->setContentsMargins(0, 0, 0, 0);
    auto* clearHotkey = new QPushButton(QStringLiteral("Очистить горячую клавишу"), hotkeyTools);
    auto* hotkeyToolHint = new QLabel(
        QStringLiteral("Выберите поле сочетания и нажмите кнопку очистки."), hotkeyTools);
    hotkeyToolHint->setObjectName(QStringLiteral("MiniNote"));
    hotkeyToolsLayout->addStretch(1);
    hotkeyToolsLayout->addWidget(hotkeyToolHint);
    hotkeyToolsLayout->addWidget(clearHotkey);
    group->addWidget(hotkeyTools);

    m_hotkeyTable = new QTableWidget(this);
    // Global QSS pads table items by 4px vertically; cell widgets inherit that
    // offset, so the editors sat 4px below the row top. For this table the
    // vertical padding is 1px on top and 0 at the bottom: the editor is 1px
    // lower than the row edge and still ends at the row boundary.
    m_hotkeyTable->setStyleSheet(
        QStringLiteral("QTableWidget::item { padding: 1px 6px 0px 6px; }"));
    m_hotkeyTable->setColumnCount(3);
    m_hotkeyTable->setHorizontalHeaderLabels({
        QStringLiteral("Действие"),
        QStringLiteral("Внутри приложения"),
        QStringLiteral("Глобальные")
    });
    m_hotkeyTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_hotkeyTable->setColumnWidth(1, 210);
    m_hotkeyTable->setColumnWidth(2, 210);
    m_hotkeyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_hotkeyTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_hotkeyTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_hotkeyTable->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_hotkeyTable->setTextElideMode(Qt::ElideRight);
    m_hotkeyTable->verticalHeader()->hide();
    m_hotkeyTable->verticalHeader()->setDefaultSectionSize(30);
    m_hotkeyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_hotkeyTable->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_hotkeyTable->setRowCount(int(sizeof(kHotkeys) / sizeof(kHotkeys[0])));
    const auto handleHotkeyEdit = [this] {
        if (!m_loadingHotkeys)
            refreshHotkeyPlaceholders();
    };

    for (int row = 0; row < m_hotkeyTable->rowCount(); ++row) {
        m_hotkeyTable->setRowHeight(row, 32);
        const HotkeyDefinition& definition = kHotkeys[row];
        m_hotkeyTable->setItem(row, 0, new QTableWidgetItem(QString::fromUtf8(definition.label)));

        const auto refreshEditor = [](QKeySequenceEdit* editor) {
            QTimer::singleShot(0, editor, [editor] {
                QLineEdit* line = editor->findChild<QLineEdit*>();
                if (!line)
                    return;
                if (editor->keySequence().isEmpty()) {
                    line->setPlaceholderText(
                        Localization::tr(QStringLiteral("Не задано")));
                    return;
                }
                // Qt calls the Windows key "Meta"; the Windows UI convention is "Win".
                QString text = editor->keySequence().toString(QKeySequence::NativeText);
                text.replace(QStringLiteral("Meta+"), QStringLiteral("Win+"));
                text.replace(QStringLiteral("Return"), QStringLiteral("Enter"));
                line->setText(text);
            });
        };

        auto* internal = new QKeySequenceEdit(m_hotkeyTable);
        internal->setMaximumSequenceLength(1);
        internal->setFixedHeight(30);
        internal->installEventFilter(this);
        m_hotkeyTable->setCellWidget(row, 1, internal);
        refreshEditor(internal);
        if (auto* line = internal->findChild<QLineEdit*>())
            line->installEventFilter(this);
        connect(internal, &QKeySequenceEdit::keySequenceChanged, internal,
                [refreshEditor, internal](const QKeySequence&) { refreshEditor(internal); });

        auto* global = new QKeySequenceEdit(m_hotkeyTable);
        global->setMaximumSequenceLength(1);
        global->setFixedHeight(30);
        global->installEventFilter(this);
        m_hotkeyTable->setCellWidget(row, 2, global);
        refreshEditor(global);
        if (auto* line = global->findChild<QLineEdit*>())
            line->installEventFilter(this);
        connect(global, &QKeySequenceEdit::keySequenceChanged, global,
                [refreshEditor, global](const QKeySequence&) { refreshEditor(global); });

        const QString key = QString::fromLatin1(definition.key);
        m_internalHotkeyEdits.insert(key, internal);
        m_hotkeyEdits.insert(key, global);
        connect(internal, &QKeySequenceEdit::keySequenceChanged, this,
                [this, internal, handleHotkeyEdit](const QKeySequence&) {
                    if (ensureHotkeyUnique(internal))
                        handleHotkeyEdit();
                });
        connect(global, &QKeySequenceEdit::keySequenceChanged, this,
                [this, global, handleHotkeyEdit](const QKeySequence&) {
                    if (ensureHotkeyUnique(global))
                        handleHotkeyEdit();
                });
    }
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (auto* editor = qobject_cast<QKeySequenceEdit*>(now))
            m_lastHotkeyEditor = editor;
        refreshHotkeyPlaceholders();
    });
    captureHotkeySnapshot();
    refreshHotkeyPlaceholders();
    int hotkeyTableHeight = m_hotkeyTable->horizontalHeader()->sizeHint().height();
    for (int row = 0; row < m_hotkeyTable->rowCount(); ++row)
        hotkeyTableHeight += m_hotkeyTable->rowHeight(row);
    m_hotkeyTable->setFixedHeight(hotkeyTableHeight + 2);
    group->addWidget(m_hotkeyTable);

    connect(clearHotkey, &QPushButton::clicked, this, [this] {
        QKeySequenceEdit* editor = m_lastHotkeyEditor;
        if (!editor) {
            QWidget* widget = QApplication::focusWidget();
            while (widget && !editor) {
                editor = qobject_cast<QKeySequenceEdit*>(widget);
                widget = widget->parentWidget();
            }
        }
        if (!editor && m_hotkeyTable) {
            int column = m_hotkeyTable->currentColumn();
            if (column != 1 && column != 2)
                column = 1;
            editor = qobject_cast<QKeySequenceEdit*>(
                m_hotkeyTable->cellWidget(m_hotkeyTable->currentRow(), column));
        }
        if (editor) {
            editor->clear();
            editor->setFocus();
            if (m_hotkeyPreset && !m_loadingHotkeys) {
                const QSignalBlocker blocker(m_hotkeyPreset);
                m_hotkeyPreset->setCurrentIndex(
                    m_hotkeyPreset->findData(QStringLiteral("user")));
                if (m_context && m_context->settings)
                    m_context->settings->setValue(QStringLiteral("HotKeys.Preset"),
                                                  QStringLiteral("user"));
            }
        }
    });
    addHint(layout, QStringLiteral(
        "Внутри приложения - сочетания, работающие в активном окне Q2Translate. "
        "Глобальная - регистрируется через RegisterHotKey. Backspace очищает поле, Esc отменяет захват."));

    connect(m_hotkeyPreset, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_loadingHotkeys)
            return;
        const QString preset = m_hotkeyPreset->currentData().toString();
        if (preset == QStringLiteral("user")) {
            if (m_context && m_context->settings)
                m_context->settings->setValue(QStringLiteral("HotKeys.Preset"),
                                              QStringLiteral("user"));
            return;
        }
        m_loadingHotkeys = true;
        if (preset == QStringLiteral("classic") || preset == QStringLiteral("modern")) {
            const bool classic = preset == QStringLiteral("classic");
            const PresetHotkey* table = classic ? kClassicHotkeys : kModernHotkeys;
            const int count = classic ? int(sizeof(kClassicHotkeys) / sizeof(kClassicHotkeys[0]))
                                      : int(sizeof(kModernHotkeys) / sizeof(kModernHotkeys[0]));
            for (int index = 0; index < count; ++index) {
                const QString key = QString::fromLatin1(table[index].key);
                if (QKeySequenceEdit* editor = m_internalHotkeyEdits.value(key))
                    editor->setKeySequence(nativeToSequence(table[index].internal));
            }
        } else {
            for (const HotkeyDefinition& definition : kHotkeys) {
                const QString key = QString::fromLatin1(definition.key);
                if (QKeySequenceEdit* editor = m_internalHotkeyEdits.value(key))
                    editor->setKeySequence(nativeToSequence(definition.internalDefault));
                if (QKeySequenceEdit* editor = m_hotkeyEdits.value(key))
                    editor->setKeySequence(nativeToSequence(definition.globalDefault));
            }
        }
        m_loadingHotkeys = false;
        captureHotkeySnapshot();
        refreshHotkeyPlaceholders();
        if (m_context && m_context->settings)
            m_context->settings->setValue(QStringLiteral("HotKeys.Preset"), preset);
        scheduleLiveApply();
    });
    layout->addStretch(1);
}
void OptionsDialog::buildPlugins(QVBoxLayout* layout)
{
    addHint(layout, QStringLiteral(
        "Управление установленными сервисами и их доступностью."));

    QVBoxLayout* group = nullptr;
    QWidget* servicesGroup =
        createGroup(layout, QStringLiteral("Установленные сервисы"), &group);
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    auto* installed = new QTreeWidget(split);
    installed->setObjectName(QStringLiteral("ConnectorTree"));
    installed->setMinimumWidth(320);
    installed->setHeaderHidden(true);
    installed->setRootIsDecorated(false);
    installed->setIndentation(14);
    installed->setIconSize(QSize(16, 16));
    installed->setUniformRowHeights(true);

    const auto categoryTitle = [](const QString& type) {
        if (type == QStringLiteral("translate")) return QStringLiteral("Перевод");
        if (type == QStringLiteral("dictionaries")) return QStringLiteral("Словари");
        if (type == QStringLiteral("ocr")) return QStringLiteral("OCR");
        if (type == QStringLiteral("tts")) return QStringLiteral("TTS");
        if (type == QStringLiteral("llm")) return QStringLiteral("LLM");
        if (type == QStringLiteral("spelling")) return QStringLiteral("Орфография");
        return QStringLiteral("Другое");
    };

    QHash<QString, QTreeWidgetItem*> categories;
    if (m_context && m_context->plugins) {
        QList<PluginManifest> manifests = m_context->plugins->manifests();
        const auto categoryRank = [](const QString& type) {
            if (type == QStringLiteral("translate")) return 0;
            if (type == QStringLiteral("llm")) return 1;
            if (type == QStringLiteral("dictionaries")) return 2;
            if (type == QStringLiteral("ocr")) return 3;
            if (type == QStringLiteral("tts")) return 4;
            if (type == QStringLiteral("spelling")) return 5;
            return 6;
        };
        std::sort(manifests.begin(), manifests.end(),
                  [&categoryRank](const PluginManifest& left, const PluginManifest& right) {
            const int leftRank = categoryRank(left.type);
            const int rightRank = categoryRank(right.type);
            if (leftRank != rightRank)
                return leftRank < rightRank;
            const int nameOrder = left.name.localeAwareCompare(right.name);
            if (nameOrder != 0)
                return nameOrder < 0;
            return left.id < right.id;
        });

        for (const PluginManifest& manifest : std::as_const(manifests)) {
            // The balancer is a single built-in proxy; it is configured on the
            // "Дополнительно" page and is not an installable connector.
            if (manifest.id == QStringLiteral("balancer"))
                continue;
            const QString category = categoryTitle(manifest.type);
            QTreeWidgetItem* groupItem = categories.value(category, nullptr);
            if (!groupItem) {
                groupItem = new QTreeWidgetItem(installed);
                groupItem->setText(0, QStringLiteral("▼ ") + category);
                groupItem->setData(0, Qt::UserRole + 1, category);
                groupItem->setFlags((groupItem->flags() | Qt::ItemIsEnabled) & ~Qt::ItemIsSelectable);
                QFont font = groupItem->font(0);
                font.setBold(true);
                groupItem->setFont(0, font);
                categories.insert(category, groupItem);
            }
            auto* item = new QTreeWidgetItem(groupItem);
            item->setText(0, QStringLiteral("%1 - %2").arg(manifest.name, manifest.version));
            if (!manifest.iconPath.isEmpty() && QFileInfo::exists(manifest.iconPath))
                item->setIcon(0, QIcon(manifest.iconPath));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, m_context->plugins->isDisabled(manifest.id)
                                      ? Qt::Unchecked : Qt::Checked);
            item->setData(0, Qt::UserRole, manifest.id);
        }
    }
    installed->expandAll();
    connect(installed, &QTreeWidget::itemClicked, this,
            [](QTreeWidgetItem* item, int) {
        if (item && !item->parent())
            item->setExpanded(!item->isExpanded());
    });
    connect(installed, &QTreeWidget::itemExpanded, this, [](QTreeWidgetItem* item) {
        if (item && !item->parent()) {
            const QString name = item->data(0, Qt::UserRole + 1).toString();
            if (!name.isEmpty())
                item->setText(0, QStringLiteral("▼ ") + name);
        }
    });
    connect(installed, &QTreeWidget::itemCollapsed, this, [](QTreeWidgetItem* item) {
        if (item && !item->parent()) {
            const QString name = item->data(0, Qt::UserRole + 1).toString();
            if (!name.isEmpty())
                item->setText(0, QStringLiteral("▶ ") + name);
        }
    });
    installed->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    installed->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    installed->setMinimumHeight(200);

    auto* detailsHost = new QWidget(split);
    auto* detailsLayout = new QVBoxLayout(detailsHost);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(8);
    m_pluginDetails = new QLabel(detailsHost);
    m_pluginDetails->setObjectName(QStringLiteral("ConnectorCard"));
    m_pluginDetails->setTextFormat(Qt::RichText);
    m_pluginDetails->setWordWrap(true);
    m_pluginDetails->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_pluginDetails->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_pluginDetails->setOpenExternalLinks(true);
    m_pluginDetails->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    detailsLayout->addWidget(m_pluginDetails);

    m_connectorScroll = new QScrollArea(detailsHost);
    m_connectorScroll->setWidgetResizable(true);
    m_connectorScroll->setFrameShape(QFrame::NoFrame);
    m_connectorScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_connectorScroll->setStyleSheet(QStringLiteral(
        "QScrollArea, QScrollArea > QWidget, QScrollArea > QWidget > QWidget {"
        " background: transparent; }"));
    detailsLayout->addWidget(m_connectorScroll, 1);

    split->addWidget(installed);
    split->addWidget(detailsHost);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    group->addWidget(split);

    const auto showPluginDetails = [this, installed] {
        if (!m_pluginDetails || !m_context || !m_context->plugins)
            return;
        QTreeWidgetItem* item = installed->currentItem();
        if (!item || !item->parent()) {
            commitConnectorSettings();
            m_pluginDetails->clear();
            rebuildConnectorForm(nullptr);
            return;
        }
        showConnectorDetails(item->data(0, Qt::UserRole).toString());
    };

    connect(installed, &QTreeWidget::currentItemChanged, this,
            [showPluginDetails](QTreeWidgetItem*, QTreeWidgetItem*) { showPluginDetails(); });
    connect(installed, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        if (!item || !item->parent() || !m_context || !m_context->plugins)
            return;
        m_context->plugins->setDisabled(item->data(0, Qt::UserRole).toString(),
                                        item->checkState(0) != Qt::Checked);
        refreshServiceTables();
        refreshSpellProviders();
        refreshServiceCombos();
    });
    if (installed->topLevelItemCount() > 0) {
        QTreeWidgetItem* first = installed->topLevelItem(0);
        if (first && first->childCount() > 0)
            installed->setCurrentItem(first->child(0));
        showPluginDetails();
    }
    addHint(layout, QStringLiteral(
        "Список отражает сервисы из services/ и Data/services/. "
        "Параметры выбранного сервиса редактируются прямо в карточке."));

    layout->setStretchFactor(servicesGroup, 1);
}

void OptionsDialog::updateConnectorCard(const PluginManifest& manifest)
{
    if (!m_pluginDetails)
        return;

    const QFontMetrics metrics(m_pluginDetails->font());
    const int available = qMax(200, m_pluginDetails->width() - 8);
    const QString folderPrefix = QStringLiteral("Папка: ");

    QString folder = QDir::fromNativeSeparators(manifest.directory);
    const int servicesIndex = folder.indexOf(QStringLiteral("/services/"));
    if (servicesIndex >= 0)
        folder = QStringLiteral("*") + folder.mid(servicesIndex);

    const QString description = manifest.description.simplified();
    const QString folderText = metrics.elidedText(
        folder, Qt::ElideMiddle,
        qMax(80, available - metrics.horizontalAdvance(folderPrefix)));
    const QString typeName = manifest.type == QStringLiteral("spelling")
        ? QStringLiteral("Орфография") : manifest.type;
    const QString link = connectorLink(manifest);
    const QString linkHtml = link.startsWith(QStringLiteral("http://"))
            || link.startsWith(QStringLiteral("https://"))
        ? QStringLiteral("<a href=\"%1\">%1</a>").arg(link.toHtmlEscaped())
        : link.toHtmlEscaped();

    const QString iconUrl = manifest.iconPath.isEmpty()
        ? QString()
        : QUrl::fromLocalFile(manifest.iconPath).toString();
    QString title;
    if (!iconUrl.isEmpty()) {
        title = QStringLiteral(
                    "<img src=\"%1\" width=\"16\" height=\"16\" style=\"vertical-align:middle\">"
                    "&nbsp;<b>%2</b>")
                    .arg(iconUrl.toHtmlEscaped(), manifest.name.toHtmlEscaped());
    } else {
        title = QStringLiteral("<b>%1</b>").arg(manifest.name.toHtmlEscaped());
    }

    const QStringList officialApi = {
        QStringLiteral("deepl"), QStringLiteral("ocr_space"), QStringLiteral("claude"),
        QStringLiteral("deepseek"), QStringLiteral("gemini"), QStringLiteral("grok"),
        QStringLiteral("openai_gpt4o"), QStringLiteral("qwen"), QStringLiteral("pollinations"),
        QStringLiteral("ollama_cloud")
    };
    const QStringList publicApi = {
        QStringLiteral("mymemory"), QStringLiteral("libretranslate"),
        QStringLiteral("wikipedia"), QStringLiteral("online_wiktionary"),
        QStringLiteral("translate_academic"), QStringLiteral("dic_academic")
    };
    const QStringList localServer = {
        QStringLiteral("ollama"), QStringLiteral("lmstudio")
    };
    QString access = manifest.access;
    if (access.isEmpty()) {
        if (manifest.engine == QStringLiteral("native")
            || localServer.contains(manifest.id)) {
            access = QStringLiteral("Локальный");
        } else if (officialApi.contains(manifest.id)) {
            access = QStringLiteral("Официальный API");
        } else if (publicApi.contains(manifest.id)) {
            access = QStringLiteral("Публичный API");
        } else {
            access = QStringLiteral("Веб-эндпоинт (неофициальный)");
        }
    }

    const QString html = QStringLiteral(
        "<div style=\"line-height:118%\">%1<br>"
        "Версия: %2<br>"
        "Тип: %3<br>"
        "Возможности: %4<br>"
        "Движок: %5<br>"
        "Доступ: %6<br>"
        "Ссылка: %7<br>"
        "Описание: %8<br>"
        "Папка: %9</div>")
        .arg(title, manifest.version.toHtmlEscaped(), typeName.toHtmlEscaped(),
             manifest.capabilities.join(QStringLiteral(", ")).toHtmlEscaped(),
             manifest.engine.toHtmlEscaped(), access.toHtmlEscaped(), linkHtml,
             description.toHtmlEscaped(), folderText.toHtmlEscaped());

    m_pluginDetails->setText(html);
}

void OptionsDialog::showConnectorDetails(const QString& id)
{
    if (!m_context || !m_context->plugins)
        return;

    commitConnectorSettings();
    for (const PluginManifest& manifest : m_context->plugins->manifests()) {
        if (manifest.id != id)
            continue;
        updateConnectorCard(manifest);
        // Laid out width is only known after the first pass: rebuild the
        // elided description/folder once more with the final geometry.
        QTimer::singleShot(0, this, [this, id] {
            if (!m_context || !m_context->plugins || !m_pluginDetails)
                return;
            for (const PluginManifest& current : m_context->plugins->manifests()) {
                if (current.id == id) {
                    updateConnectorCard(current);
                    break;
                }
            }
        });
        rebuildConnectorForm(m_context->plugins->provider(id));
        return;
    }
}

void OptionsDialog::rebuildConnectorForm(IPluginProvider* provider)
{
    if (!m_connectorScroll)
        return;

    if (m_connectorNetwork) {
        const QList<QNetworkReply*> replies = m_connectorNetwork->findChildren<QNetworkReply*>();
        for (QNetworkReply* reply : replies) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }

    auto* host = new QWidget;
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    m_connectorScroll->setWidget(host);
    m_connectorFormHost = host;

    m_connectorFields.clear();
    m_connectorFieldDefs.clear();
    m_connectorModelCombos.clear();
    m_connectorModelButtons.clear();
    m_connectorProviderId.clear();
    m_connectorTimeoutSpin = nullptr;

    if (!provider) {
        return;
    }
    m_connectorProviderId = provider->id();

    QJsonObject values = provider->settings();
    if (m_connectorSettingsCache.contains(provider->id()))
        values = m_connectorSettingsCache.value(provider->id());

    const QJsonArray schema = provider->manifest().settingsSchema;

    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(6);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    for (const QJsonValue& value : schema) {
        const QJsonObject field = value.toObject();
        const QString key = field.value(QStringLiteral("key")).toString();
        if (key.isEmpty())
            continue;
        m_connectorFieldDefs.insert(key, field);
        QWidget* editor = createConnectorField(field, values);
        if (!m_connectorFields.contains(key))
            m_connectorFields.insert(key, editor);
        QString label = field.value(QStringLiteral("label")).toString(key);
        if (field.value(QStringLiteral("required")).toBool(false))
            label += QStringLiteral(" *");
        form->addRow(label, editor);
    }

    // Every connector has its own request timeout used by the balancer.
    auto* timeoutHost = new QWidget(host);
    auto* timeoutLayout = new QHBoxLayout(timeoutHost);
    timeoutLayout->setContentsMargins(0, 0, 0, 0);
    timeoutLayout->setSpacing(8);
    m_connectorTimeoutSpin = new QSpinBox(timeoutHost);
    m_connectorTimeoutSpin->setRange(0, 300);
    m_connectorTimeoutSpin->setSuffix(QStringLiteral(" с"));
    m_connectorTimeoutSpin->setMaximumWidth(90);
    if (m_context && m_context->settings) {
        const QJsonObject timeouts =
            m_context->settings->value(QStringLiteral("Balancer.Timeouts")).toObject();
        m_connectorTimeoutSpin->setValue(timeouts.value(provider->id()).toInt());
    }
    timeoutLayout->addWidget(m_connectorTimeoutSpin);
    auto* timeoutHint = new QLabel(QStringLiteral("0 — по умолчанию"), timeoutHost);
    timeoutHint->setObjectName(QStringLiteral("FieldHint"));
    timeoutLayout->addWidget(timeoutHint);
    timeoutLayout->addStretch(1);
    form->addRow(QStringLiteral("Таймаут запроса"), timeoutHost);
    layout->addLayout(form);

    const QString engineRelative = engineComponentRelative(provider->id());
    if (!engineRelative.isEmpty() && m_context && m_context->paths) {
        const QString shippedDir = QDir(QCoreApplication::applicationDirPath())
                                       .filePath(QStringLiteral("services/") + engineRelative);
        const QString userDir = QDir(m_context->paths->dataDir())
                                    .filePath(QStringLiteral("services/") + engineRelative);
        const bool installed = QFileInfo::exists(
            QDir(userDir).filePath(QStringLiteral("manifest.json")));

        auto* engineGroup = new QGroupBox(QStringLiteral("Движок"), host);
        engineGroup->setProperty("_loc_skip", true);
        auto* engineLayout = new QVBoxLayout(engineGroup);
        engineLayout->setContentsMargins(10, 8, 10, 8);
        engineLayout->setSpacing(6);

        auto statusText = [&]() {
            const QString version = installed ? componentVersionAt(userDir)
                                              : componentVersionAt(shippedDir);
            if (installed) {
                return version.isEmpty()
                    ? QStringLiteral("Установлен")
                    : QStringLiteral("Установлен · версия %1").arg(version);
            }
            return version.isEmpty()
                ? QStringLiteral("Не установлен")
                : QStringLiteral("Не установлен · доступна версия %1").arg(version);
        };
        auto* status = new QLabel(statusText(), engineGroup);
        status->setObjectName(QStringLiteral("FieldHint"));
        status->setProperty("_loc_skip", true);
        engineLayout->addWidget(status);

        auto* buttons = new QHBoxLayout();
        buttons->setContentsMargins(0, 0, 0, 0);
        buttons->setSpacing(6);
        auto* download = new QPushButton(QStringLiteral("Скачать"), engineGroup);
        auto* update = new QPushButton(QStringLiteral("Обновить"), engineGroup);
        auto* remove = new QPushButton(QStringLiteral("Удалить"), engineGroup);
        for (QPushButton* button : {download, update, remove})
            button->setProperty("_loc_skip", true);
        download->setEnabled(!installed);
        update->setEnabled(installed);
        remove->setEnabled(installed);
        buttons->addWidget(download);
        buttons->addWidget(update);
        buttons->addWidget(remove);
        buttons->addStretch(1);
        engineLayout->addLayout(buttons);

        auto* hint = new QLabel(
            QStringLiteral("Пакет движка устанавливается в Data/services и подхватывается сразу."),
            engineGroup);
        hint->setObjectName(QStringLiteral("FieldHint"));
        hint->setProperty("_loc_skip", true);
        hint->setWordWrap(true);
        engineLayout->addWidget(hint);
        layout->addWidget(engineGroup);

        const QString providerId = provider->id();
        auto install = [this, providerId, shippedDir, userDir](bool replace) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            QString error;
            bool ok = true;
            if (replace)
                QDir(userDir).removeRecursively();
            ok = copyComponentTree(shippedDir, userDir, &error);
            QApplication::restoreOverrideCursor();
            if (!ok) {
                QMessageBox::warning(this, QStringLiteral("Движок"), error);
                return;
            }
            m_context->plugins->scan();
            rebuildConnectorForm(m_context->plugins->provider(providerId));
        };
        auto uninstall = [this, providerId, userDir] {
            if (QMessageBox::question(this, QStringLiteral("Удалить движок"),
                                      QStringLiteral("Удалить установленный пакет движка?"))
                != QMessageBox::Yes) {
                return;
            }
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const bool removed = QDir(userDir).removeRecursively();
            QApplication::restoreOverrideCursor();
            if (!removed) {
                QMessageBox::warning(
                    this, QStringLiteral("Движок"),
                    QStringLiteral("Не удалось удалить пакет: возможно, файлы используются."));
                return;
            }
            m_context->plugins->scan();
            rebuildConnectorForm(m_context->plugins->provider(providerId));
        };
        connect(download, &QPushButton::clicked, this, [install] { install(false); });
        connect(update, &QPushButton::clicked, this, [install] { install(true); });
        connect(remove, &QPushButton::clicked, this, uninstall);
    }

    layout->addStretch(1);

    const QString providerId = provider->id();
    QTimer::singleShot(0, this, [this, providerId] {
        if (m_connectorProviderId != providerId)
            return;
        for (auto it = m_connectorModelCombos.constBegin();
             it != m_connectorModelCombos.constEnd(); ++it) {
            if (!m_connectorFieldDefs.contains(it.key()))
                continue;
            refreshConnectorModels(m_connectorFieldDefs.value(it.key()), it.value(),
                                   m_connectorModelButtons.value(it.key()), false);
        }
    });
}

QWidget* OptionsDialog::createConnectorField(const QJsonObject& field,
                                             const QJsonObject& values)
{
    const QString key = field.value(QStringLiteral("key")).toString();
    const QString type = field.value(QStringLiteral("type")).toString();
    const QJsonValue current = values.value(key);
    const QJsonValue fallback = field.value(QStringLiteral("default"));

    if (type == QStringLiteral("boolean")) {
        auto* check = new QCheckBox(m_connectorFormHost);
        check->setChecked(current.toBool(fallback.toBool(false)));
        return check;
    }
    if (type == QStringLiteral("models")) {
        auto* host = new QWidget(m_connectorFormHost);
        auto* row = new QHBoxLayout(host);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        auto* combo = new DownComboBox(host);
        combo->setMinimumWidth(160);
        const QString currentValue = current.toString(fallback.toString());
        if (!currentValue.isEmpty())
            combo->addItem(currentValue, currentValue);
        auto* refresh = new QPushButton(QStringLiteral("Обновить"), host);
        row->addWidget(combo, 1);
        row->addWidget(refresh);
        m_connectorModelCombos.insert(key, combo);
        m_connectorModelButtons.insert(key, refresh);
        m_connectorFields.insert(key, combo);
        connect(refresh, &QPushButton::clicked, this, [this, key, combo, refresh] {
            if (!m_connectorFieldDefs.contains(key))
                return;
            refreshConnectorModels(m_connectorFieldDefs.value(key), combo, refresh, true);
        });
        return host;
    }
    if (type == QStringLiteral("xdxf_folders")) {
        auto* combo = new DownComboBox(m_connectorFormHost);
        combo->setMinimumWidth(220);
        combo->addItem(Localization::tr(QStringLiteral("Все словари")), QString());
        QString root;
        if (m_context && m_context->plugins && !m_connectorProviderId.isEmpty()) {
            if (IPluginProvider* provider = m_context->plugins->provider(m_connectorProviderId))
                root = provider->manifest().directory;
        }
        if (!root.isEmpty()) {
            const QDir rootDir(root);
            QSet<QString> seen;
            QStringList folders;
            QDirIterator iterator(root, {QStringLiteral("*.xdxf")}, QDir::Files,
                                  QDirIterator::Subdirectories);
            while (iterator.hasNext()) {
                const QString directory = QFileInfo(iterator.next()).absolutePath();
                const QString relative = QDir::cleanPath(rootDir.relativeFilePath(directory));
                if (relative.isEmpty() || relative == QStringLiteral(".")
                    || seen.contains(relative)) {
                    continue;
                }
                seen.insert(relative);
                folders.append(relative);
            }
            folders.sort(Qt::CaseInsensitive);
            for (const QString& folder : folders)
                combo->addItem(folder, folder);
        }
        const QString currentValue = current.toString(fallback.toString());
        int index = combo->findData(currentValue);
        if (index < 0)
            index = 0;
        combo->setCurrentIndex(index);
        m_connectorFields.insert(key, combo);
        return combo;
    }
    if (type == QStringLiteral("tessdata_languages")) {
        auto* combo = new DownComboBox(m_connectorFormHost);
        QStringList codes;
        if (m_context && m_context->plugins && !m_connectorProviderId.isEmpty()) {
            if (IPluginProvider* provider = m_context->plugins->provider(m_connectorProviderId)) {
                const QDir dir(provider->manifest().tessdataPath);
                const QFileInfoList files =
                    dir.entryInfoList({QStringLiteral("*.traineddata")}, QDir::Files);
                for (const QFileInfo& info : files) {
                    const QString code = info.completeBaseName();
                    if (code.compare(QStringLiteral("osd"), Qt::CaseInsensitive) == 0)
                        continue;
                    codes.append(code);
                }
            }
        }
        codes.sort(Qt::CaseInsensitive);
        const QString currentValue = current.toString(fallback.toString());
        for (const QString& code : codes)
            combo->addItem(code, code);
        int index = combo->findData(currentValue);
        if (index < 0 && !currentValue.isEmpty()) {
            combo->insertItem(0, currentValue, currentValue);
            index = 0;
        }
        if (index >= 0)
            combo->setCurrentIndex(index);
        return combo;
    }
    if (type == QStringLiteral("select")) {
        auto* combo = new DownComboBox(m_connectorFormHost);
        const QJsonArray options = field.value(QStringLiteral("options")).toArray();
        for (const QJsonValue& value : options) {
            const QJsonObject option = value.toObject();
            combo->addItem(option.value(QStringLiteral("label")).toString(),
                           option.value(QStringLiteral("value")).toVariant());
        }
        int index = combo->findData(current.toVariant());
        if (index < 0)
            index = combo->findData(fallback.toVariant());
        combo->setCurrentIndex(qMax(0, index));
        return combo;
    }
    if (type == QStringLiteral("number")) {
        auto* spin = new QDoubleSpinBox(m_connectorFormHost);
        spin->setRange(-1000000.0, 1000000.0);
        spin->setDecimals(3);
        spin->setValue(current.toDouble(fallback.toDouble(0.0)));
        return spin;
    }
    if (type == QStringLiteral("text")) {
        auto* host = new QWidget(m_connectorFormHost);
        auto* column = new QVBoxLayout(host);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(4);
        auto* editor = new QPlainTextEdit(host);
        // Compact prompt editor: exactly four text lines, never grows with the dialog.
        editor->setFixedHeight(editor->fontMetrics().lineSpacing() * 4
                               + 2 * editor->frameWidth() + 12);
        editor->setPlainText(current.toString(fallback.toString()));
        if (field.value(QStringLiteral("variables")).toBool(false)) {
            auto* variables = new QWidget(host);
            auto* variablesLayout = new QHBoxLayout(variables);
            variablesLayout->setContentsMargins(0, 0, 0, 0);
            variablesLayout->setSpacing(4);
            const QList<QPair<QString, QString>> items = {
                {QStringLiteral("{src}"), QStringLiteral("Исходный язык")},
                {QStringLiteral("{dst}"), QStringLiteral("Целевой язык")},
                {QStringLiteral("{text}"), QStringLiteral("Исходный текст")}
            };
            for (const auto& item : items) {
                auto* button = new QPushButton(item.first, variables);
                button->setToolTip(item.second);
                button->setMinimumWidth(58);
                variablesLayout->addWidget(button);
                connect(button, &QPushButton::clicked, editor, [editor, token = item.first] {
                    editor->insertPlainText(token);
                    editor->setFocus();
                });
            }
            variablesLayout->addStretch(1);
            column->addWidget(variables);
        }
        column->addWidget(editor, 0);
        m_connectorFields.insert(key, editor);
        return host;
    }

    auto* edit = new QLineEdit(m_connectorFormHost);
    edit->setText(current.toString(fallback.toString()));
    if (type == QStringLiteral("password"))
        edit->setEchoMode(QLineEdit::Password);
    return edit;
}

void OptionsDialog::commitConnectorSettings()
{
    if (m_connectorProviderId.isEmpty())
        return;

    QJsonObject settings;
    for (auto it = m_connectorFieldDefs.constBegin();
         it != m_connectorFieldDefs.constEnd(); ++it) {
        QWidget* widget = m_connectorFields.value(it.key());
        if (!widget)
            continue;
        const QJsonObject field = it.value();
        const QString type = field.value(QStringLiteral("type")).toString();
        QJsonValue value;
        if (type == QStringLiteral("boolean")) {
            if (auto* check = qobject_cast<QCheckBox*>(widget))
                value = check->isChecked();
        } else if (type == QStringLiteral("models") || type == QStringLiteral("select")
                   || type == QStringLiteral("tessdata_languages")
                   || type == QStringLiteral("xdxf_folders")) {
            if (QComboBox* modelCombo = m_connectorModelCombos.value(it.key()))
                value = QJsonValue::fromVariant(modelCombo->currentData());
            else if (auto* widgetCombo = qobject_cast<QComboBox*>(widget))
                value = QJsonValue::fromVariant(widgetCombo->currentData());
        } else if (type == QStringLiteral("number")) {
            if (auto* spin = qobject_cast<QDoubleSpinBox*>(widget))
                value = spin->value();
        } else if (type == QStringLiteral("text")) {
            if (auto* edit = qobject_cast<QPlainTextEdit*>(widget))
                value = edit->toPlainText();
        } else {
            if (auto* line = qobject_cast<QLineEdit*>(widget))
                value = line->text();
        }
        if (!value.isUndefined())
            settings.insert(it.key(), value);
    }
    m_connectorSettingsCache.insert(m_connectorProviderId, settings);
    if (m_connectorTimeoutSpin && m_context && m_context->settings) {
        QJsonObject timeouts =
            m_context->settings->value(QStringLiteral("Balancer.Timeouts")).toObject();
        const int seconds = m_connectorTimeoutSpin->value();
        if (seconds > 0)
            timeouts.insert(m_connectorProviderId, seconds);
        else
            timeouts.remove(m_connectorProviderId);
        m_context->settings->setValue(QStringLiteral("Balancer.Timeouts"), timeouts);
    }
}

void OptionsDialog::applyBalancerPage()
{
    if (!m_context || !m_context->settings)
        return;
    SettingsManager* settings = m_context->settings;
    QJsonArray fallbacks;
    for (const auto& pair : std::as_const(m_balancerFallbackChecks)) {
        if (pair.first && pair.first->isChecked() && pair.first->isVisible())
            fallbacks.append(pair.second);
    }
    settings->setValue(QStringLiteral("Balancer.Fallbacks"), fallbacks);
}

void OptionsDialog::applyConnectorSettings()
{
    commitConnectorSettings();
    if (!m_context || !m_context->plugins)
        return;
    for (auto it = m_connectorSettingsCache.constBegin();
         it != m_connectorSettingsCache.constEnd(); ++it) {
        if (IPluginProvider* provider = m_context->plugins->provider(it.key())) {
            if (provider->settings() != it.value())
                provider->updateSettings(it.value());
        }
    }
}

void OptionsDialog::refreshConnectorModels(const QJsonObject& field, QComboBox* combo,
                                           QPushButton* button, bool notify)
{
    if (!combo || !m_connectorNetwork)
        return;

    const QString endpointKey =
        field.value(QStringLiteral("endpoint_key")).toString(QStringLiteral("endpoint"));
    QString endpoint;
    if (auto* edit = qobject_cast<QLineEdit*>(m_connectorFields.value(endpointKey)))
        endpoint = edit->text().trimmed();
    if (endpoint.isEmpty() && m_connectorFieldDefs.contains(endpointKey)) {
        endpoint = m_connectorFieldDefs.value(endpointKey)
                       .value(QStringLiteral("default")).toString();
    }

    QString base = endpoint;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    const QString suffix = QStringLiteral("/chat/completions");
    if (base.endsWith(suffix))
        base.chop(suffix.size());
    if (base.isEmpty())
        base = QStringLiteral("http://127.0.0.1:1234/v1");
    if (!base.endsWith(QStringLiteral("/v1")))
        base += QStringLiteral("/v1");

    QNetworkRequest request(QUrl(base + QStringLiteral("/models")));
    request.setTransferTimeout(8000);
    QString apiKey;
    if (auto* keyEdit = qobject_cast<QLineEdit*>(m_connectorFields.value(QStringLiteral("api_key"))))
        apiKey = keyEdit->text().trimmed();
    if (!apiKey.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());

    if (button)
        button->setEnabled(false);
    QNetworkReply* reply = m_connectorNetwork->get(request);
    QPointer<QComboBox> comboGuard(combo);
    QPointer<QPushButton> buttonGuard(button);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, comboGuard, buttonGuard, notify] {
        reply->deleteLater();
        if (buttonGuard)
            buttonGuard->setEnabled(true);
        if (!comboGuard)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            if (notify) {
                QMessageBox::warning(this, QStringLiteral("Модели"),
                                     QStringLiteral("Не удалось получить список моделей:\n%1")
                                         .arg(reply->errorString()));
            }
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject object = document.object();
        QStringList identifiers;
        const QJsonArray rows = object.value(QStringLiteral("data")).toArray();
        for (const QJsonValue& value : rows) {
            const QString id = value.toObject().value(QStringLiteral("id")).toString();
            if (!id.isEmpty())
                identifiers.append(id);
        }
        if (identifiers.isEmpty()) {
            const QJsonArray models = object.value(QStringLiteral("models")).toArray();
            for (const QJsonValue& value : models) {
                const QString id = value.toObject().value(QStringLiteral("name")).toString();
                if (!id.isEmpty())
                    identifiers.append(id);
            }
        }
        if (identifiers.isEmpty())
            return;

        const QString currentValue = comboGuard->currentData().toString();
        comboGuard->clear();
        for (const QString& id : identifiers)
            comboGuard->addItem(id, id);
        int index = comboGuard->findData(currentValue);
        if (index < 0 && !currentValue.isEmpty()) {
            comboGuard->insertItem(0, currentValue, currentValue);
            index = 0;
        }
        if (index >= 0)
            comboGuard->setCurrentIndex(index);
    });
}

void OptionsDialog::buildNetwork(QVBoxLayout* layout)
{
    addHint(layout, QStringLiteral(
        "Сетевые параметры применяются к HTTP-мосту сервисов."));

    QVBoxLayout* group = nullptr;
    createGroup(layout, QStringLiteral("Прокси"), &group);
    addCheck(group, QStringLiteral("Proxy.SendThroughProxy"),
             QStringLiteral("Отправлять запросы через прокси-сервер"));
    auto proxyTop = addTwoColumns(group);
    addCombo(proxyTop.first, QStringLiteral("Proxy.Scheme"),
             QStringLiteral("Схема"),
             optionsFrom({
                 {QStringLiteral("http"), QStringLiteral("http")},
                 {QStringLiteral("https"), QStringLiteral("https")},
                 {QStringLiteral("socks5"), QStringLiteral("socks5")}
             }));
    addCombo(proxyTop.second, QStringLiteral("Proxy.ProxyType"),
             QStringLiteral("Тип прокси"),
             optionsFrom({
                 {QStringLiteral("Без прокси"), 0},
                 {QStringLiteral("Системный"), 1},
                 {QStringLiteral("Вручную"), 3}
             }));
    auto proxyAddress = addTwoColumns(group);
    addLine(proxyAddress.first, QStringLiteral("Proxy.Host"), QStringLiteral("Хост"));
    addNumber(proxyAddress.second, QStringLiteral("Proxy.Port"), QStringLiteral("Порт"), 0, 65535, 1);
    auto proxyAuth = addTwoColumns(group);
    addLine(proxyAuth.first, QStringLiteral("Proxy.Username"), QStringLiteral("Пользователь"));
    addLine(proxyAuth.second, QStringLiteral("Proxy.Password"), QStringLiteral("Пароль"), true);
    auto* testRow = new QWidget(this);
    auto* testLayout = new QHBoxLayout(testRow);
    testLayout->setContentsMargins(0, 0, 0, 0);
    auto* testButton = new QPushButton(QStringLiteral("Проверить соединение"), testRow);
    m_networkStatus = new QLabel(QStringLiteral("Не проверено"), testRow);
    m_networkStatus->setObjectName(QStringLiteral("MiniNote"));
    testLayout->addWidget(testButton);
    testLayout->addWidget(m_networkStatus);
    testLayout->addStretch(1);
    group->addWidget(testRow);
    connect(testButton, &QPushButton::clicked, this, &OptionsDialog::testNetwork);

    createGroup(layout, QStringLiteral("Соединение"), &group);
    auto connectionTop = addTwoColumns(group);
    addNumber(connectionTop.first, QStringLiteral("Internet.Timeout"),
              QStringLiteral("Таймаут запроса"), 5, 120, 1000, Localization::tr(QStringLiteral(" сек")));
    addNumber(connectionTop.second, QStringLiteral("Internet.ConnectTimeout"),
              QStringLiteral("Таймаут соединения"), 1, 60, 1000, Localization::tr(QStringLiteral(" сек")));
    auto connectionBottom = addTwoColumns(group);
    addNumber(connectionBottom.first, QStringLiteral("Internet.DataTimeout"),
              QStringLiteral("Таймаут данных"), 1, 60, 1000, Localization::tr(QStringLiteral(" сек")));
    addNumber(connectionBottom.second, QStringLiteral("Internet.RetryDelay"),
              QStringLiteral("Задержка между повторами"), 0, 30, 1,
              Localization::tr(QStringLiteral(" сек")));

    createGroup(layout, QStringLiteral("Повтор неудачных запросов"), &group);
    addCheck(group, QStringLiteral("Internet.RetryFailed"),
             QStringLiteral("Повторять неудачные запросы"));
    addNumber(group, QStringLiteral("Internet.RetryCount"),
              QStringLiteral("Количество повторов"), 0, 10, 1);
    layout->addStretch(1);
}
void OptionsDialog::buildExceptions(QVBoxLayout* layout)
{
    addHint(layout, QStringLiteral(
        "Исключения работают с глобальными хоткеями и mouse mode. "
        "Формат записи: [программа, класс окна]."));

    QVBoxLayout* group = nullptr;
    createGroup(layout, QStringLiteral("Режим"), &group);
    m_exceptionsModeOff = new QRadioButton(QStringLiteral("Не работать в исключениях"), this);
    m_exceptionsModeOn = new QRadioButton(QStringLiteral("Работать только в исключениях"), this);
    group->addWidget(m_exceptionsModeOff);
    group->addWidget(m_exceptionsModeOn);

    createGroup(layout, QStringLiteral("Список исключений"), &group);
    m_exceptionsTable = new QTableWidget(this);
    m_exceptionsTable->setColumnCount(3);
    m_exceptionsTable->setHorizontalHeaderLabels({
        QStringLiteral("Программа"), QStringLiteral("Класс окна"), QString()
    });
    m_exceptionsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_exceptionsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_exceptionsTable->verticalHeader()->hide();
    m_exceptionsTable->setMaximumHeight(220);
    group->addWidget(m_exceptionsTable);
    auto* addRow = new QWidget(this);
    auto* addLayout = new QHBoxLayout(addRow);
    addLayout->setContentsMargins(0, 0, 0, 0);
    m_exceptionProcess = new QLineEdit(addRow);
    m_exceptionProcess->setPlaceholderText(QStringLiteral("program.exe"));
    m_exceptionClass = new QLineEdit(addRow);
    m_exceptionClass->setPlaceholderText(QStringLiteral("WindowClass"));
    auto* addButton = new QPushButton(QStringLiteral("+ Добавить"), addRow);
    auto* currentWindow = new QPushButton(QStringLiteral("Текущее окно"), addRow);
    addLayout->addWidget(m_exceptionProcess);
    addLayout->addWidget(m_exceptionClass);
    addLayout->addWidget(addButton);
    addLayout->addWidget(currentWindow);
    group->addWidget(addRow);
    connect(addButton, &QPushButton::clicked, this, [this] {
        m_context->exceptions->addEntry(m_exceptionProcess->text(),
                                        m_exceptionClass->text(), false);
        refreshExceptions();
    });
    connect(currentWindow, &QPushButton::clicked, this, [this] {
        const WindowUtils::ForegroundInfo info = WindowUtils::foregroundWindow();
        m_exceptionProcess->setText(info.processName);
        m_exceptionClass->setText(info.windowClass);
    });
    layout->addStretch(1);
}

void OptionsDialog::buildAdvanced(QVBoxLayout* layout)
{
    addHint(layout, QStringLiteral(
        "Дополнительные параметры интерфейса и перевода."));

    QVBoxLayout* group = nullptr;
    createGroup(layout, QStringLiteral("Поведение"), &group);
    auto behaviorColumns = addTwoColumns(group);
    addCheck(behaviorColumns.first, QStringLiteral("Advanced.EnableMouseModeOnCtrl"),
             QStringLiteral("Mouse mode только по нажатому Ctrl"));
    addCheck(behaviorColumns.second,
             QStringLiteral("Advanced.CopyTranslationToClipboard"),
             QStringLiteral("Автоматически копировать перевод в буфер обмена"));

    createGroup(layout, QStringLiteral("Текст и звук"), &group);
    auto textSound = addTwoColumns(group);
    addCheck(textSound.first, QStringLiteral("Advanced.EnableSlowerListening"),
             QStringLiteral("Замедленное озвучивание (ttsspeed)"));
    addCheck(textSound.second, QStringLiteral("Advanced.LayoutIndicator"),
             QStringLiteral("Показывать индикатор языка"));

    createGroup(layout, QStringLiteral("Захват экрана"), &group);
    auto captureColumns = addTwoColumns(group);
    addCombo(captureColumns.first, QStringLiteral("Advanced.ScreenCaptureMode"),
             QStringLiteral("Режим выделения"),
             optionsFrom({
                 {QStringLiteral("С панелью инструментов"), QStringLiteral("toolbar")},
                 {QStringLiteral("Автоматическое действие"), QStringLiteral("auto")}
             }));
    addCombo(captureColumns.second, QStringLiteral("Advanced.ScreenCaptureAction"),
             QStringLiteral("Автоматическое действие"),
             optionsFrom({
                 {QStringLiteral("Скопировать"), QStringLiteral("copy")},
                 {QStringLiteral("Сохранить"), QStringLiteral("save")},
                 {QStringLiteral("Распознать"), QStringLiteral("recognize")},
                 {QStringLiteral("Перевести"), QStringLiteral("translate")}
             }));

    // Balancer: a proxy connector that walks through the enabled connectors
    // when the primary one fails or times out.
    QVBoxLayout* balancerGroup = nullptr;
    createGroup(layout, QStringLiteral("Балансировщик"), &balancerGroup);
    addHint(balancerGroup, QStringLiteral(
        "Основной коннектор используется первым; при ошибке или таймауте "
        "балансировщик по очереди пробует отмеченные резервные коннекторы."));

    QList<QPair<QString, QVariant>> balancerOptions;
    if (m_context && m_context->plugins) {
        for (const PluginManifest& manifest : m_context->plugins->manifests()) {
            if (!manifest.valid || manifest.id == QStringLiteral("balancer"))
                continue;
            if (m_context->plugins->isDisabled(manifest.id))
                continue;
            if (!manifest.capabilities.contains(QStringLiteral("translate"))
                && !manifest.capabilities.contains(QStringLiteral("llm")))
                continue;
            balancerOptions.append(qMakePair(manifest.name, QVariant(manifest.id)));
        }
    }

    auto* balancerHost = new QWidget(this);
    auto* balancerForm = new QFormLayout(balancerHost);
    balancerForm->setContentsMargins(0, 0, 0, 0);
    balancerForm->setHorizontalSpacing(12);
    balancerForm->setVerticalSpacing(10);
    balancerForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    auto* primaryCombo = new QComboBox(balancerHost);
    primaryCombo->setMinimumWidth(300);
    for (const auto& option : std::as_const(balancerOptions))
        primaryCombo->addItem(option.first, option.second);
    m_fields.append({primaryCombo, QStringLiteral("Balancer.Primary"), 1, 1});
    balancerForm->addRow(new QLabel(QStringLiteral("Основной коннектор"), balancerHost),
                         primaryCombo);

    auto* fallbacksHost = new QWidget(balancerHost);
    auto* fallbacksLayout = new QVBoxLayout(fallbacksHost);
    fallbacksLayout->setContentsMargins(0, 0, 0, 0);
    fallbacksLayout->setSpacing(2);
    const QJsonArray savedFallbacks = m_context && m_context->settings
        ? m_context->settings->value(QStringLiteral("Balancer.Fallbacks")).toArray()
        : QJsonArray();
    m_balancerFallbackChecks.clear();
    for (const auto& option : std::as_const(balancerOptions)) {
        const QString id = option.second.toString();
        auto* check = new QCheckBox(option.first, fallbacksHost);
        check->setChecked(savedFallbacks.contains(QJsonValue(id)));
        fallbacksLayout->addWidget(check);
        m_balancerFallbackChecks.append(qMakePair(check, id));
    }
    const auto syncFallbacks = [this, primaryCombo] {
        const QString primary = primaryCombo->currentData().toString();
        for (const auto& pair : std::as_const(m_balancerFallbackChecks)) {
            const bool isPrimary = pair.second == primary;
            pair.first->setVisible(!isPrimary);
        }
    };
    connect(primaryCombo, &QComboBox::currentIndexChanged, this,
            [syncFallbacks](int) { syncFallbacks(); });
    syncFallbacks();
    balancerForm->addRow(new QLabel(QStringLiteral("Резервные коннекторы"), balancerHost),
                         fallbacksHost);

    balancerGroup->addWidget(balancerHost);

    auto* balancerSwitches = new QWidget(this);
    auto* switchesGrid = new QGridLayout(balancerSwitches);
    switchesGrid->setContentsMargins(0, 0, 0, 0);
    switchesGrid->setHorizontalSpacing(16);
    switchesGrid->setVerticalSpacing(8);
    switchesGrid->setColumnStretch(0, 1);
    switchesGrid->setColumnStretch(1, 1);

    auto* autoSwitch = new QCheckBox(
        QStringLiteral("Автоматически переключаться на следующий коннектор при ошибке"),
        balancerSwitches);
    m_fields.append({autoSwitch, QStringLiteral("Balancer.AutoSwitch"), 0, 1});
    connect(autoSwitch, &QCheckBox::toggled, this, &OptionsDialog::updateDependencies);
    switchesGrid->addWidget(autoSwitch, 0, 0);

    const auto addTiming = [this](QWidget* parent, const QString& label,
                                  const QString& key, int minimum, int maximum,
                                  const QString& suffix) {
        auto* host = new QWidget(parent);
        auto* row = new QHBoxLayout(host);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto* caption = new QLabel(label, host);
        auto* spin = new QSpinBox(host);
        spin->setRange(minimum, maximum);
        spin->setSuffix(suffix);
        spin->setMinimumWidth(90);
        row->addWidget(caption, 0, Qt::AlignVCenter);
        row->addWidget(spin);
        m_fields.append({spin, key, 2, 1});
        return host;
    };

    switchesGrid->addWidget(
        addTiming(balancerSwitches, QStringLiteral("Общий таймаут коннектора"),
                  QStringLiteral("Balancer.TimeoutSeconds"), 5, 300, QStringLiteral(" с")),
        0, 1);

    auto* returnSwitch = new QCheckBox(QStringLiteral("Возвращаться на исходный коннектор"),
                                       balancerSwitches);
    m_fields.append({returnSwitch, QStringLiteral("Balancer.ReturnToPrimary"), 0, 1});
    connect(returnSwitch, &QCheckBox::toggled, this, &OptionsDialog::updateDependencies);
    switchesGrid->addWidget(returnSwitch, 1, 0);
    switchesGrid->addWidget(
        addTiming(balancerSwitches, QStringLiteral("Вернуться через"),
                  QStringLiteral("Balancer.ReturnAfterMinutes"), 1, 1440,
                  QStringLiteral(" мин")),
        1, 1);
    balancerGroup->addWidget(balancerSwitches);

    layout->addStretch(1);
}
void OptionsDialog::buildUpdates(QVBoxLayout* layout)
{
    auto* aboutLead = new QLabel(Localization::tr(
        QStringLiteral("Q2Translate — портативный переводчик для Windows на Qt 6.")), this);
    aboutLead->setObjectName(QStringLiteral("AboutLead"));
    aboutLead->setWordWrap(true);
    layout->addWidget(aboutLead);

    QVBoxLayout* group = nullptr;
    createGroup(layout, QStringLiteral("Версия"), &group);
    auto* versionRow = new QWidget(this);
    auto* versionLayout = new QHBoxLayout(versionRow);
    versionLayout->setContentsMargins(0, 0, 0, 0);
    versionLayout->addWidget(new QLabel(QStringLiteral("Текущая версия"), versionRow));
    auto* version = new QLabel(QApplication::applicationVersion(), versionRow);
    version->setStyleSheet(QStringLiteral("font-weight:600;"));
    versionLayout->addWidget(version);
    versionLayout->addStretch(1);
    group->addWidget(versionRow);

    createGroup(layout, QStringLiteral("Что умеет программа"), &group);
    const QStringList features = {
        Localization::tr(QStringLiteral(
            "• Перевод текста через 50+ коннекторов: Google, Bing, DeepL, Yandex, "
            "Reverso, локальные LLM и другие.")),
        Localization::tr(QStringLiteral(
            "• Автоопределение языка, правила перевода и быстрая смена целевого языка.")),
        Localization::tr(QStringLiteral(
            "• Всплывающее окно по двойному Ctrl или Ctrl+перетаскиванию выделения; "
            "стили, прозрачность и положение у курсора.")),
        Localization::tr(QStringLiteral(
            "• Распознавание текста с экрана: Tesseract (несколько языков) и "
            "Windows OCR (один язык).")),
        Localization::tr(QStringLiteral(
            "• Перевод документов: DOCX, PPTX, PDF, FB2, EPUB, XLIFF/TS, JSON, XML, "
            "YAML, субтитры, CSV, INI, Markdown, TXT и HTML.")),
        Localization::tr(QStringLiteral(
            "• Результат сохраняется рядом с исходным файлом с суффиксом целевого "
            "языка, оригинал не изменяется.")),
        Localization::tr(QStringLiteral(
            "• Словари XDXF, история переводов и проверка орфографии Hunspell, "
            "Nuspell или Windows.")),
        Localization::tr(QStringLiteral(
            "• Озвучивание через Windows SAPI, Google, Yandex SpeechKit и Azure; "
            "ввод текста с микрофона.")),
        Localization::tr(QStringLiteral(
            "• Глобальные горячие клавиши, исключения приложений, темы с расписанием "
            "и плотность интерфейса.")),
        Localization::tr(QStringLiteral(
            "• Прокси, таймауты и повторы запросов; переносимый профиль Data рядом "
            "с программой.")),
        Localization::tr(QStringLiteral(
            "• Коннекторы на JavaScript выполняются через QJSEngine — Python не требуется."))
    };
    auto* featuresLabel = new QLabel(features.join(QLatin1Char('\n')), this);
    featuresLabel->setObjectName(QStringLiteral("AboutFeatures"));
    featuresLabel->setWordWrap(true);
    featuresLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    group->addWidget(featuresLabel);

    createGroup(layout, QStringLiteral("Параметры запуска"), &group);
    const QList<QPair<QString, QString>> launchParameters = {
        {QStringLiteral("--translate <текст>"),
         QStringLiteral("Перевести текст и показать результат в главном окне.")},
        {QStringLiteral("--set-source <текст>"),
         QStringLiteral("Вставить текст в поле источника без запуска перевода.")},
        {QStringLiteral("--translate-document <файл>"),
         QStringLiteral("Перевести документ (PDF, DJVU, DOCX, FB2, EPUB и др.) и "
                        "сохранить результат рядом с исходным файлом.")},
        {QStringLiteral("--translate-image <файл> [цель] [источник]"),
         QStringLiteral("Перевести изображение (по умолчанию ru/auto) и сохранить "
                        "<имя>.<язык>.png.")},
        {QStringLiteral("--image-provider <id>"),
         QStringLiteral("Движок OCR для изображения: paddleocr, tesseract_local, hybrid.")},
        {QStringLiteral("--image-layout-dump <json>"),
         QStringLiteral("Сохранить разметку OCR/блоков и маски для диагностики качества.")},
        {QStringLiteral("--show-batch [файлы…]"),
         QStringLiteral("Открыть пакетный перевод документов, при необходимости с файлами.")},
        {QStringLiteral("--show-options"),
         QStringLiteral("Открыть окно настроек.")},
        {QStringLiteral("--show-dictionary"),
         QStringLiteral("Открыть словарь.")},
        {QStringLiteral("--show-history"),
         QStringLiteral("Открыть историю переводов.")},
        {QStringLiteral("--start-ocr"),
         QStringLiteral("Запустить распознавание текста с экрана.")},
        {QStringLiteral("--show-popup"),
         QStringLiteral("Показать всплывающее окно перевода.")},
        {QStringLiteral("--popup-text <текст>"),
         QStringLiteral("Показать указанный текст во всплывающем окне.")},
        {QStringLiteral("--selection-test"),
         QStringLiteral("Перевести текущее выделение (как двойной Ctrl).")},
        {QStringLiteral("--ocr-languages"),
         QStringLiteral("Открыть менеджер языков Tesseract.")},
        {QStringLiteral("--plugin-settings <id>"),
         QStringLiteral("Открыть настройки конкретного коннектора, например bing_translate.")},
        {QStringLiteral("--probe-connectors [фильтр]"),
         QStringLiteral("Проверить коннекторы и записать результат в "
                        "Data/connector_probe.log; поддерживает --probe-text, "
                        "--probe-from, --probe-to.")},
        {QStringLiteral("--tts-test [id] [текст]"),
         QStringLiteral("Проверить озвучивание, результат в Data/tts_probe.log.")},
        {QStringLiteral("--http-test [url]"),
         QStringLiteral("Проверить сеть/прокси и записать результат в лог.")},
        {QStringLiteral("--selftest"),
         QStringLiteral("Самотест (коннекторы, OCR, перевод), результат в Data/selftest.log.")},
    };
    const auto applyTableStyle = [](QWidget* host) {
        host->setObjectName(QStringLiteral("LaunchTable"));
        host->setStyleSheet(QStringLiteral(
            "#LaunchTable { border: 1px solid rgba(128,128,128,180); }"
            "#LaunchTable QLabel { border-right: 1px solid rgba(128,128,128,110);"
            " border-bottom: 1px solid rgba(128,128,128,110); padding: 4px 8px; }"
            "#LaunchTable QLabel#LaunchHeader {"
            " background: rgba(128,128,128,45); font-weight: 600; }"
            "#LaunchTable QLabel[mono=\"true\"] {"
            " font-family: Consolas,'Courier New',monospace; }"));
    };
    const auto addTableCell = [](QGridLayout* grid, int row, int column,
                                 const QString& text, bool header, bool mono) {
        auto* label = new QLabel(text, grid->parentWidget());
        label->setWordWrap(true);
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse
                                       | Qt::TextSelectableByKeyboard);
        label->setProperty("_loc_skip", true);
        label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        if (header)
            label->setObjectName(QStringLiteral("LaunchHeader"));
        if (mono)
            label->setProperty("mono", true);
        grid->addWidget(label, row, column);
    };

    auto* parametersHost = new QWidget(this);
    auto* parametersGrid = new QGridLayout(parametersHost);
    parametersGrid->setContentsMargins(0, 0, 0, 0);
    parametersGrid->setHorizontalSpacing(0);
    parametersGrid->setVerticalSpacing(0);
    parametersGrid->setColumnMinimumWidth(0, 300);
    parametersGrid->setColumnStretch(1, 1);
    addTableCell(parametersGrid, 0, 0, QStringLiteral("Параметр"), true, false);
    addTableCell(parametersGrid, 0, 1, QStringLiteral("Назначение"), true, false);
    for (int row = 0; row < launchParameters.size(); ++row) {
        addTableCell(parametersGrid, row + 1, 0, launchParameters.at(row).first, false, true);
        addTableCell(parametersGrid, row + 1, 1, launchParameters.at(row).second, false, false);
    }
    applyTableStyle(parametersHost);
    group->addWidget(parametersHost);

    group->addWidget(new QLabel(QStringLiteral("Примеры запуска"), this));
    const QList<QPair<QString, QString>> launchExamples = {
        {QStringLiteral("Q2Translate.exe --translate \"Hello, world!\""),
         QStringLiteral("Перевести текст и показать результат в главном окне.")},
        {QStringLiteral("Q2Translate.exe --translate-document "
                        "\"C:\\Books\\book.pdf\""),
         QStringLiteral("Перевести PDF и сохранить результат рядом с исходным файлом.")},
        {QStringLiteral("Q2Translate.exe --show-batch \"C:\\Books\\a.pdf\" "
                        "\"C:\\Books\\b.djvu\""),
         QStringLiteral("Открыть пакетный перевод с двумя документами.")},
        {QStringLiteral("Q2Translate.exe --translate-image "
                        "\"C:\\Pages\\page.png\" ru en --image-provider paddleocr "
                        "--image-layout-dump \"C:\\Temp\\dump.json\""),
         QStringLiteral("Перевести изображение с PaddleOCR и сохранить разметку "
                        "в dump.json.")},
        {QStringLiteral("Q2Translate.exe --translate-image "
                        "\"C:\\Pages\\scan.png\" ru auto --image-provider "
                        "tesseract_local"),
         QStringLiteral("Перевести скан через Tesseract, язык определить автоматически.")},
        {QStringLiteral("Q2Translate.exe --probe-connectors bing,google "
                        "--probe-text \"hello\" --probe-from en --probe-to ru"),
         QStringLiteral("Проверить коннекторы и записать отчёт в connector_probe.log.")},
        {QStringLiteral("Q2Translate.exe --plugin-settings bing_translate"),
         QStringLiteral("Открыть настройки коннектора Bing.")},
        {QStringLiteral("Q2Translate.exe --show-popup --popup-text "
                        "\"Текст для попапа\""),
         QStringLiteral("Показать всплывающее окно с заданным текстом.")},
        {QStringLiteral("Q2Translate.exe --tts-test google_tts \"Hello, world!\""),
         QStringLiteral("Проверить озвучивание, результат в tts_probe.log.")},
        {QStringLiteral("Q2Translate.exe --http-test https://example.com"),
         QStringLiteral("Проверить сеть и прокси.")},
        {QStringLiteral("Q2Translate.exe --selftest"),
         QStringLiteral("Запустить самотест, результат в selftest.log.")},
    };
    auto* examplesHost = new QWidget(this);
    auto* examplesGrid = new QGridLayout(examplesHost);
    examplesGrid->setContentsMargins(0, 0, 0, 0);
    examplesGrid->setHorizontalSpacing(0);
    examplesGrid->setVerticalSpacing(0);
    examplesGrid->setColumnMinimumWidth(0, 420);
    examplesGrid->setColumnStretch(1, 1);
    addTableCell(examplesGrid, 0, 0, QStringLiteral("Команда"), true, false);
    addTableCell(examplesGrid, 0, 1, QStringLiteral("Что делает"), true, false);
    for (int row = 0; row < launchExamples.size(); ++row) {
        addTableCell(examplesGrid, row + 1, 0, launchExamples.at(row).first, false, true);
        addTableCell(examplesGrid, row + 1, 1, launchExamples.at(row).second, false, false);
    }
    applyTableStyle(examplesHost);
    group->addWidget(examplesHost);

    layout->addStretch(1);
}

void OptionsDialog::loadAll()
{
    // Profiles created before the dropdown only have the boolean
    // General.MinimizeToTrayOnMinimize; seed the new key from it once.
    if (m_context && m_context->settings
        && m_context->settings->stringValue(QStringLiteral("General.MinimizeAction")).isEmpty()) {
        m_context->settings->setValue(
            QStringLiteral("General.MinimizeAction"),
            m_context->settings->boolValue(QStringLiteral("General.MinimizeToTrayOnMinimize"), false)
                ? QStringLiteral("tray") : QStringLiteral("taskbar"));
    }
    // Same for the close action: the old checkbox wrote Layout.CloseAction only
    // on save, so a profile that was never saved needs it seeded here.
    if (m_context && m_context->settings
        && m_context->settings->stringValue(QStringLiteral("Layout.CloseAction")).isEmpty()) {
        m_context->settings->setValue(
            QStringLiteral("Layout.CloseAction"),
            m_context->settings->boolValue(QStringLiteral("General.MinimizeToTrayOnClose"), true)
                ? QStringLiteral("tray") : QStringLiteral("exit"));
    }
    // OCR engines per file type: there is no shared "general OCR" field any
    // more, so seed the intended defaults for profiles that still have an
    // empty value.
    if (m_context && m_context->settings) {
        const QList<QPair<QString, QString>> ocrDefaults = {
            {QStringLiteral("Ocr.ProviderScreen"), QStringLiteral("tesseract_local")},
            {QStringLiteral("Ocr.ProviderImages"), QStringLiteral("paddleocr")},
            {QStringLiteral("Ocr.ProviderPdf"), QStringLiteral("tesseract_local")},
            {QStringLiteral("Ocr.ProviderDjvu"), QStringLiteral("tesseract_local")}
        };
        for (const auto& entry : ocrDefaults) {
            if (m_context->settings->stringValue(entry.first).isEmpty())
                m_context->settings->setValue(entry.first, entry.second);
        }
    }

    // Seeded defaults for the service switches. Dictionary, summarizer and
    // rewriter are off; spell checking is on.
    if (m_context && m_context->settings) {
        const QList<QPair<QString, bool>> switchDefaults = {
            {QStringLiteral("ServiceProviders.spellOn"), true},
            {QStringLiteral("ServiceProviders.dictOn"), false},
            {QStringLiteral("ServiceProviders.summOn"), false},
            {QStringLiteral("ServiceProviders.rewrOn"), false}
        };
        for (const auto& entry : switchDefaults) {
            if (!m_context->settings->value(entry.first).isBool())
                m_context->settings->setValue(entry.first, entry.second);
        }
    }
    if (m_context && m_context->settings) {
        const QString legacyTts = m_context->settings->stringValue(
            QStringLiteral("ServiceProviders.tts"));
        const QString mappedTts = TtsService::normalizedProviderId(legacyTts);
        if (!mappedTts.isEmpty() && mappedTts != legacyTts)
            m_context->settings->setValue(QStringLiteral("ServiceProviders.tts"),
                                          mappedTts);
    }
    loadGeneric();
    refreshSpellProviders();
    refreshServiceCombos();
    loadPopupStyleFontSizes();

    if (m_fromCombo && m_toCombo) {
        const QString from = languageSettingValue(
            m_context->settings, QStringLiteral("General.LanguageFrom"), QStringLiteral("en"));
        const QString to = languageSettingValue(
            m_context->settings, QStringLiteral("General.LanguageTo"), QStringLiteral("ru"));
        if (const int index = m_fromCombo->findData(from); index >= 0)
            m_fromCombo->setCurrentIndex(index);
        if (const int index = m_toCombo->findData(to); index >= 0)
            m_toCombo->setCurrentIndex(index);
    }

    refreshLanguageLists();
    refreshServiceTables();
    refreshExceptions();

    if (m_ocrProvider) {
        const QString provider = m_context->settings->stringValue(QStringLiteral("Ocr.Provider"));
        if (const int index = m_ocrProvider->findData(provider); index >= 0)
            m_ocrProvider->setCurrentIndex(index);
    }
    refreshOcrLanguages();
    if (m_ocrLanguage) {
        m_ocrLanguage->setSelected(
            m_context->settings->stringValue(QStringLiteral("Ocr.OcrLanguage"),
                                             QStringLiteral("eng"))
                .split(QLatin1Char('+'), Qt::SkipEmptyParts));
    }
    if (m_saveImagePath)
        m_saveImagePath->setText(
            m_context->settings->stringValue(QStringLiteral("Ocr.SaveImagePath")));

    const QString storedPreset =
        m_context->settings->stringValue(QStringLiteral("HotKeys.Preset"),
                                        QStringLiteral("user"));
    m_loadingHotkeys = true;
    for (const HotkeyDefinition& definition : kHotkeys) {
        const QString key = QString::fromLatin1(definition.key);
        const int global = m_context->settings->intValue(
            QStringLiteral("HotKeys.") + key, definition.globalDefault);
        const int internal = m_context->settings->intValue(
            QStringLiteral("HotKeys.Internal.") + key, definition.internalDefault);
        if (QKeySequenceEdit* editor = m_hotkeyEdits.value(key))
            editor->setKeySequence(nativeToSequence(global));
        if (QKeySequenceEdit* editor = m_internalHotkeyEdits.value(key))
            editor->setKeySequence(nativeToSequence(internal));
    }
    m_loadingHotkeys = false;

    QString effectivePreset = QStringLiteral("user");
    if (storedPreset == QStringLiteral("default") && hotkeysMatchDefaults()) {
        effectivePreset = QStringLiteral("default");
    } else if ((storedPreset == QStringLiteral("classic")
                || storedPreset == QStringLiteral("modern"))
               && internalHotkeysMatchPreset(storedPreset)) {
        effectivePreset = storedPreset;
    }
    if (m_hotkeyPreset) {
        const QSignalBlocker blocker(m_hotkeyPreset);
        const int index = m_hotkeyPreset->findData(effectivePreset);
        if (index >= 0)
            m_hotkeyPreset->setCurrentIndex(index);
    }
    captureHotkeySnapshot();
    refreshHotkeyPlaceholders();

    refreshThemeSchedule();
    applyDensity();
    updateDependencies();
}

void OptionsDialog::loadGeneric()
{
    if (!m_context || !m_context->settings)
        return;
    SettingsManager* settings = m_context->settings;
    for (const Field& field : std::as_const(m_fields)) {
        switch (field.kind) {
        case 0:
            if (auto* check = qobject_cast<QCheckBox*>(field.widget))
                check->setChecked(settings->boolValue(field.key, false));
            break;
        case 1:
            if (auto* combo = qobject_cast<QComboBox*>(field.widget)) {
                const QJsonValue value = settings->value(field.key);
                int index = combo->findData(value.toVariant());
                if (index < 0)
                    index = combo->findData(value.toString());
                if (index < 0 && value.isDouble())
                    index = combo->findData(value.toInt());
                if (index < 0 && field.key == QStringLiteral("Appearance.FontHinting")) {
                    const QString hint = value.toString();
                    if (hint.isEmpty() || hint == QStringLiteral("default"))
                        index = combo->findData(QStringLiteral("full"));
                }
                if (index >= 0)
                    combo->setCurrentIndex(index);
            }
            break;
        case 2:
            if (auto* spin = qobject_cast<QSpinBox*>(field.widget))
                spin->setValue(settings->intValue(field.key, spin->value())
                               / qMax(1, field.scale));
            break;
        case 3:
            if (auto* line = qobject_cast<QLineEdit*>(field.widget))
                line->setText(settings->stringValue(field.key));
            break;
        case 5:
            if (auto* spin = qobject_cast<QDoubleSpinBox*>(field.widget))
                spin->setValue(settings->value(field.key).toDouble(spin->value()));
            break;
        case 4:
            if (auto* button = qobject_cast<QPushButton*>(field.widget)) {
                QString hex = settings->stringValue(field.key, QStringLiteral("ffffff"));
                if (hex.startsWith(QLatin1Char('#')))
                    hex = hex.mid(1);
                button->setProperty("hex", hex);
                button->setText(QStringLiteral("#") + hex);
                const QColor color(QStringLiteral("#") + hex);
                button->setStyleSheet(QStringLiteral("background:#%1; color:%2;")
                                          .arg(hex, color.lightness() > 128 ? QStringLiteral("#111")
                                                                           : QStringLiteral("#fff")));
            }
            break;
        }
    }
    updateDependencies();
}

QString OptionsDialog::appearanceKey() const
{
    if (!m_context || !m_context->settings)
        return QString();
    SettingsManager* settings = m_context->settings;
    QString effectiveTheme =
        settings->stringValue(QStringLiteral("Appearance.ThemeName"),
                              QStringLiteral("Fusion Light"));
    const QString scheduledTheme = scheduledThemeName(
        settings->value(QStringLiteral("Appearance.ThemeSchedule")).toArray(),
        QTime::currentTime());
    if (!scheduledTheme.isEmpty())
        effectiveTheme = scheduledTheme;

    return QStringList{
        effectiveTheme,
        settings->stringValue(QStringLiteral("Appearance.UiFont")),
        QString::number(settings->intValue(QStringLiteral("Appearance.UiFontSize"), 13)),
        settings->boolValue(QStringLiteral("Appearance.FontAntialiasing"), true)
            ? QStringLiteral("1") : QStringLiteral("0"),
        settings->stringValue(QStringLiteral("Appearance.FontHinting"), QStringLiteral("full")),
        settings->stringValue(QStringLiteral("Appearance.EditorFont")),
        QString::number(settings->intValue(QStringLiteral("Appearance.EditorFontSize"), 14))
    }.join(QLatin1Char('|'));
}

void OptionsDialog::applyAppearanceSettings()
{
    if (!m_context || !m_context->settings)
        return;
    SettingsManager* settings = m_context->settings;
    const QString appearance = appearanceKey();
    if (appearance == m_appliedAppearanceKey)
        return;
    m_appliedAppearanceKey = appearance;

    QString effectiveTheme =
        settings->stringValue(QStringLiteral("Appearance.ThemeName"),
                              QStringLiteral("Fusion Light"));
    const QString scheduledTheme = scheduledThemeName(
        settings->value(QStringLiteral("Appearance.ThemeSchedule")).toArray(),
        QTime::currentTime());
    if (!scheduledTheme.isEmpty())
        effectiveTheme = scheduledTheme;

    ThemeManager::apply(effectiveTheme);
    QFont font = QApplication::font();
    font.setFamily(settings->stringValue(QStringLiteral("Appearance.UiFont"), font.family()));
    font.setPointSize(settings->intValue(QStringLiteral("Appearance.UiFontSize"), 13));
    FontTuning::apply(font,
                      settings->boolValue(QStringLiteral("Appearance.FontAntialiasing"), true),
                      settings->stringValue(QStringLiteral("Appearance.FontHinting"),
                                            QStringLiteral("full")));
    QApplication::setFont(font);
    qApp->setStyleSheet(qApp->styleSheet()
        + QStringLiteral("QWidget{font-family:'%1';font-size:%2px;}"
                         "QTextEdit#SourceEdit{font-family:'%3';font-size:%4px;}"
                         "QTextEdit#TargetEdit{font-family:'%3';font-size:%5px;}")
              .arg(settings->stringValue(QStringLiteral("Appearance.UiFont"),
                                         QStringLiteral("Segoe UI")))
              .arg(settings->intValue(QStringLiteral("Appearance.UiFontSize"), 13))
              .arg(settings->stringValue(QStringLiteral("Appearance.EditorFont"),
                                         QStringLiteral("Segoe UI")))
              .arg(settings->intValue(QStringLiteral("Appearance.EditorFontSize"), 14))
              .arg(settings->intValue(QStringLiteral("Appearance.EditorFontSize"), 14) + 1));
}

void OptionsDialog::applyAll()
{
    applyGeneric();
    applyLanguages();
    applyServices();
    applyHotkeys();
    applyConnectorSettings();
    applyBalancerPage();
    applyPopupStyleFontSizes();

    if (m_exceptionsModeOff && m_exceptionsModeOn)
        m_context->exceptions->setDisabledMode(m_exceptionsModeOn->isChecked());

    if (m_ocrLanguage)
        m_context->settings->setValue(QStringLiteral("Ocr.OcrLanguage"), m_ocrLanguage->currentText());
    if (m_saveImagePath)
        m_context->settings->setValue(QStringLiteral("Ocr.SaveImagePath"), m_saveImagePath->text());

    applyStartup();
    applyThemeSchedule();

    m_context->settings->setAutosaveEnabled(true);
    m_context->settings->save();
    m_context->settings->setAutosaveEnabled(false);
    applyAppearanceSettings();

    // Apply commits the current state: a later Cancel (or the window close
    // button) must only revert edits made after this point, not settings the
    // user has already confirmed with Apply.
    m_initialSettings = m_context->settings->root();
    m_initialProviderSettings.clear();
    if (m_context->plugins) {
        const QList<IPluginProvider*> providers = m_context->plugins->providers();
        for (IPluginProvider* provider : providers) {
            if (provider)
                m_initialProviderSettings.insert(provider->id(), provider->settings());
        }
    }

    if (m_context->router)
        m_context->router->reload();
    if (m_context->hotkeys)
        m_context->hotkeys->reload();
    emit settingsApplied();
}

void OptionsDialog::applyLiveSettings()
{
    if (!m_liveApplyEnabled || m_restoringSettings || !m_context || !m_context->settings)
        return;

    applyGeneric();
    applyLanguages();
    applyServices();
    applyHotkeys();
    applyThemeSchedule();

    if (m_ocrLanguage)
        m_context->settings->setValue(QStringLiteral("Ocr.OcrLanguage"), m_ocrLanguage->currentText());
    if (m_saveImagePath)
        m_context->settings->setValue(QStringLiteral("Ocr.SaveImagePath"), m_saveImagePath->text());

    applyAppearanceSettings();
    if (m_context->router)
        m_context->router->reload();
    if (m_context->hotkeys)
        m_context->hotkeys->reload();
    emit settingsApplied();
}

void OptionsDialog::scheduleLiveApply()
{
    if (!m_liveApplyEnabled || m_restoringSettings || !m_liveApplyTimer)
        return;
    m_liveApplyTimer->start();
}

void OptionsDialog::connectLiveApply()
{
    for (const Field& field : std::as_const(m_fields)) {
        QWidget* widget = field.widget;
        if (!widget)
            continue;
        if (auto* check = qobject_cast<QCheckBox*>(widget))
            connect(check, &QCheckBox::toggled, this, &OptionsDialog::scheduleLiveApply);
        else if (auto* combo = qobject_cast<QComboBox*>(widget))
            connect(combo, &QComboBox::currentIndexChanged,
                    this, &OptionsDialog::scheduleLiveApply);
        else if (auto* spin = qobject_cast<QSpinBox*>(widget))
            connect(spin, &QSpinBox::valueChanged, this, &OptionsDialog::scheduleLiveApply);
        else if (auto* line = qobject_cast<QLineEdit*>(widget))
            connect(line, &QLineEdit::textChanged, this, &OptionsDialog::scheduleLiveApply);
        else if (auto* button = qobject_cast<QPushButton*>(widget))
            connect(button, &QPushButton::clicked, this, &OptionsDialog::scheduleLiveApply);
    }

    if (m_scheduleTable) {
        connect(m_scheduleTable->model(), &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex&, const QModelIndex&, const QList<int>&) {
            scheduleLiveApply();
        });
    }
    for (auto it = m_hotkeyEdits.constBegin(); it != m_hotkeyEdits.constEnd(); ++it) {
        connect(it.value(), &QKeySequenceEdit::keySequenceChanged, this,
                &OptionsDialog::scheduleLiveApply);
    }
    for (auto it = m_internalHotkeyEdits.constBegin();
         it != m_internalHotkeyEdits.constEnd(); ++it) {
        connect(it.value(), &QKeySequenceEdit::keySequenceChanged, this,
                &OptionsDialog::scheduleLiveApply);
    }
    if (m_rulesList) {
        connect(m_rulesList->model(), &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex&, int, int) { scheduleLiveApply(); });
        connect(m_rulesList->model(), &QAbstractItemModel::rowsRemoved, this,
                [this](const QModelIndex&, int, int) { scheduleLiveApply(); });
    }
    if (m_ocrLanguage) {
        connect(m_ocrLanguage, &QComboBox::currentTextChanged, this,
                [this](const QString&) { scheduleLiveApply(); });
        connect(m_ocrLanguage, &MultiSelectComboBox::selectionChanged, this,
                &OptionsDialog::scheduleLiveApply);
    }
    if (m_saveImagePath) {
        connect(m_saveImagePath, &QLineEdit::textChanged, this,
                [this](const QString&) { scheduleLiveApply(); });
    }
}

void OptionsDialog::restoreInitialSettings()
{
    if (!m_context || !m_context->settings || m_initialSettings.isEmpty())
        return;

    m_liveApplyEnabled = false;
    m_restoringSettings = true;
    m_context->settings->setRoot(m_initialSettings);
    m_appliedAppearanceKey.clear();

    if (m_context->plugins) {
        for (auto it = m_initialProviderSettings.constBegin();
             it != m_initialProviderSettings.constEnd(); ++it) {
            IPluginProvider* provider = m_context->plugins->provider(it.key());
            if (provider && provider->settings() != it.value())
                provider->updateSettings(it.value());
        }
    }

    m_context->settings->setAutosaveEnabled(true);
    m_context->settings->save();
    m_context->settings->setAutosaveEnabled(false);
    applyAppearanceSettings();
    if (m_context->router)
        m_context->router->reload();
    if (m_context->hotkeys)
        m_context->hotkeys->reload();
    emit settingsApplied();
    m_restoringSettings = false;
}

bool OptionsDialog::hasPendingChanges() const
{
    if (!m_context)
        return false;
    if (m_context->settings && m_context->settings->root() != m_initialSettings)
        return true;
    if (m_context->plugins) {
        const QList<IPluginProvider*> providers = m_context->plugins->providers();
        for (IPluginProvider* provider : providers) {
            if (!provider)
                continue;
            const auto it = m_initialProviderSettings.constFind(provider->id());
            if (it != m_initialProviderSettings.constEnd()
                && provider->settings() != it.value()) {
                return true;
            }
        }
    }
    return false;
}

bool OptionsDialog::ensureHotkeyUnique(QKeySequenceEdit* editor)
{
    if (!editor || m_loadingHotkeys)
        return true;
    const QKeySequence sequence = editor->keySequence();
    if (sequence.isEmpty())
        return true;

    QKeySequenceEdit* duplicate = nullptr;
    QString duplicateKey;
    QString ownKey;
    bool ownInternal = false;
    for (auto it = m_hotkeyEdits.constBegin(); it != m_hotkeyEdits.constEnd(); ++it) {
        if (it.value() == editor) {
            ownKey = it.key();
            break;
        }
    }
    if (ownKey.isEmpty()) {
        for (auto it = m_internalHotkeyEdits.constBegin();
             it != m_internalHotkeyEdits.constEnd(); ++it) {
            if (it.value() == editor) {
                ownKey = it.key();
                ownInternal = true;
                break;
            }
        }
    }
    const auto scan = [&](const QHash<QString, QKeySequenceEdit*>& edits) {
        for (auto it = edits.constBegin(); it != edits.constEnd(); ++it) {
            if (it.key() == ownKey)
                continue;
            if (it.value() != editor && it.value()
                && it.value()->keySequence() == sequence) {
                duplicate = it.value();
                duplicateKey = it.key();
                return;
            }
        }
    };
    scan(m_hotkeyEdits);
    if (!duplicate)
        scan(m_internalHotkeyEdits);
    if (!duplicate)
        return true;

    QString label = duplicateKey;
    for (const HotkeyDefinition& definition : kHotkeys) {
        if (QString::fromLatin1(definition.key) == duplicateKey) {
            label = QString::fromUtf8(definition.label);
            break;
        }
    }
    QMessageBox::warning(
        this, QStringLiteral("Горячие клавиши"),
        Localization::tr(QStringLiteral("Сочетание %1 уже назначено: %2."))
            .arg(sequence.toString(QKeySequence::NativeText), label));

    const QSignalBlocker blocker(editor);
    const QKeySequence previous = ownInternal
        ? m_hotkeySnapshotInternal.value(ownKey)
        : m_hotkeySnapshotGlobal.value(ownKey);
    editor->setKeySequence(previous);
    refreshHotkeyPlaceholders();
    return false;
}

bool OptionsDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (!m_loadingHotkeys && m_hotkeyPreset
        && (event->type() == QEvent::KeyPress
            || event->type() == QEvent::ShortcutOverride)) {
        QKeySequenceEdit* editor = qobject_cast<QKeySequenceEdit*>(watched);
        if (!editor) {
            if (auto* line = qobject_cast<QLineEdit*>(watched))
                editor = qobject_cast<QKeySequenceEdit*>(line->parentWidget());
        }
        if (editor) {
            const QSignalBlocker blocker(m_hotkeyPreset);
            m_hotkeyPreset->setCurrentIndex(
                m_hotkeyPreset->findData(QStringLiteral("user")));
            if (m_context && m_context->settings) {
                m_context->settings->setValue(QStringLiteral("HotKeys.Preset"),
                                              QStringLiteral("user"));
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void OptionsDialog::reject()
{    if (!hasPendingChanges()) {
        QDialog::reject();
        return;
    }
    // Hide the window before restoring so Cancel feels instant even though
    // reloading the whole dialog takes a moment.
    hide();
    restoreInitialSettings();
    QDialog::reject();
}

void OptionsDialog::applyGeneric()
{
    if (!m_context || !m_context->settings)
        return;
    SettingsManager* settings = m_context->settings;
    for (const Field& field : std::as_const(m_fields)) {
        switch (field.kind) {
        case 0:
            if (auto* check = qobject_cast<QCheckBox*>(field.widget))
                settings->setValue(field.key, check->isChecked());
            break;
        case 1:
            if (auto* combo = qobject_cast<QComboBox*>(field.widget))
                settings->setValue(field.key, QJsonValue::fromVariant(combo->currentData()));
            break;
        case 2:
            if (auto* spin = qobject_cast<QSpinBox*>(field.widget))
                settings->setValue(field.key, spin->value() * field.scale);
            break;
        case 3:
            if (auto* line = qobject_cast<QLineEdit*>(field.widget))
                settings->setValue(field.key, line->text());
            break;
        case 5:
            if (auto* spin = qobject_cast<QDoubleSpinBox*>(field.widget))
                settings->setValue(field.key, spin->value());
            break;
        case 4:
            if (auto* button = qobject_cast<QPushButton*>(field.widget))
                settings->setValue(field.key, button->property("hex").toString());
            break;
        }
    }
}

QList<LanguageInfo> OptionsDialog::selectedFavoriteLanguages() const
{
    QSet<int> ids;
    for (auto it = m_pinnedChecks.constBegin(); it != m_pinnedChecks.constEnd(); ++it) {
        if (it.value()->isChecked())
            ids.insert(it.key());
    }

    QList<LanguageInfo> result;
    for (const LanguageInfo& language : allLanguages()) {
        if (language.id == 1)
            continue;
        if (ids.isEmpty() || ids.contains(language.id))
            result.append(language);
    }
    std::sort(result.begin(), result.end(),
              [](const LanguageInfo& left, const LanguageInfo& right) {
        return QString::localeAwareCompare(QString::fromUtf8(left.name),
                                           QString::fromUtf8(right.name)) < 0;
    });
    return result;
}

void OptionsDialog::rebuildLanguageCombos()
{
    const QList<LanguageInfo> languages = selectedFavoriteLanguages();
    const auto refill = [&languages](QComboBox* combo, bool intData, bool includeAuto) {
        if (!combo)
            return;
        const QVariant current = combo->currentData();
        combo->clear();
        if (includeAuto) {
            combo->addItem(languageName(1), intData ? QVariant(1)
                                                    : QVariant(languageCode(1)));
        }
        for (const LanguageInfo& language : languages) {
            combo->addItem(QString::fromUtf8(language.name),
                           intData ? QVariant(language.id)
                                   : QVariant(QString::fromLatin1(language.code)));
        }
        int index = current.isValid() ? combo->findData(current) : -1;
        if (index < 0 && combo->count() > 0)
            index = 0;
        if (index >= 0)
            combo->setCurrentIndex(index);
    };

    refill(m_fromCombo, false, true);
    refill(m_toCombo, false, false);
    refill(m_ruleFrom, true, true);
    refill(m_ruleTo, true, false);
}

void OptionsDialog::refreshLanguageLists()
{
    if (!m_context || !m_context->settings)
        return;
    SettingsManager* settings = m_context->settings;
    const QJsonArray pinned = settings->value(QStringLiteral("PinnedLanguages")).toArray();
    QSet<int> pinnedIds;
    for (const QJsonValue& value : pinned)
        pinnedIds.insert(value.toInt());
    for (auto it = m_pinnedChecks.constBegin(); it != m_pinnedChecks.constEnd(); ++it) {
        const QSignalBlocker blocker(it.value());
        it.value()->setChecked(pinnedIds.contains(it.key()));
    }
    rebuildLanguageCombos();

    if (m_rulesList) {
        m_rulesList->clear();
        const QJsonArray rules = settings->value(QStringLiteral("TranslationRules")).toArray();
        for (const QJsonValue& value : rules) {
            const QJsonArray pair = value.toArray();
            if (pair.size() < 2)
                continue;
            const int from = pair.at(0).toInt();
            const int to = pair.at(1).toInt();
            auto* item = new QListWidgetItem(
                QStringLiteral("%1 → %2").arg(languageNameById(from), languageNameById(to)),
                m_rulesList);
            item->setData(Qt::UserRole, QVariantList{from, to});
        }
    }
}

void OptionsDialog::applyLanguages()
{
    if (!m_context || !m_context->settings)
        return;
    SettingsManager* settings = m_context->settings;
    if (m_fromCombo && m_toCombo) {
        settings->setValue(QStringLiteral("General.LanguageFrom"),
                           m_fromCombo->currentData().toString());
        settings->setValue(QStringLiteral("General.LanguageTo"),
                           m_toCombo->currentData().toString());
    }

    QJsonArray pinned;
    for (auto it = m_pinnedChecks.constBegin(); it != m_pinnedChecks.constEnd(); ++it) {
        if (it.value()->isChecked())
            pinned.append(it.key());
    }
    settings->setValue(QStringLiteral("PinnedLanguages"), pinned);

    QJsonArray rules;
    for (int i = 0; m_rulesList && i < m_rulesList->count(); ++i) {
        const QVariantList pair = m_rulesList->item(i)->data(Qt::UserRole).toList();
        if (pair.size() < 2)
            continue;
        QJsonArray values;
        values.append(pair.at(0).toInt());
        values.append(pair.at(1).toInt());
        rules.append(values);
    }
    settings->setValue(QStringLiteral("TranslationRules"), rules);
}

void OptionsDialog::refreshServiceTables()
{
    if (!m_context || !m_context->plugins)
        return;

    const QList<PluginManifest> manifests = m_context->plugins->manifests();
    QList<PluginManifest> translators;
    QSet<QString> translationIds;
    for (const PluginManifest& manifest : manifests) {
        if (manifest.type == QStringLiteral("translate")) {
            translators.append(manifest);
            translationIds.insert(manifest.id);
        }
    }

    QStringList source = m_serviceOrder;
    if (source.isEmpty()) {
        const QJsonArray saved = m_context->settings->value(QStringLiteral("ServicesOrder")).toArray();
        for (const QJsonValue& value : saved)
            source.append(value.toString());
    }

    QStringList ordered;
    QSet<QString> seen;
    for (const QString& id : std::as_const(source)) {
        if (!translationIds.contains(id) || seen.contains(id))
            continue;
        seen.insert(id);
        ordered.append(id);
    }

    QList<PluginManifest> missing;
    for (const PluginManifest& manifest : std::as_const(translators)) {
        if (!seen.contains(manifest.id))
            missing.append(manifest);
    }
    std::sort(missing.begin(), missing.end(),
              [](const PluginManifest& left, const PluginManifest& right) {
        return left.name.localeAwareCompare(right.name) < 0;
    });
    for (const PluginManifest& manifest : std::as_const(missing))
        ordered.append(manifest.id);

    m_serviceOrder = ordered;
    refreshOrderTable();
}

void OptionsDialog::fitTableHeight(QTableWidget* table)
{
    if (!table)
        return;
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->verticalHeader()->setDefaultSectionSize(30);
    const int height = table->horizontalHeader()->sizeHint().height()
        + table->rowCount() * 30 + 6;
    table->setFixedHeight(qMax(60, height));
}
void OptionsDialog::refreshOrderTable()
{
    if (!m_orderTable || !m_context || !m_context->plugins)
        return;
    const QList<PluginManifest> manifests = m_context->plugins->manifests();
    m_orderTable->setRowCount(m_serviceOrder.size());
    for (int row = 0; row < m_serviceOrder.size(); ++row) {
        const QString id = m_serviceOrder.at(row);
        const auto it = std::find_if(manifests.begin(), manifests.end(),
                                     [&](const PluginManifest& manifest) { return manifest.id == id; });
        if (it == manifests.end())
            continue;
        const PluginManifest& manifest = *it;
        m_orderTable->setItem(row, 0, new QTableWidgetItem(manifest.name));
        m_orderTable->setItem(row, 1, new QTableWidgetItem(manifest.capabilities.join(QLatin1Char(','))));
        auto* controls = new QWidget(m_orderTable);
        auto* controlsLayout = new QHBoxLayout(controls);
        controlsLayout->setContentsMargins(0, 0, 0, 0);
        controlsLayout->setSpacing(2);
        auto* up = new QPushButton(QString::fromUtf8("▲"), controls);
        auto* down = new QPushButton(QString::fromUtf8("▼"), controls);
        up->setFixedSize(36, 20);
        down->setFixedSize(36, 20);
        up->setStyleSheet(QStringLiteral("padding:0; min-height:0; font-size:12px;"));
        down->setStyleSheet(QStringLiteral("padding:0; min-height:0; font-size:12px;"));
        up->setToolTip(QStringLiteral("Выше"));
        down->setToolTip(QStringLiteral("Ниже"));
        controlsLayout->addWidget(up);
        controlsLayout->addWidget(down);
        m_orderTable->setCellWidget(row, 2, controls);
        connect(up, &QPushButton::clicked, this, [this, row] {
            if (row <= 0)
                return;
            m_serviceOrder.swapItemsAt(row, row - 1);
            refreshOrderTable();
        });
        connect(down, &QPushButton::clicked, this, [this, row] {
            if (row + 1 >= m_serviceOrder.size())
                return;
            m_serviceOrder.swapItemsAt(row, row + 1);
            refreshOrderTable();
        });
    }
    m_orderTable->setColumnWidth(0, 216);
    m_orderTable->setColumnWidth(2, 96);
    m_orderTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_orderTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    fitTableHeight(m_orderTable);
}

void OptionsDialog::applyServices()
{
    if (!m_context || !m_context->plugins)
        return;
    SettingsManager* settings = m_context->settings;
    QJsonArray order;
    if (m_orderTable) {
        for (const QString& id : m_serviceOrder)
            order.append(id);
    }
    settings->setValue(QStringLiteral("ServicesOrder"), order);
}

void OptionsDialog::applyHotkeys()
{
    if (!m_context || !m_context->settings)
        return;
    for (const HotkeyDefinition& definition : kHotkeys) {
        const QString key = QString::fromLatin1(definition.key);
        if (QKeySequenceEdit* editor = m_hotkeyEdits.value(key)) {
            m_context->settings->setValue(QStringLiteral("HotKeys.") + key,
                                          sequenceToNative(editor->keySequence()));
        }
        if (QKeySequenceEdit* editor = m_internalHotkeyEdits.value(key)) {
            m_context->settings->setValue(QStringLiteral("HotKeys.Internal.") + key,
                                          sequenceToNative(editor->keySequence()));
        }
    }
    if (m_hotkeyPreset)
        m_context->settings->setValue(QStringLiteral("HotKeys.Preset"),
                                      m_hotkeyPreset->currentData().toString());
}

void OptionsDialog::applyStartup()
{
    if (!m_context || !m_context->settings)
        return;
    const bool enabled = m_context->settings->boolValue(QStringLiteral("General.RunAtStartup"), false);
    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                  QSettings::NativeFormat);
    if (enabled) {
        const QString path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        run.setValue(QStringLiteral("Q2Translate"), QStringLiteral("\"%1\"").arg(path));
    } else {
        run.remove(QStringLiteral("Q2Translate"));
    }
}

void OptionsDialog::refreshExceptions()
{
    if (!m_exceptionsTable || !m_context || !m_context->exceptions)
        return;
    const auto entries = m_context->exceptions->disabledEntries();
    m_exceptionsTable->setRowCount(entries.size());
    for (int row = 0; row < entries.size(); ++row) {
        m_exceptionsTable->setItem(row, 0, new QTableWidgetItem(entries.at(row).first));
        m_exceptionsTable->setItem(row, 1, new QTableWidgetItem(entries.at(row).second));
        auto* remove = new QPushButton(QStringLiteral("Удалить"), m_exceptionsTable);
        m_exceptionsTable->setCellWidget(row, 2, remove);
        connect(remove, &QPushButton::clicked, this, [this, row] {
            m_context->exceptions->removeEntry(row, false);
            refreshExceptions();
        });
    }
    m_exceptionsModeOff->setChecked(!m_context->exceptions->disabledMode());
    m_exceptionsModeOn->setChecked(m_context->exceptions->disabledMode());
}

void OptionsDialog::refreshOcrLanguages()
{
    if (!m_ocrLanguage || !m_context || !m_context->translation)
        return;
    QString provider = m_ocrProvider ? m_ocrProvider->currentData().toString() : QString();
    if (provider.isEmpty() && m_context->settings)
        provider = m_context->settings->stringValue(QStringLiteral("Ocr.Provider"));
    if (provider.isEmpty())
        provider = QStringLiteral("tesseract_local");
    const bool singleLanguage = provider == QStringLiteral("windows_ocr");
    m_ocrLanguage->setSingleSelection(singleLanguage);
    m_ocrLanguage->setToolTip(singleLanguage
        ? Localization::tr(QStringLiteral("Windows OCR использует только один язык."))
        : QString());
    const QString current = m_ocrLanguage->currentText();
    QStringList languages = m_context->translation->ocrLanguages(provider);
    languages.removeAll(QStringLiteral("osd"));
    languages.sort(Qt::CaseInsensitive);
    m_ocrLanguage->setItems(languages);

    QStringList selected = current.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    if (selected.isEmpty() && m_context->settings) {
        selected = m_context->settings
                       ->stringValue(QStringLiteral("Ocr.OcrLanguage"),
                                     QStringLiteral("eng"))
                       .split(QLatin1Char('+'), Qt::SkipEmptyParts);
    }
    m_ocrLanguage->setSelected(selected);
    if (m_ocrLanguage->selected().isEmpty() && languages.contains(QStringLiteral("eng")))
        m_ocrLanguage->setSelected({QStringLiteral("eng")});

    int widest = 0;
    const QFontMetrics metrics(m_ocrLanguage->font());
    for (const QString& language : m_ocrLanguage->selected())
        widest = qMax(widest, metrics.horizontalAdvance(language));
    m_ocrLanguage->setMinimumWidth(qMax(160, int((widest + 56) * 1.3)));
}

void OptionsDialog::browseOcrLanguages()
{
    if (!m_context || !m_context->plugins)
        return;
    IPluginProvider* provider = m_context->plugins->provider(QStringLiteral("tesseract_local"));
    if (!provider)
        provider = m_context->plugins->provider(
            m_ocrProvider ? m_ocrProvider->currentData().toString() : QString());
    if (!provider) {
        QMessageBox::warning(this, QStringLiteral("Языки OCR"),
                             QStringLiteral("Сервис Tesseract не найден."));
        return;
    }
    QString tessdata = provider->manifest().tessdataPath;
    if (tessdata.isEmpty()) {
        tessdata = QDir(QDir(m_context->paths->shippedDir())
                            .filePath(QStringLiteral("services/ocr/tesseract")))
                       .filePath(QStringLiteral("tessdata"));
    }
    TesseractLanguageDialog dialog(tessdata, this);
    dialog.exec();
    refreshOcrLanguages();
}

void OptionsDialog::refreshSpellProviders()
{
    if (!m_spellProvider)
        return;
    const QString current = m_spellProvider->currentData().toString();
    QSignalBlocker blocker(m_spellProvider);
    m_spellProvider->clear();
    if (m_context && m_context->plugins) {
        for (const PluginManifest& manifest : m_context->plugins->manifests()) {
            if (!manifest.valid || manifest.type != QStringLiteral("spelling"))
                continue;
            if (m_context->plugins->isDisabled(manifest.id))
                continue;
            m_spellProvider->addItem(manifest.name, manifest.id);
        }
    }
    if (m_spellProvider->count() == 0)
        m_spellProvider->addItem(QStringLiteral("Нет доступных коннекторов"), QString());

    QString target = current;
    if (m_context && m_context->settings) {
        const QString configured =
            m_context->settings->stringValue(QStringLiteral("ServiceProviders.spell"));
        if (!configured.isEmpty())
            target = configured;
    }
    int index = m_spellProvider->findData(target);
    if (index < 0)
        index = 0;
    m_spellProvider->setCurrentIndex(index);
    updateSpellControls();
}

void OptionsDialog::refreshServiceCombos()
{
    if (!m_context || !m_context->plugins)
        return;
    const auto refresh = [this](const QString& key, const QString& type) {
        auto* combo = qobject_cast<QComboBox*>(fieldWidget(key, 1));
        if (!combo)
            return;
        const QString current = combo->currentData().toString();
        const QSignalBlocker blocker(combo);
        combo->clear();
        for (const PluginManifest& manifest : m_context->plugins->manifests()) {
            if (!manifest.valid || manifest.type != type)
                continue;
            if (m_context->plugins->isDisabled(manifest.id))
                continue;
            combo->addItem(manifest.name, manifest.id);
        }
        if (combo->count() == 0)
            combo->addItem(QStringLiteral("Нет подключённых"), QString());
        int index = combo->findData(current);
        if (index < 0)
            index = 0;
        combo->setCurrentIndex(index);
    };
    refresh(QStringLiteral("ServiceProviders.dict"), QStringLiteral("dictionaries"));
    refresh(QStringLiteral("ServiceProviders.summ"), QStringLiteral("llm"));
    refresh(QStringLiteral("ServiceProviders.rewr"), QStringLiteral("llm"));
    refresh(QStringLiteral("ServiceProviders.tts"), QStringLiteral("tts"));
}

void OptionsDialog::loadPopupStyleFontSizes()
{
    if (!m_popupSourceFontSize || !m_popupTargetFontSize)
        return;
    auto* combo = qobject_cast<QComboBox*>(
        fieldWidget(QStringLiteral("Appearance.PopupStyle"), 1));
    const QString id = combo ? combo->currentData().toString()
                             : QStringLiteral("compact");
    const PopupStyle style = popupStyle(id);
    m_popupSourceFontSize->setValue(
        style.raw.value(QStringLiteral("source")).toObject()
            .value(QStringLiteral("fontSize")).toInt(12));
    m_popupTargetFontSize->setValue(
        style.raw.value(QStringLiteral("translation")).toObject()
            .value(QStringLiteral("fontSize")).toInt(14));
}

void OptionsDialog::applyPopupStyleFontSizes()
{
    if (!m_popupSourceFontSize || !m_popupTargetFontSize)
        return;
    auto* combo = qobject_cast<QComboBox*>(
        fieldWidget(QStringLiteral("Appearance.PopupStyle"), 1));
    const QString id = combo ? combo->currentData().toString() : QString();
    if (id.isEmpty())
        return;
    const QString path = QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("Themes/Popup/%1.json").arg(id));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!document.isObject())
        return;

    QJsonObject root = document.object();
    QJsonObject source = root.value(QStringLiteral("source")).toObject();
    QJsonObject translation = root.value(QStringLiteral("translation")).toObject();
    const int sourceSize = m_popupSourceFontSize->value();
    const int targetSize = m_popupTargetFontSize->value();
    if (source.value(QStringLiteral("fontSize")).toInt() == sourceSize
        && translation.value(QStringLiteral("fontSize")).toInt() == targetSize) {
        return;
    }
    source.insert(QStringLiteral("fontSize"), sourceSize);
    translation.insert(QStringLiteral("fontSize"), targetSize);
    root.insert(QStringLiteral("source"), source);
    root.insert(QStringLiteral("translation"), translation);

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return;
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (out.commit())
        reloadPopupStyles();
}

void OptionsDialog::updateSpellControls()
{
    const bool enabled = m_spellCheck && m_spellCheck->isChecked();
    if (m_spellProvider)
        m_spellProvider->setEnabled(enabled);
    if (m_spellLanguagesButton) {
        m_spellLanguagesButton->setEnabled(
            enabled && m_spellProvider && !m_spellProvider->currentData().toString().isEmpty());
    }
}

void OptionsDialog::applyDensity()
{
    if (!m_context || !m_context->settings)
        return;
    QString preset = m_context->settings->stringValue(QStringLiteral("Layout.Preset"),
                                                     QStringLiteral("classic"));
    if (auto* combo = qobject_cast<QComboBox*>(
            fieldWidget(QStringLiteral("Layout.Preset"), 1))) {
        preset = combo->currentData().toString();
    }
    const int row = preset == QStringLiteral("minimal") ? 28
        : (preset == QStringLiteral("compact") ? 30 : 32);
    if (m_hotkeyTable) {
        for (int index = 0; index < m_hotkeyTable->rowCount(); ++index)
            m_hotkeyTable->setRowHeight(index, row);
        int height = m_hotkeyTable->horizontalHeader()->sizeHint().height();
        for (int index = 0; index < m_hotkeyTable->rowCount(); ++index)
            height += m_hotkeyTable->rowHeight(index);
        m_hotkeyTable->setFixedHeight(height + 2);
    }
    if (m_orderTable) {
        m_orderTable->verticalHeader()->setDefaultSectionSize(row);
        for (int index = 0; index < m_orderTable->rowCount(); ++index)
            m_orderTable->setRowHeight(index, row);
        fitTableHeight(m_orderTable);
    }
    if (m_scheduleTable) {
        m_scheduleTable->verticalHeader()->setDefaultSectionSize(row);
        fitTableHeight(m_scheduleTable);
    }
    if (auto* tree = findChild<QTreeWidget*>(QStringLiteral("ConnectorTree"))) {
        tree->setStyleSheet(QStringLiteral(
            "QTreeWidget#ConnectorTree::item { padding-top:%1px; padding-bottom:%1px; }")
            .arg(preset == QStringLiteral("minimal") ? 0 : (preset == QStringLiteral("compact") ? 1 : 2)));
    }
}

void OptionsDialog::browseSpellLanguages()
{
    if (!m_context || !m_context->plugins || !m_spellProvider) {
        return;
    }
    const QString id = m_spellProvider->currentData().toString();
    if (id.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Проверка орфографии"),
                             QStringLiteral("Выберите коннектор проверки орфографии."));
        return;
    }
    ISpellProvider* spell = dynamic_cast<ISpellProvider*>(m_context->plugins->provider(id));
    if (!spell) {
        QMessageBox::warning(this, QStringLiteral("Проверка орфографии"),
                             QStringLiteral("Выбранный коннектор не поддерживает управление языками."));
        return;
    }
    SpellLanguageDialog dialog(spell, this);
    dialog.exec();
    updateSpellControls();
}

void OptionsDialog::testNetwork()
{
    if (!m_networkStatus)
        return;
    m_networkStatus->setText(QStringLiteral("Проверка…"));
    auto* manager = new QNetworkAccessManager(this);
    auto* sendCheck = qobject_cast<QCheckBox*>(
        fieldWidget(QStringLiteral("Proxy.SendThroughProxy"), 0));
    if (sendCheck && sendCheck->isChecked()) {
        auto* typeCombo = qobject_cast<QComboBox*>(
            fieldWidget(QStringLiteral("Proxy.ProxyType"), 1));
        const int proxyType = typeCombo ? typeCombo->currentData().toInt() : 0;
        if (proxyType == 0) {
            manager->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        } else if (proxyType == 1 || proxyType == 2) {
            const QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(
                QNetworkProxyQuery(QUrl(QStringLiteral("https://www.google.com"))));
            if (!proxies.isEmpty())
                manager->setProxy(proxies.first());
        } else {
            auto* scheme = qobject_cast<QComboBox*>(fieldWidget(QStringLiteral("Proxy.Scheme"), 1));
            auto* host = qobject_cast<QLineEdit*>(fieldWidget(QStringLiteral("Proxy.Host"), 3));
            auto* port = qobject_cast<QSpinBox*>(fieldWidget(QStringLiteral("Proxy.Port"), 2));
            auto* user = qobject_cast<QLineEdit*>(fieldWidget(QStringLiteral("Proxy.Username"), 3));
            auto* password = qobject_cast<QLineEdit*>(fieldWidget(QStringLiteral("Proxy.Password"), 3));
            QNetworkProxy::ProxyType type = QNetworkProxy::HttpProxy;
            if (scheme && scheme->currentData().toString() == QStringLiteral("socks5"))
                type = QNetworkProxy::Socks5Proxy;
            manager->setProxy(QNetworkProxy(type,
                                            host ? host->text() : QString(),
                                            port ? quint16(port->value()) : 0,
                                            user ? user->text() : QString(),
                                            password ? password->text() : QString()));
        }
    }
    QNetworkRequest request(QUrl(QStringLiteral("https://www.google.com/generate_204")));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = manager->get(request);
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    timer->start(12000);
    connect(reply, &QNetworkReply::finished, this, [this, reply, manager] {
        if (reply->error() == QNetworkReply::NoError)
            m_networkStatus->setText(QStringLiteral("Соединение работает"));
        else
            m_networkStatus->setText(QStringLiteral("Ошибка: %1").arg(reply->errorString()));
        reply->deleteLater();
        manager->deleteLater();
    });
}

void OptionsDialog::updateDependencies()
{
    auto setEnabled = [this](const QString& key, bool enabled) {
        for (const Field& field : std::as_const(m_fields)) {
            if (field.key == key)
                field.widget->setEnabled(enabled);
        }
    };

    bool history = true;
    if (auto* check = qobject_cast<QCheckBox*>(fieldWidget(QStringLiteral("General.EnableHistory"), 0)))
        history = check->isChecked();
    setEnabled(QStringLiteral("General.ClearHistoryOnExit"), history);

    bool proxy = false;
    if (auto* check = qobject_cast<QCheckBox*>(fieldWidget(QStringLiteral("Proxy.SendThroughProxy"), 0)))
        proxy = check->isChecked();
    int proxyType = 0;
    auto* proxyTypeCombo = qobject_cast<QComboBox*>(
        fieldWidget(QStringLiteral("Proxy.ProxyType"), 1));
    if (proxyTypeCombo)
        proxyType = proxyTypeCombo->currentData().toInt();
    if (proxy && proxyTypeCombo && proxyType == 0) {
        const int index = proxyTypeCombo->findData(3);
        if (index >= 0) {
            const QSignalBlocker blocker(proxyTypeCombo);
            proxyTypeCombo->setCurrentIndex(index);
            proxyType = 3;
        }
    }
    setEnabled(QStringLiteral("Proxy.ProxyType"), proxy);
    for (const QString& key : {QStringLiteral("Proxy.Scheme"), QStringLiteral("Proxy.Host"),
                               QStringLiteral("Proxy.Port"), QStringLiteral("Proxy.Username"),
                               QStringLiteral("Proxy.Password")}) {
        setEnabled(key, proxy && proxyType == 3);
    }

    bool retry = false;
    if (auto* check = qobject_cast<QCheckBox*>(fieldWidget(QStringLiteral("Internet.RetryFailed"), 0)))
        retry = check->isChecked();
    setEnabled(QStringLiteral("Internet.RetryCount"), retry);
    setEnabled(QStringLiteral("Internet.RetryDelay"), retry);

    updateSpellControls();

    QString captureMode = QStringLiteral("toolbar");
    if (auto* combo = qobject_cast<QComboBox*>(
            fieldWidget(QStringLiteral("Advanced.ScreenCaptureMode"), 1))) {
        captureMode = combo->currentData().toString();
    }
    setEnabled(QStringLiteral("Advanced.ScreenCaptureAction"),
               captureMode == QStringLiteral("auto"));

    QString outputType = QStringLiteral("none");
    if (auto* combo = qobject_cast<QComboBox*>(fieldWidget(QStringLiteral("General.AdditionalOutputType"), 1)))
        outputType = combo->currentData().toString();
    setEnabled(QStringLiteral("General.AdditionalOutputSource"), outputType != QStringLiteral("none"));

    updateSpellControls();
}

void OptionsDialog::resetDefaults()
{
    if (QMessageBox::question(this, QStringLiteral("Настройки"),
                              QStringLiteral("Установить все настройки по умолчанию?"))
        != QMessageBox::Yes) {
        return;
    }
    const QString defaultsPath = m_context->paths->settingsDefaultPath();
    QFile source(defaultsPath);
    if (!source.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Настройки"), source.errorString());
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(source.readAll());
    if (!document.isObject()) {
        QMessageBox::warning(this, QStringLiteral("Настройки"),
                             QStringLiteral("settings-default.json повреждён."));
        return;
    }
    QSaveFile target(m_context->settings->userSettingsPath());
    if (!target.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Настройки"), target.errorString());
        return;
    }
    const QByteArray payload = document.toJson(QJsonDocument::Indented);
    if (target.write(payload) != payload.size() || !target.commit()) {
        QMessageBox::warning(this, QStringLiteral("Настройки"),
                             QStringLiteral("Не удалось сохранить настройки."));
        return;
    }
    m_context->settings->load();
    m_serviceOrder.clear();
    loadAll();
    applyAll();
}
