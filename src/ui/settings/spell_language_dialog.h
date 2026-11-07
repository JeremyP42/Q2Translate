#pragma once

#include <QDialog>
#include <QString>

class ISpellProvider;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QPushButton;

class SpellLanguageDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SpellLanguageDialog(ISpellProvider* provider, QWidget* parent = nullptr);

private:
    void buildUi();
    void refreshItems();
    void handleItemChanged(QListWidgetItem* item);
    void installLanguage(QListWidgetItem* item, const QString& code);
    void removeLanguage(QListWidgetItem* item, const QString& code);
    void setBusy(bool busy, const QString& text = QString());

    ISpellProvider* m_provider = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QPushButton* m_closeButton = nullptr;
    int m_activeOperations = 0;
    bool m_updating = false;
};
