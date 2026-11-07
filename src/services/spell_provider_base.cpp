#include "spell_provider_base.h"

#include "core/path_manager.h"
#include "spell_language_catalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

namespace {

bool validCode(const QString& code)
{
    if (code.isEmpty() || code.size() > 16)
        return false;
    for (const QChar character : code) {
        if (!character.isLetterOrNumber() && character != QLatin1Char('-')
            && character != QLatin1Char('_')) {
            return false;
        }
    }
    return true;
}

} // namespace

SpellProviderBase::SpellProviderBase(const PluginManifest& manifest, PathManager* paths,
                                     QObject* parent)
    : IPluginProvider(parent)
    , m_manifest(manifest)
    , m_paths(paths)
    , m_network(new QNetworkAccessManager(this))
{
    loadSettings();
}

SpellProviderBase::~SpellProviderBase()
{
    qDeleteAll(m_engines);
    m_engines.clear();
}

void SpellProviderBase::loadSettings()
{
    if (m_manifest.settingsPath.isEmpty())
        return;
    QFile file(m_manifest.settingsPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (document.isObject())
        m_settings = document.object();
}

void SpellProviderBase::saveSettings()
{
    if (m_manifest.settingsPath.isEmpty())
        return;
    QDir().mkpath(QFileInfo(m_manifest.settingsPath).absolutePath());
    QSaveFile file(m_manifest.settingsPath);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(m_settings).toJson(QJsonDocument::Indented));
    file.commit();
}

bool SpellProviderBase::updateSettings(const QJsonObject& settings)
{
    m_settings = settings;
    saveSettings();
    emit settingsChanged();
    return true;
}

QString SpellProviderBase::dictionaryRoot() const
{
    if (!m_writableRoot.isEmpty())
        return m_writableRoot;

    QString root = QDir(m_manifest.directory).filePath(QStringLiteral("dictionaries"));
    if (!QDir().mkpath(root)) {
        const QString fallbackRoot = m_paths
            ? QDir(m_paths->dataDir()).filePath(
                  QStringLiteral("services/spelling/%1/dictionaries").arg(m_manifest.id))
            : QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                  .filePath(QStringLiteral("services/spelling/%1/dictionaries").arg(m_manifest.id));
        root = fallbackRoot;
        QDir().mkpath(root);
    }
    const_cast<SpellProviderBase*>(this)->m_writableRoot = root;
    return root;
}

QString SpellProviderBase::languageDirectory(const QString& code) const
{
    return QDir(dictionaryRoot()).filePath(code);
}

bool SpellProviderBase::languageInstalled(const QString& code) const
{
    if (!validCode(code))
        return false;
    const QDir directory(languageDirectory(code));
    return QFileInfo::exists(directory.filePath(QStringLiteral("index.aff")))
        && QFileInfo::exists(directory.filePath(QStringLiteral("index.dic")));
}

QString SpellProviderBase::spellDictionaryPath() const
{
    return dictionaryRoot();
}

QString SpellProviderBase::normalizedCode(const QString& code) const
{
    QString value = code.trimmed().toLower();
    if (value.isEmpty() || value == QStringLiteral("auto"))
        return QString();
    if (value == QStringLiteral("nb-no") || value == QStringLiteral("no"))
        value = QStringLiteral("nb");
    if (value == QStringLiteral("zh-cn") || value == QStringLiteral("zh-tw"))
        value = QStringLiteral("zh");
    if (value.contains(QLatin1Char('-')))
        value = value.section(QLatin1Char('-'), 0, 0);
    return value;
}

QString SpellProviderBase::guessLanguage(const QString& word) const
{
    bool cyrillic = false;
    bool greek = false;
    for (const QChar character : word) {
        const ushort value = character.unicode();
        if (value >= 0x0400 && value <= 0x04FF)
            cyrillic = true;
        else if (value >= 0x0370 && value <= 0x03FF)
            greek = true;
    }
    if (cyrillic)
        return QStringLiteral("ru");
    if (greek)
        return QStringLiteral("el");
    return QStringLiteral("en");
}

QList<SpellLanguage> SpellProviderBase::spellLanguages() const
{
    QList<SpellLanguage> result;
    QStringList known;
    for (const SpellLanguageEntry& entry : spellLanguageCatalog()) {
        const QString code = entry.code;
        known.append(code);
        SpellLanguage language;
        language.code = code;
        language.name = entry.name;
        language.installed = languageInstalled(code);
        language.downloadable = true;
        language.removable = language.installed;
        result.append(language);
    }

    const QDir root(dictionaryRoot());
    for (const QFileInfo& info : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                     QDir::Name)) {
        const QString code = info.fileName();
        if (known.contains(code) || !languageInstalled(code))
            continue;
        SpellLanguage language;
        language.code = code;
        language.name = spellLanguageName(code);
        language.installed = true;
        language.downloadable = false;
        language.removable = true;
        result.append(language);
    }
    return result;
}

ISpellEngine* SpellProviderBase::engineFor(const QString& code)
{
    if (!languageInstalled(code))
        return nullptr;
    if (ISpellEngine* engine = m_engines.value(code, nullptr)) {
        if (engine->isLoaded())
            return engine;
    }

    const QDir directory(languageDirectory(code));
    QString error;
    ISpellEngine* engine = createEngine(
        directory.filePath(QStringLiteral("index.aff")),
        directory.filePath(QStringLiteral("index.dic")), &error);
    if (!engine)
        return nullptr;
    m_engines.insert(code, engine);
    return engine;
}

bool SpellProviderBase::checkSpelling(const QString& word, const QString& language,
                                     QStringList* suggestions)
{
    if (word.isEmpty())
        return true;

    QString code = normalizedCode(language);
    if (code.isEmpty())
        code = guessLanguage(word);

    if (!languageInstalled(code)) {
        const QString base = code.section(QLatin1Char('-'), 0, 0);
        if (base != code && languageInstalled(base))
            code = base;
        else
            return true; // no dictionary -> do not underline anything
    }

    ISpellEngine* engine = engineFor(code);
    if (!engine)
        return true;
    if (engine->spell(word))
        return true;
    if (suggestions)
        *suggestions = engine->suggest(word, 7);
    return false;
}

void SpellProviderBase::downloadLanguageFile(const QString& code, const QString& suffix,
                                            const QString& targetPath,
                                            std::function<void(bool, const QString&)> done)
{
    const QUrl url(QStringLiteral(
                       "https://raw.githubusercontent.com/wooorm/dictionaries/main/"
                       "dictionaries/%1/index.%2")
                       .arg(code, suffix));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Q2Translate/2.7.1"));
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [reply, targetPath, done = std::move(done)] {
        const QByteArray data = reply->readAll();
        const QString error = reply->error() == QNetworkReply::NoError
            ? QString() : reply->errorString();
        reply->deleteLater();
        if (!error.isEmpty() || data.isEmpty()) {
            done(false, error.isEmpty() ? QStringLiteral("пустой ответ") : error);
            return;
        }
        QDir().mkpath(QFileInfo(targetPath).absolutePath());
        QSaveFile file(targetPath);
        if (!file.open(QIODevice::WriteOnly)) {
            done(false, file.errorString());
            return;
        }
        if (file.write(data) != data.size() || !file.commit()) {
            done(false, file.errorString());
            return;
        }
        done(true, QString());
    });
}

void SpellProviderBase::installSpellLanguage(
    const QString& code, std::function<void(bool, const QString&)> done)
{
    if (!isSpellLanguageCode(code) || !validCode(code)) {
        done(false, QStringLiteral("неизвестный язык: %1").arg(code));
        return;
    }

    const QString directory = languageDirectory(code);
    QDir().mkpath(directory);

    downloadLanguageFile(code, QStringLiteral("aff"),
                         QDir(directory).filePath(QStringLiteral("index.aff")),
                         [this, code, directory, done = std::move(done)](
                             bool affOk, const QString& affError) {
        if (!affOk) {
            QDir(directory).removeRecursively();
            done(false, affError);
            return;
        }
        downloadLanguageFile(code, QStringLiteral("dic"),
                             QDir(directory).filePath(QStringLiteral("index.dic")),
                             [this, code, directory, done = std::move(done)](
                                 bool dicOk, const QString& dicError) {
            if (!dicOk) {
                QDir(directory).removeRecursively();
                done(false, dicError);
                return;
            }
            delete m_engines.take(code);
            done(true, QString());
        });
    });
}

void SpellProviderBase::removeSpellLanguage(
    const QString& code, std::function<void(bool, const QString&)> done)
{
    if (!validCode(code) || !languageInstalled(code)) {
        done(false, QStringLiteral("язык не установлен: %1").arg(code));
        return;
    }
    const QString directory = languageDirectory(code);
    if (!QDir(directory).removeRecursively()) {
        done(false, QStringLiteral("не удалось удалить %1").arg(directory));
        return;
    }
    delete m_engines.take(code);
    done(true, QString());
}

void SpellProviderBase::executeAsync(const QString& capability, const QJsonObject& params,
                                    PluginCallback callback)
{
    if (capability != QStringLiteral("spelling")) {
        callback({{QStringLiteral("error"),
                   QStringLiteral("неподдерживаемая возможность: %1").arg(capability)}});
        return;
    }

    const QString action = params.value(QStringLiteral("action")).toString(
        QStringLiteral("check"));
    if (action == QStringLiteral("languages")) {
        QJsonArray languages;
        for (const SpellLanguage& language : spellLanguages()) {
            QJsonObject object;
            object.insert(QStringLiteral("code"), language.code);
            object.insert(QStringLiteral("name"), language.name);
            object.insert(QStringLiteral("installed"), language.installed);
            object.insert(QStringLiteral("downloadable"), language.downloadable);
            object.insert(QStringLiteral("removable"), language.removable);
            languages.append(object);
        }
        callback({{QStringLiteral("languages"), languages}});
        return;
    }

    const QString word = params.value(QStringLiteral("word")).toString();
    const QString language = params.value(QStringLiteral("language")).toString(
        QStringLiteral("auto"));
    QStringList suggestions;
    const bool correct = checkSpelling(word, language, &suggestions);
    QJsonArray suggestionValues;
    for (const QString& suggestion : suggestions)
        suggestionValues.append(suggestion);
    callback({{QStringLiteral("correct"), correct},
              {QStringLiteral("suggestions"), suggestionValues}});
}
