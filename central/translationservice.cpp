#include "central/translationservice.h"

#include "messagetranslator.h"
#include "ollamaclient.h"
#include "ollamadiscovery.h"
#include "protocol/centralprotocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

namespace {

// Several screens of text; a chat message is far shorter.
constexpr qsizetype MaximumTextLength = 32 * 1024;
// Per client, so one cannot keep the Ollama server busy for everyone.
constexpr int MaximumActiveTranslations = 4;
constexpr qint64 MinimumScanIntervalMs = 30 * 1000;
constexpr int AvailabilityCheckIntervalMs = 60 * 1000;

QByteArray failure(const QString &id, const QString &reason) {
  return "CENTRAL_TRANSLATE_FAILED " + id.toUtf8() + ' ' +
         CentralProtocol::encodeField(reason);
}

} // namespace

TranslationService::TranslationService(QObject *parent)
    : QObject(parent), discovery_(new OllamaDiscovery(this)),
      checker_(new OllamaClient(this)) {
  connect(discovery_, &OllamaDiscovery::scanFinished, this,
          &TranslationService::infoChanged);
  checkTimer_.setInterval(AvailabilityCheckIntervalMs);
  connect(&checkTimer_, &QTimer::timeout, checker_,
          &OllamaClient::checkAvailability);
  connect(checker_, &OllamaClient::availabilityChecked, this,
          [this](bool available, const QString &message) {
            if (checked_ && available == available_) return;
            checked_ = true;
            available_ = available;
            emit availabilityChanged(available, message);
            emit infoChanged();
          });
}

void TranslationService::setServer(const QString &url, const QString &model) {
  url_ = url.trimmed();
  while (url_.endsWith(u'/')) url_.chop(1);
  model_ = model.trimmed();
  checked_ = false;
  available_ = false;
  checkTimer_.stop();
  if (!isConfigured()) return;
  checker_->setServer(url_, model_);
  checker_->checkAvailability();
  checkTimer_.start();
}

void TranslationService::setDiscoveryEnabled(bool enabled) {
  discoveryEnabled_ = enabled;
}

bool TranslationService::isConfigured() const { return !url_.isEmpty(); }

bool TranslationService::isAvailable() const {
  return isConfigured() && available_;
}

OllamaDiscovery *TranslationService::discovery() const { return discovery_; }

QByteArray TranslationService::infoLine() const {
  QJsonObject info{{QStringLiteral("configured"), isConfigured()}};
  if (isConfigured()) {
    info.insert(QStringLiteral("available"), isAvailable());
  } else {
    QJsonArray servers;
    for (const auto &server : discovery_->servers()) {
      servers.append(QJsonObject{
          {QStringLiteral("url"), server.url},
          {QStringLiteral("models"), QJsonArray::fromStringList(server.models)}});
    }
    info.insert(QStringLiteral("discovery"), discoveryEnabled_);
    info.insert(QStringLiteral("scanning"), discovery_->isScanning());
    info.insert(QStringLiteral("servers"), servers);
  }
  return "CENTRAL_OLLAMA " +
         QJsonDocument(info).toJson(QJsonDocument::Compact).toBase64(
             QByteArray::Base64UrlEncoding);
}

void TranslationService::scan() {
  if (isConfigured() || !discoveryEnabled_ || discovery_->isScanning()) return;
  if (lastScan_.isValid() && lastScan_.elapsed() < MinimumScanIntervalMs) {
    emit infoChanged();
    return;
  }
  lastScan_.start();
  discovery_->scan();
  emit infoChanged();
}

void TranslationService::translate(QObject *client, const QString &id,
                                   const QString &request,
                                   std::function<void(const QByteArray &)> reply) {
  if (!CentralProtocol::isValidChannelId(id)) return;
  const auto json =
      QJsonDocument::fromJson(QByteArray::fromBase64(
                                  request.toLatin1(), QByteArray::Base64UrlEncoding))
          .object();
  const auto text = json.value(QStringLiteral("text")).toString();
  const auto source = json.value(QStringLiteral("source")).toString();
  const auto target = json.value(QStringLiteral("target")).toString();
  if (text.isEmpty() || text.size() > MaximumTextLength || source.isEmpty() ||
      target.isEmpty()) {
    reply(failure(id, QStringLiteral("The translation request is invalid.")));
    return;
  }
  auto url = url_;
  auto model = model_;
  if (!isConfigured()) {
    url = json.value(QStringLiteral("url")).toString();
    model = json.value(QStringLiteral("model")).toString();
    bool found = false;
    for (const auto &server : discovery_->servers()) {
      found = found || (server.url == url && server.models.contains(model));
    }
    if (!found) {
      reply(failure(id, QStringLiteral("The central host does not know the "
                                       "Ollama server %1 with model %2.")
                            .arg(url, model)));
      return;
    }
  }
  if (activeTranslations_.value(client) >= MaximumActiveTranslations) {
    reply(failure(id, QStringLiteral("Too many translations at once.")));
    return;
  }

  ++activeTranslations_[client];
  auto *translator = new MessageTranslator(this);
  translator->setServer(url, model);
  QPointer<QObject> alive(client);
  const auto finish = [this, translator, client, alive, reply](const QByteArray &line) {
    translator->deleteLater();
    if (--activeTranslations_[client] <= 0) activeTranslations_.remove(client);
    if (alive) reply(line);
  };
  connect(translator, &MessageTranslator::translated, this,
          [id, finish](const QString &translation, const QString &) {
            finish("CENTRAL_TRANSLATED " + id.toUtf8() + ' ' +
                   CentralProtocol::encodeField(translation));
          });
  connect(translator, &MessageTranslator::failed, this,
          [id, finish](const QString &error, const QString &) {
            finish(failure(id, error));
          });
  translator->translate(text, source, target);
}
