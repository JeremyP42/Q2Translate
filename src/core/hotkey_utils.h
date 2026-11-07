#pragma once

#include <QKeySequence>
#include <Qt>

namespace HotkeyUtils {

inline int qtKeyToVk(Qt::Key key)
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
    default:
        if (int(key) > 0 && int(key) < 256)
            return int(key);
        return 0;
    }
}

inline int sequenceToNative(const QKeySequence& sequence)
{
    if (sequence.isEmpty())
        return 0;
    const QKeyCombination combination = sequence[0];
    const int vk = qtKeyToVk(combination.key());
    if (!vk)
        return 0;
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    int mods = 0;
    if (modifiers & Qt::AltModifier) mods |= 1;
    if (modifiers & Qt::ControlModifier) mods |= 2;
    if (modifiers & Qt::ShiftModifier) mods |= 4;
    if (modifiers & Qt::MetaModifier) mods |= 8;
    return (mods << 8) | vk;
}

inline Qt::Key vkToQtKey(int vk)
{
    if (vk >= 0x41 && vk <= 0x5A) return Qt::Key(int(Qt::Key_A) + (vk - 0x41));
    if (vk >= 0x30 && vk <= 0x39) return Qt::Key(int(Qt::Key_0) + (vk - 0x30));
    if (vk >= 0x70 && vk <= 0x87) return Qt::Key(int(Qt::Key_F1) + (vk - 0x70));
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

inline int defaultInternalHotkey(const char* key)
{
    const QString value = QString::fromLatin1(key);
    if (value == QStringLiteral("HotKeyTextRecognition")) return 3153; // Win+Shift+Q
    return 0;
}

inline int defaultGlobalHotkey(const char* key)
{
    const QString value = QString::fromLatin1(key);
    if (value == QStringLiteral("HotKeyQuickTranslate")) return 593;   // Ctrl+Q
    if (value == QStringLiteral("HotKeyTextRecognition")) return 3153; // Win+Shift+Q
    return 0;
}
inline QKeySequence nativeToSequence(int value)
{
    if (value <= 0)
        return QKeySequence();
    const int vk = value & 0xFF;
    const int mods = (value >> 8) & 0xFF;
    const Qt::Key key = vkToQtKey(vk);
    if (key == Qt::Key_unknown)
        return QKeySequence();
    Qt::KeyboardModifiers modifiers;
    if (mods & 1) modifiers |= Qt::AltModifier;
    if (mods & 2) modifiers |= Qt::ControlModifier;
    if (mods & 4) modifiers |= Qt::ShiftModifier;
    if (mods & 8) modifiers |= Qt::MetaModifier;
    return QKeySequence(QKeyCombination(modifiers, key));
}

}
