#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

#include "core/app_context.h"
#include "core/webp_image.h"
#include "modules/image/image_translation_pipeline.h"
#include "core/exceptions_manager.h"
#include "core/history_manager.h"
#include "core/hotkey_utils.h"
#include "core/language_router.h"
#include "core/localization.h"
#include "core/path_manager.h"
#include "core/settings_manager.h"
#include "core/translation_manager.h"
#include "modules/dictionary/dictionary_manager.h"
#include "modules/dictionary/xdxf_dictionary.h"
#include "platform/win/clipboard_manager.h"
#include "platform/win/global_hotkey_manager.h"
#include "platform/win/window_utils.h"
#include "services/http_bridge.h"
#include "services/plugin_manager.h"
#include "ui/icon_factory.h"
#include "ui/font_tuning.h"
#include "ui/main_window.h"
#include "ui/theme_manager.h"
#include "ui/theme_schedule.h"
#include "ui/wheel_guard.h"
#include "ui/tray_icon.h"
#include "ui/settings/plugin_settings_dialog.h"
#include "ui/settings/tesseract_language_dialog.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QList>
#include <QPainter>
#include <QPair>
#include <QStyleFactory>
#include <QTextStream>
#include <QTimer>

namespace {

void showAbout(QWidget* parent)
{
    QMessageBox::about(
        parent,
        QStringLiteral("О программе"),
        QStringLiteral("<b>Q2Translate</b><br>"
                       "Версия %1<br><br>"
                       "Кроссплатформенное приложение на Qt 6 Widgets.<br>"
                       "JSON-плагины, локальный Tesseract OCR, XDXF-словари, "
                       "Double Ctrl и глобальные горячие клавиши.")
            .arg(QApplication::applicationVersion()));
}

bool isExcepted(AppContext& context)
{
    if (!context.exceptions)
        return false;
    const WindowUtils::ForegroundInfo info = WindowUtils::foregroundWindow();
    return context.exceptions->isExcepted(info.processName, info.windowClass);
}

void appendLog(QString& log, const QString& text)
{
    log += text;
    log += QLatin1Char('\n');
    qInfo().noquote() << text;
}

int runSelfTest(SettingsManager& settings, PluginManager& plugins,
                TranslationManager& translation, DictionaryManager& dictionary,
                PathManager& paths)
{
    Q_UNUSED(dictionary);
    QString log;
    appendLog(log, QStringLiteral("Q2Translate self-test"));
    appendLog(log, QStringLiteral("Data: %1").arg(paths.dataDir()));
    appendLog(log, QStringLiteral("Plugins: %1").arg(plugins.manifests().size()));
    appendLog(log, QStringLiteral("detect ru: %1")
                       .arg(LanguageRouter::detectLanguage(
                           QString::fromUtf8("Привет мир, как дела сегодня"))));
    appendLog(log, QStringLiteral("detect en: %1")
                       .arg(LanguageRouter::detectLanguage(
                           QStringLiteral("Hello world, how are you today"))));
    {
        // Direction follows TranslationRules ("Правила перевода").
        LanguageRouter router(&settings);
        appendLog(log, QStringLiteral("route en: %1").arg(router.resolveTarget(
                           LanguageRouter::detectLanguage(
                               QStringLiteral("Hello world, how are you today")))));
        appendLog(log, QStringLiteral("route ru: %1").arg(router.resolveTarget(
                           LanguageRouter::detectLanguage(
                               QString::fromUtf8("Привет мир, как дела сегодня")))));
    }
    for (const PluginManifest& manifest : plugins.manifests()) {
        appendLog(log, QStringLiteral("  %1 [%2] engine=%3 caps=%4")
                           .arg(manifest.id, manifest.type, manifest.engine,
                                manifest.capabilities.join(QLatin1Char(','))));
    }

    {
        QEventLoop loop;
        QStringList results;
        QObject::connect(&translation, &TranslationManager::serviceResult,
                         &loop, [&](const QString&, const QString& serviceId,
                                    const QJsonObject& result) {
            const QString error = result.value(QStringLiteral("error")).toString();
            const QString text = result.value(QStringLiteral("text")).toString();
            const QString line = error.isEmpty()
                ? QStringLiteral("%1: %2").arg(serviceId, text.left(120))
                : QStringLiteral("%1 ERROR: %2").arg(serviceId, error);
            results.append(line);
            appendLog(log, QStringLiteral("translate %1").arg(line));
        });
        QObject::connect(&translation, &TranslationManager::translationFinished,
                         &loop, &QEventLoop::quit);
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        translation.translate(QStringLiteral("Hello, world! This is a translation test."),
                              QStringLiteral("en"), QStringLiteral("ru"),
                              {QStringLiteral("google_translate"), QStringLiteral("mymemory")});
        loop.exec();
        if (results.isEmpty())
            appendLog(log, QStringLiteral("translate: no service result"));
    }

    const QList<QPair<QString, QString>> ocrTests = {
        {QStringLiteral("tesseract_local"), QStringLiteral("eng")},
        {QStringLiteral("windows_ocr"), QStringLiteral("en-US")}
    };
    for (const auto& ocrTest : ocrTests) {
        QImage image(800, 160, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        QFont font(QStringLiteral("Segoe UI"));
        font.setPixelSize(48);
        painter.setFont(font);
        painter.drawText(30, 100, QStringLiteral("Hello OCR 123"));
        painter.end();

        QEventLoop loop;
        QObject::connect(&translation, &TranslationManager::ocrResult,
                         &loop, [&](const QString& serviceId, const QJsonObject& result) {
            const QString error = result.value(QStringLiteral("error")).toString();
            const QString text = result.value(QStringLiteral("text")).toString();
            appendLog(log, error.isEmpty()
                ? QStringLiteral("ocr %1: %2").arg(serviceId, text)
                : QStringLiteral("ocr %1 ERROR: %2").arg(serviceId, error));
            loop.quit();
        });
        QTimer::singleShot(45000, &loop, &QEventLoop::quit);
        translation.ocrAsync(image, ocrTest.first, ocrTest.second);
        loop.exec();
    }

    {
        const QString samplePath = QDir(paths.dataDir())
            .filePath(QStringLiteral("services/dictionaries/local_xdxf/selftest.xdxf"));
        QDir().mkpath(QFileInfo(samplePath).absolutePath());
        QFile sample(samplePath);
        if (sample.open(QIODevice::WriteOnly | QIODevice::Text)) {
            sample.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                         "<xdxf><article><k>hello</k><def>привет</def></article>"
                         "<article><k>world</k><def>мир</def></article></xdxf>\n");
            sample.close();
        }
        XdxfDictionary xdxf(samplePath);
        QString error;
        if (xdxf.open(&error)) {
            const auto entries = xdxf.search(QStringLiteral("hello"), true);
            appendLog(log, QStringLiteral("xdxf: %1 entries, size=%2")
                               .arg(entries.size()).arg(xdxf.size()));
        } else {
            appendLog(log, QStringLiteral("xdxf ERROR: %1").arg(error));
        }
    }

    QFile logFile(QDir(paths.dataDir()).filePath(QStringLiteral("selftest.log")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&logFile);
        stream << log;
    }
    return 0;
}

int runConnectorProbe(PluginManager& plugins, PathManager& paths)
{
    const QStringList arguments = QCoreApplication::arguments();
    QStringList ids;
    for (const PluginManifest& manifest : plugins.manifests()) {
        if (manifest.type != QStringLiteral("llm"))
            ids.append(manifest.id);
    }
    ids.sort();

    const int probeIndex = arguments.indexOf(QStringLiteral("--probe-connectors"));
    if (probeIndex >= 0 && probeIndex + 1 < arguments.size()) {
        const QString filter = arguments.at(probeIndex + 1);
        if (!filter.startsWith(QStringLiteral("--"))) {
            const QStringList tokens = filter.split(QLatin1Char(','), Qt::SkipEmptyParts);
            QStringList filtered;
            for (const QString& id : std::as_const(ids)) {
                for (const QString& token : tokens) {
                    if (id.contains(token)) {
                        filtered.append(id);
                        break;
                    }
                }
            }
            ids = filtered;
        }
    }

    const auto argumentValue = [&arguments](const QString& name, const QString& fallback) {
        const int index = arguments.indexOf(name);
        if (index >= 0 && index + 1 < arguments.size()
            && !arguments.at(index + 1).startsWith(QStringLiteral("--"))) {
            return arguments.at(index + 1);
        }
        return fallback;
    };
    const QString probeText = argumentValue(QStringLiteral("--probe-text"),
                                            QStringLiteral("hello"));
    const QString probeFrom = argumentValue(QStringLiteral("--probe-from"),
                                            QStringLiteral("en"));
    const QString probeTo = argumentValue(QStringLiteral("--probe-to"),
                                          QStringLiteral("ru"));

    QString log;
    appendLog(log, QStringLiteral("Q2Translate connector probe (%1 connectors)").arg(ids.size()));

    for (const QString& id : ids) {
        IPluginProvider* provider = plugins.provider(id);
        if (!provider) {
            appendLog(log, QStringLiteral("%1: no provider").arg(id));
            continue;
        }

        QStringList kinds;
        if (provider->capabilities().contains(QStringLiteral("translate")))
            kinds.append(QStringLiteral("translate"));
        if (provider->capabilities().contains(QStringLiteral("dictionary")))
            kinds.append(QStringLiteral("dictionary"));
        if (provider->capabilities().contains(QStringLiteral("spelling")))
            kinds.append(QStringLiteral("spelling"));

        for (const QString& kind : kinds) {
            QJsonObject params;
            params.insert(QStringLiteral("text"), probeText);
            params.insert(QStringLiteral("word"), probeText);
            params.insert(QStringLiteral("from"), probeFrom);
            params.insert(QStringLiteral("to"), probeTo);
            params.insert(QStringLiteral("language"), QStringLiteral("en"));
            params.insert(QStringLiteral("action"), QStringLiteral("check"));

            QEventLoop loop;
            QJsonObject result;
            QTimer::singleShot(12000, &loop, &QEventLoop::quit);
            provider->executeAsync(kind, params, [&](const QJsonObject& value) {
                result = value;
                loop.quit();
            });
            loop.exec();

            const QString error = result.value(QStringLiteral("error")).toString();
            if (error.isEmpty()) {
                const QString text = result.value(QStringLiteral("text")).toString();
                const QString html = result.value(QStringLiteral("html")).toString();
                const QString sample = !text.isEmpty() ? text : html;
                const QString spelling = result.contains(QStringLiteral("correct"))
                    ? QStringLiteral("correct=%1")
                          .arg(result.value(QStringLiteral("correct")).toBool() ? "true" : "false")
                    : sample.simplified().left(90);
                appendLog(log, QStringLiteral("%1 %2 OK: %3")
                                   .arg(id, kind, spelling));
            } else {
                appendLog(log, QStringLiteral("%1 %2 FAIL: %3")
                                   .arg(id, kind, error.simplified().left(160)));
            }
        }
    }

    QFile logFile(QDir(paths.dataDir()).filePath(QStringLiteral("connector_probe.log")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&logFile);
        stream << log;
    }
    return 0;
}

int runTtsProbe(PluginManager& plugins, PathManager& paths)
{
    const QStringList arguments = QCoreApplication::arguments();
    const int index = arguments.indexOf(QStringLiteral("--tts-test"));
    QString id = QStringLiteral("google_tts");
    if (index >= 0 && index + 1 < arguments.size()
        && !arguments.at(index + 1).startsWith(QStringLiteral("--"))) {
        id = arguments.at(index + 1);
    }
    QString text = QStringLiteral("Hello world, this is a speech test.");
    if (index >= 0 && index + 2 < arguments.size()
        && !arguments.at(index + 2).startsWith(QStringLiteral("--"))) {
        text = arguments.at(index + 2);
    }

    QString log;
    appendLog(log, QStringLiteral("Q2Translate TTS probe: %1").arg(id));
    IPluginProvider* provider = plugins.provider(id);
    if (!provider || !provider->capabilities().contains(QStringLiteral("tts"))) {
        appendLog(log, QStringLiteral("%1 FAIL: connector not available").arg(id));
        QFile logFile(QDir(paths.dataDir()).filePath(QStringLiteral("tts_probe.log")));
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Text))
            QTextStream(&logFile) << log;
        return 2;
    }

    QJsonObject params;
    params.insert(QStringLiteral("text"), text);
    params.insert(QStringLiteral("lang"), QStringLiteral("en"));
    QEventLoop loop;
    QJsonObject result;
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    provider->executeAsync(QStringLiteral("tts"), params, [&](const QJsonObject& value) {
        result = value;
        loop.quit();
    });
    loop.exec();

    const QString error = result.value(QStringLiteral("error")).toString();
    const bool ok = error.isEmpty() && result.value(QStringLiteral("ok")).toBool(false);
    appendLog(log, ok ? QStringLiteral("%1 OK").arg(id)
                      : QStringLiteral("%1 FAIL: %2").arg(id, error.simplified().left(200)));
    QFile logFile(QDir(paths.dataDir()).filePath(QStringLiteral("tts_probe.log")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Text))
        QTextStream(&logFile) << log;
    return ok ? 0 : 1;
}

int runHttpProbe(PluginManager& plugins, PathManager& paths)
{
    const QStringList arguments = QCoreApplication::arguments();
    const int index = arguments.indexOf(QStringLiteral("--http-test"));
    QString url = QStringLiteral("https://www.google.com/generate_204");
    if (index >= 0 && index + 1 < arguments.size()
        && !arguments.at(index + 1).startsWith(QStringLiteral("--"))) {
        url = arguments.at(index + 1);
    }

    QString log;
    appendLog(log, QStringLiteral("Q2Translate HTTP probe: %1").arg(url));
    appendLog(log, QStringLiteral("config: %1")
                       .arg(QString::fromUtf8(
                           QJsonDocument(plugins.networkConfig()).toJson(QJsonDocument::Compact))));
    HttpBridge bridge(plugins.networkConfig());
    QElapsedTimer timer;
    timer.start();
    const QVariantMap result = bridge.get(url);
    appendLog(log, QStringLiteral("status=%1 error=%2 elapsed=%3ms")
                       .arg(result.value(QStringLiteral("status")).toInt())
                       .arg(result.value(QStringLiteral("error")).toString())
                       .arg(timer.elapsed()));
    QFile logFile(QDir(paths.dataDir()).filePath(QStringLiteral("http_probe.log")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Text))
        QTextStream(&logFile) << log;
    return result.value(QStringLiteral("status")).toInt() > 0 ? 0 : 1;
}

// The built-in hotkey profile contains OCR (Win+Shift+Q) and Ctrl+Q for quick
// translate. Profiles that still hold the previous built-in set are migrated
// once; any profile with customised values is left as is.
// Connectors that need no account and were verified against their public
// endpoints.  They were disabled in older default profiles; enable them once
// without touching connectors that require a key, a local server or cookies.
//
// The default translation order below is a one-time ranking by practical
// quality and accuracy.  It is written to ServicesOrder once; afterwards the
// user's own order is preserved.
const char* const kDefaultTranslationOrder[] = {
    "google_translate",
    "google_new",
    "deepl",
    "bing_translate",
    "yandexinl",
    "reverso",
    "youdao",
    "baidu",
    "sogou",
    "mymemory",
    "lara_translate",
    "lara",
    "mozhi",
    "multi",
    "freetranslations",
    "translateeu",
    "translatoreu",
    "webtran",
    "promt",
    "libretranslate"
};

QStringList defaultTranslationOrder()
{
    QStringList order;
    for (const char* id : kDefaultTranslationOrder)
        order.append(QString::fromLatin1(id));
    return order;
}

void migrateConnectorDefaults(SettingsManager& settings)
{
    const int revision =
        settings.intValue(QStringLiteral("General.ConnectorDefaultsRevision"), 0);
    if (revision >= 6)
        return;

    QJsonArray disabled = settings.value(QStringLiteral("DisabledServices")).toArray();

    if (revision < 4) {
        const QStringList noAuthConnectors = {
            QStringLiteral("freetranslations"),
            QStringLiteral("google_new"),
            QStringLiteral("lara_translate"),
            QStringLiteral("multi"),
            QStringLiteral("sogou"),
            QStringLiteral("translateeu"),
            QStringLiteral("translatoreu"),
            QStringLiteral("webtran"),
            QStringLiteral("yandexinl")
        };

        QJsonArray updated;
        for (const QJsonValue& value : std::as_const(disabled)) {
            if (!noAuthConnectors.contains(value.toString()))
                updated.append(value);
        }
        bool laraDisabled = false;
        for (const QJsonValue& value : std::as_const(updated)) {
            if (value.toString() == QStringLiteral("lara")) {
                laraDisabled = true;
                break;
            }
        }
        if (!laraDisabled)
            updated.append(QStringLiteral("lara"));

        disabled = updated;
        settings.setValue(QStringLiteral("ServicesOrder"),
                          QJsonArray::fromStringList(defaultTranslationOrder()));
    }

    // These public web translators are switched off by default.  The user can
    // still enable any of them on the Connectors page.
    const QStringList disabledByDefault = {
        QStringLiteral("translateeu"),
        QStringLiteral("lara"),
        QStringLiteral("lara_translate"),
        QStringLiteral("multi"),
        QStringLiteral("libretranslate"),
        QStringLiteral("promt")
    };
    QJsonArray updated = disabled;
    for (const QString& id : disabledByDefault) {
        bool present = false;
        for (const QJsonValue& value : std::as_const(updated)) {
            if (value.toString() == id) {
                present = true;
                break;
            }
        }
        if (!present)
            updated.append(id);
    }

    settings.setValue(QStringLiteral("DisabledServices"), updated);
    settings.setValue(QStringLiteral("General.ConnectorDefaultsRevision"), 6);
    settings.save();
}

// The spell, dictionary, summarizer and rewriter switches used to be UI-only.
// Enable them once so existing profiles keep working, then leave the user's
// choice alone.
void migrateServiceSwitches(SettingsManager& settings)
{
    const int revision =
        settings.intValue(QStringLiteral("General.ServiceSwitchesRevision"), 0);
    if (revision >= 2)
        return;

    // Spell checking stays on.  Dictionary, summarizer and rewriter are opt-in
    // services and stay off until the user enables them.
    if (revision < 1)
        settings.setValue(QStringLiteral("ServiceProviders.spellOn"), true);
    settings.setValue(QStringLiteral("ServiceProviders.dictOn"), false);
    settings.setValue(QStringLiteral("ServiceProviders.summOn"), false);
    settings.setValue(QStringLiteral("ServiceProviders.rewrOn"), false);
    settings.setValue(QStringLiteral("General.ServiceSwitchesRevision"), 2);
    settings.save();
}

void migrateProxyType(SettingsManager& settings)
{
    const int revision = settings.intValue(QStringLiteral("General.ProxyTypeRevision"), 0);
    if (revision >= 1)
        return;
    QJsonObject proxy = settings.value(QStringLiteral("Proxy")).toObject();
    const int type = proxy.value(QStringLiteral("ProxyType")).toInt(0);
    const bool send = proxy.value(QStringLiteral("SendThroughProxy")).toBool(false);
    const QString host = proxy.value(QStringLiteral("Host")).toString().trimmed();
    int updated = type;
    if (type == 2)
        updated = 1;
    else if (type == 0 && send && !host.isEmpty())
        updated = 3;
    else if (type == 0)
        updated = 0;
    proxy.insert(QStringLiteral("ProxyType"), updated);
    settings.setValue(QStringLiteral("Proxy"), proxy);
    settings.setValue(QStringLiteral("General.ProxyTypeRevision"), 1);
    settings.save();
}

void migrateHotkeyProfile(SettingsManager& settings)
{
    struct DefaultHotkey {
        const char* key;
        int global;
        int internal;
    };
    // The built-in profile that older versions installed.  It is used only to
    // recognise profiles that were never customised, so they can be reduced to
    // the new minimal default set.
    const DefaultHotkey legacy[] = {
        {"HotKeyShowMainWindow", 0, 0},
        {"HotKeyQuickTranslate", 593, 0},
        {"HotKeyListenText", 581, 0},
        {"HotKeyTextRecognition", 585, 0},
        {"HotKeyReplaceSelection", 1620, 0},
        {"HotKeySwitchTargetLanguage", 0, 588},
        {"HotKeyDictionary", 580, 0},
        {"HotKeyTranslate", 0, 525},
        {"HotKeyFocusSource", 0, 305},
        {"HotKeyFocusTarget", 0, 306},
        {"HotKeyFocusExtra", 0, 307},
        {"HotKeyCopyTranslationText", 0, 1603},
        {"HotKeyClearInput", 0, 1624},
        {"HotKeySwapLanguages", 0, 1619},
        {"HotKeyOpenSettings", 0, 700},
        {"HotKeyShowHistory", 0, 1608},
        {"HotKeyTranslateDocument", 0, 1604}
    };

    const auto applyMinimal = [&settings, &legacy] {
        for (const DefaultHotkey& entry : legacy) {
            const QString key = QString::fromLatin1(entry.key);
            settings.setValue(QStringLiteral("HotKeys.") + key, 0);
            settings.setValue(QStringLiteral("HotKeys.Internal.") + key, 0);
        }
        settings.setValue(QStringLiteral("HotKeys.HotKeyQuickTranslate"), 0);
        settings.setValue(QStringLiteral("HotKeys.HotKeyReplaceSelection"), 1617);
        settings.setValue(QStringLiteral("HotKeys.HotKeyTextRecognition"), 3153);
        settings.setValue(QStringLiteral("HotKeys.Internal.HotKeyTextRecognition"), 3153);
        settings.setValue(QStringLiteral("HotKeys.Preset"), QStringLiteral("default"));
    };

    int revision = settings.value(QStringLiteral("HotKeys.ProfileRevision")).toInt(0);
    if (revision >= 5)
        return;

    if (revision < 2) {
        if (revision < 1) {
            // Fresh profiles start with only quick translate and OCR.
            applyMinimal();
            settings.setValue(QStringLiteral("HotKeys.ProfileRevision"), 1);
        }

        bool legacyMatch = true;
        for (const DefaultHotkey& entry : legacy) {
            const QString key = QString::fromLatin1(entry.key);
            if (settings.intValue(QStringLiteral("HotKeys.") + key, entry.global) != entry.global
                || settings.intValue(QStringLiteral("HotKeys.Internal.") + key, entry.internal)
                    != entry.internal) {
                legacyMatch = false;
                break;
            }
        }
        if (legacyMatch)
            applyMinimal();

        settings.setValue(QStringLiteral("HotKeys.ProfileRevision"), 2);
        revision = 2;
    }

    if (revision < 3) {
        // Shift+Q is the default replacement hotkey. Only seed it when the
        // user has not assigned another key; a custom binding is preserved.
        if (settings.intValue(QStringLiteral("HotKeys.HotKeyReplaceSelection"), 0) == 0)
            settings.setValue(QStringLiteral("HotKeys.HotKeyReplaceSelection"), 1105);
        settings.setValue(QStringLiteral("HotKeys.ProfileRevision"), 3);
        revision = 3;
    }

    if (revision < 4) {
        // Ctrl+Q is the default replacement hotkey and belongs to the global
        // scope only. The old quick-popup default moves out of the way.
        if (settings.intValue(QStringLiteral("HotKeys.HotKeyQuickTranslate"), 0) == 593)
            settings.setValue(QStringLiteral("HotKeys.HotKeyQuickTranslate"), 0);
        const int replace = settings.intValue(QStringLiteral("HotKeys.HotKeyReplaceSelection"), 0);
        if (replace == 0 || replace == 1105)
            settings.setValue(QStringLiteral("HotKeys.HotKeyReplaceSelection"), 593);
        settings.setValue(QStringLiteral("HotKeys.ProfileRevision"), 4);
        revision = 4;
    }

    if (revision < 5) {
        const int replace = settings.intValue(QStringLiteral("HotKeys.HotKeyReplaceSelection"), 0);
        if (replace == 0 || replace == 593 || replace == 1105)
            settings.setValue(QStringLiteral("HotKeys.HotKeyReplaceSelection"), 1617);
        settings.setValue(QStringLiteral("HotKeys.ProfileRevision"), 5);
        settings.save();
    }
}

}

int main(int argc, char* argv[])
{
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    WheelGuard wheelGuard;
    app.installEventFilter(&wheelGuard);
    Localization::install(&app);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setApplicationName(QStringLiteral("Q2Translate"));
    QApplication::setApplicationDisplayName(QStringLiteral("Q2Translate"));
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QApplication::setQuitOnLastWindowClosed(true);
    QApplication::setWindowIcon(IconFactory::appIcon());

    PathManager paths;
    if (!paths.init()) {
        QMessageBox::critical(nullptr, QStringLiteral("Q2Translate"),
                              QStringLiteral("Не удалось создать папку данных:\n%1")
                                  .arg(paths.dataDir()));
        return 1;
    }

    SettingsManager settings(&paths);
    QString settingsError;
    if (!settings.load(&settingsError)) {
        QMessageBox::critical(nullptr, QStringLiteral("Q2Translate"),
                              QStringLiteral("Не удалось загрузить настройки:\n%1")
                                  .arg(settingsError));
        return 1;
    }
    migrateHotkeyProfile(settings);
    migrateConnectorDefaults(settings);
    migrateServiceSwitches(settings);
    migrateProxyType(settings);

    ThemeScheduleController themeSchedule(&settings);
    themeSchedule.start();
    QFont applicationFont = app.font();
    applicationFont.setFamily(settings.stringValue(QStringLiteral("Appearance.UiFont"),
                                                   applicationFont.family()));
    applicationFont.setPointSize(settings.intValue(QStringLiteral("Appearance.UiFontSize"), 13));
    FontTuning::apply(applicationFont,
                      settings.boolValue(QStringLiteral("Appearance.FontAntialiasing"), true),
                      settings.stringValue(QStringLiteral("Appearance.FontHinting"),
                                           QStringLiteral("full")));
    app.setFont(applicationFont);
    const int editorFontSize = settings.intValue(QStringLiteral("Appearance.EditorFontSize"), 14);
    app.setStyleSheet(app.styleSheet()
        + QStringLiteral("QWidget{font-family:'%1';font-size:%2px;}"
                         "QTextEdit#SourceEdit{font-family:'%3';font-size:%4px;}"
                         "QTextEdit#TargetEdit{font-family:'%3';font-size:%5px;}")
              .arg(settings.stringValue(QStringLiteral("Appearance.UiFont"),
                                         QStringLiteral("Segoe UI")))
              .arg(settings.intValue(QStringLiteral("Appearance.UiFontSize"), 13))
              .arg(settings.stringValue(QStringLiteral("Appearance.EditorFont"), QStringLiteral("Segoe UI")))
              .arg(editorFontSize)
              .arg(editorFontSize + 1));

    ExceptionsManager exceptions(&paths);
    exceptions.load();

    HistoryManager history(&paths, &settings);
    history.load();

    PluginManager plugins(&paths, &settings);
    plugins.scan();

    LanguageRouter router(&settings);
    TranslationManager translation(&paths, &settings, &plugins, &router, &history);
    DictionaryManager dictionary(&paths, &settings, &plugins);

    if (QCoreApplication::arguments().contains(QStringLiteral("--selftest")))
        return runSelfTest(settings, plugins, translation, dictionary, paths);

    if (QCoreApplication::arguments().contains(QStringLiteral("--probe-connectors")))
        return runConnectorProbe(plugins, paths);

    if (QCoreApplication::arguments().contains(QStringLiteral("--tts-test")))
        return runTtsProbe(plugins, paths);

    if (QCoreApplication::arguments().contains(QStringLiteral("--http-test")))
        return runHttpProbe(plugins, paths);

    ClipboardManager clipboard;
    GlobalHotkeyManager hotkeys(&settings);

    AppContext context;
    context.paths = &paths;
    context.settings = &settings;
    context.plugins = &plugins;
    context.router = &router;
    context.history = &history;
    context.translation = &translation;
    context.dictionary = &dictionary;
    context.exceptions = &exceptions;
    context.clipboard = &clipboard;
    context.hotkeys = &hotkeys;

    Localization::load(settings.stringValue(QStringLiteral("General.LocaleFoderName"),
                                          QStringLiteral("Russian")));
    MainWindow window(&context);
    TrayIcon tray(&settings, &plugins);
    tray.setHotkeysEnabled(settings.boolValue(QStringLiteral("HotKeys.EnableHotKeys"), true));

    QObject::connect(&tray, &TrayIcon::showMainWindowRequested, &window, &MainWindow::showMainWindow);
    QObject::connect(&tray, &TrayIcon::translateClipboardRequested, &window,
                     &MainWindow::translateSelectionInMain);
    QObject::connect(&tray, &TrayIcon::settingsRequested, &window, &MainWindow::showOptions);
    QObject::connect(&tray, &TrayIcon::historyRequested, &window, &MainWindow::showHistory);
    QObject::connect(&tray, &TrayIcon::dictionaryRequested, &window, &MainWindow::showDictionary);
    QObject::connect(&tray, &TrayIcon::ocrRequested, &window, &MainWindow::startOcr);
    QObject::connect(&tray, &TrayIcon::aboutRequested, &window, [&window] { showAbout(&window); });
    QObject::connect(&tray, &TrayIcon::quitRequested, &window, &MainWindow::requestQuit);
    QObject::connect(&tray, &TrayIcon::hotkeysToggled, &window, [&](bool enabled) {
        settings.setValue(QStringLiteral("HotKeys.EnableHotKeys"), enabled);
        settings.save();
        hotkeys.setEnabled(enabled);
    });

    auto hotkeyLog = [&](const QString& text) {
        QFile file(QDir(paths.dataDir()).filePath(QStringLiteral("hotkeys.log")));
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            file.write((QDateTime::currentDateTime().toString(Qt::ISODate) + " " + text + "\n").toUtf8());
    };
    hotkeyLog(QStringLiteral("start enabled=%1 hook=%2 mousehook=%3")
                  .arg(hotkeys.isEnabled())
                  .arg(hotkeys.isHookInstalled())
                  .arg(hotkeys.isMouseHookInstalled()));
    for (const QString& error : hotkeys.registrationErrors())
        hotkeyLog(QStringLiteral("registration failed: %1").arg(error));

    QObject::connect(&hotkeys, &GlobalHotkeyManager::doubleCtrlDetected, &window, [&] {
        hotkeyLog(QStringLiteral("doubleCtrl detected"));
        if (!settings.boolValue(QStringLiteral("HotKeys.DoubleCtrlShowMain"), true))
            return;
        // Never inject Ctrl+C into our own modal dialogs (notably the hotkey
        // editor, which would otherwise record "Ctrl+C" on a double Ctrl).
        if (QApplication::activeModalWidget()) {
            hotkeyLog(QStringLiteral("doubleCtrl modal"));
            return;
        }
        if (isExcepted(context)) {
            hotkeyLog(QStringLiteral("doubleCtrl excepted"));
            return;
        }
        window.translateSelectionInMain();
        hotkeyLog(QStringLiteral("doubleCtrl dispatched"));
    });

    QObject::connect(&hotkeys, &GlobalHotkeyManager::selectionGestureDetected, &window,
                     [&] {
                         hotkeyLog(QStringLiteral("mouse selection detected"));
                         if (!settings.boolValue(
                                 QStringLiteral("Advanced.EnableMouseModeOnCtrl"), false))
                             return;
                         if (QApplication::activeModalWidget())
                             return;
                         if (isExcepted(context))
                             return;
                         window.showPopup();
                     });

    QObject::connect(&hotkeys, &GlobalHotkeyManager::hotkeyActivated, &window,
                     [&](const QString& action) {
        hotkeyLog(QStringLiteral("hotkey %1").arg(action));
        if (action != QStringLiteral("show_main") && isExcepted(context))
            return;
        if (action == QStringLiteral("show_main"))
            window.showMainWindow();
        else if (action == QStringLiteral("quick_translate"))
            window.showPopup();
        else if (action == QStringLiteral("listen_text"))
            window.speakSelection();
        else if (action == QStringLiteral("ocr"))
            window.startOcr();
        else if (action == QStringLiteral("replace_selection"))
            window.replaceSelectionWithTranslation();
        else if (action == QStringLiteral("switch_target_language"))
            window.switchTargetLanguage();
        else if (action == QStringLiteral("dictionary"))
            window.showDictionary();
        else if (action == QStringLiteral("translate"))
            window.translateCurrent();
        else if (action == QStringLiteral("focus_source"))
            window.focusSource();
        else if (action == QStringLiteral("focus_target"))
            window.focusTarget();
        else if (action == QStringLiteral("focus_extra"))
            window.focusExtra();
        else if (action == QStringLiteral("copy_translation"))
            window.copyTranslation();
        else if (action == QStringLiteral("clear_input"))
            window.clearInput();
        else if (action == QStringLiteral("swap_languages"))
            window.swapLanguages();
        else if (action == QStringLiteral("open_settings"))
            window.showOptions();
        else if (action == QStringLiteral("show_history"))
            window.showHistory();
        else if (action == QStringLiteral("translate_document"))
            window.translateDocument();
    });

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &window, [&] {
        window.saveState();
        if (settings.boolValue(QStringLiteral("General.ClearHistoryOnExit"), false))
            history.clear();
        settings.save();
        history.save();
        exceptions.save();
    });

    // Headless runs must not be swallowed by an already running window: the probe
    // would forward "show" and exit before the requested test executes.
    const QStringList startupArguments = QCoreApplication::arguments();
    const bool standaloneRun = startupArguments.contains(QStringLiteral("--selftest"))
        || startupArguments.contains(QStringLiteral("--translate-image"))
        || startupArguments.contains(QStringLiteral("--translate-document"))
        || startupArguments.contains(QStringLiteral("--show-batch"))
        || startupArguments.contains(QStringLiteral("--probe-connectors"))
        || startupArguments.contains(QStringLiteral("--probe-text"))
        || startupArguments.contains(QStringLiteral("--http-test"))
        || startupArguments.contains(QStringLiteral("--tts-test"));

    const QString serverName = QStringLiteral("Q2Translate.SingleInstance");
    QLocalSocket probe;
    if (!standaloneRun) {
        probe.connectToServer(serverName);
        if (probe.waitForConnected(300)) {
            probe.write("show");
            probe.waitForBytesWritten(300);
            return 0;
        }
    }
    QLocalServer::removeServer(serverName);
    QLocalServer singleInstance;
    singleInstance.listen(serverName);
    QObject::connect(&singleInstance, &QLocalServer::newConnection, &window, [&] {
        while (QLocalSocket* socket = singleInstance.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, &window, [&window, socket] {
                const QByteArray command = socket->readAll().trimmed();
                if (command == "translate")
                    window.translateSelectionInMain();
                else
                    window.showMainWindow();
            });
        }
    });

    tray.show();

    int startupAction = settings.intValue(QStringLiteral("General.MainWindowStartupAction"), 2);
    if (startupAction < 0)
        startupAction = 2;
    if (!tray.isAvailable())
        startupAction = 1;
    if (startupAction == 1) {
        if (settings.boolValue(QStringLiteral("General.WindowMainMaximized"), false))
            window.showMaximized();
        else
            window.show();
    }

    const QStringList arguments = QCoreApplication::arguments();
    const int translateIndex = arguments.indexOf(QStringLiteral("--translate"));
    if (translateIndex >= 0 && translateIndex + 1 < arguments.size()) {
        const QString text = arguments.at(translateIndex + 1);
        QTimer::singleShot(600, &window, [&window, text] {
            window.showMainWindow();
            window.translateText(text);
        });
    }
    const int setSourceIndex = arguments.indexOf(QStringLiteral("--set-source"));
    if (setSourceIndex >= 0 && setSourceIndex + 1 < arguments.size()) {
        const QString text = arguments.at(setSourceIndex + 1);
        QTimer::singleShot(600, &window, [&window, text] {
            window.showMainWindow();
            window.setSourceText(text, true);
        });
    }
    const int documentIndex = arguments.indexOf(QStringLiteral("--translate-document"));
    if (documentIndex >= 0 && documentIndex + 1 < arguments.size()) {
        const QString path = arguments.at(documentIndex + 1);
        QTimer::singleShot(700, &window, [&window, path] {
            window.showMainWindow();
            window.translateDocumentFile(path);
        });
    }
    // Headless check of the image pipeline: OCR, block translation, overlay, saved PNG.
    // The result is written to Data/image-test.log because the app is a GUI binary.
    const int imageIndex = arguments.indexOf(QStringLiteral("--translate-image"));
    if (imageIndex >= 0 && imageIndex + 1 < arguments.size()) {
        const QString path = arguments.at(imageIndex + 1);
        QString target = QStringLiteral("ru");
        if (imageIndex + 2 < arguments.size()
            && !arguments.at(imageIndex + 2).startsWith(QStringLiteral("--"))) {
            target = arguments.at(imageIndex + 2);
        }
        QString source = QStringLiteral("auto");
        if (imageIndex + 3 < arguments.size()
            && !arguments.at(imageIndex + 3).startsWith(QStringLiteral("--"))) {
            source = arguments.at(imageIndex + 3);
        }
        QString dumpPath;
        const int dumpIndex = arguments.indexOf(QStringLiteral("--image-layout-dump"));
        if (dumpIndex >= 0 && dumpIndex + 1 < arguments.size()
            && !arguments.at(dumpIndex + 1).startsWith(QStringLiteral("--"))) {
            dumpPath = arguments.at(dumpIndex + 1);
        }
        QString imageProvider;
        const int imageProviderIndex = arguments.indexOf(QStringLiteral("--image-provider"));
        if (imageProviderIndex >= 0 && imageProviderIndex + 1 < arguments.size()
            && !arguments.at(imageProviderIndex + 1).startsWith(QStringLiteral("--"))) {
            imageProvider = arguments.at(imageProviderIndex + 1);
        }
        QTimer::singleShot(500, &window,
                           [&context, &paths, path, target, source, dumpPath, imageProvider] {
            const QFileInfo info(path);
            const QString logPath =
                QDir(paths.dataDir()).filePath(QStringLiteral("image-test.log"));
            QFile::remove(logPath);
            const auto writeLog = [logPath](const QString& text) {
                QFile log(logPath);
                if (log.open(QIODevice::Append | QIODevice::WriteOnly)) {
                    log.write(text.toUtf8());
                    log.write("\n");
                }
            };

            QImage image = loadImageFile(path);
            if (image.isNull()) {
                writeLog(QStringLiteral("ERROR: cannot read %1").arg(path));
                QCoreApplication::exit(2);
                return;
            }

            auto* pipeline = new ImageTranslationPipeline(&context);
            if (!dumpPath.isEmpty())
                pipeline->setLayoutDumpPath(dumpPath);
            QObject::connect(pipeline, &ImageTranslationPipeline::finished,
                             pipeline, [pipeline, info, target, writeLog](const QImage& translated,
                                                                         const QString& source,
                                                                         const QString& text) {
                const QString output = info.dir().filePath(
                    QStringLiteral("%1.%2.png").arg(info.completeBaseName(), target));
                writeLog(QStringLiteral("BEFORE SAVE: null=%1 size=%2x%3 path=%4")
                             .arg(translated.isNull()).arg(translated.width())
                             .arg(translated.height()).arg(output));
                const bool saved = translated.save(output, "PNG");
                writeLog(QStringLiteral("OUTPUT: %1 saved=%2").arg(output).arg(saved));
                writeLog(QStringLiteral("SOURCE: %1").arg(source.simplified().left(400)));
                writeLog(QStringLiteral("TRANSLATED: %1").arg(text.simplified().left(400)));
                pipeline->deleteLater();
                QCoreApplication::exit(saved ? 0 : 3);
            });
            QObject::connect(pipeline, &ImageTranslationPipeline::failed, pipeline,
                             [pipeline, writeLog](const QString& message) {
                writeLog(QStringLiteral("ERROR: %1").arg(message));
                pipeline->deleteLater();
                QCoreApplication::exit(4);
            });
            pipeline->start(image, source, target, imageProvider);
        });
    }
    if (arguments.contains(QStringLiteral("--selection-test")))
        QTimer::singleShot(1400, &window, &MainWindow::translateSelectionInMain);
    const int batchIndex = arguments.indexOf(QStringLiteral("--show-batch"));
    if (batchIndex >= 0) {
        QStringList files;
        for (int index = batchIndex + 1; index < arguments.size(); ++index) {
            if (arguments.at(index).startsWith(QStringLiteral("--")))
                break;
            files.append(arguments.at(index));
        }
        QTimer::singleShot(700, &window, [&window, files] {
            window.showMainWindow();
            window.batchTranslate(files);
        });
    }
    const int pluginIndex = arguments.indexOf(QStringLiteral("--plugin-settings"));
    if (pluginIndex >= 0 && pluginIndex + 1 < arguments.size()) {
        const QString id = arguments.at(pluginIndex + 1);
        QTimer::singleShot(800, &window, [&plugins, &window, id] {
            if (IPluginProvider* provider = plugins.provider(id)) {
                PluginSettingsDialog dialog(provider, &window);
                dialog.exec();
            }
        });
    }
    if (arguments.contains(QStringLiteral("--ocr-languages"))) {
        QTimer::singleShot(600, &window, [&] {
            QString tessdata = QDir(QDir(paths.shippedDir())
                                        .filePath(QStringLiteral("services/ocr/tesseract")))
                                   .filePath(QStringLiteral("tessdata"));
            TesseractLanguageDialog dialog(tessdata, &window);
            dialog.exec();
            QCoreApplication::quit();
        });
    }
    if (arguments.contains(QStringLiteral("--show-options")))
        QTimer::singleShot(800, &window, &MainWindow::showOptions);
    if (arguments.contains(QStringLiteral("--about")))
        QTimer::singleShot(600, &window, &MainWindow::showAbout);
    if (arguments.contains(QStringLiteral("--show-dictionary")))
        QTimer::singleShot(800, &window, &MainWindow::showDictionary);
    if (arguments.contains(QStringLiteral("--show-history")))
        QTimer::singleShot(800, &window, &MainWindow::showHistory);
    if (arguments.contains(QStringLiteral("--start-ocr")))
        QTimer::singleShot(800, &window, &MainWindow::startOcr);
    if (arguments.contains(QStringLiteral("--show-popup")))
        QTimer::singleShot(800, &window, &MainWindow::showPopup);
    const int popupTextIndex = arguments.indexOf(QStringLiteral("--popup-text"));
    if (popupTextIndex >= 0 && popupTextIndex + 1 < arguments.size()) {
        const QString text = arguments.at(popupTextIndex + 1);
        QTimer::singleShot(800, &window, [&window, text] {
            window.showPopupText(text);
        });
    }
    return app.exec();
}
