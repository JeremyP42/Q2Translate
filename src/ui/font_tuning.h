#pragma once

#include <QFont>
#include <QString>

namespace FontTuning {

void apply(QFont& font, bool antialiasing, const QString& hinting);

} // namespace FontTuning
