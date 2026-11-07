#include "history_window.h"

#include "core/app_context.h"
#include "core/history_manager.h"
#include "core/localization.h"
#include "icon_factory.h"
#include "services/plugin_manager.h"
#include "theme_manager.h"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

HistoryWindow::HistoryWindow(AppContext* context, QWidget* parent)
    : QDialog(parent)
    , m_context(context)
{
    setWindowTitle(QStringLiteral("История переводов"));
    resize(820, 560);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* top = new QHBoxLayout;
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(QStringLiteral("Поиск по тексту…"));
    top->addWidget(m_filter, 1);
    m_countLabel = new QLabel(this);
    m_countLabel->setObjectName(QStringLiteral("MiniNote"));
    top->addWidget(m_countLabel);
    root->addLayout(top);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_cardsHost = new QWidget(m_scroll);
    m_cardsLayout = new QVBoxLayout(m_cardsHost);
    m_cardsLayout->setContentsMargins(0, 0, 6, 0);
    m_cardsLayout->setSpacing(8);
    m_cardsLayout->addStretch(1);
    m_scroll->setWidget(m_cardsHost);
    root->addWidget(m_scroll, 1);

    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    auto* clearButton = new QPushButton(QStringLiteral("Очистить"), this);
    auto* closeButton = new QPushButton(QStringLiteral("Закрыть"), this);
    bottom->addWidget(clearButton);
    bottom->addWidget(closeButton);
    root->addLayout(bottom);

    connect(m_filter, &QLineEdit::textChanged, this, &HistoryWindow::refresh);
    connect(clearButton, &QPushButton::clicked, this, [this] {
        if (!m_context || !m_context->history)
            return;
        if (QMessageBox::question(this, QStringLiteral("История"),
                                  QStringLiteral("Удалить всю историю переводов?"))
            == QMessageBox::Yes) {
            m_context->history->clear();
        }
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    if (m_context && m_context->history)
        connect(m_context->history, &HistoryManager::changed, this, &HistoryWindow::refresh);

    refresh();
}

void HistoryWindow::refresh()
{
    if (!m_cardsLayout)
        return;

    while (m_cardsLayout->count() > 1) {
        QLayoutItem* item = m_cardsLayout->takeAt(0);
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    if (!m_context || !m_context->history) {
        m_countLabel->setText(QStringLiteral("Записей: 0"));
        return;
    }

    const QString cardSurface = ThemeManager::color(QStringLiteral("surfaceAlt")).name();
    const QString cardBorder = ThemeManager::color(QStringLiteral("border")).name();
    const QString muted = ThemeManager::color(QStringLiteral("muted")).name();
    const QString textColor = ThemeManager::color(QStringLiteral("text")).name();

    const QJsonArray entries = m_context->history->entries();
    const QString filter = m_filter ? m_filter->text().trimmed() : QString();
    int shown = 0;

    for (int index = entries.size() - 1; index >= 0; --index) {
        const QJsonObject entry = entries.at(index).toObject();
        const QString source = entry.value(QStringLiteral("source")).toString();
        const QString translation = entry.value(QStringLiteral("translation")).toString();
        if (!filter.isEmpty() && !source.contains(filter, Qt::CaseInsensitive)
            && !translation.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }

        const QDateTime dateTime =
            QDateTime::fromString(entry.value(QStringLiteral("timestamp")).toString(),
                                  Qt::ISODate);
        const QString dateText = dateTime.isValid()
            ? dateTime.toString(QStringLiteral("dd.MM.yyyy HH:mm"))
            : QStringLiteral("—");
        QString service = entry.value(QStringLiteral("service")).toString();
        if (m_context && m_context->plugins) {
            for (const PluginManifest& manifest : m_context->plugins->manifests()) {
                if (manifest.id == service) {
                    service = manifest.name;
                    break;
                }
            }
        }
        if (service.isEmpty())
            service = QStringLiteral("—");

        auto* card = new QFrame(m_cardsHost);
        card->setObjectName(QStringLiteral("HistoryCard"));
        card->setStyleSheet(QStringLiteral(
            "QFrame#HistoryCard { background:%1; border:1px solid %2; border-radius:8px; }")
                                .arg(cardSurface, cardBorder));
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 8, 10, 10);
        cardLayout->setSpacing(4);

        auto* header = new QHBoxLayout;
        header->setSpacing(8);
        auto* meta = new QLabel(QStringLiteral("%1  ·  %2").arg(dateText, service), card);
        meta->setStyleSheet(QStringLiteral("color:%1;font-size:11px;border:none;")
                                .arg(muted));
        header->addWidget(meta);
        header->addStretch(1);
        auto* open = new QToolButton(card);
        open->setObjectName(QStringLiteral("SideButton"));
        open->setText(QString::fromUtf8("↗"));
        open->setToolTip(Localization::tr(QStringLiteral("Открыть в главном окне")));
        open->setAccessibleName(Localization::tr(QStringLiteral("Открыть в главном окне")));
        open->setFixedSize(26, 26);
        open->setCursor(Qt::PointingHandCursor);
        header->addWidget(open);
        auto* remove = new QToolButton(card);
        remove->setObjectName(QStringLiteral("SideButton"));
        remove->setIcon(IconFactory::icon(IconFactory::Glyph::Trash,
                                          ThemeManager::color(QStringLiteral("muted")), 15));
        remove->setIconSize(QSize(16, 16));
        remove->setFixedSize(26, 26);
        remove->setCursor(Qt::PointingHandCursor);
        remove->setToolTip(Localization::tr(QStringLiteral("Удалить запись")));
        remove->setAccessibleName(Localization::tr(QStringLiteral("Удалить запись")));
        header->addWidget(remove);
        cardLayout->addLayout(header);

        auto* sourceLabel = new QLabel(source, card);
        sourceLabel->setWordWrap(true);
        sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        sourceLabel->setStyleSheet(QStringLiteral(
            "color:%1;font-size:13px;border:none;background:transparent;").arg(textColor));
        cardLayout->addWidget(sourceLabel);

        auto* translationLabel = new QLabel(translation, card);
        translationLabel->setWordWrap(true);
        translationLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        translationLabel->setStyleSheet(QStringLiteral(
            "color:%1;font-size:13px;border:none;background:transparent;").arg(muted));
        cardLayout->addWidget(translationLabel);

        connect(remove, &QToolButton::clicked, this,
                [this, index] { deleteEntry(index); });
        connect(open, &QToolButton::clicked, this,
                [this, entry] { emit entryActivated(entry); });

        m_cardsLayout->insertWidget(m_cardsLayout->count() - 1, card);
        ++shown;
    }

    m_countLabel->setText(QStringLiteral("Записей: %1").arg(shown));
    if (shown == 0) {
        auto* empty = new QLabel(QStringLiteral("История пуста."), m_cardsHost);
        empty->setObjectName(QStringLiteral("MiniNote"));
        m_cardsLayout->insertWidget(0, empty);
    }
}

void HistoryWindow::deleteEntry(int historyIndex)
{
    if (!m_context || !m_context->history)
        return;
    m_context->history->removeAt(historyIndex);
}
