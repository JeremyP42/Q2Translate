#include "spell_language_dialog.h"

#include "core/localization.h"
#include "services/spell_provider.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

SpellLanguageDialog::SpellLanguageDialog(ISpellProvider* provider, QWidget* parent)
    : QDialog(parent)
    , m_provider(provider)
{
    setWindowTitle(Localization::tr(QStringLiteral("Языки проверки орфографии")));
    resize(560, 620);
    buildUi();
    refreshItems();
}

void SpellLanguageDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    auto* title = new QLabel(Localization::tr(QStringLiteral("Языки проверки орфографии")), this);
    title->setStyleSheet(QStringLiteral("font-size:16px; font-weight:600;"));
    root->addWidget(title);

    const QString directory = m_provider ? m_provider->spellDictionaryPath() : QString();
    QString description = Localization::tr(QStringLiteral(
        "Поставьте галочку, чтобы скачать словарь языка в папку коннектора. "
        "Снятие галочки удаляет файлы."));
    if (!directory.isEmpty()) {
        description += QLatin1Char('\n')
            + Localization::tr(QStringLiteral("Папка:")) + QLatin1Char(' ') + directory;
    }
    auto* hint = new QLabel(description, this);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("FieldHint"));
    root->addWidget(hint);

    auto* filterRow = new QHBoxLayout;
    auto* refresh = new QPushButton(Localization::tr(QStringLiteral("Обновить список")), this);
    filterRow->addWidget(refresh);
    filterRow->addStretch(1);
    m_status = new QLabel(Localization::tr(QStringLiteral("Готово")), this);
    m_status->setObjectName(QStringLiteral("MiniNote"));
    filterRow->addWidget(m_status);
    root->addLayout(filterRow);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    root->addWidget(m_list, 1);

    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    root->addWidget(m_progress);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_closeButton = new QPushButton(Localization::tr(QStringLiteral("Готово")), this);
    m_closeButton->setObjectName(QStringLiteral("TranslateButton"));
    buttons->addWidget(m_closeButton);
    root->addLayout(buttons);

    connect(refresh, &QPushButton::clicked, this, &SpellLanguageDialog::refreshItems);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (!m_updating)
            handleItemChanged(item);
    });
}

void SpellLanguageDialog::refreshItems()
{
    if (!m_provider)
        return;

    m_updating = true;
    m_list->clear();
    const QList<SpellLanguage> languages = m_provider->spellLanguages();
    if (languages.isEmpty()) {
        auto* item = new QListWidgetItem(
            Localization::tr(QStringLiteral("Нет доступных языков")), m_list);
        item->setFlags(Qt::NoItemFlags);
        m_updating = false;
        return;
    }

    for (const SpellLanguage& language : languages) {
        const QString displayName = Localization::tr(language.name);
        auto* item = new QListWidgetItem(
            QStringLiteral("%1  (%2)%3")
                .arg(displayName, language.code,
                     language.installed ? QStringLiteral(" ✓") : QString()),
            m_list);
        item->setData(Qt::UserRole, language.code);
        item->setData(Qt::UserRole + 1, displayName);
        item->setData(Qt::UserRole + 2, language.removable);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(language.installed ? Qt::Checked : Qt::Unchecked);
        if (!language.downloadable) {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            item->setToolTip(Localization::tr(
                QStringLiteral("Язык управляется системой Windows")));
        }
    }
    m_updating = false;
}

void SpellLanguageDialog::handleItemChanged(QListWidgetItem* item)
{
    if (!item || !m_provider)
        return;
    const QString code = item->data(Qt::UserRole).toString();
    const bool installed = item->checkState() == Qt::Checked;
    if (installed)
        installLanguage(item, code);
    else
        removeLanguage(item, code);
}

void SpellLanguageDialog::installLanguage(QListWidgetItem* item, const QString& code)
{
    item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
    const QString name = item->data(Qt::UserRole + 1).toString();
    setBusy(true, Localization::tr(QStringLiteral("Скачивание:")) + QLatin1Char(' ') + name);
    QPointer<SpellLanguageDialog> self(this);
    m_provider->installSpellLanguage(code, [self, name](bool ok, const QString& error) {
        SpellLanguageDialog* dialog = self.data();
        if (!dialog)
            return;
        if (ok) {
            dialog->m_status->setText(
                Localization::tr(QStringLiteral("Загружен язык:")) + QLatin1Char(' ') + name);
        } else {
            dialog->m_status->setText(
                Localization::tr(QStringLiteral("Ошибка загрузки:")) + QLatin1Char(' ') + error);
            QMessageBox::warning(dialog,
                                 Localization::tr(QStringLiteral("Языки проверки орфографии")),
                                 Localization::tr(QStringLiteral("Не удалось скачать"))
                                     + QLatin1Char(' ') + name + QStringLiteral(":\n") + error);
        }
        dialog->refreshItems();
        dialog->setBusy(false);
    });
}

void SpellLanguageDialog::removeLanguage(QListWidgetItem* item, const QString& code)
{
    item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
    const QString name = item->data(Qt::UserRole + 1).toString();
    setBusy(true, Localization::tr(QStringLiteral("Удаление:")) + QLatin1Char(' ') + name);
    QPointer<SpellLanguageDialog> self(this);
    m_provider->removeSpellLanguage(code, [self, name](bool ok, const QString& error) {
        SpellLanguageDialog* dialog = self.data();
        if (!dialog)
            return;
        if (ok) {
            dialog->m_status->setText(
                Localization::tr(QStringLiteral("Удалён язык:")) + QLatin1Char(' ') + name);
        } else {
            dialog->m_status->setText(
                Localization::tr(QStringLiteral("Не удалось удалить:"))
                    + QLatin1Char(' ') + error);
            QMessageBox::warning(dialog,
                                 Localization::tr(QStringLiteral("Языки проверки орфографии")),
                                 Localization::tr(QStringLiteral("Не удалось удалить"))
                                     + QLatin1Char(' ') + name + QStringLiteral(":\n") + error);
        }
        dialog->refreshItems();
        dialog->setBusy(false);
    });
}

void SpellLanguageDialog::setBusy(bool busy, const QString& text)
{
    if (!m_status || !m_progress || !m_closeButton)
        return;
    if (busy)
        ++m_activeOperations;
    else if (m_activeOperations > 0)
        --m_activeOperations;

    if (!text.isEmpty())
        m_status->setText(text);

    const bool active = m_activeOperations > 0;
    m_progress->setVisible(active);
    m_progress->setRange(0, 0);
    m_closeButton->setEnabled(!active);
}
