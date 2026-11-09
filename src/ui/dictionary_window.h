#pragma once

#include <QDialog>
#include <QHash>
#include <QJsonObject>

class AppContext;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTabWidget;
class QTextBrowser;

class DictionaryWindow : public QDialog
{
    Q_OBJECT

public:
    explicit DictionaryWindow(AppContext* context, QWidget* parent = nullptr);

private:
    void buildUi();
    void refreshDictionaries();
    void refreshHistory();
    void performSearch();
    void rebuildTabs();
    QTextBrowser* ensureTab(const QString& id, const QString& title);
    void appendCardToTab(const QString& id, const QString& title, const QString& subtitle,
                         const QString& html, bool error = false);
    void appendCard(const QString& title, const QString& subtitle, const QString& html,
                    bool error = false);

    AppContext* m_context = nullptr;
    QLineEdit* m_query = nullptr;
    QComboBox* m_historyCombo = nullptr;
    QListWidget* m_languageList = nullptr;
    QListWidget* m_onlineList = nullptr;
    QTabWidget* m_tabs = nullptr;
    QHash<QString, QTextBrowser*> m_tabBrowsers;
    QHash<QString, QString> m_tabHtml;
    QHash<QString, QString> m_tabTitles;
    QCheckBox* m_exact = nullptr;
    QLabel* m_status = nullptr;
    int m_pending = 0;
    bool m_gotResult = false;
    QString m_resultHtml;
};
