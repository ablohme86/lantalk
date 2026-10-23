#pragma once

#include <QNetworkAccessManager>
#include <QObject>

class OllamaClient final : public QObject {
    Q_OBJECT

public:
    explicit OllamaClient(QObject *parent = nullptr);
    void translate(const QString &text, const QString &sourceLanguage, const QString &targetLanguage);

signals:
    void translated(const QString &translation, const QString &originalMessage);
    void failed(const QString &message);

private:
    QNetworkAccessManager manager_;
};
