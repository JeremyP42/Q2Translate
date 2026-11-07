#include "localization.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFile>
#include <QGroupBox>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTabBar>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidget>
#include <QWidget>

#include <algorithm>
#include <functional>

namespace {

QHash<QString, QString> g_map;
QHash<QString, QString> g_base;
QStringList g_replaceKeys;
QStringList g_baseReplaceKeys;
QString g_folder = QStringLiteral("Russian");

QString localeRoot()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Locales"));
}

QString localeFilePath(const QString& key)
{
    if (key.isEmpty())
        return QString();
    const QDir root(localeRoot());
    const QFileInfoList files = root.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo& info : files) {
        if (info.completeBaseName().compare(key, Qt::CaseInsensitive) == 0)
            return info.absoluteFilePath();
    }
    // Legacy layout: Locales/<Language>/lang.json.
    const QString legacy =
        QDir(root.filePath(key)).filePath(QStringLiteral("lang.json"));
    return QFileInfo::exists(legacy) ? legacy : QString();
}

void ensureBase()
{
    if (!g_base.isEmpty())
        return;
    QFile file(QStringLiteral(":/lang-base.json"));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return;
    const QJsonObject object = document.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isString())
            g_base.insert(it.key(), it.value().toString());
    }
    g_baseReplaceKeys = g_base.keys();
    std::sort(g_baseReplaceKeys.begin(), g_baseReplaceKeys.end(),
              [](const QString& left, const QString& right) {
                  return left.size() > right.size();
              });
}

QString translationFor(const QString& source)
{
    if (source.isEmpty())
        return source;
    ensureBase();

    QString english = g_base.value(source, source);
    if (english == source) {
        for (const QString& key : g_baseReplaceKeys) {
            if (key.size() < 4 || !english.contains(key))
                continue;
            english.replace(key, g_base.value(key));
        }
    }

    if (g_folder == QLatin1String("Russian")) {
        const auto exact = g_map.constFind(english);
        if (exact != g_map.constEnd())
            return exact.value();
        return source;
    }

    if (g_folder == QLatin1String("English"))
        return english;

    const auto exact = g_map.constFind(english);
    if (exact != g_map.constEnd())
        return exact.value();
    return english;
}

void applyText(QObject* object, const QString& propertyName, const QString& current,
               const std::function<void(const QString&)>& setter)
{
    const QString sourceKey = QStringLiteral("_loc_src_") + propertyName;
    const QString translatedKey = QStringLiteral("_loc_dst_") + propertyName;
    const QString last = object->property(translatedKey.toUtf8().constData()).toString();
    QString source = object->property(sourceKey.toUtf8().constData()).toString();
    if (source.isEmpty() || current != last) {
        source = current;
        object->setProperty(sourceKey.toUtf8().constData(), source);
    }

    const QString translated = translationFor(source);
    object->setProperty(translatedKey.toUtf8().constData(), translated);
    if (translated != current)
        setter(translated);
}

void translateCombo(QComboBox* combo)
{
    const int count = combo->count();
    QStringList sources = combo->property("_loc_combo_sources").toStringList();
    QStringList last = combo->property("_loc_combo_last").toStringList();
    if (sources.size() != count || last.size() != count) {
        sources.clear();
        last.clear();
        for (int i = 0; i < count; ++i) {
            sources.append(combo->itemText(i));
            last.append(combo->itemText(i));
        }
    }
    for (int i = 0; i < count; ++i) {
        const QString current = combo->itemText(i);
        if (current != last.at(i))
            sources[i] = current;
        const QString translated = translationFor(sources.at(i));
        last[i] = translated;
        if (current != translated)
            combo->setItemText(i, translated);
    }
    combo->setProperty("_loc_combo_sources", sources);
    combo->setProperty("_loc_combo_last", last);
}

void translateTabBar(QTabBar* bar)
{
    const int count = bar->count();
    QStringList sources = bar->property("_loc_tab_sources").toStringList();
    QStringList last = bar->property("_loc_tab_last").toStringList();
    if (sources.size() != count || last.size() != count) {
        sources.clear();
        last.clear();
        for (int i = 0; i < count; ++i) {
            sources.append(bar->tabText(i));
            last.append(bar->tabText(i));
        }
    }
    for (int i = 0; i < count; ++i) {
        const QString current = bar->tabText(i);
        if (current != last.at(i))
            sources[i] = current;
        const QString translated = translationFor(sources.at(i));
        last[i] = translated;
        if (current != translated)
            bar->setTabText(i, translated);
    }
    bar->setProperty("_loc_tab_sources", sources);
    bar->setProperty("_loc_tab_last", last);
}

void translateTree(QTreeWidget* tree)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree->topLevelItem(i);
        if (!item)
            continue;
        QString source = item->data(0, Qt::UserRole + 20).toString();
        QString current = item->text(0);
        if (source.isEmpty()) {
            source = current;
            item->setData(0, Qt::UserRole + 20, source);
        }
        QString prefix;
        if (source.startsWith(QStringLiteral("▼ ")) || source.startsWith(QStringLiteral("▶ "))) {
            prefix = source.left(2);
            source = source.mid(2);
        }
        const QString translated = prefix + translationFor(source);
        if (current != translated)
            item->setText(0, translated);
    }
}

void translateHeaders(QTableWidget* table)
{
    if (!table)
        return;
    QStringList sources = table->property("_loc_header_sources").toStringList();
    QStringList last = table->property("_loc_header_last").toStringList();
    const int count = table->columnCount();
    if (sources.size() != count || last.size() != count) {
        sources.clear();
        last.clear();
        for (int i = 0; i < count; ++i) {
            const QString text = table->horizontalHeaderItem(i)
                ? table->horizontalHeaderItem(i)->text() : QString();
            sources.append(text);
            last.append(text);
        }
    }
    for (int i = 0; i < count; ++i) {
        QTableWidgetItem* item = table->horizontalHeaderItem(i);
        if (!item)
            continue;
        const QString current = item->text();
        if (current != last.at(i))
            sources[i] = current;
        const QString translated = translationFor(sources.at(i));
        last[i] = translated;
        if (current != translated)
            item->setText(translated);
    }
    table->setProperty("_loc_header_sources", sources);
    table->setProperty("_loc_header_last", last);
}

void translateObject(QObject* object)
{
    if (!object || object->property("_loc_skip").toBool())
        return;

    if (auto* widget = qobject_cast<QWidget*>(object)) {
        applyText(widget, QStringLiteral("windowTitle"), widget->windowTitle(),
                  [widget](const QString& value) { widget->setWindowTitle(value); });

        if (auto* label = qobject_cast<QLabel*>(widget)) {
            if (label->objectName() != QLatin1String("PopupSource"))
                applyText(label, QStringLiteral("text"), label->text(),
                          [label](const QString& value) { label->setText(value); });
        }
        if (auto* group = qobject_cast<QGroupBox*>(widget)) {
            applyText(group, QStringLiteral("title"), group->title(),
                      [group](const QString& value) { group->setTitle(value); });
        }
        if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
            applyText(button, QStringLiteral("text"), button->text(),
                      [button](const QString& value) { button->setText(value); });
        }
        if (auto* line = qobject_cast<QLineEdit*>(widget)) {
            applyText(line, QStringLiteral("placeholderText"), line->placeholderText(),
                      [line](const QString& value) { line->setPlaceholderText(value); });
        }
        if (auto* text = qobject_cast<QTextEdit*>(widget)) {
            applyText(text, QStringLiteral("placeholderText"), text->placeholderText(),
                      [text](const QString& value) { text->setPlaceholderText(value); });
        }
        if (auto* text = qobject_cast<QPlainTextEdit*>(widget)) {
            applyText(text, QStringLiteral("placeholderText"), text->placeholderText(),
                      [text](const QString& value) { text->setPlaceholderText(value); });
        }
        if (auto* combo = qobject_cast<QComboBox*>(widget))
            translateCombo(combo);
        if (auto* bar = qobject_cast<QTabBar*>(widget))
            translateTabBar(bar);
        if (auto* tree = qobject_cast<QTreeWidget*>(widget))
            translateTree(tree);
        if (auto* table = qobject_cast<QTableWidget*>(widget))
            translateHeaders(table);
        if (auto* menu = qobject_cast<QMenu*>(widget)) {
            applyText(menu, QStringLiteral("title"), menu->title(),
                      [menu](const QString& value) { menu->setTitle(value); });
            for (QAction* action : menu->actions()) {
                applyText(action, QStringLiteral("text"), action->text(),
                          [action](const QString& value) { action->setText(value); });
            }
        }
    }

    if (auto* action = qobject_cast<QAction*>(object)) {
        applyText(action, QStringLiteral("text"), action->text(),
                  [action](const QString& value) { action->setText(value); });
    }

    for (QObject* child : object->children())
        translateObject(child);
}

class TranslationFilter final : public QObject
{
public:
    explicit TranslationFilter(QApplication* application)
        : QObject(application)
    {
        auto* timer = new QTimer(this);
        timer->setInterval(300);
        connect(timer, &QTimer::timeout, this, [] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (widget && widget->isVisible())
                    translateObject(widget);
            }
        });
        timer->start();
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::Polish:
        case QEvent::LanguageChange:
            if (auto* widget = qobject_cast<QWidget*>(watched))
                translateObject(widget);
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }
};

} // namespace

namespace Localization {

QStringList availableLanguages()
{
    QStringList result;
    const QDir root(localeRoot());
    const QFileInfoList files = root.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo& info : files) {
        const QString key = info.completeBaseName();
        if (!key.isEmpty() && !result.contains(key, Qt::CaseInsensitive))
            result.append(key);
    }
    if (result.isEmpty())
        result = {QStringLiteral("Russian"), QStringLiteral("English")};
    std::sort(result.begin(), result.end(),
              [](const QString& left, const QString& right) {
        return QString::localeAwareCompare(languageDisplayName(left),
                                           languageDisplayName(right)) < 0;
    });
    return result;
}

QString languageDisplayName(const QString& folder)
{
    QFile file(localeFilePath(folder));
    QString english;
    QString native;
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (document.isObject()) {
            const QJsonObject object = document.object();
            english = object.value(QStringLiteral("LanguageEnglish")).toString().trimmed();
            native = object.value(QStringLiteral("LanguageNative")).toString().trimmed();
            if (english.isEmpty())
                english = object.value(QStringLiteral("Language")).toString().trimmed();
            if (native.isEmpty())
                native = object.value(QStringLiteral("Language")).toString().trimmed();
        }
    }
    if (english.isEmpty())
        english = folder == QLatin1String("Russian") ? QStringLiteral("Russian") : folder;
    if (native.isEmpty())
        native = folder == QLatin1String("Russian") ? QStringLiteral("Русский") : english;
    return QStringLiteral("%1 (%2)").arg(english, native);
}

bool load(const QString& folder)
{
    g_map.clear();
    g_replaceKeys.clear();
    g_folder = folder.isEmpty() ? QStringLiteral("Russian") : folder;

    QFile file(localeFilePath(g_folder));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return false;

    const QJsonObject object = document.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.key() == QLatin1String("Language")
            || it.key() == QLatin1String("LanguageEnglish")
            || it.key() == QLatin1String("LanguageNative")
            || !it.value().isString())
            continue;
        g_map.insert(it.key(), it.value().toString());
    }
    g_replaceKeys = g_map.keys();
    std::sort(g_replaceKeys.begin(), g_replaceKeys.end(),
              [](const QString& left, const QString& right) {
                  return left.size() > right.size();
              });
    return true;
}

QString tr(const QString& source)
{
    return translationFor(source);
}

void install(QApplication* application)
{
    if (!application)
        return;
    application->installEventFilter(new TranslationFilter(application));
}

void retranslateAll()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget && widget->isVisible())
            translateObject(widget);
    }
}

} // namespace Localization
