#pragma once

#include <QNetworkAccessManager>
#include <QObject>

class OllamaClient final : public QObject {
    Q_OBJECT

public:
    explicit OllamaClient(QObject *parent = nullptr);
    // Uses this server and model instead of the ones in the settings.
    void setServer(const QString &url, const QString &model);
    void translate(const QString &text, const QString &sourceLanguage, const QString &targetLanguage);
    // Checks that the configured server answers and has the configured model.
    void checkAvailability();

signals:
    void translated(const QString &translation, const QString &originalMessage);
    void failed(const QString &message, const QString &originalMessage);
    void availabilityChecked(bool available, const QString &message);

private:
    QNetworkAccessManager manager_;
    QString url_;
    QString model_;
};
