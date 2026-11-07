#include "wheel_guard.h"

#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDial>
#include <QEvent>
#include <QSlider>
#include <QTabBar>
#include <QWidget>

WheelGuard::WheelGuard(QObject* parent)
    : QObject(parent)
{
}

bool WheelGuard::eventFilter(QObject* watched, QEvent* event)
{
    if (!event || event->type() != QEvent::Wheel)
        return false;

    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget)
        return false;

    const bool valueControl = qobject_cast<QAbstractSpinBox*>(widget) != nullptr
        || qobject_cast<QComboBox*>(widget) != nullptr
        || qobject_cast<QSlider*>(widget) != nullptr
        || qobject_cast<QDial*>(widget) != nullptr
        || qobject_cast<QTabBar*>(widget) != nullptr;
    if (!valueControl)
        return false;

    QWidget* parent = widget->parentWidget();
    while (parent) {
        if (auto* area = qobject_cast<QAbstractScrollArea*>(parent)) {
            if (area->viewport() != widget)
                QApplication::sendEvent(area->viewport(), event);
            return true;
        }
        parent = parent->parentWidget();
    }
    return true;
}
