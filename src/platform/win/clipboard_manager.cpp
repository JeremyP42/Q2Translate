#include "clipboard_manager.h"

#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QTimer>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

ClipboardManager::ClipboardManager(QObject* parent)
    : QObject(parent)
{
}

QString ClipboardManager::copySelection(int delayMs, QString* error)
{
    QClipboard* clipboard = QApplication::clipboard();
    if (!clipboard) {
        if (error)
            *error = QStringLiteral("Буфер обмена недоступен");
        return QString();
    }

#ifdef Q_OS_WIN
    // Wait for the clipboard sequence number to change instead of clearing the
    // clipboard first: clearing produced a dataChanged notification that ended
    // the wait before Ctrl+C had actually copied anything.
    const DWORD sequenceBefore = GetClipboardSequenceNumber();
#endif

    if (!sendCopyShortcut()) {
        if (error)
            *error = QStringLiteral("Не удалось отправить Ctrl+C");
        return QString();
    }

#ifdef Q_OS_WIN
    const int timeout = qBound(150, delayMs, 2000);
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&loop, sequenceBefore] {
        if (GetClipboardSequenceNumber() != sequenceBefore)
            loop.quit();
    });
    QTimer::singleShot(timeout, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    poll.stop();

    if (GetClipboardSequenceNumber() == sequenceBefore)
        return QString();
    return clipboard->text();
#else
    const QString previous = clipboard->text();
    clipboard->clear();

    QEventLoop loop;
    QObject::connect(clipboard, &QClipboard::dataChanged, &loop, &QEventLoop::quit);
    QTimer::singleShot(qBound(40, delayMs, 1000), &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    const QString selected = clipboard->text();
    if (selected.isEmpty())
        clipboard->setText(previous);
    return selected;
#endif
}

void ClipboardManager::copyText(const QString& text)
{
    if (QClipboard* clipboard = QApplication::clipboard())
        clipboard->setText(text);
}

void ClipboardManager::pasteText(const QString& text)
{
    copyText(text);
    sendPasteShortcut();
}

bool ClipboardManager::sendCopyShortcut()
{
#ifdef Q_OS_WIN
    INPUT inputs[4] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'C';
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'C';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(4, inputs, sizeof(INPUT)) == 4;
#else
    return false;
#endif
}

bool ClipboardManager::sendPasteShortcut()
{
#ifdef Q_OS_WIN
    INPUT inputs[4] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(4, inputs, sizeof(INPUT)) == 4;
#else
    return false;
#endif
}
