#pragma once

#include <QObject>
#include <QString>

class ClipboardManager : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardManager(QObject* parent = nullptr);

    QString copySelection(int delayMs = 600, QString* error = nullptr);
    void copyText(const QString& text);
    void pasteText(const QString& text);

private:
    static bool sendCopyShortcut();
    static bool sendPasteShortcut();
};
