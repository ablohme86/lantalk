#pragma once

#include <QNetworkAccessManager>
#include <QObject>

class OllamaClient final : public QObject {
    Q_OBJECT

public:
    explicit OllamaClient(QObject *parent = nullptr);
    void translate(const QString &text, const QString &sourceLanguage, const QString &targetLanguage);
    // Checks that the configured server answers and has the configured model.
    void checkAvailability();

signals:
    void translated(const QString &translation, const QString &originalMessage);
    void failed(const QString &message, const QString &originalMessage);
    void availabilityChecked(bool available, const QString &message);

private:
    QNetworkAccessManager manager_;
};
