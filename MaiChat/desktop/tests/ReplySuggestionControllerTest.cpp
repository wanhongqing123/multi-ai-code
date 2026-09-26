#include <QSignalSpy>
#include <QtTest>

#include <atomic>
#include <memory>
#include <mutex>

#include "agent/ReplySuggestionController.h"

namespace {

class ReplyModel final : public MaiModelClient {
public:
  explicit ReplyModel(std::string response) : response_(std::move(response)) {}

  MaiError stream(const MaiModelRequest &request, const MaiStreamSink &sink,
                  const std::atomic<bool> &cancel) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      request_ = request;
    }
    if (!cancel.load() && sink.onText)
      sink.onText(response_);
    return {};
  }

  MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }

  MaiModelRequest request() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return request_;
  }

private:
  std::string response_;
  mutable std::mutex mutex_;
  MaiModelRequest request_;
};

} // namespace

class ReplySuggestionControllerTest : public QObject {
  Q_OBJECT

private slots:
  void returnsThreeStylesFromATransientToolFreeRequest();
  void rejectsMalformedModelOutput();
};

void ReplySuggestionControllerTest::
    returnsThreeStylesFromATransientToolFreeRequest() {
  auto model = std::make_unique<ReplyModel>(
      R"({"natural":"好的，我晚点看","casual":"收到，马上安排 😄","professional":"已收到，我会在今天完成审核。"})");
  ReplyModel *probe = model.get();
  ReplySuggestionController controller(std::move(model),
                                       QStringLiteral("glm-5.3"));
  QSignalSpy ready(&controller, &ReplySuggestionController::suggestionsReady);

  const quint64 requestId = controller.requestSuggestions(
      QStringLiteral("owner"), QStringLiteral("alice"),
      QStringLiteral("message-2"),
      {{false, QStringLiteral("这份文档今天能审核吗？")},
       {true, QStringLiteral("我先看一下")}});
  QVERIFY(requestId > 0);
  QVERIFY(ready.wait(5000));

  const QList<QVariant> event = ready.takeFirst();
  QCOMPARE(event.at(0).toULongLong(), requestId);
  QCOMPARE(event.at(1).toString(), QStringLiteral("owner"));
  QCOMPARE(event.at(2).toString(), QStringLiteral("alice"));
  QCOMPARE(event.at(3).toString(), QStringLiteral("message-2"));
  QCOMPARE(event.at(4).toString(), QStringLiteral("好的，我晚点看"));
  QCOMPARE(event.at(5).toString(), QStringLiteral("收到，马上安排 😄"));
  QCOMPARE(event.at(6).toString(),
           QStringLiteral("已收到，我会在今天完成审核。"));

  const MaiModelRequest request = probe->request();
  QVERIFY(request.tools.empty());
  QCOMPARE(request.messages.size(), std::size_t(1));
  QCOMPARE(request.messages.front().role, MaiModelRole::User);
  QVERIFY(QString::fromStdString(request.messages.front().content)
              .contains(QStringLiteral("这份文档今天能审核吗？")));
}

void ReplySuggestionControllerTest::rejectsMalformedModelOutput() {
  ReplySuggestionController controller(std::make_unique<ReplyModel>("not json"),
                                       QStringLiteral("glm-5.3"));
  QSignalSpy failed(&controller, &ReplySuggestionController::suggestionsFailed);
  controller.requestSuggestions(
      QStringLiteral("owner"), QStringLiteral("alice"),
      QStringLiteral("message-1"), {{false, QStringLiteral("hello")}});
  QVERIFY(failed.wait(5000));
  QVERIFY(!failed.first().at(4).toString().isEmpty());
}

QTEST_MAIN(ReplySuggestionControllerTest)
#include "ReplySuggestionControllerTest.moc"
