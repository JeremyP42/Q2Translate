#pragma once

class PathManager;
class SettingsManager;
class PluginManager;
class LanguageRouter;
class HistoryManager;
class TranslationManager;
class DictionaryManager;
class ExceptionsManager;
class ClipboardManager;
class GlobalHotkeyManager;

struct AppContext {
    PathManager* paths = nullptr;
    SettingsManager* settings = nullptr;
    PluginManager* plugins = nullptr;
    LanguageRouter* router = nullptr;
    HistoryManager* history = nullptr;
    TranslationManager* translation = nullptr;
    DictionaryManager* dictionary = nullptr;
    ExceptionsManager* exceptions = nullptr;
    ClipboardManager* clipboard = nullptr;
    GlobalHotkeyManager* hotkeys = nullptr;
};
