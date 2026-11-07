#pragma once

#include <QString>

class AppContext;

namespace TtsService {

// Maps legacy display names ("Windows SAPI", "Google TTS", ...) to connector
// ids. Empty input stays empty; unknown values are returned unchanged.
QString normalizedProviderId(const QString& value);

// Speaks the text through the configured TTS connector.  Respects the global
// "ServiceProviders.ttsOn" switch.
void speak(AppContext* context, const QString& text, const QString& language,
           bool slower = false);

} // namespace TtsService
