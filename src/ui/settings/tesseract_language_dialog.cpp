#include "tesseract_language_dialog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QUrl>
#include <QVBoxLayout>

namespace {

struct OcrLanguage {
    const char* code;
    const char* name;
};

const OcrLanguage kOcrLanguages[] = {
    {"eng", "Английский"},
    {"rus", "Русский"},
    {"ukr", "Украинский"},
    {"bel", "Белорусский"},
    {"deu", "Немецкий"},
    {"fra", "Французский"},
    {"spa", "Испанский"},
    {"ita", "Итальянский"},
    {"por", "Португальский"},
    {"nld", "Нидерландский"},
    {"pol", "Польский"},
    {"ces", "Чешский"},
    {"slk", "Словацкий"},
    {"slv", "Словенский"},
    {"hrv", "Хорватский"},
    {"srp", "Сербский"},
    {"bul", "Болгарский"},
    {"ron", "Румынский"},
    {"hun", "Венгерский"},
    {"ell", "Греческий"},
    {"tur", "Турецкий"},
    {"swe", "Шведский"},
    {"dan", "Датский"},
    {"fin", "Финский"},
    {"nor", "Норвежский"},
    {"est", "Эстонский"},
    {"lav", "Латышский"},
    {"lit", "Литовский"},
    {"kat", "Грузинский"},
    {"aze", "Азербайджанский"},
    {"kaz", "Казахский"},
    {"uzb", "Узбекский"},
    {"ara", "Арабский"},
    {"heb", "Иврит"},
    {"fas", "Персидский"},
    {"hin", "Хинди"},
    {"ben", "Бенгальский"},
    {"tam", "Тамильский"},
    {"tel", "Телугу"},
    {"kan", "Каннада"},
    {"mal", "Малаялам"},
    {"mar", "Маратхи"},
    {"guj", "Гуджарати"},
    {"pan", "Пенджаби"},
    {"urd", "Урду"},
    {"vie", "Вьетнамский"},
    {"tha", "Тайский"},
    {"ind", "Индонезийский"},
    {"msa", "Малайский"},
    {"fil", "Филиппинский"},
    {"jpn", "Японский"},
    {"chi_sim", "Китайский (упрощ.)"},
    {"chi_tra", "Китайский (традиц.)"},
    {"kor", "Корейский"},
    {"osd", "Определение ориентации"}
};

}

TesseractLanguageDialog::TesseractLanguageDialog(const QString& tessdataPath, QWidget* parent)
    : QDialog(parent)
    , m_tessdataPath(tessdataPath)
    , m_network(new QNetworkAccessManager(this))
{
    setWindowTitle(QStringLiteral("Языки Tesseract OCR"));
    resize(560, 620);
    buildUi();
    refreshItems();
}

void TesseractLanguageDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("Языки распознавания"), this);
    title->setStyleSheet(QStringLiteral("font-size:16px; font-weight:600;"));
    root->addWidget(title);

    auto* description = new QLabel(
        QStringLiteral("Поставьте галочку, чтобы скачать язык из tessdata. "
                       "Снятие галочки удаляет файл языка из папки Tesseract.\n"
                       "Папка: %1").arg(m_tessdataPath), this);
    description->setWordWrap(true);
    description->setObjectName(QStringLiteral("FieldHint"));
    root->addWidget(description);

    auto* filterRow = new QHBoxLayout;
    auto* filter = new QPushButton(QStringLiteral("Обновить список"), this);
    filterRow->addWidget(filter);
    filterRow->addStretch(1);
    m_status = new QLabel(QStringLiteral("Готово"), this);
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
    m_closeButton = new QPushButton(QStringLiteral("Готово"), this);
    m_closeButton->setObjectName(QStringLiteral("TranslateButton"));
    buttons->addWidget(m_closeButton);
    root->addLayout(buttons);

    connect(filter, &QPushButton::clicked, this, &TesseractLanguageDialog::refreshItems);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (!m_updating)
            handleItemChanged(item);
    });
}

void TesseractLanguageDialog::refreshItems()
{
    m_updating = true;
    m_list->clear();
    for (const OcrLanguage& language : kOcrLanguages) {
        const bool installed = QFileInfo::exists(filePathFor(QString::fromLatin1(language.code)));
        auto* item = new QListWidgetItem(
            QStringLiteral("%1  (%2)%3")
                .arg(QString::fromUtf8(language.name),
                     QString::fromLatin1(language.code),
                     installed ? QStringLiteral(" ✓") : QString()),
            m_list);
        item->setData(Qt::UserRole, QString::fromLatin1(language.code));
        item->setData(Qt::UserRole + 1, QString::fromUtf8(language.name));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(installed ? Qt::Checked : Qt::Unchecked);
    }
    m_updating = false;
}

QString TesseractLanguageDialog::filePathFor(const QString& code) const
{
    return QDir(m_tessdataPath).filePath(code + QStringLiteral(".traineddata"));
}

void TesseractLanguageDialog::handleItemChanged(QListWidgetItem* item)
{
    if (!item)
        return;
    const QString code = item->data(Qt::UserRole).toString();
    const QString name = item->data(Qt::UserRole + 1).toString();
    const QString path = filePathFor(code);
    if (item->checkState() == Qt::Checked) {
        if (!QFileInfo::exists(path))
            downloadLanguage(item, code);
    } else {
        if (QFileInfo::exists(path)) {
            if (QFile::remove(path))
                m_status->setText(QStringLiteral("Удалён язык: %1").arg(name));
            else
                m_status->setText(QStringLiteral("Не удалось удалить: %1").arg(name));
        }
    }
}

void TesseractLanguageDialog::downloadLanguage(QListWidgetItem* item, const QString& code, bool fallback)
{
    QDir().mkpath(m_tessdataPath);
    const QUrl url(fallback
        ? QStringLiteral("https://raw.githubusercontent.com/tesseract-ocr/tessdata/main/%1.traineddata").arg(code)
        : QStringLiteral("https://cdn.jsdelivr.net/gh/tesseract-ocr/tessdata@main/%1.traineddata").arg(code));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Q2Translate/2.7.1"));
    QNetworkReply* reply = m_network->get(request);
    ++m_activeDownloads;
    item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
    m_status->setText(QStringLiteral("Скачивание: %1").arg(item->data(Qt::UserRole + 1).toString()));
    m_progress->setVisible(true);
    m_progress->setRange(0, 0);
    updateBusyState();

    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (total > 0) {
                    m_progress->setRange(0, int(total));
                    m_progress->setValue(int(received));
                }
            });
    connect(reply, &QNetworkReply::finished, this, [this, reply, item, code, fallback] {
        --m_activeDownloads;
        item->setFlags(item->flags() | Qt::ItemIsEnabled);
        const QByteArray payload = reply->readAll();
        const QString error = reply->error() == QNetworkReply::NoError
            ? QString() : reply->errorString();
        reply->deleteLater();

        if (!fallback && (!error.isEmpty() || payload.isEmpty())) {
            downloadLanguage(item, code, true);
            return;
        }

        bool success = error.isEmpty() && !payload.isEmpty();
        if (success) {
            QSaveFile file(filePathFor(code));
            if (file.open(QIODevice::WriteOnly)) {
                if (file.write(payload) != payload.size() || !file.commit())
                    success = false;
            } else {
                success = false;
            }
        }

        m_updating = true;
        item->setCheckState(success ? Qt::Checked : Qt::Unchecked);
        m_updating = false;
        m_status->setText(success
            ? QStringLiteral("Загружен язык: %1").arg(item->data(Qt::UserRole + 1).toString())
            : QStringLiteral("Ошибка загрузки: %1").arg(error));
        if (!success) {
            QMessageBox::warning(this, QStringLiteral("Языки OCR"),
                                 QStringLiteral("Не удалось скачать %1:\n%2").arg(code, error));
        }
        updateBusyState();
    });
}

void TesseractLanguageDialog::updateBusyState()
{
    if (m_activeDownloads <= 0) {
        m_activeDownloads = 0;
        m_progress->setVisible(false);
        m_closeButton->setEnabled(true);
    } else {
        m_closeButton->setEnabled(false);
    }
}
