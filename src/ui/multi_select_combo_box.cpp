#include "multi_select_combo_box.h"

#include <QAbstractItemView>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTimer>

MultiSelectComboBox::MultiSelectComboBox(QWidget* parent)
    : QComboBox(parent)
{
    auto* items = new QStandardItemModel(this);
    setModel(items);
    auto* listView = new QListView(this);
    setView(listView);
    setEditable(true);
    setInsertPolicy(QComboBox::NoInsert);
    lineEdit()->setReadOnly(true);
    lineEdit()->setFocusPolicy(Qt::ClickFocus);
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(10);

    listView->installEventFilter(this);
    listView->setSelectionMode(QAbstractItemView::NoSelection);
    connect(listView, &QAbstractItemView::pressed, this,
            &MultiSelectComboBox::toggleIndex);
    if (lineEdit())
        lineEdit()->installEventFilter(this);
}

void MultiSelectComboBox::setItems(const QStringList& values)
{
    auto* items = qobject_cast<QStandardItemModel*>(model());
    if (!items)
        return;
    const QStringList keep = selected();
    items->clear();
    for (const QString& value : values) {
        auto* item = new QStandardItem(value);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setData(Qt::Unchecked, Qt::CheckStateRole);
        items->appendRow(item);
    }
    setSelected(keep);
}

void MultiSelectComboBox::setSelected(const QStringList& values)
{
    auto* items = qobject_cast<QStandardItemModel*>(model());
    if (!items)
        return;
    QStringList normalized;
    for (const QString& raw : values) {
        for (const QString& part : raw.split(QRegularExpression(QStringLiteral("[+,;\\s]+")),
                                             Qt::SkipEmptyParts)) {
            normalized.append(part);
        }
    }
    if (m_single && normalized.size() > 1)
        normalized = {normalized.first()};
    for (int row = 0; row < items->rowCount(); ++row) {
        QStandardItem* item = items->item(row);
        item->setCheckState(normalized.contains(item->text(), Qt::CaseInsensitive)
                                ? Qt::Checked : Qt::Unchecked);
    }
    updateText();
}

void MultiSelectComboBox::setSingleSelection(bool single)
{
    if (m_single == single)
        return;
    m_single = single;
    if (m_single) {
        const QStringList current = selected();
        if (current.size() > 1)
            setSelected({current.first()});
    }
}

QStringList MultiSelectComboBox::selected() const
{
    QStringList result;
    const auto* items = qobject_cast<const QStandardItemModel*>(model());
    if (!items)
        return result;
    for (int row = 0; row < items->rowCount(); ++row) {
        if (items->item(row)->checkState() == Qt::Checked)
            result.append(items->item(row)->text());
    }
    return result;
}

void MultiSelectComboBox::showPopup()
{
    QComboBox::showPopup();
}

void MultiSelectComboBox::hidePopup()
{
    const bool reopen = m_reopen;
    m_reopen = false;
    QComboBox::hidePopup();
    if (reopen) {
        QTimer::singleShot(0, this, &MultiSelectComboBox::showPopup);
    }
}

void MultiSelectComboBox::toggleIndex(const QModelIndex& index)
{
    auto* items = qobject_cast<QStandardItemModel*>(model());
    if (!items || !index.isValid())
        return;
    QStandardItem* item = items->itemFromIndex(index);
    if (!item)
        return;
    if (m_single) {
        if (item->checkState() == Qt::Checked) {
            updateText();
            return;
        }
        for (int row = 0; row < items->rowCount(); ++row)
            items->item(row)->setCheckState(Qt::Unchecked);
        item->setCheckState(Qt::Checked);
        m_reopen = true;
        updateText();
        emit selectionChanged();
        QTimer::singleShot(0, this, [this] { updateText(); });
        QTimer::singleShot(300, this, [this] { m_reopen = false; });
        return;
    }
    if (item->checkState() == Qt::Checked && selected().size() <= 1) {
        // Keep at least one OCR language selected.
        updateText();
        return;
    }
    item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    m_reopen = true;
    updateText();
    emit selectionChanged();
    QTimer::singleShot(0, this, [this] { updateText(); });
    QTimer::singleShot(300, this, [this] { m_reopen = false; });
}

void MultiSelectComboBox::updateText()
{
    const QString joined = selected().join(QLatin1Char('+'));
    setEditText(joined);
    setToolTip(joined);
}

bool MultiSelectComboBox::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == lineEdit() && event->type() == QEvent::MouseButtonPress) {
        showPopup();
        return true;
    }
    return QComboBox::eventFilter(watched, event);
}
