#include "spell_check_highlighter.h"

#include "services/spell_service.h"
#include "theme_manager.h"

#include <QColor>
#include <QRegularExpression>
#include <QTextCharFormat>
#include <QTextDocument>

SpellCheckHighlighter::SpellCheckHighlighter(QTextDocument* document,
                                             PluginManager* plugins,
                                             SettingsManager* settings)
    : QSyntaxHighlighter(document)
    , m_service(std::make_unique<SpellService>(plugins, settings))
{
}

SpellCheckHighlighter::~SpellCheckHighlighter() = default;

void SpellCheckHighlighter::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    m_cache.clear();
    rehighlight();
}

void SpellCheckHighlighter::setLanguage(const QString& code)
{
    if (m_language == code)
        return;
    m_language = code;
    m_cache.clear();
    rehighlight();
}

void SpellCheckHighlighter::clearCache()
{
    m_cache.clear();
    rehighlight();
}

bool SpellCheckHighlighter::isWordMisspelled(const QString& word) const
{
    if (!m_enabled || word.isEmpty())
        return false;
    return !m_service->checkWord(word, m_language);
}

QStringList SpellCheckHighlighter::suggestionsFor(const QString& word, int limit) const
{
    if (!m_enabled || word.isEmpty())
        return {};
    return m_service->suggestions(word, m_language, limit);
}

bool SpellCheckHighlighter::isMisspelled(const QString& word)
{
    if (word.size() < 2)
        return false;
    const QString key = word.toLower();
    const auto cached = m_cache.constFind(key);
    if (cached != m_cache.constEnd())
        return cached.value();
    const bool misspelled = !m_service->checkWord(word, m_language);
    m_cache.insert(key, misspelled);
    return misspelled;
}

void SpellCheckHighlighter::highlightBlock(const QString& text)
{
    if (!m_enabled || text.isEmpty())
        return;

    QTextCharFormat misspelled;
    misspelled.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    misspelled.setUnderlineColor(ThemeManager::color(QStringLiteral("danger")));

    static const QRegularExpression wordPattern(
        QStringLiteral("[\\p{L}][\\p{L}'\u2019-]*"));
    QRegularExpressionMatchIterator iterator = wordPattern.globalMatch(text);
    while (iterator.hasNext()) {
        const QRegularExpressionMatch match = iterator.next();
        if (isMisspelled(match.captured(0)))
            setFormat(match.capturedStart(), match.capturedLength(), misspelled);
    }
}
