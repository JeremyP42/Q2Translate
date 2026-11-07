#pragma once

#include <QDialog>
#include <QJsonObject>

class AppContext;
class QLabel;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;
class QWidget;

class HistoryWindow : public QDialog
{
    Q_OBJECT

public:
    explicit HistoryWindow(AppContext* context, QWidget* parent = nullptr);

signals:
    void entryActivated(const QJsonObject& entry);

private:
    void refresh();
    void deleteEntry(int historyIndex);

    AppContext* m_context = nullptr;
    QLineEdit* m_filter = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_cardsHost = nullptr;
    QVBoxLayout* m_cardsLayout = nullptr;
    QLabel* m_countLabel = nullptr;
};
