#pragma once

#include "language_catalog.h"

#include <QList>
#include <QSet>

class SettingsManager;

QSet<int> favoriteLanguageIds(const SettingsManager* settings);

// Languages marked as favorites in the settings. When the list is empty the
// full catalogue is returned so the interface stays usable.
QList<LanguageInfo> favoriteLanguageList(const SettingsManager* settings,
                                         bool includeAuto = false);
