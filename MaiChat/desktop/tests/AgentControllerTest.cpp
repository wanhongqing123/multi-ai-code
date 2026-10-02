#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
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
#include <thread>
#include <vector>

#include "agent/AgentController.h"
#include "MaiScreenshot.h"
#include "MaiScreenshotTool.h"

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

class HostProbeTool final : public MaiTool {
public:
    std::string name() const override { return "maichat_probe"; }
    std::string description() const override { return "Probe a host-provided capability."; }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{}})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        return MaiToolResult::success("ok");
    }
};

class OutsideReadModel final : public MaiModelClient {
public:
    explicit OutsideReadModel(std::string arguments, std::string tool = "read")
        : arguments_(std::move(arguments)), tool_(std::move(tool)) {}
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        if (calls_++ == 0 && sink.onToolCall)
            sink.onToolCall(MaiToolInvocation{"outside-file", tool_, arguments_});
        else if (sink.onText)
            sink.onText("done");
        return {};
    }
    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }

private:
    std::string arguments_;
    std::string tool_;
    std::atomic<int> calls_{0};
};

class ConcurrentReasoningModel final : public MaiModelClient {
public:
    std::atomic<bool> firstReasoningSent{false};
    std::atomic<bool> release{false};

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        if (request.messages.back().content == "session A") {
            if (sink.onReasoning) sink.onReasoning("first");
            firstReasoningSent.store(true);
            while (!release.load() && !cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (sink.onReasoning && !cancel.load()) sink.onReasoning("second");
        } else if (sink.onText) {
            sink.onText("session B done");
        }
        return {};
    }

    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }
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
    void registers_host_tools_after_builtin_tools();
    void finishingAnotherSessionDoesNotReclassifyReasoning();
    void desktopReadsOutsideSessionWorkingDirectory();
    void desktopWriteOutsideWaitsForApproval();
};

void AgentControllerTest::desktopReadsOutsideSessionWorkingDirectory() {
    QTemporaryDir workspace;
    QTemporaryDir otherDirectory;
    QVERIFY(workspace.isValid());
    QVERIFY(otherDirectory.isValid());
    const QString source = QDir(otherDirectory.path()).filePath(QStringLiteral("outside.txt"));
    QFile file(source);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("outside-visible") > 0);
    file.close();
    const std::string arguments =
        QJsonDocument(QJsonObject{{QStringLiteral("path"), source}})
            .toJson(QJsonDocument::Compact).toStdString();
    AgentController controller(std::make_unique<OutsideReadModel>(arguments), QString());
    const QString sessionId = controller.createSession(workspace.path());
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("read the file")));
    controller.agent().waitIdle();

    bool readOutside = false;
    for (const MaiMessage& message : controller.agent().listMessages(sessionId.toStdString())) {
        for (const MaiMessagePart& part : message.parts) {
            const auto* tool = std::get_if<MaiToolPart>(&part.body);
            if (tool != nullptr && tool->tool == "read" &&
                tool->output.find("outside-visible") != std::string::npos)
                readOutside = true;
        }
    }
    QVERIFY(readOutside);
}

void AgentControllerTest::desktopWriteOutsideWaitsForApproval() {
    QTemporaryDir workspace;
    QTemporaryDir otherDirectory;
    QVERIFY(workspace.isValid());
    QVERIFY(otherDirectory.isValid());
    const QString target = QDir(otherDirectory.path()).filePath(QStringLiteral("outside.txt"));
    const std::string arguments = QJsonDocument(QJsonObject{
        {QStringLiteral("path"), target}, {QStringLiteral("content"), QStringLiteral("approved")}})
                                      .toJson(QJsonDocument::Compact).toStdString();
    AgentController controller(
        std::make_unique<OutsideReadModel>(arguments, "write"), QString());
    const QString sessionId = controller.createSession(workspace.path());
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("write outside")));
    QTRY_COMPARE(controller.agent().listPendingPermissions().size(), std::size_t(1));
    QVERIFY(!QFile::exists(target));
    const QString permission =
        QString::fromStdString(controller.agent().listPendingPermissions().front().id);
    QVERIFY(controller.approvePermission(permission));
    controller.agent().waitIdle();
    QFile written(target);
    QVERIFY(written.open(QIODevice::ReadOnly));
    QCOMPARE(written.readAll(), QByteArray("approved"));
}

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
    std::unique_ptr<MaiTool> screenshot = makeMaiScreenshotTool();
    QCOMPARE(QString::fromStdString(screenshot->name()), QStringLiteral("screenshot"));
    QVERIFY(screenshot->requiresApproval(QStringLiteral("{}").toStdString()));
    MaiToolContext textModelContext;
    textModelContext.model = "glm-5.3";
    const MaiToolResult unsupported = screenshot->execute("{}", textModelContext);
    QVERIFY(unsupported.hasError());
    QCOMPARE(unsupported.error().code(), MaiErrorCode::NotSupported);
    QVERIFY(QString::fromStdString(unsupported.error().message()).contains(
        QStringLiteral("glm-5.3-flash")));

    auto model = std::make_unique<ScriptedModel>(std::string(), std::string(kText));
    ScriptedModel* scripted = model.get();
    AgentController controller(std::move(model), QString());
    const QString sessionId = controller.createSession(QDir::currentPath());
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("inspect the screen")));
    QVERIFY(finished.wait(15000));

    bool foundScreenshot = false;
    bool foundViewImage = false;
    bool foundListWindows = false;
    for (const MaiToolSpec& tool : scripted->lastRequest().tools) {
        if (tool.name == "screenshot") foundScreenshot = true;
        if (tool.name == "view_image") foundViewImage = true;
        if (tool.name == "list_windows") foundListWindows = true;
    }
    QCOMPARE(foundScreenshot, maiIsScreenshotSupported());
    QVERIFY(foundViewImage);
    QCOMPARE(foundListWindows, maiIsScreenshotSupported());
}

void AgentControllerTest::registers_host_tools_after_builtin_tools() {
    auto model = std::make_unique<ScriptedModel>(std::string(), std::string(kText));
    ScriptedModel* scripted = model.get();
    AgentController controller(
        std::move(model), QString(),
        [](MaiToolRegistry& registry) { registry.add(std::make_unique<HostProbeTool>()); });
    const QString sessionId = controller.createSession(QDir::currentPath());
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("use the host")));
    QVERIFY(finished.wait(15000));

    bool foundBuiltin = false;
    bool foundHost = false;
#if defined(MAICHAT_TEST_CV_VIDEO_ANALYSIS)
    bool foundSceneDetect = false;
    bool foundMotionDetect = false;
#endif
#if defined(MAICHAT_TEST_RVM_BUNDLED)
    bool foundVideoMatting = false;
#endif
    for (const MaiToolSpec& tool : scripted->lastRequest().tools) {
        if (tool.name == "read") foundBuiltin = true;
        if (tool.name == "maichat_probe") foundHost = true;
#if defined(MAICHAT_TEST_CV_VIDEO_ANALYSIS)
        if (tool.name == "cv_scene_detect") foundSceneDetect = true;
        if (tool.name == "cv_motion_detect") foundMotionDetect = true;
#endif
#if defined(MAICHAT_TEST_RVM_BUNDLED)
        if (tool.name == "cv_video_matting") foundVideoMatting = true;
#endif
    }
    QVERIFY(foundBuiltin);
    QVERIFY(foundHost);
#if defined(MAICHAT_TEST_CV_VIDEO_ANALYSIS)
    QVERIFY(foundSceneDetect);
    QVERIFY(foundMotionDetect);
#endif
#if defined(MAICHAT_TEST_RVM_BUNDLED)
    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
    const bool mattingAssetsPresent =
        QFileInfo::exists(applicationDirectory.filePath(
            QStringLiteral("MaiAgentModels/rvm_mobilenetv3_fp32.onnx"))) &&
        QFileInfo::exists(applicationDirectory.filePath(QStringLiteral("onnxruntime.dll"))) &&
        QFileInfo::exists(applicationDirectory.filePath(
            QStringLiteral("onnxruntime_providers_shared.dll")));
    QCOMPARE(foundVideoMatting, mattingAssetsPresent);
#endif
}

void AgentControllerTest::finishingAnotherSessionDoesNotReclassifyReasoning() {
    auto model = std::make_unique<ConcurrentReasoningModel>();
    ConcurrentReasoningModel* controlled = model.get();
    AgentController controller(std::move(model), QString());
    const QString firstSession = controller.createSession(QDir::currentPath());
    const QString secondSession = controller.createSession(QDir::currentPath());
    QSignalSpy reasoning(&controller, &AgentController::reasoningDelta);
    QSignalSpy text(&controller, &AgentController::textDelta);
    QSignalSpy finished(&controller, &AgentController::turnFinished);

    QVERIFY(controller.sendPrompt(firstSession, QStringLiteral("session A")));
    QTRY_VERIFY(controlled->firstReasoningSent.load());
    QTRY_VERIFY(reasoning.count() >= 1);
    QVERIFY(controller.sendPrompt(secondSession, QStringLiteral("session B")));
    QTRY_VERIFY(std::any_of(finished.cbegin(), finished.cend(), [&](const QList<QVariant>& args) {
        return args.first().toString() == secondSession;
    }));
    controlled->release.store(true);
    controller.agent().waitIdle();
    QTRY_VERIFY(std::any_of(finished.cbegin(), finished.cend(), [&](const QList<QVariant>& args) {
        return args.first().toString() == firstSession;
    }));
    int firstReasoningParts = 0;
    for (const QList<QVariant>& args : reasoning) {
        if (args.first().toString() == firstSession) ++firstReasoningParts;
    }
    QCOMPARE(firstReasoningParts, 2);
    for (const QList<QVariant>& args : text) {
        QVERIFY(args.first().toString() != firstSession);
    }
}

QTEST_MAIN(AgentControllerTest)
#include "AgentControllerTest.moc"
