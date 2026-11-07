#include "windows_spell_provider.h"

#include <QLocale>
#include <QSet>

#include <objbase.h>
#include <spellcheck.h>

#include <string>

namespace {

const CLSID kSpellCheckerFactory = {0x7ab36653, 0x1796, 0x484b,
                                    {0xbd, 0xfa, 0xe7, 0x4f, 0x1d, 0xb7, 0xc1, 0xdc}};
const IID kSpellCheckerFactoryIid = {0x8e018a9d, 0x2415, 0x4677,
                                     {0xbf, 0x08, 0x79, 0x4e, 0xa6, 0x1f, 0x94, 0xbb}};

} // namespace

WindowsSpellProvider::WindowsSpellProvider(const PluginManifest& manifest, QObject* parent)
    : IPluginProvider(parent)
    , m_manifest(manifest)
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = initialized == S_OK || initialized == S_FALSE;
}

WindowsSpellProvider::~WindowsSpellProvider()
{
    releaseChecker();
    if (m_factory) {
        m_factory->Release();
        m_factory = nullptr;
    }
    if (m_comInitialized)
        CoUninitialize();
}

bool WindowsSpellProvider::updateSettings(const QJsonObject& settings)
{
    m_settings = settings;
    emit settingsChanged();
    return true;
}

bool WindowsSpellProvider::ensureFactory()
{
    if (m_factory)
        return true;
    const HRESULT result = CoCreateInstance(kSpellCheckerFactory, nullptr,
                                             CLSCTX_INPROC_SERVER,
                                             kSpellCheckerFactoryIid,
                                             reinterpret_cast<void**>(&m_factory));
    return SUCCEEDED(result) && m_factory;
}

void WindowsSpellProvider::releaseChecker()
{
    if (m_checker) {
        m_checker->Release();
        m_checker = nullptr;
    }
    m_checkerTag.clear();
    m_cache.clear();
}

bool WindowsSpellProvider::ensureChecker(const QString& language)
{
    const QString tag = languageTag(language);
    if (m_checker && m_checkerTag == tag)
        return true;
    releaseChecker();
    if (!ensureFactory())
        return false;
    const std::wstring value = tag.toStdWString();
    const HRESULT result = m_factory->CreateSpellChecker(value.c_str(), &m_checker);
    if (FAILED(result)) {
        m_checker = nullptr;
        return false;
    }
    m_checkerTag = tag;
    return true;
}

QString WindowsSpellProvider::languageTag(const QString& code) const
{
    QString normalized = code.trimmed().toLower();
    if (normalized.isEmpty() || normalized == QStringLiteral("auto"))
        normalized = QLocale::system().name().toLower();
    if (normalized.contains(QLatin1Char('-')))
        return normalized;
    if (normalized == QStringLiteral("en")) return QStringLiteral("en-US");
    if (normalized == QStringLiteral("ru")) return QStringLiteral("ru-RU");
    if (normalized == QStringLiteral("de")) return QStringLiteral("de-DE");
    if (normalized == QStringLiteral("fr")) return QStringLiteral("fr-FR");
    if (normalized == QStringLiteral("es")) return QStringLiteral("es-ES");
    if (normalized == QStringLiteral("it")) return QStringLiteral("it-IT");
    if (normalized == QStringLiteral("pt")) return QStringLiteral("pt-BR");
    if (normalized == QStringLiteral("uk")) return QStringLiteral("uk-UA");
    if (normalized == QStringLiteral("pl")) return QStringLiteral("pl-PL");
    if (normalized == QStringLiteral("tr")) return QStringLiteral("tr-TR");
    if (normalized == QStringLiteral("nl")) return QStringLiteral("nl-NL");
    if (normalized == QStringLiteral("sv")) return QStringLiteral("sv-SE");
    if (normalized == QStringLiteral("da")) return QStringLiteral("da-DK");
    if (normalized == QStringLiteral("fi")) return QStringLiteral("fi-FI");
    if (normalized == QStringLiteral("nb")) return QStringLiteral("nb-NO");
    if (normalized == QStringLiteral("cs")) return QStringLiteral("cs-CZ");
    if (normalized == QStringLiteral("sk")) return QStringLiteral("sk-SK");
    if (normalized == QStringLiteral("hu")) return QStringLiteral("hu-HU");
    if (normalized == QStringLiteral("ro")) return QStringLiteral("ro-RO");
    if (normalized == QStringLiteral("bg")) return QStringLiteral("bg-BG");
    if (normalized == QStringLiteral("hr")) return QStringLiteral("hr-HR");
    if (normalized == QStringLiteral("sr")) return QStringLiteral("sr-Latn-RS");
    if (normalized == QStringLiteral("sl")) return QStringLiteral("sl-SI");
    if (normalized == QStringLiteral("el")) return QStringLiteral("el-GR");
    if (normalized == QStringLiteral("he") || normalized == QStringLiteral("iw"))
        return QStringLiteral("he-IL");
    if (normalized == QStringLiteral("ar")) return QStringLiteral("ar-SA");
    return normalized;
}

QList<SpellLanguage> WindowsSpellProvider::spellLanguages() const
{
    QList<SpellLanguage> result;
    auto* self = const_cast<WindowsSpellProvider*>(this);
    if (!self->ensureFactory())
        return result;

    IEnumString* languages = nullptr;
    if (FAILED(m_factory->get_SupportedLanguages(&languages)) || !languages)
        return result;

    QSet<QString> seen;
    LPOLESTR value = nullptr;
    while (languages->Next(1, &value, nullptr) == S_OK) {
        const QString tag = QString::fromWCharArray(value);
        CoTaskMemFree(value);
        value = nullptr;
        if (tag.isEmpty() || seen.contains(tag))
            continue;
        seen.insert(tag);
        SpellLanguage language;
        language.code = tag;
        const QLocale locale(tag);
        QString name = locale.nativeLanguageName();
        if (name.isEmpty())
            name = tag;
        language.name = name;
        language.installed = true;
        language.downloadable = false;
        language.removable = false;
        result.append(language);
    }
    languages->Release();
    return result;
}

bool WindowsSpellProvider::checkSpelling(const QString& word, const QString& language,
                                         QStringList* suggestions)
{
    if (word.isEmpty())
        return true;
    if (!ensureChecker(language))
        return true;

    const QString key = m_checkerTag + QLatin1Char('|') + word.toLower();
    const auto cached = m_cache.constFind(key);
    if (cached != m_cache.constEnd())
        return cached.value();

    IEnumSpellingError* errors = nullptr;
    const std::wstring value = word.toStdWString();
    const HRESULT result = m_checker->Check(value.c_str(), &errors);
    bool correct = true;
    if (SUCCEEDED(result) && errors) {
        ISpellingError* error = nullptr;
        if (errors->Next(&error) == S_OK && error) {
            correct = false;
            error->Release();
        }
        errors->Release();
    }
    if (!correct && suggestions) {
        IEnumString* values = nullptr;
        if (SUCCEEDED(m_checker->Suggest(value.c_str(), &values)) && values) {
            LPOLESTR item = nullptr;
            while (values->Next(1, &item, nullptr) == S_OK) {
                suggestions->append(QString::fromWCharArray(item));
                CoTaskMemFree(item);
                item = nullptr;
                if (suggestions->size() >= 7)
                    break;
            }
            values->Release();
        }
    }
    m_cache.insert(key, correct);
    return correct;
}

void WindowsSpellProvider::installSpellLanguage(
    const QString& code, std::function<void(bool, const QString&)> done)
{
    done(false, QStringLiteral("языки Windows устанавливаются в параметрах системы (%1)")
                   .arg(code));
}

void WindowsSpellProvider::removeSpellLanguage(
    const QString& code, std::function<void(bool, const QString&)> done)
{
    done(false, QStringLiteral("языки Windows управляются системой (%1)").arg(code));
}

void WindowsSpellProvider::executeAsync(const QString& capability,
                                        const QJsonObject& params,
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
    callback({{QStringLiteral("correct"), checkSpelling(word, language)}});
}
