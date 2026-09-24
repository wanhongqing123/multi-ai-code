#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "agent/AgentController.h"
#include "agent/DesktopScreenshotTool.h"

// AgentController 的活儿只有一件：把 MaiAgent 的事件从**核心的线程**搬到 Qt 主线程。
// 所以这份用例的重点不是"信号发出来了没有"，而是"它们到达的时候人在哪个线程上"。
//
// 光断言"收到了信号"是验不到东西的：即使一行排队代码都没有、直接在网络线程上 emit，
// QSignalSpy 照样能记到（它就是个槽）。必须比对线程 id 才算数。
namespace {

// 把一段 UTF-8 切成一个个完整字符。首字节的高位决定这个字符占几个字节。
std::vector<std::string_view> splitUtf8(const std::string& text) {
    std::vector<std::string_view> pieces;
    const std::string_view view(text);
    for (std::size_t i = 0; i < view.size();) {
        const unsigned char lead = static_cast<unsigned char>(view[i]);
        std::size_t width = 1;
        if ((lead & 0xE0u) == 0xC0u) {
            width = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            width = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            width = 4;
        }
        width = std::min(width, view.size() - i);
        pieces.push_back(view.substr(i, width));
        i += width;
    }
    return pieces;
}

// 一个最小的模型实现。直接实现公开头里的 MaiModelClient 接口，
// 不依赖 MaiAgent 的测试支持库——这样验的正好是"第三方怎么嵌这个库"。
class ScriptedModel final : public MaiModelClient {
public:
    ScriptedModel(std::string reasoning, std::string text)
        : reasoning_(std::move(reasoning)), text_(std::move(text)) {}

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        {
            std::lock_guard<std::mutex> lock(requestMutex_);
            lastRequest_ = request;
        }
        // 记下这一轮跑在哪个线程上。待会儿要和槽里看到的线程比对。
        workerThread_.store(QThread::currentThreadId(), std::memory_order_relaxed);
        requests_.fetch_add(1, std::memory_order_relaxed);

        // 一个字符一个字符吐，**按 UTF-8 的字符边界切，不按字节**。
        //
        // 这不是图省事：MaiStreamSink 的契约要求每次交付都是完整的 UTF-8。
        // 第一版这里写的是 substr(i, 1)，按字节切，
        // 结果中文全成了问号——而真实的客户端不会这样（MaiOpenAiClient 交出去的是解析完的 JSON 字符
        // 串值），所以那是假模型在造一个现实中不存在的输入。
        if (sink.onReasoning) {
            for (const std::string_view piece : splitUtf8(reasoning_)) {
                if (cancel.load(std::memory_order_relaxed)) return {};
                sink.onReasoning(piece);
            }
        }
        if (sink.onText) {
            for (const std::string_view piece : splitUtf8(text_)) {
                if (cancel.load(std::memory_order_relaxed)) return {};
                sink.onText(piece);
            }
        }
        return {};
    }

    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }

    Qt::HANDLE workerThread() const {
        return workerThread_.load(std::memory_order_relaxed);
    }
    int requests() const {
        return requests_.load(std::memory_order_relaxed);
    }
    MaiModelRequest lastRequest() const {
        std::lock_guard<std::mutex> lock(requestMutex_);
        return lastRequest_;
    }

private:
    std::string reasoning_;
    std::string text_;
    std::atomic<Qt::HANDLE> workerThread_{nullptr};
    std::atomic<int> requests_{0};
    mutable std::mutex requestMutex_;
    MaiModelRequest lastRequest_;
};

}  // namespace

class AgentControllerTest : public QObject {
    Q_OBJECT

private slots:
    void streams_on_the_main_thread();
    void separates_reasoning_from_text();
    void reports_a_missing_model_instead_of_hanging();
    void sends_selected_images_as_multimodal_input();
    void registers_a_screenshot_tool_that_requires_approval();
};

// 载荷不是文案：中文 + emoji，验证 UTF-8 一路（回调 -> 事件 -> QString）不走样。
// 合起来是 "你好，我在主线程上 🙂"
static const char kText[] =
    "你好，我在主线程上 \xF0\x9F\x99\x82";
static const char kDraft[] = "the user said hi; answer briefly";

void AgentControllerTest::streams_on_the_main_thread() {
    auto model = std::make_unique<ScriptedModel>(std::string(kDraft), std::string(kText));
    ScriptedModel* scripted = model.get();

    AgentController controller(std::move(model), QString());
    const QString sessionId = controller.createSession(QDir::currentPath());
    QVERIFY(!sessionId.isEmpty());

    // 记下每一条增量到达时所在的线程。
    //
    // **这里必须显式 DirectConnection。** 用默认的 Auto 会让这条检查变成假的：
    // Auto 自己就会在发射线程和接收线程不同的时候排队到主线程，
    // 于是不管 AgentController 有没有做排队，
    // lambda 都在主线程上跑——量到的是**Qt 替测试做的那一跳**，不是被测对象做的那一跳。
    //
    // 这不是推测：第一版就是 Auto，
    // 把 AgentController 里的 QueuedConnection 改成 DirectConnection 之后用例照样全绿。
    // 改成 Direct 之后才分得开。
    QString assembled;
    QVector<Qt::HANDLE> deliveryThreads;
    connect(&controller, &AgentController::textDelta, this,
            [&](const QString&, const QString&, const QString&, const QString& delta) {
                assembled += delta;
                deliveryThreads.append(QThread::currentThreadId());
            },
            Qt::DirectConnection);

    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("hi")));

    // sendPrompt 是异步的：这里必须转事件循环，否则排队的事件永远投递不出去。
    QVERIFY(finished.wait(15000));

    QCOMPARE(assembled, QString::fromUtf8(kText));

    // ---- 这一条才是这个类存在的理由 ----核心那一轮跑在自己的线程上，
    // 而每一条增量都必须在**主线程**上到达。
    QVERIFY(!deliveryThreads.isEmpty());
    const Qt::HANDLE mainThread = QThread::currentThreadId();
    for (Qt::HANDLE seen : deliveryThreads) {
        QCOMPARE(seen, mainThread);
    }
    // 而且两者确实**不是同一个线程**——否则上面那条断言恒真，等于什么都没验。
    QVERIFY(scripted->workerThread() != nullptr);
    QVERIFY(scripted->workerThread() != mainThread);
}

void AgentControllerTest::separates_reasoning_from_text() {
    auto model = std::make_unique<ScriptedModel>(std::string(kDraft), std::string(kText));
    AgentController controller(std::move(model), QString());
    const QString sessionId = controller.createSession(QDir::currentPath());

    QString text;
    QString reasoning;
    connect(&controller, &AgentController::textDelta, this,
            [&](const QString&, const QString&, const QString&, const QString& d) { text += d; });
    connect(&controller, &AgentController::reasoningDelta, this,
            [&](const QString&, const QString&, const QString&, const QString& d) {
                reasoning += d;
            });

    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("hi")));
    QVERIFY(finished.wait(15000));

    // 思考过程是模型的草稿。混进正文的话，
    // 界面上就是一段自言自语接着真答案——MaiAgent 那边真踩过这个，
    // 根因是事件广播早于落库导致订阅方分不出片段类型。
    QCOMPARE(text, QString::fromUtf8(kText));
    QCOMPARE(reasoning, QString::fromUtf8(kDraft));
}

void AgentControllerTest::reports_a_missing_model_instead_of_hanging() {
    // 没配模型时（界面刚装好、还没填 key）：建会话照常，发消息要**当场**拿到错误，
    // 不能挂在那儿等一个永远不会来的回答。
    AgentController::ModelConfig empty;
    AgentController controller(empty, QString());

    const QString sessionId = controller.createSession(QDir::currentPath());
    QVERIFY(!sessionId.isEmpty());

    QSignalSpy failed(&controller, &AgentController::turnFailed);
    controller.sendPrompt(sessionId, QStringLiteral("hi"));
    QVERIFY(failed.wait(10000));
    QVERIFY(!failed.first().at(1).toString().isEmpty());
}

void AgentControllerTest::sends_selected_images_as_multimodal_input() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("photo.png"));
    QFile image(imagePath);
    QVERIFY(image.open(QIODevice::WriteOnly));
    QCOMPARE(image.write("\x89PNG", 4), 4);
    image.close();

    auto model = std::make_unique<ScriptedModel>(std::string(), std::string(kText));
    ScriptedModel* scripted = model.get();
    AgentController controller(std::move(model), QString());
    const QString sessionId = controller.createSession(directory.path());
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("看图"), {imagePath}));
    QVERIFY(finished.wait(15000));

    const MaiModelRequest request = scripted->lastRequest();
    QCOMPARE(request.messages.size(), std::size_t(1));
    QCOMPARE(request.messages.back().images.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(request.messages.back().images.front().path), imagePath);
    QCOMPARE(QString::fromStdString(request.messages.back().images.front().mimeType),
             QStringLiteral("image/png"));
}

void AgentControllerTest::registers_a_screenshot_tool_that_requires_approval() {
    std::unique_ptr<MaiTool> screenshot = makeDesktopScreenshotTool();
    QCOMPARE(QString::fromStdString(screenshot->name()), QStringLiteral("screenshot"));
    QVERIFY(screenshot->requiresApproval(QStringLiteral("{}").toStdString()));

    auto model = std::make_unique<ScriptedModel>(std::string(), std::string(kText));
    ScriptedModel* scripted = model.get();
    AgentController controller(std::move(model), QString());
    const QString sessionId = controller.createSession(QDir::currentPath());
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("inspect the screen")));
    QVERIFY(finished.wait(15000));

    bool foundScreenshot = false;
    for (const MaiToolSpec& tool : scripted->lastRequest().tools) {
        if (tool.name == "screenshot") foundScreenshot = true;
    }
    QVERIFY(foundScreenshot);
}

QTEST_MAIN(AgentControllerTest)
#include "AgentControllerTest.moc"
