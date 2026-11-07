#pragma once

#include <QDialog>
#include <QHash>
#include <QString>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QNetworkAccessManager;
class QProgressBar;
class QPushButton;

class TesseractLanguageDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TesseractLanguageDialog(const QString& tessdataPath, QWidget* parent = nullptr);

private:
    void buildUi();
    void refreshItems();
    void handleItemChanged(QListWidgetItem* item);
    void downloadLanguage(QListWidgetItem* item, const QString& code, bool fallback = false);
    QString filePathFor(const QString& code) const;
    void updateBusyState();

    QString m_tessdataPath;
    QListWidget* m_list = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QNetworkAccessManager* m_network = nullptr;
    QPushButton* m_closeButton = nullptr;
    int m_activeDownloads = 0;
    bool m_updating = false;
};
