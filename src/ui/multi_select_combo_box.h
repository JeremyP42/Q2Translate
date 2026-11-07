#pragma once

#include <QComboBox>
#include <QStringList>

// A combo box whose popup stays open while the user toggles several items.
// The edit text is the joined list, e.g. "eng+rus".
class MultiSelectComboBox : public QComboBox
{
    Q_OBJECT

public:
    explicit MultiSelectComboBox(QWidget* parent = nullptr);

    void setItems(const QStringList& items);
    void setSelected(const QStringList& values);
    QStringList selected() const;
    void setSingleSelection(bool single);

    void showPopup() override;
    void hidePopup() override;

signals:
    void selectionChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void toggleIndex(const QModelIndex& index);
    void updateText();

    bool m_reopen = false;
    bool m_single = false;
};
