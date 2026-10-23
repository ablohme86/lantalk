#pragma once

#include "protocol/quotedmessage.h"

#include <QObject>
#include <QString>

#include <memory>

// Translates a chat message with Ollama while leaving its quotes as they are
// (see QuotedMessage); a quote marked "[quote translate]" is translated too.
// Used like OllamaClient: several messages may be in flight at once, and the
// signals carry the message without translate marks so callers can match it.
class MessageTranslator final : public QObject {
    Q_OBJECT

public:
    explicit MessageTranslator(QObject *parent = nullptr);
    void translate(const QString &message, const QString &sourceLanguage, const QString &targetLanguage);

signals:
    void translated(const QString &translation, const QString &originalMessage);
    void failed(const QString &message, const QString &originalMessage);

private:
    struct Job;
    void step(const std::shared_ptr<Job> &job);
};
