#include "dictionary_window.h"
#include "down_combo_box.h"

#include "core/app_context.h"
#include "core/localization.h"
#include "core/language_catalog.h"
#include "core/language_preferences.h"
#include "core/settings_manager.h"
#include "modules/dictionary/dictionary_manager.h"
#include "services/plugin_manager.h"
#include "ui/theme_manager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTextBrowser>
#include <QTabWidget>
#include <QTextCursor>
#include <QVBoxLayout>

DictionaryWindow::DictionaryWindow(AppContext* context, QWidget* parent)
    : QDialog(parent)
    , m_context(context)
{
    setWindowTitle(QStringLiteral("Словарь"));
    resize(980, 640);
    buildUi();
    refreshDictionaries();
    refreshHistory();
}

void DictionaryWindow::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* searchRow = new QHBoxLayout;
    m_query = new QLineEdit(this);
    m_query->setPlaceholderText(QStringLiteral("Введите слово или фразу…"));
    m_query->setClearButtonEnabled(true);
    searchRow->addWidget(m_query, 1);

    auto* searchButton = new QPushButton(QStringLiteral("Найти"), this);
    searchButton->setObjectName(QStringLiteral("TranslateButton"));
    searchRow->addWidget(searchButton);

    m_exact = new QCheckBox(QStringLiteral("Точное совпадение"), this);
    searchRow->addWidget(m_exact);
    searchRow->addStretch(1);
    searchRow->addWidget(new QLabel(QStringLiteral("История:"), this));
    m_historyCombo = new DownComboBox(this);
    m_historyCombo->setMinimumWidth(180);
    searchRow->addWidget(m_historyCombo);
    root->addLayout(searchRow);

    auto* body = new QHBoxLayout;
    body->setSpacing(12);

    auto* side = new QWidget(this);
    side->setFixedWidth(250);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(8);

    auto* languagesGroup = new QGroupBox(QStringLiteral("Языки поиска"), side);
    auto* languagesLayout = new QVBoxLayout(languagesGroup);
    m_languageList = new QListWidget(languagesGroup);
    const QList<LanguageInfo> languages =
        favoriteLanguageList(m_context ? m_context->settings : nullptr, false);
    const QString source = m_context && m_context->settings
        ? normalizeLanguageCode(m_context->settings->stringValue(
              QStringLiteral("General.LanguageFrom"), QStringLiteral("en")))
        : QStringLiteral("en");
    const QString target = m_context && m_context->settings
        ? normalizeLanguageCode(m_context->settings->stringValue(
              QStringLiteral("General.LanguageTo"), QStringLiteral("ru")))
        : QStringLiteral("ru");
    for (const LanguageInfo& language : languages) {
        const QString code = QString::fromLatin1(language.code);
        auto* item = new QListWidgetItem(QString::fromUtf8(language.name), m_languageList);
        item->setData(Qt::UserRole, code);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState((code == source || code == target) ? Qt::Checked : Qt::Unchecked);
    }
    languagesLayout->addWidget(m_languageList, 1);
    sideLayout->addWidget(languagesGroup, 1);

    auto* dictionariesGroup = new QGroupBox(QStringLiteral("Словари"), side);
    auto* dictionariesLayout = new QVBoxLayout(dictionariesGroup);
    m_onlineList = new QListWidget(dictionariesGroup);
    dictionariesLayout->addWidget(m_onlineList, 1);

    sideLayout->addWidget(dictionariesGroup, 1);

    body->addWidget(side, 0);

    auto* right = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    m_tabs = new QTabWidget(right);
    m_tabs->setObjectName(QStringLiteral("DictionaryTabs"));
    m_tabs->setDocumentMode(true);
    rightLayout->addWidget(m_tabs, 1);
    m_status = new QLabel(QStringLiteral("Готово"), right);
    m_status->setObjectName(QStringLiteral("MiniNote"));
    rightLayout->addWidget(m_status);
    body->addWidget(right, 1);

    root->addLayout(body, 1);

    auto* closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    if (QPushButton* close = closeButtons->button(QDialogButtonBox::Close))
        close->setText(QStringLiteral("Закрыть"));
    connect(closeButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(closeButtons);

    connect(searchButton, &QPushButton::clicked, this, &DictionaryWindow::performSearch);
    connect(m_query, &QLineEdit::returnPressed, this, &DictionaryWindow::performSearch);
    connect(m_historyCombo, &QComboBox::activated, this, [this](int index) {
        if (index < 0)
            return;
        m_query->setText(m_historyCombo->itemText(index));
        performSearch();
    });
    connect(m_onlineList, &QListWidget::itemChanged, this, [this](QListWidgetItem*) {
        rebuildTabs();
    });
}

void DictionaryWindow::refreshDictionaries()
{
    if (!m_context || !m_context->dictionary)
        return;

    const bool blocked = m_onlineList->blockSignals(true);
    m_onlineList->clear();
    const QString preferred = m_context->settings
        ? m_context->settings->stringValue(QStringLiteral("ServiceProviders.dict")) : QString();
    const QList<PluginManifest> dictionaries = m_context->dictionary->onlineDictionaries();
    for (const PluginManifest& manifest : dictionaries) {
        auto* item = new QListWidgetItem(manifest.name, m_onlineList);
        item->setData(Qt::UserRole, manifest.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(preferred.isEmpty() || preferred == manifest.id
                                ? Qt::Checked : Qt::Unchecked);
    }
    m_onlineList->blockSignals(blocked);

    rebuildTabs();
}

void DictionaryWindow::rebuildTabs()
{
    if (!m_tabs)
        return;
    m_tabs->clear();
    m_tabBrowsers.clear();
    m_tabHtml.clear();
    m_tabTitles.clear();

    for (int index = 0; index < m_onlineList->count(); ++index) {
        QListWidgetItem* item = m_onlineList->item(index);
        if (!item || item->checkState() != Qt::Checked)
            continue;
        const QString id = item->data(Qt::UserRole).toString();
        ensureTab(id, item->text());
    }

    if (m_tabs->count() == 0) {
        auto* empty = new QTextBrowser(m_tabs);
        empty->setHtml(QStringLiteral(
            "<div style='padding:28px;text-align:center'>"
            "Отметьте хотя бы один словарь в списке слева.</div>"));
        m_tabBrowsers.insert(QStringLiteral("__empty__"), empty);
        m_tabs->addTab(empty, QStringLiteral("Словари"));
    }
}

QTextBrowser* DictionaryWindow::ensureTab(const QString& id, const QString& title)
{
    if (QTextBrowser* existing = m_tabBrowsers.value(id))
        return existing;
    auto* browser = new QTextBrowser(m_tabs);
    browser->setObjectName(QStringLiteral("DictionaryResults"));
    browser->setOpenExternalLinks(true);
    m_tabBrowsers.insert(id, browser);
    m_tabTitles.insert(id, title);
    m_tabHtml.insert(id, QString());
    m_tabs->addTab(browser, title);
    return browser;
}

void DictionaryWindow::refreshHistory()
{
    m_historyCombo->clear();
    if (!m_context || !m_context->dictionary)
        return;
    const QStringList history = m_context->dictionary->history();
    if (history.isEmpty()) {
        m_historyCombo->addItem(QStringLiteral("Пусто"));
        m_historyCombo->setEnabled(false);
        return;
    }
    m_historyCombo->setEnabled(true);
    m_historyCombo->addItems(history);
}

void DictionaryWindow::performSearch()
{
    if (!m_context || !m_context->dictionary)
        return;
    const QString word = m_query->text().trimmed();
    if (word.isEmpty())
        return;

    m_context->dictionary->addHistory(word);
    refreshHistory();
    rebuildTabs();

    QStringList ids;
    QHash<QString, QString> nameToId;
    for (int index = 0; index < m_onlineList->count(); ++index) {
        QListWidgetItem* item = m_onlineList->item(index);
        if (!item || item->checkState() != Qt::Checked)
            continue;
        const QString id = item->data(Qt::UserRole).toString();
        ids.append(id);
        nameToId.insert(item->text(), id);
    }
    if (ids.isEmpty()) {
        m_status->setText(QStringLiteral("Отметьте хотя бы один словарь"));
        return;
    }

    const QString pageText = ThemeManager::color(QStringLiteral("text")).name();
    const QString pageSurface = ThemeManager::color(QStringLiteral("surface")).name();
    const QString pageBorder = ThemeManager::color(QStringLiteral("border")).name();
    const QString pageMuted = ThemeManager::color(QStringLiteral("muted")).name();
    const QString pageAccent = ThemeManager::color(QStringLiteral("accentText")).name();
    const QString pageDanger = ThemeManager::color(QStringLiteral("danger")).name();
    const QString header = QStringLiteral(
        "<html><head><style>"
        "body{font-family:'Segoe UI';font-size:14px;line-height:1.45;color:%1;background:%2;}"
        ".card{border:1px solid %3;border-radius:9px;margin:10px 8px;padding:12px 15px;background:%2;}"
        ".card h3{margin:0 0 5px 0;font-size:15px;color:%4;}"
        ".card .sub{color:%5;font-size:11px;margin-bottom:7px;}"
        ".card .err{color:%6;}"
        ".dict-pos{font-weight:600;margin-top:8px;color:%4;}"
        ".dict-defs{margin:5px 0 9px 20px;padding:0;}"
        "</style></head><body>")
        .arg(pageText, pageSurface, pageBorder, pageAccent, pageMuted, pageDanger);

    for (const QString& id : std::as_const(ids)) {
        m_tabHtml[id] = header;
        if (QTextBrowser* browser = ensureTab(id, m_tabTitles.value(id)))
            browser->setHtml(header);
    }
    m_pending = ids.size();
    m_gotResult = false;
    m_status->setText(QStringLiteral("Поиск…"));

    const auto finish = [this] {
        if (m_pending > 0)
            return;
        m_status->setText(m_gotResult ? QStringLiteral("Готово")
                                      : Localization::tr(QStringLiteral("Ничего не найдено")));
    };

    QPointer<DictionaryWindow> self(this);

    QStringList searchLanguages;
    for (int index = 0; index < m_languageList->count(); ++index) {
        QListWidgetItem* item = m_languageList->item(index);
        if (item && item->checkState() == Qt::Checked)
            searchLanguages.append(item->data(Qt::UserRole).toString());
    }
    if (searchLanguages.isEmpty()) {
        m_status->setText(QStringLiteral("Отметьте язык поиска"));
        return;
    }
    m_pending = ids.size() * searchLanguages.size();
    for (const QString& language : std::as_const(searchLanguages)) {
        m_context->dictionary->searchOnline(word, language, language, ids,
            [self, nameToId, finish](const QString& service, const QJsonObject& result) {
            DictionaryWindow* window = self.data();
            if (!window)
                return;
            const QString id = nameToId.value(service);
            const QString error = result.value(QStringLiteral("error")).toString();
            if (!error.isEmpty()) {
                if (!id.isEmpty())
                    window->appendCardToTab(id, service, error, QString(), true);
            } else {
                window->m_gotResult = true;
                QString html = result.value(QStringLiteral("html")).toString();
                if (html.isEmpty()) {
                    const QString text = result.value(QStringLiteral("text")).toString();
                    html = QStringLiteral("<p>%1</p>")
                               .arg(text.toHtmlEscaped().replace(QLatin1Char('\n'),
                                                                 QStringLiteral("<br>")));
                }
                const QString title = result.value(QStringLiteral("title")).toString(service);
                if (!id.isEmpty())
                    window->appendCardToTab(id, title,
                                            result.value(QStringLiteral("url")).toString(), html);
            }
                --window->m_pending;
                finish();
            },
            m_exact->isChecked());
    }
}

void DictionaryWindow::appendCardToTab(const QString& id, const QString& title,
                                       const QString& subtitle, const QString& html,
                                       bool error)
{
    QTextBrowser* browser = m_tabBrowsers.value(id);
    if (!browser) {
        browser = ensureTab(id, m_tabTitles.value(id, title));
    }
    const QString card = QStringLiteral(
        "<div class='card'><h3>%1</h3>%2<div class='%4'>%3</div></div>")
        .arg(title.toHtmlEscaped(),
             subtitle.isEmpty() ? QString()
                                : QStringLiteral("<div class='sub'>%1</div>")
                                      .arg(subtitle.toHtmlEscaped()),
             error ? QStringLiteral("<p>%1</p>").arg(html.toHtmlEscaped()) : html,
             error ? QStringLiteral("err") : QStringLiteral("definitions"));
    m_tabHtml[id] += card;
    browser->setHtml(m_tabHtml[id] + QStringLiteral("</body></html>"));
    QTextCursor cursor = browser->textCursor();
    cursor.movePosition(QTextCursor::End);
    browser->setTextCursor(cursor);
    m_tabs->setCurrentWidget(browser);
}

void DictionaryWindow::appendCard(const QString& title, const QString& subtitle,
                                  const QString& html, bool error)
{
    const QString id = m_tabs->currentIndex() >= 0
        ? m_tabTitles.key(m_tabs->tabText(m_tabs->currentIndex()))
        : QString();
    if (!id.isEmpty())
        appendCardToTab(id, title, subtitle, html, error);
}


