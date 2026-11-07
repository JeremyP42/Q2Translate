#pragma once

#include "spell_provider_base.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

// Small, dependency-free Hunspell dictionary reader.
//
// It implements the parts of the .aff/.dic format that are needed for
// spell checking: flag modes, aliases, IGNORE, forbidden/need-affix flags and
// prefix/suffix rules with cross products.  Suggestions and compound-word
// expansion are not generated; languages that rely heavily on compounding may
// report compound words as unknown.
class HunspellEngine : public ISpellEngine
{
public:
    HunspellEngine() = default;
    ~HunspellEngine() override = default;

    bool load(const QString& affPath, const QString& dicPath, QString* error = nullptr);
    bool isLoaded() const override { return m_loaded; }
    bool spell(const QString& word) const override;
    QStringList suggest(const QString& word, int limit = 7) const override;

private:
    enum class FlagMode { Single, Long, Num };

    struct AffixRule
    {
        QString strip;
        QString add;
        QRegularExpression condition;
        bool conditionAlways = true;
    };

    struct AffixGroup
    {
        QString flag;
        bool cross = false;
        QVector<AffixRule> rules;
    };

    struct RuleRef
    {
        int group = -1;
        int rule = -1;
    };

    bool hasFlag(const QString& flags, const QString& flag) const;
    bool checkVariant(const QString& word) const;
    bool checkRoot(const QString& root, const QString& requiredFlag, bool affixed) const;
    bool trySuffix(const QString& word, bool allowPrefix) const;
    bool tryPrefix(const QString& word, bool allowSuffix) const;
    bool tryPrefixOnRoot(const QString& root, const QString& suffixFlag) const;
    bool trySuffixOnRoot(const QString& root, const QString& prefixFlag) const;
    QString cleanWord(const QString& word) const;
    QString expandAlias(const QString& flags) const;
    bool conditionMatches(const AffixRule& rule, const QString& root, bool prefix) const;

    QHash<QString, QString> m_words;
    QVector<AffixGroup> m_suffixGroups;
    QVector<AffixGroup> m_prefixGroups;
    QHash<QString, int> m_suffixFlags;
    QHash<QString, int> m_prefixFlags;
    QHash<QChar, QVector<RuleRef>> m_suffixIndex;
    QHash<QChar, QVector<RuleRef>> m_prefixIndex;

    QVector<QString> m_aliases;
    QString m_forbiddenFlag;
    QString m_needAffixFlag;
    QString m_onlyInCompoundFlag;
    QString m_noSuggestFlag;
    QString m_ignore;
    QString m_tryChars;
    FlagMode m_flagMode = FlagMode::Single;
    bool m_loaded = false;
};
