#include "tray_icon.h"

#include "services/plugin_manager.h"

#include "core/settings_manager.h"
#include "icon_factory.h"

#include <QAction>
#include <QMenu>
#include <QSignalBlocker>
#include <QSystemTrayIcon>

TrayIcon::TrayIcon(SettingsManager* settings, PluginManager* plugins, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_plugins(plugins)
{
    m_tray = new QSystemTrayIcon(IconFactory::appIcon(), this);
    m_tray->setToolTip(QStringLiteral("Q2Translate"));

    buildMenu();
    m_tray->setContextMenu(m_menu);

    if (m_settings) {
        connect(m_settings, &SettingsManager::changed, this,
                &TrayIcon::updateServiceVisibility);
    }
    updateServiceVisibility();

    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::DoubleClick)
                    emit showMainWindowRequested();
            });
}

TrayIcon::~TrayIcon()
{
    if (m_tray) {
        m_tray->hide();
        m_tray->setContextMenu(nullptr);
    }
    delete m_menu;
    m_menu = nullptr;
}

void TrayIcon::buildMenu()
{
    m_menu = new QMenu();

    QAction* showAction = m_menu->addAction(QStringLiteral("Открыть Q2Translate"));
    connect(showAction, &QAction::triggered, this, [this] { emit showMainWindowRequested(); });
    m_menu->setDefaultAction(showAction);

    QAction* translateAction = m_menu->addAction(QStringLiteral("Перевести выделение"));
    connect(translateAction, &QAction::triggered, this, [this] { emit translateClipboardRequested(); });

    m_menu->addSeparator();

    m_historyAction = m_menu->addAction(QStringLiteral("История"));
    connect(m_historyAction, &QAction::triggered, this, [this] { emit historyRequested(); });

    m_dictionaryAction = m_menu->addAction(QStringLiteral("Словарь"));
    connect(m_dictionaryAction, &QAction::triggered, this, [this] { emit dictionaryRequested(); });

    m_ocrAction = m_menu->addAction(QStringLiteral("Распознавание текста"));
    connect(m_ocrAction, &QAction::triggered, this, [this] { emit ocrRequested(); });

    m_hotkeysAction = m_menu->addAction(QStringLiteral("Глобальные горячие клавиши"));
    m_hotkeysAction->setCheckable(true);
    m_hotkeysAction->setChecked(true);
    connect(m_hotkeysAction, &QAction::toggled, this, [this](bool enabled) {
        emit hotkeysToggled(enabled);
    });

    m_menu->addSeparator();

    QAction* settingsAction = m_menu->addAction(QStringLiteral("Настройки…"));
    connect(settingsAction, &QAction::triggered, this, [this] { emit settingsRequested(); });

    QAction* aboutAction = m_menu->addAction(QStringLiteral("О программе"));
    connect(aboutAction, &QAction::triggered, this, [this] { emit aboutRequested(); });

    m_menu->addSeparator();

    QAction* quitAction = m_menu->addAction(QStringLiteral("Выход"));
    connect(quitAction, &QAction::triggered, this, [this] { emit quitRequested(); });
}

void TrayIcon::updateServiceVisibility()
{
    if (!m_settings)
        return;
    if (m_historyAction)
        m_historyAction->setVisible(
            m_settings->boolValue(QStringLiteral("General.EnableHistory"), false));
    if (m_dictionaryAction) {
        bool available = false;
        if (m_plugins) {
            for (const PluginManifest& manifest : m_plugins->manifests()) {
                if (manifest.valid
                    && manifest.capabilities.contains(QStringLiteral("dictionary"))
                    && !m_plugins->isDisabled(manifest.id)
                    && m_plugins->provider(manifest.id)) {
                    available = true;
                    break;
                }
            }
        }
        m_dictionaryAction->setVisible(available);
    }
    if (m_ocrAction)
        m_ocrAction->setVisible(
            m_settings->boolValue(QStringLiteral("ServiceProviders.ocrOn"), true));
}

void TrayIcon::show()
{
    if (!m_tray || !QSystemTrayIcon::isSystemTrayAvailable())
        return;
    m_tray->show();
}

void TrayIcon::hide()
{
    if (m_tray)
        m_tray->hide();
}

bool TrayIcon::isAvailable() const
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayIcon::setHotkeysEnabled(bool enabled)
{
    if (!m_hotkeysAction)
        return;
    QSignalBlocker blocker(m_hotkeysAction);
    m_hotkeysAction->setChecked(enabled);
}
