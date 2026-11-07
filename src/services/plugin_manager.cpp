#include "plugin_manager.h"

#include "core/path_manager.h"
#include "core/settings_manager.h"
#include "js_plugin_provider.h"
#include "tesseract_provider.h"
#include "windows_ocr_provider.h"
#include "paddle_ocr_provider.h"
#include "hunspell_provider.h"
#include "windows_spell_provider.h"
#include "nuspell_provider.h"
#include "tts_provider.h"
#include "xdxf_provider.h"
#include "balancer_provider.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>
#include <utility>

PluginManager::PluginManager(PathManager* paths, SettingsManager* settings, QObject* parent)
    : QObject(parent)
    , m_paths(paths)
    , m_settings(settings)
{
}

void PluginManager::scan()
{
    for (IPluginProvider* provider : std::as_const(m_providers))
        delete provider;
    m_providers.clear();
    m_manifests.clear();

    if (!m_paths)
        return;

    scanRoot(QDir(m_paths->shippedDir()).filePath(QStringLiteral("services")));
    scanRoot(QDir(m_paths->dataDir()).filePath(QStringLiteral("services")));

    QStringList serviceOrder;
    if (m_settings) {
        const QJsonArray saved = m_settings->value(QStringLiteral("ServicesOrder")).toArray();
        for (const QJsonValue& value : saved) {
            const QString id = value.toString();
            if (!id.isEmpty())
                serviceOrder.append(id);
        }
    }
    const auto orderRank = [&serviceOrder](const QString& id) {
        const int index = serviceOrder.indexOf(id);
        return index >= 0 ? index : 100000;
    };

    std::sort(m_manifests.begin(), m_manifests.end(), [&orderRank](const PluginManifest& left,
                                                                   const PluginManifest& right) {
        const int leftRank = orderRank(left.id);
        const int rightRank = orderRank(right.id);
        if (leftRank != rightRank)
            return leftRank < rightRank;
        if (left.order != right.order)
            return left.order < right.order;
        return left.name.localeAwareCompare(right.name) < 0;
    });

    const QJsonObject networkConfig = this->networkConfig();
    for (const PluginManifest& manifest : std::as_const(m_manifests)) {
        if (!manifest.valid)
            continue;

        IPluginProvider* provider = nullptr;
        if (manifest.engine == QStringLiteral("javascript")) {
            provider = new JsPluginProvider(manifest, networkConfig, this);
        } else if (manifest.engine == QStringLiteral("native")
                   && manifest.nativeProvider == QStringLiteral("windows_ocr")) {
            provider = new WindowsOcrProvider(manifest, this);
        } else if (manifest.engine == QStringLiteral("native")
                   && manifest.nativeProvider == QStringLiteral("paddleocr")) {
            provider = new PaddleOcrProvider(manifest, this);
        } else if (manifest.engine == QStringLiteral("native")
                   && manifest.nativeProvider == QStringLiteral("hunspell")) {
            provider = new HunspellProvider(manifest, m_paths, this);
        } else if (manifest.engine == QStringLiteral("native")
                   && manifest.nativeProvider == QStringLiteral("nuspell")) {
            provider = new NuspellProvider(manifest, m_paths, this);
        } else if (manifest.engine == QStringLiteral("native")
                   && manifest.nativeProvider == QStringLiteral("tts")) {
            provider = new TtsProvider(manifest, this);
        } else if (manifest.engine == QStringLiteral("native")
                   && manifest.nativeProvider == QStringLiteral("xdxf")) {
            provider = new XdxfProvider(manifest, this);
        } else if (manifest.engine == QStringLiteral("native")
                   && manifest.nativeProvider == QStringLiteral("balancer")) {
            provider = new BalancerProvider(manifest, this, m_settings, this);
        } else if (manifest.engine == QStringLiteral("native")
                   && manifest.nativeProvider == QStringLiteral("windows_spell")) {
            provider = new WindowsSpellProvider(manifest, this);
        } else if (manifest.engine == QStringLiteral("native")
                   && manifest.capabilities.contains(QStringLiteral("ocr"))
                   && !manifest.nativeExecutable.isEmpty()) {
            provider = new TesseractProvider(manifest, this);
        }

        if (provider)
            m_providers.insert(manifest.id, provider);
    }
    emit changed();
}

QList<IPluginProvider*> PluginManager::providers() const
{
    QList<IPluginProvider*> result;
    for (const PluginManifest& manifest : m_manifests) {
        if (m_providers.contains(manifest.id))
            result.append(m_providers.value(manifest.id));
    }
    return result;
}

IPluginProvider* PluginManager::provider(const QString& id) const
{
    return m_providers.value(id, nullptr);
}

QList<IPluginProvider*> PluginManager::providersFor(const QString& capability) const
{
    QList<IPluginProvider*> result;
    for (const PluginManifest& manifest : m_manifests) {
        if (isDisabled(manifest.id))
            continue;
        if (!manifest.capabilities.contains(capability))
            continue;
        if (IPluginProvider* provider = m_providers.value(manifest.id, nullptr))
            result.append(provider);
    }
    return result;
}

QString PluginManager::normalizedServiceId(const QJsonValue& value) const
{
    if (value.isString()) {
        const QString id = value.toString();
        return id.isEmpty() || id == QStringLiteral("auto") ? QString() : id;
    }

    const int numeric = value.toInt();
    switch (numeric) {
    case 1:
    case 11:
        return QStringLiteral("google_translate");
    case 26:
        return QStringLiteral("mymemory");
    case 31:
        return QStringLiteral("deepl");
    default:
        return QString();
    }
}

QStringList PluginManager::activeTranslationIds() const
{
    QStringList result;
    if (!m_settings)
        return result;

    const QJsonArray active = m_settings->value(QStringLiteral("General.ActiveServices")).toArray();
    for (const QJsonValue& value : active) {
        const QString id = normalizedServiceId(value);
        if (id.isEmpty())
            continue;
        IPluginProvider* provider = m_providers.value(id, nullptr);
        if (provider && provider->capabilities().contains(QStringLiteral("translate"))
            && !isDisabled(id) && !result.contains(id)) {
            result.append(id);
        }
    }

    if (result.isEmpty()) {
        const QList<IPluginProvider*> providers = providersFor(QStringLiteral("translate"));
        if (!providers.isEmpty())
            result.append(providers.first()->id());
    }
    return result;
}

void PluginManager::setActiveServices(const QStringList& ids)
{
    if (!m_settings)
        return;
    QJsonArray array;
    for (const QString& id : ids)
        array.append(id);
    m_settings->setValue(QStringLiteral("General.ActiveServices"), array);
    m_settings->save();
    emit changed();
}

bool PluginManager::isDisabled(const QString& id) const
{
    if (!m_settings)
        return false;
    const QJsonArray disabled = m_settings->value(QStringLiteral("DisabledServices")).toArray();
    for (const QJsonValue& value : disabled) {
        if (value.toString() == id)
            return true;
    }

    for (const PluginManifest& manifest : m_manifests) {
        if (manifest.id != id)
            continue;
        if (manifest.enabledByDefault)
            return false;
        const QJsonArray enabled = m_settings->value(QStringLiteral("EnabledServices")).toArray();
        for (const QJsonValue& value : enabled) {
            if (value.toString() == id)
                return false;
        }
        // Shipped as opt-in: stays off until the user enables it explicitly.
        return true;
    }
    return false;
}

void PluginManager::setDisabled(const QString& id, bool disabled)
{
    if (!m_settings)
        return;
    QJsonArray values = m_settings->value(QStringLiteral("DisabledServices")).toArray();
    QJsonArray updated;
    for (const QJsonValue& value : values) {
        if (value.toString() != id)
            updated.append(value);
    }
    if (disabled)
        updated.append(id);
    m_settings->setValue(QStringLiteral("DisabledServices"), updated);

    QJsonArray enabledValues = m_settings->value(QStringLiteral("EnabledServices")).toArray();
    QJsonArray enabled;
    for (const QJsonValue& value : enabledValues) {
        if (value.toString() != id)
            enabled.append(value);
    }
    if (!disabled)
        enabled.append(id);
    m_settings->setValue(QStringLiteral("EnabledServices"), enabled);

    m_settings->save();
    emit changed();
}

QJsonObject PluginManager::networkConfig() const
{
    QJsonObject config;
    if (!m_settings)
        return config;
    config.insert(QStringLiteral("Timeout"),
                  m_settings->intValue(QStringLiteral("Internet.Timeout"), 20000));
    config.insert(QStringLiteral("ConnectTimeout"),
                  m_settings->intValue(QStringLiteral("Internet.ConnectTimeout"), 5000));
    config.insert(QStringLiteral("DataTimeout"),
                  m_settings->intValue(QStringLiteral("Internet.DataTimeout"), 15000));
    config.insert(QStringLiteral("RetryFailed"),
                  m_settings->boolValue(QStringLiteral("Internet.RetryFailed"), false));
    config.insert(QStringLiteral("RetryCount"),
                  m_settings->intValue(QStringLiteral("Internet.RetryCount"), 1));
    config.insert(QStringLiteral("RetryDelay"),
                  m_settings->intValue(QStringLiteral("Internet.RetryDelay"), 1));
    config.insert(QStringLiteral("Proxy"),
                  m_settings->value(QStringLiteral("Proxy")).toObject());
    return config;
}

void PluginManager::refreshNetworkConfig()
{
    const QJsonObject config = networkConfig();
    for (IPluginProvider* provider : std::as_const(m_providers)) {
        if (auto* js = qobject_cast<JsPluginProvider*>(provider))
            js->setNetworkConfig(config);
    }
}

void PluginManager::scanRoot(const QString& root)
{
    QDir rootDir(root);
    if (!rootDir.exists())
        return;

    QDirIterator iterator(root, {QStringLiteral("manifest.json")}, QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString manifestPath = iterator.next();
        PluginManifest manifest = parseManifest(manifestPath, root);
        if (manifest.id.isEmpty())
            continue;

        if (!manifest.valid) {
            qWarning().noquote() << "[plugins]" << manifest.id << ":" << manifest.error;
            continue;
        }

        bool replaced = false;
        for (int index = 0; index < m_manifests.size(); ++index) {
            if (m_manifests.at(index).id == manifest.id) {
                if (manifest.type.isEmpty())
                    manifest.type = m_manifests.at(index).type;
                m_manifests[index] = manifest;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            if (manifest.type.isEmpty())
                manifest.type = QStringLiteral("translate");
            m_manifests.append(manifest);
        }
    }
}

PluginManifest PluginManager::parseManifest(const QString& manifestPath, const QString& root)
{
    PluginManifest manifest;
    manifest.directory = QFileInfo(manifestPath).absolutePath();

    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        manifest.error = QStringLiteral("не удалось открыть manifest.json");
        return manifest;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        manifest.error = QStringLiteral("ошибка JSON: %1").arg(parseError.errorString());
        return manifest;
    }

    const QJsonObject object = document.object();
    manifest.raw = object;
    manifest.id = object.value(QStringLiteral("id")).toString(manifest.directory.section('/', -1));
    manifest.name = object.value(QStringLiteral("name")).toString(manifest.id);
    manifest.version = object.value(QStringLiteral("version")).toString();
    manifest.description = object.value(QStringLiteral("description")).toString();
    manifest.access = object.value(QStringLiteral("access")).toString();
    manifest.engine = object.value(QStringLiteral("engine")).toString(QStringLiteral("javascript"));
    manifest.type = object.value(QStringLiteral("type")).toString();

    const QString relative = QDir(root).relativeFilePath(manifest.directory);
    const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (manifest.type.isEmpty() && parts.size() >= 2)
        manifest.type = parts.first();

    auto resolvePath = [&manifest](const QString& value) -> QString {
        if (value.isEmpty())
            return QString();
        const QFileInfo info(value);
        if (info.isAbsolute())
            return info.absoluteFilePath();
        return QDir(manifest.directory).filePath(value);
    };

    const QString scriptName = object.value(QStringLiteral("service_script"))
                                   .toString(QStringLiteral("service.js"));
    manifest.serviceScript = resolvePath(scriptName);
    manifest.iconPath = resolvePath(object.value(QStringLiteral("icon"))
                                        .toString(QStringLiteral("icon.png")));
    manifest.nativeExecutable = resolvePath(object.value(QStringLiteral("native_executable")).toString());
    manifest.nativeProvider = object.value(QStringLiteral("native_provider")).toString();
    manifest.tessdataPath = resolvePath(object.value(QStringLiteral("tessdata_path")).toString());
    manifest.settingsPath = settingsPathFor(manifest.id);

    const QJsonArray capabilities = object.value(QStringLiteral("capabilities")).toArray();
    for (const QJsonValue& value : capabilities)
        manifest.capabilities.append(value.toString());

    manifest.settingsSchema = object.value(QStringLiteral("settings_schema")).toArray();
    manifest.order = object.value(QStringLiteral("order")).toInt(100);
    manifest.enabledByDefault = object.value(QStringLiteral("enabled_by_default")).toBool(true);

    if (manifest.engine == QStringLiteral("javascript")
        && !QFileInfo::exists(manifest.serviceScript)) {
        manifest.error = QStringLiteral("service.js не найден");
        return manifest;
    }
    if (manifest.engine == QStringLiteral("native")
        && manifest.nativeProvider.isEmpty()
        && !QFileInfo::exists(manifest.nativeExecutable)) {
        manifest.error = QStringLiteral("нативная библиотека/исполняемый файл не найдены");
        return manifest;
    }
    if (manifest.capabilities.isEmpty()) {
        manifest.error = QStringLiteral("не указаны capabilities");
        return manifest;
    }

    manifest.valid = true;
    return manifest;
}

QString PluginManager::settingsPathFor(const QString& id) const
{
    if (!m_paths)
        return QString();
    return QDir(QDir(m_paths->dataDir()).filePath(QStringLiteral("services")))
        .filePath(id + QStringLiteral("/settings.json"));
}
