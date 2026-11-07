#include "global_hotkey_manager.h"

#include "core/settings_manager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>

#ifdef Q_OS_WIN
#include <QDateTime>
#endif

GlobalHotkeyManager* GlobalHotkeyManager::s_instance = nullptr;

GlobalHotkeyManager::GlobalHotkeyManager(SettingsManager* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
    s_instance = this;
    if (QCoreApplication::instance())
        QCoreApplication::instance()->installNativeEventFilter(this);

#ifdef Q_OS_WIN
    m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, GetModuleHandleW(nullptr), 0);
    if (!m_hook)
        qWarning() << "[hotkeys] SetWindowsHookEx failed:" << GetLastError();
#endif

    reload();
}

GlobalHotkeyManager::~GlobalHotkeyManager()
{
    unregisterAll();
#ifdef Q_OS_WIN
    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
    if (m_mouseHook) {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
    }
#endif
    if (QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(this);
    if (s_instance == this)
        s_instance = nullptr;
}

bool GlobalHotkeyManager::isHookInstalled() const
{
#ifdef Q_OS_WIN
    return m_hook != nullptr;
#else
    return false;
#endif
}

bool GlobalHotkeyManager::isMouseHookInstalled() const
{
#ifdef Q_OS_WIN
    return m_mouseHook != nullptr;
#else
    return false;
#endif
}

void GlobalHotkeyManager::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    m_ctrlHeld = false;
    if (enabled)
        registerHotkeys();
    else
        unregisterAll();
}

void GlobalHotkeyManager::reload()
{
#ifdef Q_OS_WIN
    unregisterAll();
    m_enabled = !m_settings
        || m_settings->boolValue(QStringLiteral("HotKeys.EnableHotKeys"), true);
    if (m_enabled)
        registerHotkeys();
    updateMouseMode();
#endif
}

void GlobalHotkeyManager::updateMouseMode()
{
#ifdef Q_OS_WIN
    const bool enabled = m_settings
        && m_settings->boolValue(QStringLiteral("Advanced.EnableMouseModeOnCtrl"), false);
    m_mouseModeEnabled = enabled;
    if (enabled && !m_mouseHook) {
        m_mouseHook =
            SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
        if (!m_mouseHook)
            qWarning() << "[mouse-mode] SetWindowsHookEx failed:" << GetLastError();
    } else if (!enabled && m_mouseHook) {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
        m_mousePressed = false;
        m_mouseDragging = false;
        m_mouseCtrlGesture = false;
        m_pendingSelectionGesture = false;
    }
#endif
}

void GlobalHotkeyManager::unregisterAll()
{
#ifdef Q_OS_WIN
    for (auto it = m_idToAction.constBegin(); it != m_idToAction.constEnd(); ++it)
        UnregisterHotKey(nullptr, it.key());
#endif
    m_idToAction.clear();
}

void GlobalHotkeyManager::registerHotkeys()
{
#ifdef Q_OS_WIN
    if (!m_settings)
        return;

    struct HotkeyDefinition {
        const char* action;
        const char* key;
        int defaultValue;
    };

    const HotkeyDefinition definitions[] = {
        {"show_main", "HotKeyShowMainWindow", 0},
        {"quick_translate", "HotKeyQuickTranslate", 593},
        {"listen_text", "HotKeyListenText", 0},
        {"ocr", "HotKeyTextRecognition", 3153},
        {"replace_selection", "HotKeyReplaceSelection", 0},
        {"switch_target_language", "HotKeySwitchTargetLanguage", 0},
        {"dictionary", "HotKeyDictionary", 0},
        {"translate", "HotKeyTranslate", 0},
        {"focus_source", "HotKeyFocusSource", 0},
        {"focus_target", "HotKeyFocusTarget", 0},
        {"focus_extra", "HotKeyFocusExtra", 0},
        {"copy_translation", "HotKeyCopyTranslationText", 0},
        {"clear_input", "HotKeyClearInput", 0},
        {"swap_languages", "HotKeySwapLanguages", 0},
        {"open_settings", "HotKeyOpenSettings", 0},
        {"show_history", "HotKeyShowHistory", 0},
        {"translate_document", "HotKeyTranslateDocument", 0}
    };
    m_nextId = 1;
    for (const HotkeyDefinition& definition : definitions) {
        const int value = m_settings->intValue(
            QStringLiteral("HotKeys.") + QString::fromLatin1(definition.key),
            definition.defaultValue);
        if (value <= 0)
            continue;

        const UINT modifiers = UINT((value >> 8) & 0xFF);
        const UINT virtualKey = UINT(value & 0xFF);
        if (virtualKey == 0 || (modifiers & ~UINT(MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN)))
            continue;

        const int id = m_nextId++;
        if (RegisterHotKey(nullptr, id, modifiers, virtualKey)) {
            m_idToAction.insert(id, QString::fromLatin1(definition.action));
        } else {
            qWarning() << "[hotkeys] RegisterHotKey failed for" << definition.action
                       << "error" << GetLastError();
        }
    }
#endif
}

bool GlobalHotkeyManager::nativeEventFilter(const QByteArray& eventType, void* message,
                                            qintptr* result)
{
    Q_UNUSED(result);
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        auto* msg = static_cast<MSG*>(message);
        if (msg->message == WM_HOTKEY) {
            const QString action = m_idToAction.value(int(msg->wParam));
            if (!action.isEmpty()) {
                emit hotkeyActivated(action);
                return true;
            }
        }
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
#endif
    return false;
}

LRESULT CALLBACK GlobalHotkeyManager::keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
#ifdef Q_OS_WIN
    GlobalHotkeyManager* instance = s_instance;
    if (code == HC_ACTION && instance
        && (instance->m_enabled || instance->m_pendingSelectionGesture)) {
        const auto* keyboard = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        const DWORD key = keyboard->vkCode;
        const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        const bool ctrl = key == VK_LCONTROL || key == VK_RCONTROL;
        const bool injected = (keyboard->flags & LLKHF_INJECTED) != 0;

        // Synthetic keys (our own Ctrl+C for copying, remappers, RDP) must not
        // build a double tap, but injected Ctrl is allowed: keyboard utilities
        // and remote sessions deliver all input that way.
        if (injected && !ctrl) {
            instance->m_lastCtrlRelease = 0;
            instance->m_pendingDouble = false;
            instance->m_ctrlClean = false;
            return CallNextHookEx(instance->m_hook, code, wParam, lParam);
        }

        if (ctrl) {
            if (down) {
                if (!instance->m_ctrlHeld) {
                    instance->m_ctrlHeld = true;
                    instance->m_ctrlClean = true;
                    const quint64 now = GetTickCount64();
                    if (instance->m_lastCtrlRelease > 0
                        && now >= instance->m_lastCtrlRelease
                        && now - instance->m_lastCtrlRelease <= 650) {
                        instance->m_pendingDouble = true;
                    }
                }
            } else if (up) {
                instance->m_ctrlHeld = false;
                if (instance->m_pendingSelectionGesture) {
                    instance->m_pendingSelectionGesture = false;
                    QMetaObject::invokeMethod(instance, [instance] {
                        emit instance->selectionGestureDetected();
                    }, Qt::QueuedConnection);
                }
                if (instance->m_pendingDouble) {
                    // Fire after the key is released: otherwise the synthetic
                    // Ctrl+C used to copy the selection arrives while the user
                    // is still holding Ctrl and the foreground app ignores it.
                    instance->m_pendingDouble = false;
                    instance->m_lastCtrlRelease = 0;
                    QMetaObject::invokeMethod(instance, [instance] {
                        emit instance->doubleCtrlDetected();
                    }, Qt::QueuedConnection);
                } else if (instance->m_ctrlClean) {
                    instance->m_lastCtrlRelease = GetTickCount64();
                } else {
                    instance->m_lastCtrlRelease = 0;
                }
            }
        } else if (down || up) {
            instance->m_lastCtrlRelease = 0;
            instance->m_pendingDouble = false;
            instance->m_ctrlClean = false;
        }
    }
    return CallNextHookEx(instance ? instance->m_hook : nullptr, code, wParam, lParam);
#else
    Q_UNUSED(code);
    Q_UNUSED(wParam);
    Q_UNUSED(lParam);
    return 0;
#endif
}

LRESULT CALLBACK GlobalHotkeyManager::mouseProc(int code, WPARAM wParam, LPARAM lParam)
{
#ifdef Q_OS_WIN
    GlobalHotkeyManager* instance = s_instance;
    if (code == HC_ACTION && instance && instance->m_mouseModeEnabled) {
        const auto* mouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        const bool ctrlHeld = instance->m_ctrlHeld;
        if (wParam == WM_LBUTTONDOWN) {
            instance->m_mousePressed = true;
            instance->m_mouseDragging = false;
            instance->m_mouseCtrlGesture = ctrlHeld;
            instance->m_mouseDownPoint = QPoint(int(mouse->pt.x), int(mouse->pt.y));
        } else if (wParam == WM_MOUSEMOVE && instance->m_mousePressed) {
            const QPoint position(int(mouse->pt.x), int(mouse->pt.y));
            if ((position - instance->m_mouseDownPoint).manhattanLength() > 6)
                instance->m_mouseDragging = true;
        } else if (wParam == WM_LBUTTONUP) {
            const bool gesture = instance->m_mousePressed
                && instance->m_mouseCtrlGesture && instance->m_mouseDragging;
            instance->m_mousePressed = false;
            instance->m_mouseDragging = false;
            if (gesture) {
                if (!ctrlHeld) {
                    QMetaObject::invokeMethod(instance, [instance] {
                        emit instance->selectionGestureDetected();
                    }, Qt::QueuedConnection);
                } else {
                    instance->m_pendingSelectionGesture = true;
                }
            }
        }
    }
    return CallNextHookEx(instance ? instance->m_mouseHook : nullptr, code, wParam, lParam);
#else
    Q_UNUSED(code);
    Q_UNUSED(wParam);
    Q_UNUSED(lParam);
    return 0;
#endif
}
