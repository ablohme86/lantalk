#include "translationrestorer.h"

#include "messagetranslator.h"

#include "appconfig.h"
#include "protocol/chatsession.h"
#include "protocol/conversationidentity.h"

#include <QSettings>
#include <QVariantMap>

namespace {

QString settingsKey(const QString &address, const QString &peerName)
{
    const auto digest = ConversationIdentity::digest(address, peerName);
    return digest.isEmpty() ? QString() : QStringLiteral("liveTranslate/%1").arg(digest);
}

} // namespace

TranslationSettings::Preference TranslationSettings::load(const QString &address, const QString &peerName)
{
    const auto key = settingsKey(address, peerName);
    if (key.isEmpty()) return {};
    const auto stored = QSettings().value(key).toMap();
    Preference preference{stored.value(QStringLiteral("enabled")).toBool(),
                          stored.value(QStringLiteral("myLanguage")).toString(),
                          stored.value(QStringLiteral("peerLanguage")).toString()};
    const auto available = languages();
    if (!available.contains(preference.myLanguage) || !available.contains(preference.peerLanguage)) {
        return {};
    }
    return preference;
}

void TranslationSettings::save(const QString &address, const QString &peerName,
                               const Preference &preference)
{
    const auto key = settingsKey(address, peerName);
    if (key.isEmpty()) return;
    QVariantMap stored;
    stored.insert(QStringLiteral("enabled"), preference.enabled);
    stored.insert(QStringLiteral("myLanguage"), preference.myLanguage);
    stored.insert(QStringLiteral("peerLanguage"), preference.peerLanguage);
    QSettings().setValue(key, stored);
}

TranslationRestorer::TranslationRestorer(ChatSession *session, QObject *parent)
    : QObject(parent), session_(session)
{
    retryTimer_.setSingleShot(true);
    retryTimer_.setInterval(TranslationResumeRetryIntervalMs);
    connect(&retryTimer_, &QTimer::timeout, this, &TranslationRestorer::attemptResume);
    connect(&ollama_, &OllamaClient::availabilityChecked, this, &TranslationRestorer::availabilityChecked);
    connect(session_, &ChatSession::peerIdentified, this, &TranslationRestorer::attemptResume);
    connect(session_, &ChatSession::disconnected, this, [this] { retryTimer_.stop(); });
    connect(session_, &ChatSession::liveTranslationGranted, this, [this] {
        if (requestedMyLanguage_.isEmpty() || requestedPeerLanguage_.isEmpty()) return;
        TranslationSettings::save(session_->peerAddress(), session_->identityName(),
                                  {true, requestedMyLanguage_, requestedPeerLanguage_});
    });
}

void TranslationRestorer::request(const QString &myLanguage, const QString &peerLanguage)
{
    retryTimer_.stop();
    requestedMyLanguage_ = myLanguage;
    requestedPeerLanguage_ = peerLanguage;
    session_->requestLiveTranslation(myLanguage, peerLanguage);
}

void TranslationRestorer::disable()
{
    retryTimer_.stop();
    session_->stopLiveTranslation();
    auto preference = savedPreference();
    if (!preference.enabled) return;
    preference.enabled = false;
    TranslationSettings::save(session_->peerAddress(), session_->identityName(), preference);
}

void TranslationRestorer::retry()
{
    attemptResume();
}

TranslationSettings::Preference TranslationRestorer::savedPreference() const
{
    return TranslationSettings::load(session_->peerAddress(), session_->identityName());
}

void TranslationRestorer::setRetryInterval(int milliseconds)
{
    retryTimer_.setInterval(milliseconds);
}

bool TranslationRestorer::canResume() const
{
    return session_->isHandshakeComplete() && session_->isConnected()
           && !session_->liveTranslationActiveOrPending() && savedPreference().enabled;
}

void TranslationRestorer::attemptResume()
{
    if (!canResume()) {
        retryTimer_.stop();
        return;
    }
    if (MessageTranslator::hasRemote()) {
        // The central host checks its own Ollama server.
        availabilityChecked(true, {});
        return;
    }
    if (!isOllamaConfigured()) {
        // retry() is called when the Ollama settings change.
        emit statusChanged(QStringLiteral("Live Translate will resume once Ollama is configured."));
        return;
    }
    if (checkingAvailability_) return;
    checkingAvailability_ = true;
    ollama_.checkAvailability();
}

void TranslationRestorer::availabilityChecked(bool available, const QString &message)
{
    checkingAvailability_ = false;
    if (!canResume()) return;
    if (!available) {
        emit statusChanged(QStringLiteral("%1 Live Translate will resume automatically.").arg(message));
        retryTimer_.start();
        return;
    }
    const auto preference = savedPreference();
    requestedMyLanguage_ = preference.myLanguage;
    requestedPeerLanguage_ = preference.peerLanguage;
    emit resumeRequested(preference.myLanguage, preference.peerLanguage);
    session_->requestLiveTranslation(preference.myLanguage, preference.peerLanguage);
}
