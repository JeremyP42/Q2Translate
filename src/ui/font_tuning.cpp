#include "font_tuning.h"

namespace FontTuning {

void apply(QFont& font, bool antialiasing, const QString& hinting)
{
    font.setStyleStrategy(antialiasing ? QFont::PreferAntialias : QFont::NoAntialias);

    if (hinting == QStringLiteral("none"))
        font.setHintingPreference(QFont::PreferNoHinting);
    else if (hinting == QStringLiteral("vertical"))
        font.setHintingPreference(QFont::PreferVerticalHinting);
    else
        font.setHintingPreference(QFont::PreferFullHinting);
}

} // namespace FontTuning
