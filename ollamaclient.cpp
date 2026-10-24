#include "ollamaclient.h"

#include "appconfig.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

QString withoutLatestTag(const QString &model)
{
    return model.endsWith(QStringLiteral(":latest")) ? model.chopped(7) : model;
}

} // namespace

OllamaClient::OllamaClient(QObject *parent) : QObject(parent) {}

void OllamaClient::setServer(const QString &url, const QString &model)
{
    url_ = url;
    model_ = model;
}

void OllamaClient::checkAvailability()
{
    QSettings settings;
    const auto endpoint = settings.value(QStringLiteral("ollama/url"), DefaultOllamaUrl).toString().trimmed();
    const auto model = settings.value(QStringLiteral("ollama/model"), DefaultOllamaModel).toString().trimmed();
    if (endpoint.isEmpty() || model.isEmpty()) {
        emit availabilityChecked(false, QStringLiteral("Ollama is not configured."));
        return;
    }

    QNetworkRequest request(QUrl(endpoint + QStringLiteral("/api/tags")));
    request.setTransferTimeout(5000);
    auto *reply = manager_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, model] {
        const auto error = reply->error();
        const auto errorText = reply->errorString();
        const auto body = reply->readAll();
        reply->deleteLater();
        if (error != QNetworkReply::NoError) {
            emit availabilityChecked(false, QStringLiteral("Ollama is unreachable: %1").arg(errorText));
            return;
        }
        const auto models = QJsonDocument::fromJson(body).object().value(QStringLiteral("models")).toArray();
        const auto wanted = withoutLatestTag(model);
        for (const auto &entry : models) {
            const auto object = entry.toObject();
            if (withoutLatestTag(object.value(QStringLiteral("name")).toString()) == wanted
                || withoutLatestTag(object.value(QStringLiteral("model")).toString()) == wanted) {
                emit availabilityChecked(true, {});
                return;
            }
        }
        emit availabilityChecked(false, QStringLiteral("Ollama model %1 is not installed.").arg(model));
    });
}

void OllamaClient::translate(const QString &text, const QString &sourceLanguage, const QString &targetLanguage)
{
    QSettings settings;
    const auto endpoint = url_.isEmpty()
        ? settings.value(QStringLiteral("ollama/url"), DefaultOllamaUrl).toString() : url_;
    auto model = url_.isEmpty()
        ? settings.value(QStringLiteral("ollama/model"), DefaultOllamaModel).toString() : model_;
    if (model == QStringLiteral("translategemma")) {
        model = QString::fromLatin1(DefaultOllamaModel);
    }
    if (endpoint.trimmed().isEmpty() || model.trimmed().isEmpty()) {
        emit failed(QStringLiteral("Configure an Ollama URL and model in Settings before using Live Translate."), text);
        return;
    }

    const QString prompt = QStringLiteral(
        "You are a professional %1 to %2 translator.\n\n"
        "Translate the message naturally, preserving the casual tone and all technical "
        "identifiers, code, URLs, and product names exactly as written.\n\n"
        "Output only the translation, without explanations.\n\n"
        "Translate this message:\n\n%3")
                               .arg(sourceLanguage, targetLanguage, text);
    QJsonObject payload{{QStringLiteral("model"), model}, {QStringLiteral("prompt"), prompt},
                        {QStringLiteral("stream"), false}};
    QNetworkRequest request(QUrl(endpoint + QStringLiteral("/api/generate")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    auto *reply = manager_.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, text] {
        const auto error = reply->error();
        const auto errorText = reply->errorString();
        const auto body = reply->readAll();
        reply->deleteLater();
        if (error != QNetworkReply::NoError) {
            const auto serverError = QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toString();
            emit failed(QStringLiteral("Ollama request failed: %1").arg(
                serverError.isEmpty() ? errorText : serverError), text);
            return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(body, &parseError);
        const auto translation = document.object().value(QStringLiteral("response")).toString();
        if (parseError.error != QJsonParseError::NoError || translation.isEmpty()) {
            emit failed(QStringLiteral("Ollama returned an invalid translation response."), text);
            return;
        }
        emit translated(translation.trimmed(), text);
    });
}
