#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

struct DictionaryEntry {
    QString word;
    QString definitionHtml;
    QString dictionaryName;
};

class XdxfDictionary
{
public:
    explicit XdxfDictionary(const QString& path);

    bool open(QString* error = nullptr);
    QVector<DictionaryEntry> search(const QString& word, bool exactMatch) const;

    QString name() const { return m_name; }
    QString path() const { return m_path; }
    int size() const { return int(m_entries.size()); }

private:
    struct Value {
        QString word;
        QStringList definitions;
    };

    bool loadFromCache();
    bool saveCache();
    bool parseXml(QString* error);
    static QString normalize(const QString& word);

    QString m_path;
    QString m_name;
    QHash<QString, Value> m_entries;
    bool m_open = false;
};
