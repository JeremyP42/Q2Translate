#pragma once

#include <QDialog>
#include <QJsonObject>

class AppContext;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
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
    void appendCard(const QString& title, const QString& subtitle, const QString& html,
                    bool error = false);
    void updateZoom();

    AppContext* m_context = nullptr;
    QLineEdit* m_query = nullptr;
    QComboBox* m_fromCombo = nullptr;
    QComboBox* m_toCombo = nullptr;
    QComboBox* m_historyCombo = nullptr;
    QComboBox* m_localCombo = nullptr;
    QListWidget* m_onlineList = nullptr;
    QTextBrowser* m_results = nullptr;
    QCheckBox* m_exact = nullptr;
    QSlider* m_zoom = nullptr;
    QLabel* m_status = nullptr;
    int m_lastZoom = 100;
    int m_pending = 0;
    bool m_gotResult = false;
    QString m_resultHtml;
};
