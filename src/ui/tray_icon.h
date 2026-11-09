#pragma once

#include <QObject>
#include <QString>

class QAction;
class QMenu;
class QSystemTrayIcon;
class SettingsManager;
class PluginManager;

class TrayIcon : public QObject
{
    Q_OBJECT

public:
    explicit TrayIcon(SettingsManager* settings, PluginManager* plugins = nullptr,
                      QObject* parent = nullptr);
    ~TrayIcon() override;

    void show();
    void hide();
    bool isAvailable() const;
    void setHotkeysEnabled(bool enabled);

signals:
    void showMainWindowRequested();
    void translateClipboardRequested();
    void settingsRequested();
    void historyRequested();
    void dictionaryRequested();
    void ocrRequested();
    void aboutRequested();
    void hotkeysToggled(bool enabled);
    void quitRequested();

private:
    void buildMenu();
    void updateServiceVisibility();

    SettingsManager* m_settings = nullptr;
    PluginManager* m_plugins = nullptr;
    QSystemTrayIcon* m_tray = nullptr;
    QMenu* m_menu = nullptr;
    QAction* m_hotkeysAction = nullptr;
    QAction* m_historyAction = nullptr;
    QAction* m_dictionaryAction = nullptr;
    QAction* m_ocrAction = nullptr;
};
