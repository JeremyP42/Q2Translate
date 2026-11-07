#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QHash>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QNetworkAccessManager;
class QPlainTextEdit;
class IPluginProvider;
class QWidget;

class PluginSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PluginSettingsDialog(IPluginProvider* provider, QWidget* parent = nullptr);

private:
    QWidget* createField(const QJsonObject& field);
    QJsonValue fieldValue(const QString& key) const;
    void setFieldValue(const QString& key, const QJsonValue& value);
    void updateVisibility();
    void refreshModelList(const QString& key, QComboBox* combo, QPushButton* button,
                          bool notify = true);
    void resetToDefaults();
    void accept() override;

    IPluginProvider* m_provider = nullptr;
    QHash<QString, QWidget*> m_fields;
    QHash<QString, QJsonObject> m_fieldDefinitions;
    QHash<QString, QComboBox*> m_modelCombos;
    QNetworkAccessManager* m_network = nullptr;
};
