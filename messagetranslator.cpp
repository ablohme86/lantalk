#include "messagetranslator.h"

#include "appconfig.h"
#include "ollamaclient.h"

#include <QPointer>

#include <memory>

namespace {

MessageTranslator::Remote &remoteTranslator()
{
    static MessageTranslator::Remote remote;
    return remote;
}

bool needsTranslation(const QuotedMessage::Segment &segment)
{
    return (!segment.quote || segment.translate) && !segment.text.trimmed().isEmpty();
}

} // namespace

// One message: its segments are translated one after another by a client of
// its own, so replies cannot be confused with another message's. The client's
// connections hold the job, so deleting the client releases it.
struct MessageTranslator::Job {
    QList<QuotedMessage::Segment> segments;
    QString original;
    QString sourceLanguage;
    QString targetLanguage;
    int next = 0;
    OllamaClient *client = nullptr;
};

MessageTranslator::MessageTranslator(QObject *parent) : QObject(parent) {}

void MessageTranslator::setRemote(Remote remote)
{
    remoteTranslator() = std::move(remote);
}

bool MessageTranslator::hasRemote()
{
    return bool(remoteTranslator());
}

bool MessageTranslator::isAvailable()
{
    return hasRemote() || isOllamaConfigured();
}

void MessageTranslator::setServer(const QString &url, const QString &model)
{
    url_ = url;
    model_ = model;
}

void MessageTranslator::translate(const QString &message, const QString &sourceLanguage,
                                  const QString &targetLanguage)
{
    if (hasRemote() && url_.isEmpty()) {
        // The remote handles quotes itself, as this class does below.
        const auto original = QuotedMessage::withoutTranslateFlags(message);
        QPointer<MessageTranslator> self(this);
        remoteTranslator()(message, sourceLanguage, targetLanguage,
                           [self, original](bool ok, const QString &result) {
                               if (!self) return;
                               if (ok) emit self->translated(result, original);
                               else emit self->failed(result, original);
                           });
        return;
    }
    auto job = std::make_shared<Job>();
    job->segments = QuotedMessage::parse(message);
    job->original = QuotedMessage::withoutTranslateFlags(message);
    job->sourceLanguage = sourceLanguage;
    job->targetLanguage = targetLanguage;
    job->client = new OllamaClient(this);
    if (!url_.isEmpty()) job->client->setServer(url_, model_);
    connect(job->client, &OllamaClient::translated, this, [this, job](const QString &translation, const QString &) {
        job->segments[job->next].text = translation;
        ++job->next;
        step(job);
    });
    connect(job->client, &OllamaClient::failed, this, [this, job](const QString &error, const QString &) {
        job->client->deleteLater();
        emit failed(error, job->original);
    });
    // Always asynchronous, like OllamaClient, even when nothing needs Ollama.
    QMetaObject::invokeMethod(this, [this, job] { step(job); }, Qt::QueuedConnection);
}

void MessageTranslator::step(const std::shared_ptr<Job> &job)
{
    while (job->next < job->segments.size() && !needsTranslation(job->segments.at(job->next))) {
        ++job->next;
    }
    if (job->next < job->segments.size()) {
        job->client->translate(job->segments.at(job->next).text, job->sourceLanguage, job->targetLanguage);
        return;
    }
    job->client->deleteLater();
    emit translated(QuotedMessage::compose(job->segments, false), job->original);
}
