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

    searchRow->addWidget(new QLabel(QStringLiteral("Масштаб"), this));
    m_zoom = new QSlider(Qt::Horizontal, this);
    m_zoom->setRange(80, 140);
    m_zoom->setValue(100);
    m_zoom->setFixedWidth(90);
    searchRow->addWidget(m_zoom);
    root->addLayout(searchRow);

    auto* languageRow = new QHBoxLayout;
    m_fromCombo = new DownComboBox(this);
    m_toCombo = new DownComboBox(this);
    const QList<LanguageInfo> languages =
        favoriteLanguageList(m_context ? m_context->settings : nullptr, false);
    for (const LanguageInfo& language : languages) {
        const QString code = QString::fromLatin1(language.code);
        const QString name = QString::fromUtf8(language.name);
        m_fromCombo->addItem(name, code);
        m_toCombo->addItem(name, code);
    }
    if (m_context && m_context->settings) {
        const QString from = normalizeLanguageCode(
            m_context->settings->stringValue(QStringLiteral("General.LanguageFrom"),
                                             QStringLiteral("en")));
        const QString to = normalizeLanguageCode(
            m_context->settings->stringValue(QStringLiteral("General.LanguageTo"),
                                             QStringLiteral("ru")));
        if (m_fromCombo->findData(from) < 0)
            m_fromCombo->addItem(languageNameByCode(from), from);
        if (m_toCombo->findData(to) < 0)
            m_toCombo->addItem(languageNameByCode(to), to);
        if (const int index = m_fromCombo->findData(from); index >= 0)
            m_fromCombo->setCurrentIndex(index);
        if (const int index = m_toCombo->findData(to); index >= 0)
            m_toCombo->setCurrentIndex(index);
    }
    auto* swap = new QPushButton(QStringLiteral("⇄"), this);
    languageRow->addWidget(m_fromCombo);
    languageRow->addWidget(swap);
    languageRow->addWidget(m_toCombo);
    languageRow->addStretch(1);
    languageRow->addWidget(new QLabel(QStringLiteral("История:"), this));
    m_historyCombo = new DownComboBox(this);
    m_historyCombo->setMinimumWidth(160);
    languageRow->addWidget(m_historyCombo);
    root->addLayout(languageRow);

    auto* body = new QHBoxLayout;
    body->setSpacing(12);

    auto* side = new QWidget(this);
    side->setFixedWidth(250);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(8);

    auto* localGroup = new QGroupBox(QStringLiteral("Локальные XDXF"), side);
    auto* localLayout = new QVBoxLayout(localGroup);
    m_localCombo = new DownComboBox(localGroup);
    localLayout->addWidget(m_localCombo);
    auto* localButtons = new QHBoxLayout;
    auto* addDictionary = new QPushButton(QStringLiteral("Добавить…"), localGroup);
    auto* removeDictionary = new QPushButton(QStringLiteral("Удалить"), localGroup);
    localButtons->addWidget(addDictionary);
    localButtons->addWidget(removeDictionary);
    localLayout->addLayout(localButtons);
    sideLayout->addWidget(localGroup);

    auto* onlineGroup = new QGroupBox(QStringLiteral("Онлайн-словари"), side);
    auto* onlineLayout = new QVBoxLayout(onlineGroup);
    m_onlineList = new QListWidget(onlineGroup);
    onlineLayout->addWidget(m_onlineList);
    sideLayout->addWidget(onlineGroup, 1);

    body->addWidget(side, 0);

    auto* right = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    m_results = new QTextBrowser(right);
    m_results->setObjectName(QStringLiteral("DictionaryResults"));
    m_results->setOpenExternalLinks(true);
    m_results->setHtml(QStringLiteral(
        "<div style='padding:28px;text-align:center'>"
        "Введите слово и нажмите «Найти».</div>"));
    rightLayout->addWidget(m_results, 1);
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
    connect(swap, &QPushButton::clicked, this, [this] {
        const int from = m_fromCombo->currentIndex();
        const int to = m_toCombo->currentIndex();
        m_fromCombo->setCurrentIndex(to);
        m_toCombo->setCurrentIndex(from);
    });
    connect(m_zoom, &QSlider::valueChanged, this, &DictionaryWindow::updateZoom);
    connect(addDictionary, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Добавить XDXF-словарь"), QString(),
            QStringLiteral("XDXF (*.xdxf);;Все файлы (*.*)"));
        if (path.isEmpty())
            return;
        if (m_context && m_context->dictionary) {
            m_context->dictionary->addLocalDictionary(path);
            refreshDictionaries();
        }
    });
    connect(removeDictionary, &QPushButton::clicked, this, [this] {
        if (!m_context || !m_context->dictionary)
            return;
        const QString path = m_localCombo->currentData().toString();
        if (path.isEmpty())
            return;
        m_context->dictionary->removeLocalDictionary(path);
        refreshDictionaries();
    });
}

void DictionaryWindow::refreshDictionaries()
{
    if (!m_context || !m_context->dictionary)
        return;

    m_localCombo->clear();
    const QStringList paths = m_context->dictionary->localPaths();
    for (const QString& path : paths)
        m_localCombo->addItem(QFileInfo(path).fileName(), path);
    if (paths.isEmpty())
        m_localCombo->addItem(QStringLiteral("Нет словарей"), QString());

    m_onlineList->clear();
    const bool dictOn = !m_context->settings
        || m_context->settings->boolValue(QStringLiteral("ServiceProviders.dictOn"), true);
    const QString preferred = m_context->settings
        ? m_context->settings->stringValue(QStringLiteral("ServiceProviders.dict")) : QString();
    for (const PluginManifest& manifest : m_context->dictionary->onlineDictionaries()) {
        auto* item = new QListWidgetItem(manifest.name, m_onlineList);
        item->setData(Qt::UserRole, manifest.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        if (!dictOn) {
            item->setFlags(item->flags() & ~Qt::ItemFlags(Qt::ItemIsEnabled));
            item->setCheckState(Qt::Unchecked);
        } else {
            item->setCheckState(preferred.isEmpty() || preferred == manifest.id
                                    ? Qt::Checked : Qt::Unchecked);
        }
    }
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

    const QString pageText = ThemeManager::color(QStringLiteral("text")).name();
    const QString pageSurface = ThemeManager::color(QStringLiteral("surface")).name();
    const QString pageBorder = ThemeManager::color(QStringLiteral("border")).name();
    const QString pageMuted = ThemeManager::color(QStringLiteral("muted")).name();
    const QString pageAccent = ThemeManager::color(QStringLiteral("accentText")).name();
    const QString pageDanger = ThemeManager::color(QStringLiteral("danger")).name();
    m_resultHtml = QStringLiteral(
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
    m_results->setHtml(m_resultHtml);
    m_pending = 1;
    m_gotResult = false;
    m_status->setText(QStringLiteral("Поиск…"));

    const QString from = m_fromCombo->currentData().toString();
    const QString to = m_toCombo->currentData().toString();
    QPointer<DictionaryWindow> self(this);

    m_context->dictionary->searchLocalAsync(word, m_exact->isChecked(),
        [self](const QVector<DictionaryEntry>& entries) {
            DictionaryWindow* window = self.data();
            if (!window)
                return;
            for (const DictionaryEntry& entry : entries) {
                window->m_gotResult = true;
                window->appendCard(entry.word, entry.dictionaryName, entry.definitionHtml);
            }
            if (--window->m_pending == 0) {
                if (!window->m_gotResult) {
                    window->appendCard(QStringLiteral("Локальный поиск"), QString(),
                               Localization::tr(QStringLiteral("<p>Ничего не найдено.</p>")));
                    if (window->m_status)
                        window->m_status->setText(Localization::tr(QStringLiteral("Ничего не найдено")));
                } else {
                    if (window->m_status)
                        window->m_status->setText(QStringLiteral("Готово"));
                }
            }
        });

    QStringList onlineIds;
    for (int index = 0; index < m_onlineList->count(); ++index) {
        QListWidgetItem* item = m_onlineList->item(index);
        if (item->checkState() == Qt::Checked)
            onlineIds.append(item->data(Qt::UserRole).toString());
    }
    if (onlineIds.isEmpty())
        return;

    m_pending += onlineIds.size();
    m_context->dictionary->searchOnline(word, from, to, onlineIds,
        [self](const QString& service, const QJsonObject& result) {
            DictionaryWindow* window = self.data();
            if (!window)
                return;
            const QString error = result.value(QStringLiteral("error")).toString();
            if (!error.isEmpty()) {
                window->appendCard(service, error, QString(), true);
            } else {
                window->m_gotResult = true;
                QString html = result.value(QStringLiteral("html")).toString();
                if (html.isEmpty()) {
                    const QString text = result.value(QStringLiteral("text")).toString();
                    html = QStringLiteral("<p>%1</p>")
                               .arg(text.toHtmlEscaped().replace(QLatin1Char('\n'),
                                                                 QStringLiteral("<br>")));
                }
                window->appendCard(result.value(QStringLiteral("title")).toString(service),
                           result.value(QStringLiteral("url")).toString(), html);
            }
            if (--window->m_pending == 0 && window->m_status)
                window->m_status->setText(window->m_gotResult ? QStringLiteral("Готово")
                                              : Localization::tr(QStringLiteral("Ничего не найдено")));
        });
}

void DictionaryWindow::appendCard(const QString& title, const QString& subtitle,
                                  const QString& html, bool error)
{
    const QString card = QStringLiteral(
        "<div class='card'><h3>%1</h3>%2<div class='%4'>%3</div></div>")
        .arg(title.toHtmlEscaped(),
             subtitle.isEmpty() ? QString()
                                : QStringLiteral("<div class='sub'>%1</div>")
                                      .arg(subtitle.toHtmlEscaped()),
             error ? QStringLiteral("<p>%1</p>").arg(html.toHtmlEscaped()) : html,
             error ? QStringLiteral("err") : QStringLiteral("definitions"));
    m_resultHtml += card;
    m_results->setHtml(m_resultHtml + QStringLiteral("</body></html>"));
    QTextCursor cursor = m_results->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_results->setTextCursor(cursor);
}

void DictionaryWindow::updateZoom()
{
    const int target = m_zoom->value();
    if (target > m_lastZoom) {
        for (int index = m_lastZoom; index < target; index += 10)
            m_results->zoomIn(1);
    } else if (target < m_lastZoom) {
        for (int index = target; index < m_lastZoom; index += 10)
            m_results->zoomOut(1);
    }
    m_lastZoom = target;
}
