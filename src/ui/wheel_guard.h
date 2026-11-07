#pragma once

#include <QObject>

// Prevents the mouse wheel from changing values of spin boxes, sliders,
// combo boxes and similar controls. The scroll is forwarded to the nearest
// scroll area so the page keeps scrolling naturally.
class WheelGuard : public QObject
{
    Q_OBJECT

public:
    explicit WheelGuard(QObject* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};
