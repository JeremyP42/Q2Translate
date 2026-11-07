#include "xdxf_dictionary.h"

#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>

#include <algorithm>

namespace {
constexpr quint32 kCacheMagic = 0x51544931;
constexpr quint32 kCacheVersion = 1;
}

XdxfDictionary::XdxfDictionary(const QString& path)
    : m_path(path)
    , m_name(QFileInfo(path).completeBaseName())
{
}

bool XdxfDictionary::open(QString* error)
{
    if (m_open)
        return true;

    if (!QFileInfo::exists(m_path)) {
        if (error)
            *error = QStringLiteral("Файл словаря не найден: %1").arg(m_path);
        return false;
    }

    if (!loadFromCache()) {
        if (!parseXml(error))
            return false;
        saveCache();
    }

    m_open = true;
    return true;
}

QVector<DictionaryEntry> XdxfDictionary::search(const QString& word, bool exactMatch) const
{
    QVector<DictionaryEntry> result;
    const QString key = normalize(word);
    if (key.isEmpty())
        return result;

    if (exactMatch) {
        const auto it = m_entries.constFind(key);
        if (it == m_entries.constEnd())
            return result;
        for (const QString& definition : it->definitions) {
            DictionaryEntry entry;
            entry.word = it->word;
            entry.definitionHtml = QStringLiteral("<p>%1</p>")
                                       .arg(definition.toHtmlEscaped()
                                                .replace(QLatin1Char('\n'), QStringLiteral("<br>")));
            entry.dictionaryName = m_name;
            result.append(entry);
        }
        return result;
    }

    QVector<QPair<int, const Value*>> matches;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        int score = -1;
        if (it.key() == key)
            score = 0;
        else if (it.key().startsWith(key))
            score = 1;
        else if (it.key().contains(key))
            score = 2;
        if (score >= 0)
            matches.append(qMakePair(score, &it.value()));
    }

    std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
        if (left.first != right.first)
            return left.first < right.first;
        return left.second->word.localeAwareCompare(right.second->word) < 0;
    });

    const int limit = qMin(50, int(matches.size()));
    for (int index = 0; index < limit; ++index) {
        const Value* value = matches.at(index).second;
        QString html;
        for (const QString& definition : value->definitions) {
            html += QStringLiteral("<p>%1</p>")
                        .arg(definition.toHtmlEscaped()
                                 .replace(QLatin1Char('\n'), QStringLiteral("<br>")));
        }
        DictionaryEntry entry;
        entry.word = value->word;
        entry.definitionHtml = html;
        entry.dictionaryName = m_name;
        result.append(entry);
    }
    return result;
}

QString XdxfDictionary::normalize(const QString& word)
{
    QString result = word.trimmed().toLower();
    result = result.simplified();
    return result;
}

bool XdxfDictionary::loadFromCache()
{
    const QString cachePath = m_path + QStringLiteral(".qti");
    const QFileInfo sourceInfo(m_path);
    const QFileInfo cacheInfo(cachePath);
    if (!cacheInfo.exists() || cacheInfo.lastModified() < sourceInfo.lastModified())
        return false;

    QFile file(cachePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 version = 0;
    stream >> magic >> version;
    if (magic != kCacheMagic || version != kCacheVersion)
        return false;

    quint32 count = 0;
    stream >> count;
    if (stream.status() != QDataStream::Ok)
        return false;

    QHash<QString, Value> entries;
    for (quint32 index = 0; index < count; ++index) {
        QString key;
        Value value;
        stream >> key >> value.word >> value.definitions;
        if (stream.status() != QDataStream::Ok)
            return false;
        entries.insert(key, value);
    }

    m_entries = entries;
    return true;
}

bool XdxfDictionary::saveCache()
{
    const QString cachePath = m_path + QStringLiteral(".qti");
    QFile file(cachePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << kCacheMagic << kCacheVersion << quint32(m_entries.size());
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it)
        stream << it.key() << it.value().word << it.value().definitions;
    return stream.status() == QDataStream::Ok;
}

bool XdxfDictionary::parseXml(QString* error)
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }

    m_entries.clear();
    QXmlStreamReader reader(&file);
    QString currentWord;
    QStringList currentDefinitions;

    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;
        const QString entryName = reader.name().toString();
        if (entryName != QStringLiteral("article") && entryName != QStringLiteral("ar"))
            continue;

        currentWord.clear();
        currentDefinitions.clear();
        QString plainText;

        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isEndElement() && reader.name() == entryName)
                break;
            if (reader.isStartElement()) {
                const QString name = reader.name().toString();
                if (name == QStringLiteral("k")) {
                    const QString value =
                        reader.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
                    if (!value.isEmpty())
                        currentWord = value;
                } else if (name == QStringLiteral("def")) {
                    const QString value =
                        reader.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
                    if (!value.isEmpty())
                        currentDefinitions.append(value);
                } else if (name == QStringLiteral("tr")) {
                    // Phonetic transcription is not a definition.
                    reader.readElementText(QXmlStreamReader::IncludeChildElements);
                }
            } else if (reader.isCharacters() && !reader.isWhitespace()) {
                plainText += reader.text();
            }
        }

        if (currentDefinitions.isEmpty()) {
            const QString value = plainText.simplified();
            if (!value.isEmpty())
                currentDefinitions.append(value);
        }

        if (!currentWord.isEmpty() && !currentDefinitions.isEmpty()) {
            const QString key = normalize(currentWord);
            Value& value = m_entries[key];
            if (value.word.isEmpty())
                value.word = currentWord;
            for (const QString& definition : currentDefinitions) {
                if (!value.definitions.contains(definition))
                    value.definitions.append(definition);
            }
        }
    }

    if (reader.hasError()) {
        if (error)
            *error = reader.errorString();
        return false;
    }
    if (m_entries.isEmpty()) {
        if (error)
            *error = QStringLiteral("В словаре не найдено ни одной статьи");
        return false;
    }
    return true;
}
