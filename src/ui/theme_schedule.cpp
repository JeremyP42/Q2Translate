#include "theme_schedule.h"

#include "core/settings_manager.h"
#include "theme_manager.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QTimer>

namespace {

QTime parseTime(const QString& text)
{
    QTime time = QTime::fromString(text, QStringLiteral("HH:mm"));
    if (!time.isValid())
        time = QTime::fromString(text, QStringLiteral("H:mm"));
    return time;
}

} // namespace

QString scheduledThemeName(const QJsonArray& schedule, const QTime& now)
{
    if (!now.isValid())
        return QString();

    const QStringList knownThemes = ThemeManager::themeNames();
    for (const QJsonValue& value : schedule) {
        if (!value.isObject())
            continue;
        const QJsonObject entry = value.toObject();
        // Entries disabled in the schedule table stay saved but are skipped.
        if (!entry.value(QStringLiteral("enabled")).toBool(true))
            continue;
        const QTime start = parseTime(entry.value(QStringLiteral("start")).toString());
        const QTime end = parseTime(entry.value(QStringLiteral("end")).toString());
        if (!start.isValid() || !end.isValid() || start == end)
            continue;

        const bool matches = start < end ? (now >= start && now < end)
                                         : (now >= start || now < end);
        if (!matches)
            continue;

        const QString theme = entry.value(QStringLiteral("theme")).toString();
        if (!theme.isEmpty() && knownThemes.contains(theme))
            return theme;
    }
    return QString();
}

ThemeScheduleController::ThemeScheduleController(SettingsManager* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(15000);
    connect(m_timer, &QTimer::timeout, this, &ThemeScheduleController::applyNow);

    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(300);
    connect(m_reloadTimer, &QTimer::timeout, this, &ThemeScheduleController::applyNow);

    if (m_settings) {
        connect(m_settings, &SettingsManager::changed, this,
                [this] { m_reloadTimer->start(); });
    }
}

void ThemeScheduleController::start()
{
    m_appliedTheme.clear();
    applyNow();
    m_timer->start();
}

QString ThemeScheduleController::desiredTheme() const
{
    if (!m_settings)
        return QString();

    const QString base = m_settings->stringValue(QStringLiteral("Appearance.ThemeName"),
                                                 QStringLiteral("Fusion Light"));
    const QJsonArray schedule =
        m_settings->value(QStringLiteral("Appearance.ThemeSchedule")).toArray();
    const QString scheduled = scheduledThemeName(schedule, QTime::currentTime());
    if (!scheduled.isEmpty())
        return scheduled;
    if (m_settings->boolValue(QStringLiteral("Appearance.SyncWithSystemTheme"), false)) {
#ifdef Q_OS_WIN
        QSettings personalize(
            QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\"
                           "CurrentVersion\\Themes\\Personalize"),
            QSettings::NativeFormat);
        const int light = personalize.value(QStringLiteral("AppsUseLightTheme"), 1).toInt();
        return light == 0 ? QStringLiteral("Fusion Dark") : QStringLiteral("Fusion Light");
#endif
    }
    return base;
}

void ThemeScheduleController::applyNow()
{
    const QString theme = desiredTheme();
    if (theme.isEmpty() || theme == m_appliedTheme)
        return;
    if (ThemeManager::apply(theme))
        m_appliedTheme = theme;
}
