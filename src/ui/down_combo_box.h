#pragma once

#include <QComboBox>
#include <QFontMetrics>
#include <QListView>
#include <QScreen>
#include <QWidget>
#include <QWheelEvent>

class DownComboBox : public QComboBox
{
public:
    explicit DownComboBox(QWidget* parent = nullptr)
        : QComboBox(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
    }

    void wheelEvent(QWheelEvent* event) override
    {
        // Only an explicit click may change the selection; the wheel must
        // scroll the surrounding page instead of cycling through items.
        event->ignore();
    }

    void showPopup() override
    {
        QComboBox::showPopup();
        QWidget* popup = view() ? view()->window() : nullptr;
        if (!popup)
            return;

        const QRect available = screen() ? screen()->availableGeometry()
                                         : QRect(0, 0, 1920, 1080);
        QPoint position = mapToGlobal(QPoint(0, height()));
        if (position.y() > available.bottom())
            position.setY(available.top());
        if (position.x() + popup->width() > available.right())
            position.setX(qMax(available.left(), available.right() - popup->width()));
        const int availableHeight = qMax(0, available.bottom() - position.y() + 1);
        if (availableHeight > 0 && popup->height() > availableHeight)
            popup->resize(popup->width(), availableHeight);
        popup->move(position);
        popup->raise();
        if (view() && currentIndex() >= 0)
            view()->scrollTo(model()->index(currentIndex(), 0),
                             QAbstractItemView::PositionAtCenter);
    }
};

class MultiColumnComboBox : public DownComboBox
{
public:
    explicit MultiColumnComboBox(int columns, QWidget* parent = nullptr)
        : DownComboBox(parent)
        , m_columns(columns)
    {
        setMaxVisibleItems(20);
        if (auto* list = qobject_cast<QListView*>(view())) {
            list->setViewMode(QListView::IconMode);
            list->setFlow(QListView::LeftToRight);
            list->setWrapping(true);
            list->setResizeMode(QListView::Adjust);
            list->setMovement(QListView::Static);
            list->setUniformItemSizes(true);
            list->setSpacing(2);
            list->setGridSize(QSize(150, 28));
        }
    }

    void showPopup() override
    {
        int cellWidth = 0;
        if (auto* list = qobject_cast<QListView*>(view())) {
            const QFontMetrics metrics(list->font());
            for (int i = 0; i < count(); ++i)
                cellWidth = qMax(cellWidth, metrics.horizontalAdvance(itemText(i)));
            cellWidth = qMax(110, cellWidth + 26);
            list->setGridSize(QSize(cellWidth, 28));
        }
        DownComboBox::showPopup();
        QWidget* popup = view() ? view()->window() : nullptr;
        if (!popup)
            return;
        int cell = 150;
        if (auto* list = qobject_cast<QListView*>(view())) {
            if (list->gridSize().width() > 0)
                cell = list->gridSize().width();
        }
        const int width = m_columns * cell + 22;
        const int rows = (count() + m_columns - 1) / m_columns;
        const int visibleRows = qMin(rows, 20);
        int height = visibleRows * 28 + 6;
        const QRect available = screen() ? screen()->availableGeometry()
                                         : QRect(0, 0, 1920, 1080);
        const int availableBelow = available.bottom() - popup->y() + 1;
        if (availableBelow > 0)
            height = qMin(height, availableBelow);
        popup->setFixedWidth(width);
        popup->resize(width, height);
        if (currentIndex() >= 0)
            view()->scrollTo(model()->index(currentIndex(), 0),
                             QAbstractItemView::PositionAtCenter);
    }

private:
    int m_columns = 3;
};
