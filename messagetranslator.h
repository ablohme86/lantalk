#pragma once

#include "protocol/quotedmessage.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

// Translates a chat message with Ollama while leaving its quotes as they are
// (see QuotedMessage); a quote marked "[quote translate]" is translated too.
// Used like OllamaClient: several messages may be in flight at once, and the
// signals carry the message without translate marks so callers can match it.
class MessageTranslator final : public QObject {
    Q_OBJECT

public:
    // Translates a whole message somewhere else, e.g. on the central host;
    // `done` gets the translation, or the error with ok false.
    using Remote = std::function<void(const QString &message, const QString &sourceLanguage,
                                      const QString &targetLanguage,
                                      std::function<void(bool ok, const QString &result)> done)>;

    explicit MessageTranslator(QObject *parent = nullptr);
    // While set, every translator sends its messages there instead of to
    // the configured Ollama server; an empty Remote ends that.
    static void setRemote(Remote remote);
    static bool hasRemote();
    // A remote or a configured Ollama server can translate.
    static bool isAvailable();
    // Uses this server and model instead of the ones in the settings.
    void setServer(const QString &url, const QString &model);
    void translate(const QString &message, const QString &sourceLanguage, const QString &targetLanguage);

signals:
    void translated(const QString &translation, const QString &originalMessage);
    void failed(const QString &message, const QString &originalMessage);

private:
    struct Job;
    void step(const std::shared_ptr<Job> &job);

    QString url_;
    QString model_;
};
