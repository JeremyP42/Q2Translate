#include "xdxf_provider.h"

#include "core/localization.h"
#include "modules/dictionary/xdxf_dictionary.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QtConcurrent>

namespace {

QJsonObject errorResult(const QString& text)
{
    QJsonObject result;
    result.insert(QStringLiteral("error"), text);
    return result;
}

} // namespace

XdxfProvider::XdxfProvider(const PluginManifest& manifest, QObject* parent)
    : IPluginProvider(parent)
    , m_manifest(manifest)
{
    loadSettings();
}

QJsonObject XdxfProvider::defaultSettings() const
{
    QJsonObject settings;
    for (const QJsonValue& value : m_manifest.settingsSchema) {
        const QJsonObject field = value.toObject();
        const QString key = field.value(QStringLiteral("key")).toString();
        if (key.isEmpty())
            continue;
        settings.insert(key, field.value(QStringLiteral("default")));
    }
    return settings;
}

bool XdxfProvider::loadSettings()
{
    m_settings = defaultSettings();
    if (m_manifest.settingsPath.isEmpty())
        return true;
    QFile file(m_manifest.settingsPath);
    if (!file.open(QIODevice::ReadOnly))
        return true;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return true;
    const QJsonObject stored = document.object();
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it)
        m_settings.insert(it.key(), it.value());
    return true;
}

bool XdxfProvider::saveSettings()
{
    if (m_manifest.settingsPath.isEmpty())
        return false;
    QDir().mkpath(QFileInfo(m_manifest.settingsPath).absolutePath());
    QSaveFile file(m_manifest.settingsPath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray data = QJsonDocument(m_settings).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit())
        return false;
    return true;
}

bool XdxfProvider::updateSettings(const QJsonObject& settings)
{
    m_settings = settings;
    const bool saved = saveSettings();
    emit settingsChanged();
    return saved;
}

QStringList XdxfProvider::dictionaryFolders(const QString& root)
{
    QStringList folders;
    if (root.isEmpty())
        return folders;
    const QDir rootDir(root);
    QSet<QString> seen;
    QDirIterator iterator(root, {QStringLiteral("*.xdxf")}, QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString directory = QFileInfo(iterator.next()).absolutePath();
        const QString relative = QDir::cleanPath(rootDir.relativeFilePath(directory));
        if (relative.isEmpty() || relative == QStringLiteral("."))
            continue;
        if (seen.contains(relative))
            continue;
        seen.insert(relative);
        folders.append(relative);
    }
    folders.sort(Qt::CaseInsensitive);
    return folders;
}

QStringList XdxfProvider::dictionaryFiles() const
{
    QStringList files;
    QString root = m_manifest.directory;
    if (root.isEmpty())
        return files;
    const QString folder = m_settings.value(QStringLiteral("folder")).toString().trimmed();
    if (!folder.isEmpty() && folder != QStringLiteral(".")) {
        const QString selected = QDir(root).filePath(folder);
        if (QFileInfo::exists(selected))
            root = selected;
    }
    QDirIterator iterator(root, {QStringLiteral("*.xdxf")}, QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext())
        files.append(iterator.next());
    files.sort(Qt::CaseInsensitive);
    return files;
}

void XdxfProvider::executeAsync(const QString& capability, const QJsonObject& params,
                                PluginCallback callback)
{
    if (capability != QStringLiteral("dictionary")) {
        callback(errorResult(Localization::tr(QStringLiteral("XDXF: неподдерживаемая возможность."))));
        return;
    }
    const QString word = params.value(QStringLiteral("word")).toString(
        params.value(QStringLiteral("text")).toString()).trimmed();
    const bool exact = params.value(QStringLiteral("exact")).toBool();
    if (word.isEmpty()) {
        callback(errorResult(Localization::tr(QStringLiteral("XDXF: пустой запрос."))));
        return;
    }
    const QStringList files = dictionaryFiles();
    if (files.isEmpty()) {
        callback(errorResult(Localization::tr(QStringLiteral(
            "XDXF: в папке коннектора не найдены файлы словарей (*.xdxf)."))));
        return;
    }

    auto future = QtConcurrent::run([files, word, exact] {
        QVector<DictionaryEntry> entries;
        for (const QString& path : files) {
            XdxfDictionary dictionary(path);
            QString error;
            if (!dictionary.open(&error))
                continue;
            entries += dictionary.search(word, exact);
        }
        return entries;
    });
    auto* watcher = new QFutureWatcher<QVector<DictionaryEntry>>(this);
    connect(watcher, &QFutureWatcher<QVector<DictionaryEntry>>::finished, this,
            [watcher, word, callback] {
                const QVector<DictionaryEntry> entries = watcher->result();
                watcher->deleteLater();
                if (entries.isEmpty()) {
                    QJsonObject result;
                    result.insert(QStringLiteral("title"), word);
                    result.insert(QStringLiteral("html"),
                                  Localization::tr(QStringLiteral("<p>Ничего не найдено.</p>")));
                    result.insert(QStringLiteral("text"), QString());
                    callback(result);
                    return;
                }
                QString html;
                for (const DictionaryEntry& entry : entries) {
                    html += QStringLiteral("<div class='dict-pos'>%1</div>%2")
                                .arg(entry.dictionaryName.toHtmlEscaped(),
                                     entry.definitionHtml);
                }
                QJsonObject result;
                result.insert(QStringLiteral("title"), entries.first().word);
                result.insert(QStringLiteral("html"), html);
                result.insert(QStringLiteral("text"), entries.first().word);
                callback(result);
            });
    watcher->setFuture(future);
}
