#pragma once

#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QStringList>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

class SettingsManager;

class GlobalHotkeyManager : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit GlobalHotkeyManager(SettingsManager* settings, QObject* parent = nullptr);
    ~GlobalHotkeyManager() override;

    void reload();
    bool isEnabled() const { return m_enabled; }
    bool isHookInstalled() const;
    bool isMouseHookInstalled() const;
    QStringList registrationErrors() const { return m_registrationErrors; }
    void setEnabled(bool enabled);

signals:
    void doubleCtrlDetected();
    void selectionGestureDetected();
    void hotkeyActivated(const QString& action);

protected:
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam);
    void unregisterAll();
    void registerHotkeys();
    void updateMouseMode();

    SettingsManager* m_settings = nullptr;
    QHash<int, QString> m_idToAction;
    QStringList m_registrationErrors;
    int m_nextId = 1;
    bool m_enabled = true;
    bool m_ctrlHeld = false;
    bool m_ctrlClean = false;
    bool m_pendingDouble = false;
    quint64 m_lastCtrlRelease = 0;
    bool m_mouseModeEnabled = false;
    bool m_mousePressed = false;
    bool m_mouseDragging = false;
    bool m_mouseCtrlGesture = false;
    bool m_pendingSelectionGesture = false;
    QPoint m_mouseDownPoint;
#ifdef Q_OS_WIN
    HHOOK m_hook = nullptr;
    HHOOK m_mouseHook = nullptr;
#endif
    static GlobalHotkeyManager* s_instance;
};
