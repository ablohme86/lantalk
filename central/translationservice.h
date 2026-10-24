#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

class OllamaDiscovery;

// Live Translate for web clients, which cannot reach Ollama themselves or
// look for it on the network: lantalkd translates with the Ollama server
// given on its command line, or with one it found on its own network.
//
// CENTRAL_OLLAMA <info> tells clients what is available (base64url JSON:
// {"configured": true} or {"configured": false, "discovery": bool,
// "scanning": bool, "servers": [{"url", "models": [...]}]}).
// CENTRAL_OLLAMA_SCAN asks for a new search. CENTRAL_TRANSLATE <id> <request>
// (base64url JSON: text, source, target, and url and model unless one is
// configured) is answered with CENTRAL_TRANSLATED <id> <text> or
// CENTRAL_TRANSLATE_FAILED <id> <reason>. Only found servers are used, so
// clients cannot make lantalkd contact other addresses.
class TranslationService final : public QObject {
  Q_OBJECT

public:
  explicit TranslationService(QObject *parent = nullptr);

  // Every translation goes to this server and model; nothing is searched.
  void setServer(const QString &url, const QString &model);
  void setDiscoveryEnabled(bool enabled);
  bool isConfigured() const;
  OllamaDiscovery *discovery() const;

  QByteArray infoLine() const;
  // Searches the network unless that happened moments ago.
  void scan();
  // `reply` gets the answer line unless `client` is gone by then.
  void translate(QObject *client, const QString &id, const QString &request,
                 std::function<void(const QByteArray &)> reply);

signals:
  void infoChanged();

private:
  QString url_;
  QString model_;
  bool discoveryEnabled_ = true;
  OllamaDiscovery *discovery_;
  QElapsedTimer lastScan_;
  QHash<QObject *, int> activeTranslations_;
};
