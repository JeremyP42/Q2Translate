#include "theme_manager.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QPair>
#include <QSet>
#include <algorithm>

namespace {

struct ThemeColors {
    QString bg;
    QString surface;
    QString surfaceAlt;
    QString surfaceHover;
    QString border;
    QString borderStrong;
    QString text;
    QString textSecondary;
    QString muted;
    QString accent;
    QString accentHover;
    QString accentActive;
    QString accentSoft;
    QString accentBorder;
    QString accentText;
    QString onAccent;
    QString danger;
    QString success;
    QString tooltipBg;
    QString tooltipText;
    QString scrollbar;
    QString scrollbarHover;
    QString onDanger;
    QString onAccentPressed;
};

struct ThemeSpec {
    QString name;
    bool dark = false;
    ThemeColors colors;
    QJsonObject raw;
};

ThemeColors parseColors(const QJsonObject& object)
{
    const auto value = [&object](const char* key, const QString& fallback) {
        const QString text = object.value(QString::fromLatin1(key)).toString();
        return text.isEmpty() ? fallback : text;
    };
    return {
        // defaults for missing JSON keys (values of "Fusion Dark" in themes.json); keep in sync with themes.json
        value("bg", QStringLiteral("#141a21")),
        value("surface", QStringLiteral("#1a2129")),
        value("surfaceAlt", QStringLiteral("#2a3139")),
        value("surfaceHover", QStringLiteral("#353c43")),
        value("border", QStringLiteral("#474d54")),
        value("borderStrong", QStringLiteral("#757a83")),
        value("text", QStringLiteral("#e8edf4")),
        value("textSecondary", QStringLiteral("#c2c7ce")),
        value("muted", QStringLiteral("#8e99a8")),
        value("accent", QStringLiteral("#4da3ff")),
        value("accentHover", QStringLiteral("#62aeff")),
        value("accentActive", QStringLiteral("#74b7ff")),
        value("accentSoft", QStringLiteral("#213852")),
        value("accentBorder", QStringLiteral("#33659b")),
        value("accentText", QStringLiteral("#68b1ff")),
        value("onAccent", QStringLiteral("#08131f")),
        value("danger", QStringLiteral("#e47777")),
        value("success", QStringLiteral("#4fc381")),
        value("tooltipBg", QStringLiteral("#1a2129")),
        value("tooltipText", QStringLiteral("#e8edf4")),
        value("scrollbar", QStringLiteral("#656a71")),
        value("scrollbarHover", QStringLiteral("#5b81aa")),
        value("onDanger", QStringLiteral("#08131f")),
        value("onAccentPressed", QStringLiteral("#08131f"))
    };
}

QList<ThemeSpec> loadBuiltinThemes()
{
    QList<ThemeSpec> result;
    QFile file(QStringLiteral(":/themes.json"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (document.isArray()) {
            for (const QJsonValue& value : document.array()) {
                if (!value.isObject())
                    continue;
                const QJsonObject object = value.toObject();
                ThemeSpec spec;
                spec.name = object.value(QStringLiteral("name")).toString();
                spec.dark = object.value(QStringLiteral("dark")).toBool();
                spec.colors = parseColors(object);
                spec.raw = object;
                if (!spec.name.isEmpty())
                    result.append(spec);
            }
        }
    }
    return result;
}

ThemeSpec specFromObject(QJsonObject object)
{
    QJsonObject flat = object;
    const QJsonObject nested = object.value(QStringLiteral("colors")).toObject();
    if (!nested.isEmpty()) {
        flat.remove(QStringLiteral("colors"));
        for (auto it = nested.constBegin(); it != nested.constEnd(); ++it)
            flat.insert(it.key(), it.value());
    }

    // Accept the old two-theme files as overrides too.
    const auto alias = [&flat](const char* from, const char* to) {
        const QString source = QString::fromLatin1(from);
        const QString target = QString::fromLatin1(to);
        if (flat.contains(source) && !flat.contains(target))
            flat.insert(target, flat.value(source));
    };
    alias("window", "bg");
    alias("panel", "surface");
    alias("panel2", "surfaceAlt");
    alias("border2", "borderStrong");
    alias("accent2", "accentHover");
    alias("faint", "muted");

    ThemeSpec spec;
    spec.name = flat.value(QStringLiteral("name")).toString();
    spec.dark = flat.value(QStringLiteral("dark")).toBool();
    spec.colors = parseColors(flat);
    spec.raw = flat;
    return spec;
}

QList<ThemeSpec> loadExternalThemes()
{
    QList<ThemeSpec> result;
    const QDir applicationDir(QCoreApplication::applicationDirPath());
    QDir directory(applicationDir.filePath(QStringLiteral("Themes/General")));
    if (!directory.exists())
        directory = QDir(applicationDir.filePath(QStringLiteral("Themes")));
    if (!directory.exists())
        return result;

    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo& info : files) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (document.isObject()) {
            const ThemeSpec spec = specFromObject(document.object());
            if (!spec.name.isEmpty())
                result.append(spec);
        } else if (document.isArray()) {
            for (const QJsonValue& value : document.array()) {
                if (!value.isObject())
                    continue;
                const ThemeSpec spec = specFromObject(value.toObject());
                if (!spec.name.isEmpty())
                    result.append(spec);
            }
        }
    }
    return result;
}

QList<ThemeSpec> buildThemes()
{
    const QList<ThemeSpec> builtin = loadBuiltinThemes();
    const QList<ThemeSpec> external = loadExternalThemes();

    QHash<QString, ThemeSpec> overrides;
    for (const ThemeSpec& theme : external)
        overrides.insert(theme.name.toLower(), theme);

    QList<ThemeSpec> result;
    QSet<QString> used;
    for (const ThemeSpec& theme : builtin) {
        const QString key = theme.name.toLower();
        const auto candidate = overrides.constFind(key);
        if (candidate != overrides.constEnd()) {
            result.append(candidate.value());
            used.insert(key);
        } else {
            result.append(theme);
        }
    }
    for (const ThemeSpec& theme : external) {
        const QString key = theme.name.toLower();
        if (!used.contains(key) && !std::any_of(result.cbegin(), result.cend(),
                                                [&key](const ThemeSpec& current) {
                                                    return current.name.toLower() == key;
                                                })) {
            result.append(theme);
            used.insert(key);
        }
    }

    // The built-in resource is the last resort for a broken installation.
    if (result.isEmpty()) {
        qFatal("themes.json missing or corrupt - this is a packaging bug, not a runtime fallback. Rebuild the qrc.");
    }
    return result;
}

QList<ThemeSpec> g_themes;

const QList<ThemeSpec>& themes()
{
    if (g_themes.isEmpty())
        g_themes = buildThemes();
    return g_themes;
}

const ThemeSpec* findTheme(const QString& name)
{
    for (const ThemeSpec& theme : themes()) {
        if (theme.name.compare(name, Qt::CaseInsensitive) == 0)
            return &theme;
    }
    return nullptr;
}

const ThemeSpec& fallbackTheme(bool dark)
{
    for (const ThemeSpec& theme : themes()) {
        if (theme.dark == dark)
            return theme;
    }
    return themes().first();
}

QString styledSheet(const QString& base, const ThemeColors& colors)
{
    QString style = base;
    QList<QPair<QString, QString>> tokens = {
        {QStringLiteral("$bg"), colors.bg},
        {QStringLiteral("$surface"), colors.surface},
        {QStringLiteral("$surface-alt"), colors.surfaceAlt},
        {QStringLiteral("$surface-hover"), colors.surfaceHover},
        {QStringLiteral("$border"), colors.border},
        {QStringLiteral("$border-strong"), colors.borderStrong},
        {QStringLiteral("$text"), colors.text},
        {QStringLiteral("$text-secondary"), colors.textSecondary},
        {QStringLiteral("$muted"), colors.muted},
        {QStringLiteral("$accent"), colors.accent},
        {QStringLiteral("$accent-hover"), colors.accentHover},
        {QStringLiteral("$accent-active"), colors.accentActive},
        {QStringLiteral("$accent-soft"), colors.accentSoft},
        {QStringLiteral("$accent-border"), colors.accentBorder},
        {QStringLiteral("$accent-text"), colors.accentText},
        {QStringLiteral("$on-accent"), colors.onAccent},
        {QStringLiteral("$on-accent-pressed"), colors.onAccentPressed},
        {QStringLiteral("$on-danger"), colors.onDanger},
        {QStringLiteral("$danger"), colors.danger},
        {QStringLiteral("$success"), colors.success},
        {QStringLiteral("$tooltip-bg"), colors.tooltipBg},
        {QStringLiteral("$tooltip-text"), colors.tooltipText},
        {QStringLiteral("$scrollbar"), colors.scrollbar},
        {QStringLiteral("$scrollbar-hover"), colors.scrollbarHover}
    };
    std::sort(tokens.begin(), tokens.end(),
              [](const QPair<QString, QString>& left, const QPair<QString, QString>& right) {
                  return left.first.size() > right.first.size();
              });
    for (const auto& token : tokens)
        style.replace(token.first, token.second);
    return style;
}

QPalette paletteFor(const ThemeColors& colors)
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(colors.bg));
    palette.setColor(QPalette::WindowText, QColor(colors.text));
    palette.setColor(QPalette::Base, QColor(colors.surface));
    palette.setColor(QPalette::AlternateBase, QColor(colors.surfaceAlt));
    palette.setColor(QPalette::Text, QColor(colors.text));
    palette.setColor(QPalette::Button, QColor(colors.surfaceAlt));
    palette.setColor(QPalette::ButtonText, QColor(colors.text));
    palette.setColor(QPalette::ToolTipBase, QColor(colors.tooltipBg));
    palette.setColor(QPalette::ToolTipText, QColor(colors.tooltipText));
    palette.setColor(QPalette::Highlight, QColor(colors.accent));
    palette.setColor(QPalette::HighlightedText, QColor(colors.onAccent));
    palette.setColor(QPalette::PlaceholderText, QColor(colors.muted));
    palette.setColor(QPalette::Link, QColor(colors.accentText));
    palette.setColor(QPalette::Light, QColor(colors.surface));
    palette.setColor(QPalette::Midlight, QColor(colors.surfaceHover));
    palette.setColor(QPalette::Dark, QColor(colors.borderStrong));
    palette.setColor(QPalette::Mid, QColor(colors.border));
    return palette;
}

QString g_activeTheme;

}

namespace ThemeManager {

QStringList themeNames()
{
    QStringList names;
    for (const ThemeSpec& theme : themes())
        names.append(theme.name);
    return names;
}

void reload()
{
    g_themes = buildThemes();
}

bool isDark(const QString& themeName)
{
    if (const ThemeSpec* theme = findTheme(themeName))
        return theme->dark;
    return themeName.contains(QStringLiteral("dark"), Qt::CaseInsensitive)
        || themeName.contains(QStringLiteral("тёмн"), Qt::CaseInsensitive)
        || themeName.contains(QStringLiteral("темн"), Qt::CaseInsensitive);
}

QString styleSheet(const QString& themeName)
{
    const ThemeSpec& theme = findTheme(themeName)
        ? *findTheme(themeName) : fallbackTheme(isDark(themeName));
    g_activeTheme = theme.name;
    QFile file(QStringLiteral(":/style.qss"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return styledSheet(QString::fromUtf8(file.readAll()), theme.colors);
}

bool apply(const QString& themeName)
{
    const QString style = styleSheet(themeName);
    if (style.isEmpty())
        return false;

    QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app)
        return false;

    const ThemeSpec& theme = findTheme(themeName)
        ? *findTheme(themeName) : fallbackTheme(isDark(themeName));
    app->setPalette(paletteFor(theme.colors));
    app->setStyleSheet(style);
    return true;
}

QString activeTheme()
{
    return g_activeTheme;
}

QColor color(const QString& token, const QString& themeName)
{
    QString target = themeName.isEmpty() ? g_activeTheme : themeName;
    if (target.isEmpty()) {
        if (themes().isEmpty())
            return QColor();
        target = themes().first().name;
        qWarning("theme_manager: active theme is not set, using %s", qPrintable(target));
    }

    const ThemeSpec* theme = findTheme(target);
    if (!theme) {
        qWarning("theme_manager: unknown theme %s", qPrintable(target));
        return QColor();
    }

    const QJsonValue value = theme->raw.value(token);
    if (value.isUndefined()) {
        qWarning("theme_manager: token %s not found in theme %s",
                 qPrintable(token), qPrintable(target));
        return QColor();
    }
    return QColor(value.toString());
}

}
