#include "hunspell_engine.h"

#include <QFile>
#include <QPair>
#include <QStringView>

#include <algorithm>

namespace {

QString decodeText(const QByteArray& data, bool utf8)
{
    QString text = utf8 ? QString::fromUtf8(data) : QString::fromLatin1(data);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    return text;
}

QStringList splitLines(const QString& text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString& line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
    }
    return lines;
}

QString normalizedFlag(const QString& value)
{
    return value == QStringLiteral("0") ? QString() : value;
}

} // namespace

bool HunspellEngine::load(const QString& affPath, const QString& dicPath, QString* error)
{
    m_words.clear();
    m_suffixGroups.clear();
    m_prefixGroups.clear();
    m_suffixFlags.clear();
    m_prefixFlags.clear();
    m_suffixIndex.clear();
    m_prefixIndex.clear();
    m_aliases.clear();
    m_forbiddenFlag.clear();
    m_needAffixFlag.clear();
    m_onlyInCompoundFlag.clear();
    m_noSuggestFlag.clear();
    m_ignore.clear();
    m_tryChars.clear();
    m_flagMode = FlagMode::Single;
    m_loaded = false;

    QFile affFile(affPath);
    if (!affFile.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("не удалось открыть %1").arg(affPath);
        return false;
    }
    const QByteArray affData = affFile.readAll();
    const bool utf8 = affData.contains("SET UTF-8") || affData.contains("SET utf-8");
    const QStringList affLines = splitLines(decodeText(affData, utf8));

    bool aliasesStarted = false;
    for (const QString& rawLine : affLines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QStringLiteral("FLAG "))) {
            const QString mode = line.mid(5).trimmed();
            if (mode == QStringLiteral("long"))
                m_flagMode = FlagMode::Long;
            else if (mode == QStringLiteral("num"))
                m_flagMode = FlagMode::Num;
            else
                m_flagMode = FlagMode::Single;
        } else if (line.startsWith(QStringLiteral("SET "))) {
            // Encoding is handled before parsing.
        } else if (line.startsWith(QStringLiteral("IGNORE "))) {
            m_ignore = line.mid(7).trimmed();
        } else if (line.startsWith(QStringLiteral("TRY "))) {
            m_tryChars = line.mid(4).trimmed();
        } else if (line.startsWith(QStringLiteral("AF "))) {
            if (!aliasesStarted) {
                aliasesStarted = true;
                continue; // first AF line is the alias count
            }
            m_aliases.append(line.mid(3).trimmed());
        }
    }

    const auto groupIndex = [this](QVector<AffixGroup>& groups,
                                   QHash<QString, int>& index,
                                   const QString& flag) {
        const auto found = index.constFind(flag);
        if (found != index.constEnd())
            return found.value();
        AffixGroup group;
        group.flag = flag;
        groups.append(group);
        const int value = groups.size() - 1;
        index.insert(flag, value);
        return value;
    };

    for (const QString& rawLine : affLines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        const QStringList parts =
            line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() < 2)
            continue;

        const QString directive = parts.at(0);
        if (directive == QStringLiteral("FORBIDDENWORD")) {
            m_forbiddenFlag = parts.at(1);
        } else if (directive == QStringLiteral("NEEDAFFIX")
                   || directive == QStringLiteral("PSEUDOROOT")) {
            m_needAffixFlag = parts.at(1);
        } else if (directive == QStringLiteral("ONLYINCOMPOUND")) {
            m_onlyInCompoundFlag = parts.at(1);
        } else if (directive == QStringLiteral("NOSUGGEST")) {
            m_noSuggestFlag = parts.at(1);
        } else if (directive == QStringLiteral("SFX") || directive == QStringLiteral("PFX")) {
            if (parts.size() >= 4
                && (parts.at(2) == QStringLiteral("Y") || parts.at(2) == QStringLiteral("N"))) {
                const QString flag = parts.at(1);
                QVector<AffixGroup>& groups = directive == QStringLiteral("SFX")
                    ? m_suffixGroups : m_prefixGroups;
                QHash<QString, int>& index = directive == QStringLiteral("SFX")
                    ? m_suffixFlags : m_prefixFlags;
                const int group = groupIndex(groups, index, flag);
                groups[group].cross = parts.at(2) == QStringLiteral("Y");
                continue;
            }

            if (parts.size() < 4)
                continue;
            const QString flag = parts.at(1);
            QVector<AffixGroup>& groups = directive == QStringLiteral("SFX")
                ? m_suffixGroups : m_prefixGroups;
            QHash<QString, int>& index = directive == QStringLiteral("SFX")
                ? m_suffixFlags : m_prefixFlags;
            const int group = groupIndex(groups, index, flag);

            AffixRule rule;
            rule.strip = normalizedFlag(parts.at(2));
            rule.add = normalizedFlag(parts.at(3));
            const QString condition = parts.size() > 4 ? parts.at(4) : QStringLiteral(".");
            if (condition.isEmpty() || condition == QStringLiteral(".")) {
                rule.conditionAlways = true;
            } else {
                const QString pattern = directive == QStringLiteral("SFX")
                    ? QStringLiteral("(?:%1)$").arg(condition)
                    : QStringLiteral("^(?:%1)").arg(condition);
                rule.condition = QRegularExpression(
                    pattern, QRegularExpression::CaseInsensitiveOption);
                rule.conditionAlways = !rule.condition.isValid();
            }
            groups[group].rules.append(rule);
        }
    }

    for (int group = 0; group < m_suffixGroups.size(); ++group) {
        const AffixGroup& value = m_suffixGroups.at(group);
        for (int rule = 0; rule < value.rules.size(); ++rule) {
            const QString& add = value.rules.at(rule).add;
            if (add.isEmpty())
                continue;
            m_suffixIndex[add.at(add.size() - 1)].append({group, rule});
        }
    }
    for (int group = 0; group < m_prefixGroups.size(); ++group) {
        const AffixGroup& value = m_prefixGroups.at(group);
        for (int rule = 0; rule < value.rules.size(); ++rule) {
            const QString& add = value.rules.at(rule).add;
            if (add.isEmpty())
                continue;
            m_prefixIndex[add.at(0)].append({group, rule});
        }
    }

    QFile dicFile(dicPath);
    if (!dicFile.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("не удалось открыть %1").arg(dicPath);
        return false;
    }
    const QStringList dicLines = splitLines(decodeText(dicFile.readAll(), utf8));
    bool firstLine = true;
    QSet<QChar> alphabet;
    for (const QString& rawLine : dicLines) {
        QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (firstLine) {
            firstLine = false;
            bool isCount = false;
            line.toInt(&isCount);
            if (isCount && !line.contains(QLatin1Char('/')))
                continue;
        }

        const int slash = line.indexOf(QLatin1Char('/'));
        QString word = slash >= 0 ? line.left(slash) : line;
        QString flags = slash >= 0 ? line.mid(slash + 1).trimmed() : QString();
        for (int index = 0; index < flags.size(); ++index) {
            if (flags.at(index).isSpace()) {
                flags = flags.left(index);
                break;
            }
        }
        word = cleanWord(word);
        if (word.isEmpty() || word.contains(QLatin1Char(' ')))
            continue;
        for (const QChar character : word) {
            if (character.isLetter())
                alphabet.insert(character.toLower());
        }
        flags = expandAlias(flags);

        const auto existing = m_words.constFind(word);
        if (existing == m_words.constEnd()) {
            m_words.insert(word, flags);
        } else if (!flags.isEmpty()) {
            QString merged = existing.value();
            if (!merged.contains(flags)) {
                if (m_flagMode == FlagMode::Num)
                    merged += QLatin1Char(',') + flags;
                else
                    merged += flags;
                m_words.insert(word, merged);
            }
        }
    }

    m_loaded = !m_words.isEmpty();
    if (!m_loaded && error)
        *error = QStringLiteral("словарь пуст");

    if (m_tryChars.isEmpty()) {
        QList<QChar> characters = alphabet.values();
        std::sort(characters.begin(), characters.end(),
                  [](QChar left, QChar right) { return left.unicode() < right.unicode(); });
        for (const QChar character : characters)
            m_tryChars.append(character);
    }
    return m_loaded;
}

QString HunspellEngine::expandAlias(const QString& flags) const
{
    if (m_aliases.isEmpty() || flags.isEmpty())
        return flags;
    bool ok = false;
    const int index = flags.toInt(&ok);
    if (ok && index >= 1 && index <= m_aliases.size())
        return m_aliases.at(index - 1);
    return flags;
}

QString HunspellEngine::cleanWord(const QString& word) const
{
    QString result = word.trimmed();
    if (!m_ignore.isEmpty()) {
        for (const QChar character : m_ignore)
            result.remove(character);
    }
    return result;
}

bool HunspellEngine::hasFlag(const QString& flags, const QString& flag) const
{
    if (flags.isEmpty() || flag.isEmpty())
        return false;
    switch (m_flagMode) {
    case FlagMode::Single:
        return flags.contains(flag.at(0));
    case FlagMode::Long:
        if (flag.size() < 2)
            return false;
        for (int index = 0; index + 1 < flags.size(); index += 2) {
            if (flags.mid(index, 2) == flag)
                return true;
        }
        return false;
    case FlagMode::Num:
        return flags.split(QLatin1Char(','), Qt::SkipEmptyParts).contains(flag);
    }
    return false;
}

bool HunspellEngine::conditionMatches(const AffixRule& rule, const QString& root,
                                      bool prefix) const
{
    Q_UNUSED(prefix);
    if (rule.conditionAlways)
        return true;
    return rule.condition.match(root).hasMatch();
}

bool HunspellEngine::checkRoot(const QString& root, const QString& requiredFlag,
                               bool affixed) const
{
    const auto found = m_words.constFind(root);
    if (found == m_words.constEnd())
        return false;
    const QString flags = found.value();
    if (hasFlag(flags, m_forbiddenFlag) || hasFlag(flags, m_onlyInCompoundFlag))
        return false;
    if (!affixed && hasFlag(flags, m_needAffixFlag))
        return false;
    if (!requiredFlag.isEmpty() && !hasFlag(flags, requiredFlag))
        return false;
    return true;
}

bool HunspellEngine::trySuffix(const QString& word, bool allowPrefix) const
{
    if (word.size() < 2)
        return false;
    const auto refs = m_suffixIndex.constFind(word.at(word.size() - 1));
    if (refs == m_suffixIndex.constEnd())
        return false;
    for (const RuleRef& ref : refs.value()) {
        const AffixGroup& group = m_suffixGroups.at(ref.group);
        const AffixRule& rule = group.rules.at(ref.rule);
        if (rule.add.isEmpty() || !word.endsWith(rule.add))
            continue;
        const QString root = word.left(word.size() - rule.add.size()) + rule.strip;
        if (root.isEmpty() || !conditionMatches(rule, root, false))
            continue;
        if (!checkRoot(root, group.flag, true))
            continue;
        if (allowPrefix && group.cross && tryPrefixOnRoot(root, group.flag))
            return true;
        return true;
    }
    return false;
}

bool HunspellEngine::tryPrefix(const QString& word, bool allowSuffix) const
{
    if (word.size() < 2)
        return false;
    const auto refs = m_prefixIndex.constFind(word.at(0));
    if (refs == m_prefixIndex.constEnd())
        return false;
    for (const RuleRef& ref : refs.value()) {
        const AffixGroup& group = m_prefixGroups.at(ref.group);
        const AffixRule& rule = group.rules.at(ref.rule);
        if (rule.add.isEmpty() || !word.startsWith(rule.add))
            continue;
        const QString root = rule.strip + word.mid(rule.add.size());
        if (root.isEmpty() || !conditionMatches(rule, root, true))
            continue;
        if (!checkRoot(root, group.flag, true))
            continue;
        if (allowSuffix && group.cross && trySuffixOnRoot(root, group.flag))
            return true;
        return true;
    }
    return false;
}

bool HunspellEngine::tryPrefixOnRoot(const QString& root, const QString& suffixFlag) const
{
    if (root.size() < 2)
        return false;
    const auto refs = m_prefixIndex.constFind(root.at(0));
    if (refs == m_prefixIndex.constEnd())
        return false;
    for (const RuleRef& ref : refs.value()) {
        const AffixGroup& group = m_prefixGroups.at(ref.group);
        const AffixRule& rule = group.rules.at(ref.rule);
        if (!group.cross || rule.add.isEmpty() || !root.startsWith(rule.add))
            continue;
        const QString base = rule.strip + root.mid(rule.add.size());
        if (base.isEmpty() || !conditionMatches(rule, base, true))
            continue;
        if (!checkRoot(base, group.flag, true))
            continue;
        const auto found = m_words.constFind(base);
        if (found != m_words.constEnd() && hasFlag(found.value(), suffixFlag))
            return true;
    }
    return false;
}

bool HunspellEngine::trySuffixOnRoot(const QString& root, const QString& prefixFlag) const
{
    if (root.size() < 2)
        return false;
    const auto refs = m_suffixIndex.constFind(root.at(root.size() - 1));
    if (refs == m_suffixIndex.constEnd())
        return false;
    for (const RuleRef& ref : refs.value()) {
        const AffixGroup& group = m_suffixGroups.at(ref.group);
        const AffixRule& rule = group.rules.at(ref.rule);
        if (!group.cross || rule.add.isEmpty() || !root.endsWith(rule.add))
            continue;
        const QString base = root.left(root.size() - rule.add.size()) + rule.strip;
        if (base.isEmpty() || !conditionMatches(rule, base, false))
            continue;
        if (!checkRoot(base, group.flag, true))
            continue;
        const auto found = m_words.constFind(base);
        if (found != m_words.constEnd() && hasFlag(found.value(), prefixFlag))
            return true;
    }
    return false;
}

bool HunspellEngine::checkVariant(const QString& word) const
{
    const auto found = m_words.constFind(word);
    if (found != m_words.constEnd()) {
        const QString flags = found.value();
        if (!hasFlag(flags, m_forbiddenFlag)
            && !hasFlag(flags, m_onlyInCompoundFlag)
            && !hasFlag(flags, m_needAffixFlag)) {
            return true;
        }
    }
    if (trySuffix(word, true))
        return true;
    if (tryPrefix(word, true))
        return true;
    return false;
}

bool HunspellEngine::spell(const QString& word) const
{
    if (!m_loaded)
        return true;
    QString value = cleanWord(word);
    if (value.isEmpty())
        return true;

    if (checkVariant(value))
        return true;

    const QString lower = value.toLower();
    if (lower != value && checkVariant(lower))
        return true;
    QString title = lower;
    if (!title.isEmpty())
        title[0] = title.at(0).toUpper();
    if (title != value && title != lower && checkVariant(title))
        return true;
    const QString upper = value.toUpper();
    if (upper != value && upper != lower && upper != title && checkVariant(upper))
        return true;
    return false;
}

QStringList HunspellEngine::suggest(const QString& word, int limit) const
{
    if (!m_loaded || limit <= 0)
        return {};
    QString value = cleanWord(word);
    if (value.isEmpty() || value.size() > 24)
        return {};

    const QString lower = value.toLower();
    const bool capitalized = !value.isEmpty() && value.at(0).isUpper();
    QString alphabet;
    QSet<QChar> seenCharacters;
    for (const QChar character : m_tryChars) {
        const QChar normalized = character.toLower();
        if (!normalized.isLetter() || seenCharacters.contains(normalized))
            continue;
        seenCharacters.insert(normalized);
        alphabet.append(normalized);
    }
    if (alphabet.isEmpty())
        alphabet = QStringLiteral("abcdefghijklmnopqrstuvwxyz");

    QHash<QString, int> best;
    const auto consider = [this, &best, &lower](const QString& candidate, int weight) {
        if (candidate.isEmpty() || candidate == lower)
            return;
        if (!spell(candidate))
            return;
        if (m_words.contains(candidate))
            weight = 0;
        const auto existing = best.constFind(candidate);
        if (existing == best.constEnd() || weight < existing.value())
            best.insert(candidate, weight);
    };

    const int length = lower.size();
    for (int index = 0; index + 1 < length; ++index) {
        QString candidate = lower;
        candidate[index] = lower.at(index + 1);
        candidate[index + 1] = lower.at(index);
        consider(candidate, 0);
    }
    for (int index = 0; index < length; ++index) {
        for (const QChar character : alphabet) {
            if (character == lower.at(index))
                continue;
            QString candidate = lower;
            candidate[index] = character;
            consider(candidate, 1);
        }
    }
    for (int index = 0; index <= length; ++index) {
        for (const QChar character : alphabet) {
            QString candidate = lower;
            candidate.insert(index, character);
            consider(candidate, 2);
        }
    }
    for (int index = 0; index < length; ++index)
        consider(lower.left(index) + lower.mid(index + 1), 3);

    QList<QPair<int, QString>> ranked;
    ranked.reserve(best.size());
    for (auto it = best.constBegin(); it != best.constEnd(); ++it)
        ranked.append(qMakePair(it.value(), it.key()));
    std::sort(ranked.begin(), ranked.end(),
              [](const QPair<int, QString>& left, const QPair<int, QString>& right) {
                  if (left.first != right.first)
                      return left.first < right.first;
                  return left.second.localeAwareCompare(right.second) < 0;
              });

    QStringList result;
    for (const auto& candidate : ranked) {
        QString suggestion = candidate.second;
        if (capitalized && !suggestion.isEmpty())
            suggestion[0] = suggestion.at(0).toUpper();
        if (!result.contains(suggestion))
            result.append(suggestion);
        if (result.size() >= limit)
            break;
    }
    return result;
}
