#include "agent/ReplySuggestionController.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "MaiOpenAiClient.h"

namespace {

std::string toUtf8(const QString &value) {
  const QByteArray bytes = value.toUtf8();
  return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString fromUtf8(const std::string &value) {
  return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

QString cleanedJson(QString response) {
  response = response.trimmed();
  if (!response.startsWith(QStringLiteral("```")))
    return response;
  const int firstLineEnd = response.indexOf(QLatin1Char('\n'));
  const int closingFence = response.lastIndexOf(QStringLiteral("```"));
  if (firstLineEnd < 0 || closingFence <= firstLineEnd)
    return response;
  return response.mid(firstLineEnd + 1, closingFence - firstLineEnd - 1)
      .trimmed();
}

bool parseSuggestions(const QString &response, QString *natural,
                      QString *casual, QString *professional, QString *error) {
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(cleanedJson(response).toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    if (error)
      *error = QStringLiteral("模型没有返回可解析的回复建议，请重试。");
    return false;
  }
  const QJsonObject object = document.object();
  const QString parsedNatural =
      object.value(QStringLiteral("natural")).toString().trimmed();
  const QString parsedCasual =
      object.value(QStringLiteral("casual")).toString().trimmed();
  const QString parsedProfessional =
      object.value(QStringLiteral("professional")).toString().trimmed();
  if (parsedNatural.isEmpty() || parsedCasual.isEmpty() ||
      parsedProfessional.isEmpty()) {
    if (error)
      *error = QStringLiteral("模型返回的三种回复不完整，请重试。");
    return false;
  }
  constexpr int kMaxSuggestionLength = 240;
  if (natural)
    *natural = parsedNatural.left(kMaxSuggestionLength);
  if (casual)
    *casual = parsedCasual.left(kMaxSuggestionLength);
  if (professional)
    *professional = parsedProfessional.left(kMaxSuggestionLength);
  return true;
}

MaiModelRequest makeRequest(const QString &modelName,
                            const QVector<ReplySuggestionTurn> &turns) {
  QJsonArray conversation;
  for (const ReplySuggestionTurn &turn : turns) {
    const QString text = turn.text.trimmed();
    if (text.isEmpty())
      continue;
    conversation.append(QJsonObject{
        {QStringLiteral("speaker"),
         turn.outgoing ? QStringLiteral("me") : QStringLiteral("friend")},
        {QStringLiteral("text"), text.left(4000)},
    });
  }
  const QByteArray context =
      QJsonDocument(conversation).toJson(QJsonDocument::Compact);

  MaiModelRequest request;
  request.model = toUtf8(modelName);
  request.temperature = 0.5;
  request.baseInstructions =
      "You generate reply suggestions for a private chat. Return only one JSON "
      "object with "
      "exactly these string fields: natural, casual, professional. Each value "
      "must be a short "
      "reply in the language used by the conversation. The natural reply "
      "should sound neutral, "
      "the casual reply relaxed, and the professional reply suitable for work. "
      "Do not include "
      "Markdown fences, explanations, or any action that sends a message.";
  MaiModelMessage message;
  message.role = MaiModelRole::User;
  message.content = "Recent conversation, oldest first:\n" +
                    toUtf8(QString::fromUtf8(context));
  request.messages.push_back(std::move(message));
  return request;
}

} // namespace

struct ReplySuggestionController::Runtime {
  std::unique_ptr<MaiModelClient> model;
  QString modelName;
  std::thread worker;
  std::atomic<bool> cancel{false};
  std::atomic<quint64> nextRequestId{0};

  void stopAndJoin() {
    cancel.store(true, std::memory_order_relaxed);
    if (worker.joinable())
      worker.join();
  }
};

ReplySuggestionController::ReplySuggestionController(const Config &config,
                                                     QObject *parent)
    : QObject(parent), runtime_(std::make_unique<Runtime>()) {
  setConfig(config);
}

ReplySuggestionController::ReplySuggestionController(
    std::unique_ptr<MaiModelClient> model, QString modelName, QObject *parent)
    : QObject(parent), runtime_(std::make_unique<Runtime>()) {
  runtime_->model = std::move(model);
  runtime_->modelName = std::move(modelName);
}

ReplySuggestionController::~ReplySuggestionController() {
  runtime_->stopAndJoin();
}

void ReplySuggestionController::setConfig(const Config &config) {
  runtime_->stopAndJoin();
  runtime_->model.reset();
  runtime_->modelName = config.modelName;
  if (config.baseUrl.trimmed().isEmpty())
    return;
  MaiModelConfig modelConfig;
  modelConfig.baseUrl = toUtf8(config.baseUrl.trimmed());
  modelConfig.apiKey = toUtf8(config.apiKey);
  modelConfig.wire = MaiWireApi::Responses;
  runtime_->model = makeMaiModelClient(std::move(modelConfig));
}

quint64 ReplySuggestionController::requestSuggestions(
    const QString &accountId, const QString &peerId,
    const QString &latestMessageId,
    const QVector<ReplySuggestionTurn> &recentTurns) {
  runtime_->stopAndJoin();
  runtime_->cancel.store(false, std::memory_order_relaxed);
  const quint64 requestId = runtime_->nextRequestId.fetch_add(1) + 1;
  QPointer<ReplySuggestionController> guard(this);
  Runtime *runtime = runtime_.get();
  runtime_->worker = std::thread([guard, runtime, requestId, accountId, peerId,
                                  latestMessageId, recentTurns] {
    if (!runtime->model) {
      QMetaObject::invokeMethod(
          guard.data(),
          [guard, requestId, accountId, peerId, latestMessageId] {
            if (guard)
              emit guard->suggestionsFailed(
                  requestId, accountId, peerId, latestMessageId,
                  QStringLiteral("请先连接云端模型服务。"));
          },
          Qt::QueuedConnection);
      return;
    }

    std::string response;
    MaiStreamSink sink;
    sink.onText = [&](std::string_view delta) {
      response.append(delta.data(), delta.size());
    };
    const MaiError modelError = runtime->model->stream(
        makeRequest(runtime->modelName, recentTurns), sink, runtime->cancel);
    if (runtime->cancel.load(std::memory_order_relaxed) ||
        modelError.code() == MaiErrorCode::Canceled) {
      return;
    }

    QString natural;
    QString casual;
    QString professional;
    QString parseError;
    const bool parsed =
        !modelError && parseSuggestions(fromUtf8(response), &natural, &casual,
                                        &professional, &parseError);
    const QString failure =
        modelError ? fromUtf8(modelError.message()) : parseError;
    QMetaObject::invokeMethod(
        guard.data(),
        [guard, requestId, accountId, peerId, latestMessageId, parsed, natural,
         casual, professional, failure] {
          if (!guard)
            return;
          if (parsed) {
            emit guard->suggestionsReady(requestId, accountId, peerId,
                                         latestMessageId, natural, casual,
                                         professional);
          } else {
            emit guard->suggestionsFailed(requestId, accountId, peerId,
                                          latestMessageId, failure);
          }
        },
        Qt::QueuedConnection);
  });
  return requestId;
}

void ReplySuggestionController::cancel() {
  runtime_->cancel.store(true, std::memory_order_relaxed);
}
