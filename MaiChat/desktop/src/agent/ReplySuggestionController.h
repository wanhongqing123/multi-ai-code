// 文件作用：桌面端创作中心和 Agent 会话；ReplySuggestionController 负责流程编排、异步回调和状态转换，本文件给出其声明。
// 类型关系：ReplySuggestionController final : public QObject；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 ReplySuggestionController 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 直接依赖：MaiModelClient.h；实现与这些接口协作。
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
