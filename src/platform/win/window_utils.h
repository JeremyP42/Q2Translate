#pragma once

#include <QString>

namespace WindowUtils {

struct ForegroundInfo {
    QString processName;
    QString windowClass;
    QString windowTitle;
    quintptr handle = 0;
};

ForegroundInfo foregroundWindow();

}
