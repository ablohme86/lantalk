#pragma once

#include "ollamaclient.h"

#include <QObject>
#include <QString>
#include <QTimer>

class ChatSession;

namespace TranslationSettings {

struct Preference {
    bool enabled = false;
    QString myLanguage;
    QString peerLanguage;
};

// Preferences are stored per conversation (peer address + nickname), so each
// tab keeps its own languages across reconnects and application restarts.
Preference load(const QString &address, const QString &peerName);
void save(const QString &address, const QString &peerName, const Preference &preference);

} // namespace TranslationSettings

// Remembers a conversation's Live Translate languages and turns translation
// back on whenever the conversation (re)connects, as soon as the configured
// Ollama server is reachable and has the configured model.
class TranslationRestorer final : public QObject {
    Q_OBJECT

public:
    explicit TranslationRestorer(ChatSession *session, QObject *parent = nullptr);
    // User-initiated changes; the preference is saved once the peer grants it.
    void request(const QString &myLanguage, const QString &peerLanguage);
    void disable();
    // Re-evaluates the saved preference, e.g. after Ollama settings change.
    void retry();
    TranslationSettings::Preference savedPreference() const;
    void setRetryInterval(int milliseconds);

signals:
    // Emitted right before an automatic request so the UI can adopt the languages.
    void resumeRequested(const QString &myLanguage, const QString &peerLanguage);
    void statusChanged(const QString &message);

private:
    bool canResume() const;
    void attemptResume();
    void availabilityChecked(bool available, const QString &message);

    ChatSession *session_;
    OllamaClient ollama_;
    QTimer retryTimer_;
    QString requestedMyLanguage_;
    QString requestedPeerLanguage_;
    bool checkingAvailability_ = false;
};
