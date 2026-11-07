#pragma once

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QTime>

class SettingsManager;
class QTimer;

// Returns the theme name configured for the interval that contains `now`,
// or an empty string when no interval covers it.
QString scheduledThemeName(const QJsonArray& schedule, const QTime& now);

// Applies the scheduled theme, falling back to the theme selected in the
// interface outside of the configured intervals.
class ThemeScheduleController : public QObject
{
    Q_OBJECT

public:
    explicit ThemeScheduleController(SettingsManager* settings, QObject* parent = nullptr);

    void start();

public slots:
    void applyNow();

private:
    QString desiredTheme() const;

    SettingsManager* m_settings = nullptr;
    QTimer* m_timer = nullptr;
    QTimer* m_reloadTimer = nullptr;
    QString m_appliedTheme;
};
