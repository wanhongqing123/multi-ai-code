#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

#include "MaiModelClient.h"

struct ReplySuggestionTurn {
  bool outgoing = false;
  QString text;
};

// Generates disposable reply suggestions for a normal MaiChat conversation. It
// does not create a MaiAgent session, persist model history, register tools, or
// send any IM message.
class ReplySuggestionController final : public QObject {
  Q_OBJECT

public:
  struct Config {
    QString baseUrl;
    QString apiKey;
    QString modelName;
  };

  explicit ReplySuggestionController(const Config &config,
                                     QObject *parent = nullptr);
  ReplySuggestionController(std::unique_ptr<MaiModelClient> model,
                            QString modelName, QObject *parent = nullptr);
  ~ReplySuggestionController() override;

  void setConfig(const Config &config);
  quint64 requestSuggestions(const QString &accountId, const QString &peerId,
                             const QString &latestMessageId,
                             const QVector<ReplySuggestionTurn> &recentTurns);
  void cancel();

signals:
  void suggestionsReady(quint64 requestId, const QString &accountId,
                        const QString &peerId, const QString &latestMessageId,
                        const QString &natural, const QString &casual,
                        const QString &professional);
  void suggestionsFailed(quint64 requestId, const QString &accountId,
                         const QString &peerId, const QString &latestMessageId,
                         const QString &message);

private:
  struct Runtime;
  std::unique_ptr<Runtime> runtime_;
};

Q_DECLARE_METATYPE(ReplySuggestionTurn)
Q_DECLARE_METATYPE(QVector<ReplySuggestionTurn>)
