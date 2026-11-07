#include "plugin_settings_dialog.h"

#include "core/localization.h"
#include "ui/down_combo_box.h"

#include "services/provider_interface.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

PluginSettingsDialog::PluginSettingsDialog(IPluginProvider* provider, QWidget* parent)
    : QDialog(parent)
    , m_provider(provider)
    , m_network(new QNetworkAccessManager(this))
{
    setWindowTitle(QStringLiteral("Настройки — %1").arg(
        provider ? provider->manifest().name : QStringLiteral("Сервис")));
    resize(520, 460);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(12);

    auto* title = new QLabel(provider ? provider->manifest().name : QString(), this);
    title->setStyleSheet(QStringLiteral("font-size:16px; font-weight:600;"));
    layout->addWidget(title);

    if (!provider) {
        layout->addWidget(new QLabel(QStringLiteral("Сервис недоступен."), this));
        return;
    }

    if (!provider->manifest().description.isEmpty()) {
        auto* description = new QLabel(provider->manifest().description, this);
        description->setWordWrap(true);
        description->setObjectName(QStringLiteral("FieldHint"));
        layout->addWidget(description);
    }

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* form = new QFormLayout(content);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(8);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    const QJsonArray schema = provider->manifest().settingsSchema;
    for (const QJsonValue& value : schema) {
        const QJsonObject field = value.toObject();
        const QString key = field.value(QStringLiteral("key")).toString();
        if (key.isEmpty())
            continue;
        m_fieldDefinitions.insert(key, field);
        QWidget* editor = createField(field);
        m_fields.insert(key, editor);
        QString label = field.value(QStringLiteral("label")).toString(key);
        if (field.value(QStringLiteral("required")).toBool(false))
            label += QStringLiteral(" *");
        form->addRow(label, editor);
    }

    if (schema.isEmpty()) {
        form->addRow(new QLabel(QStringLiteral("У этого сервиса нет настраиваемых параметров."),
                                content));
    }

    scroll->setWidget(content);
    layout->addWidget(scroll, 1);

    auto* buttons = new QDialogButtonBox(this);
    auto* resetButton = buttons->addButton(QStringLiteral("Сбросить"), QDialogButtonBox::ResetRole);
    buttons->addButton(QDialogButtonBox::Ok);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(resetButton, &QPushButton::clicked, this, &PluginSettingsDialog::resetToDefaults);
    connect(buttons, &QDialogButtonBox::accepted, this, &PluginSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &PluginSettingsDialog::reject);
    layout->addWidget(buttons);

    updateVisibility();
}

QWidget* PluginSettingsDialog::createField(const QJsonObject& field)
{
    const QString key = field.value(QStringLiteral("key")).toString();
    const QString type = field.value(QStringLiteral("type")).toString();
    const QJsonValue current = m_provider->settings().value(key);
    const QJsonValue fallback = m_provider->defaultSettings().value(key);

    if (type == QStringLiteral("boolean")) {
        auto* check = new QCheckBox(this);
        check->setChecked(current.toBool(fallback.toBool(false)));
        connect(check, &QCheckBox::toggled, this, &PluginSettingsDialog::updateVisibility);
        return check;
    }
    if (type == QStringLiteral("models")) {
        auto* host = new QWidget(this);
        auto* layout = new QHBoxLayout(host);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        auto* combo = new DownComboBox(host);
        combo->setMinimumWidth(160);
        const QString currentValue = current.toString(fallback.toString());
        if (!currentValue.isEmpty())
            combo->addItem(currentValue, currentValue);
        auto* refresh = new QPushButton(QStringLiteral("Обновить модели"), host);
        layout->addWidget(combo, 1);
        layout->addWidget(refresh);
        m_modelCombos.insert(key, combo);
        connect(refresh, &QPushButton::clicked, this, [this, key, combo, refresh] {
            refreshModelList(key, combo, refresh, true);
        });
        refreshModelList(key, combo, refresh, false);
        return host;
    }
    if (type == QStringLiteral("xdxf_folders")) {
        auto* combo = new DownComboBox(this);
        combo->setMinimumWidth(220);
        combo->addItem(Localization::tr(QStringLiteral("Все словари")), QString());
        const QDir rootDir(m_provider->manifest().directory);
        QSet<QString> seen;
        QStringList folders;
        QDirIterator iterator(rootDir.absolutePath(), {QStringLiteral("*.xdxf")}, QDir::Files,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString directory = QFileInfo(iterator.next()).absolutePath();
            const QString relative = QDir::cleanPath(rootDir.relativeFilePath(directory));
            if (relative.isEmpty() || relative == QStringLiteral(".") || seen.contains(relative))
                continue;
            seen.insert(relative);
            folders.append(relative);
        }
        folders.sort(Qt::CaseInsensitive);
        for (const QString& folder : folders)
            combo->addItem(folder, folder);
        const QString currentValue = current.toString(fallback.toString());
        int index = combo->findData(currentValue);
        if (index < 0)
            index = 0;
        combo->setCurrentIndex(index);
        return combo;
    }
    if (type == QStringLiteral("select")) {
        auto* combo = new DownComboBox(this);
        const QJsonArray options = field.value(QStringLiteral("options")).toArray();
        for (const QJsonValue& value : options) {
            const QJsonObject option = value.toObject();
            combo->addItem(option.value(QStringLiteral("label")).toString(),
                           option.value(QStringLiteral("value")).toVariant());
        }
        const int index = combo->findData(current.toVariant());
        const int fallbackIndex = combo->findData(fallback.toVariant());
        combo->setCurrentIndex(index >= 0 ? index : qMax(0, fallbackIndex));
        connect(combo, &QComboBox::currentIndexChanged, this, &PluginSettingsDialog::updateVisibility);
        return combo;
    }
    if (type == QStringLiteral("number")) {
        auto* spin = new QDoubleSpinBox(this);
        spin->setRange(-1000000.0, 1000000.0);
        spin->setDecimals(3);
        spin->setValue(current.toDouble(fallback.toDouble(0.0)));
        connect(spin, &QDoubleSpinBox::valueChanged, this, &PluginSettingsDialog::updateVisibility);
        return spin;
    }
    if (type == QStringLiteral("text")) {
        auto* edit = new QPlainTextEdit(this);
        edit->setMinimumHeight(90);
        edit->setPlainText(current.toString(fallback.toString()));
        connect(edit, &QPlainTextEdit::textChanged, this, &PluginSettingsDialog::updateVisibility);
        return edit;
    }

    auto* edit = new QLineEdit(this);
    edit->setText(current.toString(fallback.toString()));
    if (type == QStringLiteral("password"))
        edit->setEchoMode(QLineEdit::Password);
    connect(edit, &QLineEdit::textChanged, this, &PluginSettingsDialog::updateVisibility);
    return edit;
}

QJsonValue PluginSettingsDialog::fieldValue(const QString& key) const
{
    if (QComboBox* modelCombo = m_modelCombos.value(key, nullptr))
        return QJsonValue::fromVariant(modelCombo->currentData());
    QWidget* widget = m_fields.value(key, nullptr);
    if (!widget)
        return QJsonValue();
    if (auto* line = qobject_cast<QLineEdit*>(widget))
        return line->text();
    if (auto* text = qobject_cast<QPlainTextEdit*>(widget))
        return text->toPlainText();
    if (auto* check = qobject_cast<QCheckBox*>(widget))
        return check->isChecked();
    if (auto* combo = qobject_cast<QComboBox*>(widget))
        return QJsonValue::fromVariant(combo->currentData());
    if (auto* spin = qobject_cast<QDoubleSpinBox*>(widget))
        return spin->value();
    return QJsonValue();
}

void PluginSettingsDialog::setFieldValue(const QString& key, const QJsonValue& value)
{
    if (QComboBox* modelCombo = m_modelCombos.value(key, nullptr)) {
        const int index = modelCombo->findData(value.toVariant());
        if (index >= 0)
            modelCombo->setCurrentIndex(index);
        return;
    }
    QWidget* widget = m_fields.value(key, nullptr);
    if (!widget)
        return;
    if (auto* line = qobject_cast<QLineEdit*>(widget))
        line->setText(value.toString());
    else if (auto* text = qobject_cast<QPlainTextEdit*>(widget))
        text->setPlainText(value.toString());
    else if (auto* check = qobject_cast<QCheckBox*>(widget))
        check->setChecked(value.toBool());
    else if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        const int index = combo->findData(value.toVariant());
        if (index >= 0)
            combo->setCurrentIndex(index);
    } else if (auto* spin = qobject_cast<QDoubleSpinBox*>(widget))
        spin->setValue(value.toDouble());
}

void PluginSettingsDialog::updateVisibility()
{
    for (auto it = m_fieldDefinitions.constBegin(); it != m_fieldDefinitions.constEnd(); ++it) {
        const QString condition = it.value().value(QStringLiteral("visible_if")).toString().trimmed();
        QWidget* widget = m_fields.value(it.key(), nullptr);
        if (!widget)
            continue;
        if (condition.isEmpty()) {
            widget->setVisible(true);
            continue;
        }

        const int separator = condition.indexOf(QStringLiteral("=="));
        if (separator < 0) {
            widget->setVisible(true);
            continue;
        }
        const QString dependency = condition.left(separator).trimmed();
        QString expected = condition.mid(separator + 2).trimmed();
        if (expected.startsWith(QLatin1Char('"')) && expected.endsWith(QLatin1Char('"')))
            expected = expected.mid(1, expected.size() - 2);

        const QJsonValue actualValue = fieldValue(dependency);
        QString actual;
        if (actualValue.isBool())
            actual = actualValue.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        else if (actualValue.isDouble())
            actual = QString::number(actualValue.toDouble(), 'g', 12);
        else
            actual = actualValue.toString();

        widget->setVisible(actual.compare(expected, Qt::CaseInsensitive) == 0);
    }
}

void PluginSettingsDialog::refreshModelList(const QString& key, QComboBox* combo,
                                            QPushButton* button, bool notify)
{
    if (!combo || !button || !m_network)
        return;

    const QJsonObject definition = m_fieldDefinitions.value(key);
    const QString endpointKey = definition.value(QStringLiteral("endpoint_key"))
                                    .toString(QStringLiteral("endpoint"));
    QString endpoint = fieldValue(endpointKey).toString().trimmed();
    if (endpoint.isEmpty()) {
        endpoint = definition.value(QStringLiteral("default")).toString();
    }
    if (endpoint.isEmpty())
        return;

    endpoint.remove(QRegularExpression(QStringLiteral("/+$")));
    if (endpoint.endsWith(QStringLiteral("/chat/completions")))
        endpoint.chop(QStringLiteral("/chat/completions").size());
    if (endpoint.endsWith(QStringLiteral("/v1")))
        endpoint += QStringLiteral("/models");
    else
        endpoint += QStringLiteral("/v1/models");

    QNetworkRequest request{QUrl(endpoint)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    const QString apiKey = fieldValue(QStringLiteral("api_key")).toString();
    if (!apiKey.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
    request.setRawHeader("Accept", "application/json");

    button->setEnabled(false);
    const QString previous = combo->currentData().toString();
    QNetworkReply* reply = m_network->get(request);
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    timer->start(12000);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, combo, button, previous, notify] {
        button->setEnabled(true);
        const QByteArray payload = reply->readAll();
        const QString error = reply->error() == QNetworkReply::NoError
            ? QString() : reply->errorString();
        reply->deleteLater();

        QStringList models;
        if (error.isEmpty()) {
            const QJsonDocument document = QJsonDocument::fromJson(payload);
            const QJsonArray items = document.object().value(QStringLiteral("data")).toArray();
            for (const QJsonValue& value : items) {
                const QString id = value.toObject().value(QStringLiteral("id")).toString();
                if (!id.isEmpty() && !models.contains(id))
                    models.append(id);
            }
        }
        models.sort(Qt::CaseInsensitive);

        if (models.isEmpty()) {
            if (notify) {
                QMessageBox::warning(this, QStringLiteral("Модели"),
                    QStringLiteral("Не удалось получить список моделей.\n%1")
                        .arg(error.isEmpty() ? QStringLiteral("Сервер вернул пустой список.") : error));
            }
            return;
        }

        combo->blockSignals(true);
        combo->clear();
        for (const QString& model : models)
            combo->addItem(model, model);
        if (!previous.isEmpty()) {
            const int index = combo->findData(previous);
            if (index >= 0)
                combo->setCurrentIndex(index);
        }
        combo->blockSignals(false);
    });
}
void PluginSettingsDialog::resetToDefaults()
{
    if (!m_provider)
        return;
    const QJsonObject defaults = m_provider->defaultSettings();
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it)
        setFieldValue(it.key(), it.value());
    updateVisibility();
}

void PluginSettingsDialog::accept()
{
    if (!m_provider)
        return;
    if (m_fieldDefinitions.isEmpty()) {
        QDialog::accept();
        return;
    }

    QJsonObject settings;
    for (auto it = m_fieldDefinitions.constBegin(); it != m_fieldDefinitions.constEnd(); ++it) {
        const QJsonObject field = it.value();
        const QJsonValue value = fieldValue(it.key());
        if (field.value(QStringLiteral("required")).toBool(false)) {
            const bool empty = value.isString() ? value.toString().trimmed().isEmpty()
                                                : value.isUndefined() || value.isNull();
            if (empty) {
                QMessageBox::warning(this, QStringLiteral("Настройки"),
                                     QStringLiteral("Заполните обязательное поле «%1».")
                                         .arg(field.value(QStringLiteral("label")).toString(it.key())));
                return;
            }
        }
        settings.insert(it.key(), value);
    }

    m_provider->updateSettings(settings);
    QDialog::accept();
}
