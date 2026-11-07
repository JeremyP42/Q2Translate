#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QSyntaxHighlighter>

#include <memory>

class PluginManager;
class SettingsManager;
class SpellService;

class SpellCheckHighlighter : public QSyntaxHighlighter
{
public:
    explicit SpellCheckHighlighter(QTextDocument* document,
                                   PluginManager* plugins = nullptr,
                                   SettingsManager* settings = nullptr);
    ~SpellCheckHighlighter() override;

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }
    void setLanguage(const QString& code);
    void clearCache();
    bool isWordMisspelled(const QString& word) const;
    QStringList suggestionsFor(const QString& word, int limit = 7) const;

protected:
    void highlightBlock(const QString& text) override;

private:
    bool isMisspelled(const QString& word);

    std::unique_ptr<SpellService> m_service;
    QHash<QString, bool> m_cache;
    QString m_language;
    bool m_enabled = false;
};
