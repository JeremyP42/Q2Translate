#include "window_utils.h"

#include <QFileInfo>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace WindowUtils {

#ifdef Q_OS_WIN
namespace {

QString processNameForWindow(HWND window)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0)
        return QString();

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return QString();

    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = DWORD(sizeof(buffer) / sizeof(buffer[0]));
    QString path;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size))
        path = QString::fromWCharArray(buffer, int(size));
    CloseHandle(process);

    if (path.isEmpty())
        return QString();
    return QFileInfo(path).fileName();
}

}
#endif

ForegroundInfo foregroundWindow()
{
    ForegroundInfo info;
#ifdef Q_OS_WIN
    HWND window = GetForegroundWindow();
    if (!window)
        return info;

    info.handle = reinterpret_cast<quintptr>(window);
    info.processName = processNameForWindow(window);

    wchar_t className[256] = {};
    if (GetClassNameW(window, className, int(sizeof(className) / sizeof(className[0]))) > 0)
        info.windowClass = QString::fromWCharArray(className);

    wchar_t title[1024] = {};
    if (GetWindowTextW(window, title, int(sizeof(title) / sizeof(title[0]))) > 0)
        info.windowTitle = QString::fromWCharArray(title);
#endif
    return info;
}

}
