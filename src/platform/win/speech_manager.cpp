#include "speech_manager.h"

#include <QDebug>
#include <QtConcurrent>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <sapi.h>
#include <windows.h>
#endif

namespace SpeechManager {

void speak(const QString& text, bool slower)
{
    const QString content = text.trimmed();
    if (content.isEmpty())
        return;

#ifdef Q_OS_WIN
    QFuture<void> future = QtConcurrent::run([content, slower] {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ISpVoice* voice = nullptr;
        const HRESULT created = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                                 IID_ISpVoice,
                                                 reinterpret_cast<void**>(&voice));
        if (SUCCEEDED(created) && voice) {
            if (slower)
                voice->SetRate(-4);
            voice->Speak(reinterpret_cast<LPCWSTR>(content.utf16()), 0, nullptr);
            voice->Release();
        } else {
            qWarning() << "[speech] SAPI voice is unavailable";
        }
        if (SUCCEEDED(initialized))
            CoUninitialize();
    });
    Q_UNUSED(future);
#else
    Q_UNUSED(content);
#endif
}

}
