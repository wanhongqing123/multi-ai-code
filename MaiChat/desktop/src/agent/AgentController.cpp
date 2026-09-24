#include "agent/AgentController.h"
#include "MaiOpenAiClient.h"
#include "MaiMemoryStore.h"
#include "MaiSqliteStore.h"

#include <QHash>
#include <QMetaType>
#include <variant>

namespace {

const char* const kDesktopSystemPrompt = R"(You are the AI assistant embedded in MaiChat.
Format every user-facing text response as valid GitHub Flavored Markdown (GFM), preserving actual
line breaks. Plain prose is valid Markdown; use headings, lists, fenced code blocks, links, and
tables only when they improve readability. Put a blank line before and after each table. Put the
header, delimiter row, and every table row on separate lines, with one delimiter cell per column.
Never imitate a table by writing pipe-separated rows in a single paragraph. Do not wrap the entire
response in a code fence.)";

std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString fromUtf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

// 界面要知道一个片段是正文、思考还是工具卡。增量事件本身不带这个信息（见头文件里那一段），
// 所以查一次记下来。
enum class PartKind { Unknown, Text, Reasoning, Tool };

}  // namespace

struct AgentController::Runtime {
    std::unique_ptr<MaiAgent> agent;
    MaiEventBus::Token token = 0;
    QString openError;
    QString lastError;
    // 只有主线程碰它：所有事件都排队到主线程之后才分类。
    QHash<QString, PartKind> partKinds;
};

namespace {

// 存储打不开时退化成内存存储，而不是让整个界面起不来。
//
// 理由：数据库打不开的常见原因是磁盘满、路径没权限、或者文件被另一个进程占着。
// 这些情况下用户仍然应该能用 agent（只是这次的对话不会留下来），而不是面对一个打不开的窗口。
// 错误从 openError() 取，界面自己决定怎么提示。
std::unique_ptr<MaiSessionStore> openStore(const QString& databasePath, QString& openError) {
    if (databasePath.isEmpty()) return makeMaiMemoryStore();

    auto opened = makeMaiSqliteStore(toUtf8(databasePath));
    if (!opened) {
        openError = fromUtf8(opened.error().message());
        return makeMaiMemoryStore();
    }
    return std::move(opened.value());
}

std::unique_ptr<MaiAgent> buildAgent(std::unique_ptr<MaiModelClient> model,
                                     std::unique_ptr<MaiSessionStore> store,
                                     const QString& defaultModel,
                                     MaiApprovalPolicy approvalPolicy = MaiApprovalPolicy::OnRequest) {
    // 内置工具（read / write / glob / grep）总是装着。会话没有工作目录时工具层会明确拒绝，
    // 要不要把工具声明给模型看是 MaiContextBuilder 的事，不是这里的。
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiBuiltinTools(*tools);

    MaiAgent::Options options;
    if (!defaultModel.isEmpty()) options.defaultModel = toUtf8(defaultModel);
    // 0 = 无限等。桌面端有人盯着，超时自动拒绝等于替用户做决定。
    options.permissionTimeoutMs = 0;
    options.approvalPolicy = approvalPolicy;
    options.baseInstructions = kDesktopSystemPrompt;

    return std::make_unique<MaiAgent>(std::move(store), std::move(model), std::move(tools),
                                      options);
}

}  // namespace

AgentController::AgentController(std::unique_ptr<MaiModelClient> model,
                                 const QString& databasePath, QObject* parent)
    : QObject(parent), runtime_(std::make_unique<Runtime>()) {
    // 排队投递要求这个类型是注册过的。放在构造里而不是全局静态，是因为注册本身是幂等且线程安全的，
    // 而全局静态的初始化顺序不好讲。
    qRegisterMetaType<MaiEvent>("MaiEvent");

    runtime_->agent = buildAgent(std::move(model), openStore(databasePath, runtime_->openError),
                                 QString(), MaiApprovalPolicy::OnRequest);

    // **显式 QueuedConnection，不用 Auto。**
    //
    // Auto 是按"发射线程和接收方线程是不是同一个"在运行期决定的：核心的工作线程过来的走排队，
    // 而主线程自己调 submit() 时同步发出的那几条（会话建好、消息建好）会走直连。两条路混着，
    // 事件到达界面的顺序就不再和发生顺序一致了——界面可能先收到某条增量，再收到"这条消息建好了"。
    //
    // 一律排队就都是先进先出，而且分类逻辑永远只在主线程上跑。
    connect(this, &AgentController::eventQueued, this, &AgentController::onEventQueued,
            Qt::QueuedConnection);

    // 订阅要放在最后：前面几步没做完就收到事件的话，onEventQueued 会碰到半个对象。
    runtime_->token = runtime_->agent->eventBus().subscribe(
        [this](const MaiEvent& event) { rawEvent(event); });
}

AgentController::AgentController(const ModelConfig& model, const QString& databasePath,
                                 QObject* parent)
    : QObject(parent), runtime_(std::make_unique<Runtime>()) {
    qRegisterMetaType<MaiEvent>("MaiEvent");

    // baseUrl 为空 = 空转。建会话、翻历史照常，发消息会以 NotConfigured 收场。
    std::unique_ptr<MaiModelClient> client;
    if (!model.baseUrl.isEmpty()) {
        MaiModelConfig config;
        config.baseUrl = toUtf8(model.baseUrl);
        config.apiKey = toUtf8(model.apiKey);
        client = makeMaiModelClient(config);
    }

    runtime_->agent = buildAgent(std::move(client), openStore(databasePath, runtime_->openError),
                                 model.modelName, model.approvalPolicy);

    connect(this, &AgentController::eventQueued, this, &AgentController::onEventQueued,
            Qt::QueuedConnection);
    runtime_->token = runtime_->agent->eventBus().subscribe(
        [this](const MaiEvent& event) { rawEvent(event); });
}

AgentController::~AgentController() {
    // 顺序很讲究，别调。
    //
    // 退订只是第一道：MaiEventBus 明说了"退订返回之后，处理函数仍可能正在别的线程上执行"。
    // 真正让它安全的是下面那句 reset——MaiAgent 的析构会叫停所有在跑的轮次**并等它们退出**，
    // 返回之后就再没有线程能进到 rawEvent 里了。
    //
    // 反过来写（先 reset 再退订）也能跑，
    // 但析构过程中发出的最后几条事件会打到一个正在销毁的 QObject 上。
    runtime_->agent->eventBus().unsubscribe(runtime_->token);
    runtime_->agent.reset();
}

QString AgentController::openError() const {
    return runtime_->openError;
}

QString AgentController::lastError() const {
    return runtime_->lastError;
}

MaiAgent& AgentController::agent() {
    return *runtime_->agent;
}

// ---- 变更 ----

QString AgentController::createSession(const QString& directory, const QString& title) {
    MaiResult<std::string> created =
        runtime_->agent->submit(MaiCreateSession{toUtf8(directory), toUtf8(title), {}});
    if (!created) {
        runtime_->lastError = fromUtf8(created.error().message());
        return QString();
    }
    return fromUtf8(created.value());
}

bool AgentController::sendPrompt(const QString& sessionId, const QString& text) {
    MaiResult<std::string> sent =
        runtime_->agent->submit(MaiSendPrompt{toUtf8(sessionId), toUtf8(text)});
    if (!sent) {
        runtime_->lastError = fromUtf8(sent.error().message());
        return false;
    }
    return true;
}

bool AgentController::interrupt(const QString& sessionId) {
    MaiResult<std::string> stopped = runtime_->agent->submit(MaiInterrupt{toUtf8(sessionId)});
    if (!stopped) {
        runtime_->lastError = fromUtf8(stopped.error().message());
        return false;
    }
    return true;
}

bool AgentController::setApprovalPolicy(MaiApprovalPolicy policy) {
    if (!runtime_->agent->setApprovalPolicy(policy)) {
        runtime_->lastError = QStringLiteral("请先停止正在执行的任务，再切换批准方式。");
        return false;
    }
    return true;
}

MaiApprovalPolicy AgentController::approvalPolicy() const {
    return runtime_->agent->approvalPolicy();
}

bool AgentController::approvePermission(const QString& permissionId, bool approveForSession) {
    const MaiPermissionDecision decision = approveForSession
                                               ? MaiPermissionDecision::ApprovedForSession
                                               : MaiPermissionDecision::Approved;
    MaiResult<std::string> replied =
        runtime_->agent->submit(MaiReplyPermission{toUtf8(permissionId), decision});
    if (!replied) {
        runtime_->lastError = fromUtf8(replied.error().message());
        return false;
    }
    return true;
}

bool AgentController::denyPermission(const QString& permissionId) {
    MaiResult<std::string> replied = runtime_->agent->submit(
        MaiReplyPermission{toUtf8(permissionId), MaiPermissionDecision::Denied});
    if (!replied) {
        runtime_->lastError = fromUtf8(replied.error().message());
        return false;
    }
    return true;
}

bool AgentController::answerQuestion(const QString& questionId, const QString& answer) {
    MaiResult<std::string> replied =
        runtime_->agent->submit(MaiReplyQuestion{toUtf8(questionId), toUtf8(answer)});
    if (!replied) {
        runtime_->lastError = fromUtf8(replied.error().message());
        return false;
    }
    return true;
}

std::vector<MaiQuestionRequest> AgentController::pendingQuestions() const {
    return runtime_->agent->listPendingQuestions();
}

std::vector<MaiSubAgentInfo> AgentController::subAgents(const QString& parentSessionId) const {
    return runtime_->agent->listSubAgents(toUtf8(parentSessionId));
}

bool AgentController::isChildOf(const QString& sessionId, const QString& parentSessionId) const {
    if (sessionId.isEmpty() || parentSessionId.isEmpty()) return false;
    MaiSession session;
    if (!runtime_->agent->getSession(toUtf8(sessionId), session)) return false;
    return fromUtf8(session.parentId) == parentSessionId;
}

// ---- 事件 ----

void AgentController::rawEvent(const MaiEvent& event) {
    // **这里跑在核心的线程上**（流式期间是网络读线程）。能做的只有一件事：把事件排队交给主线程。
    // emit 一个跨线程的排队连接就是加锁往队列塞个指针，
    // 符合 MaiEventBus 那条"处理函数里不许做慢活"的契约。
    //
    // 不要在这里查存储、碰部件、或者做任何分类——那些都在 onEventQueued 里做。
    emit eventQueued(event);
}

void AgentController::onEventQueued(const MaiEvent& event) {
    // 这里已经在主线程上了：查存储、刷界面都随意。
    const QString sessionId = fromUtf8(event.sessionId);
    const QString messageId = fromUtf8(event.messageId);
    const QString partId = fromUtf8(event.partId);

    switch (event.type) {
        case MaiEventType::MessagePartUpdated: {
            // 片段第一次出现，或者工具卡状态变了。查一次存储看它是什么。
            //
            // 工具片段每次都要查（状态会变）；正文和思考只需要查第一次——它们的类型不会变，
            // 之后来的全是增量。
            const auto known = runtime_->partKinds.constFind(partId);
            if (known != runtime_->partKinds.constEnd() && known.value() != PartKind::Tool) return;

            PartKind kind = PartKind::Unknown;
            for (const MaiMessage& message : runtime_->agent->listMessages(event.sessionId)) {
                if (message.id != event.messageId) continue;
                for (const MaiMessagePart& part : message.parts) {
                    if (part.id != event.partId) continue;
                    if (std::get_if<MaiTextPart>(&part.body) != nullptr) {
                        kind = PartKind::Text;
                    } else if (std::get_if<MaiReasoningPart>(&part.body) != nullptr) {
                        kind = PartKind::Reasoning;
                    } else {
                        kind = PartKind::Tool;
                    }
                    break;
                }
                break;
            }
            if (kind == PartKind::Unknown) return;

            runtime_->partKinds.insert(partId, kind);
            if (kind == PartKind::Tool) emit toolPartChanged(sessionId, messageId, partId);
            // 正文和思考片段这里不发信号：内容还是空的，界面没什么可画。等第一条增量到了再说。
            return;
        }

        case MaiEventType::MessagePartDelta: {
            // 两种增量的 field 都是 "text"，只能靠上面那张表区分。
            // 查不到就当正文——但那说明 message.part.updated 没到，属于上游的 bug，
            // 不是这里该悄悄兜住的事（MaiAgent 那边为此改过一次"先落库再广播"）。
            const PartKind kind = runtime_->partKinds.value(partId, PartKind::Text);
            const QString delta = fromUtf8(event.delta);
            if (kind == PartKind::Reasoning) {
                emit reasoningDelta(sessionId, messageId, partId, delta);
            } else {
                emit textDelta(sessionId, messageId, partId, delta);
            }
            return;
        }

        case MaiEventType::SessionIdle:
            // 一轮结束，这一轮的片段表可以丢了。不丢的话长会话会一直涨。
            runtime_->partKinds.clear();
            emit turnFinished(sessionId);
            return;

        case MaiEventType::SessionError:
            emit turnFailed(sessionId, fromUtf8(event.detail));
            return;

        case MaiEventType::SessionUpdated:
            if (!event.detail.empty()) {
                emit sessionTitleChanged(sessionId, fromUtf8(event.detail));
            }
            return;

        case MaiEventType::PermissionAsked:
            emit permissionAsked(fromUtf8(event.permissionId), sessionId);
            return;

        case MaiEventType::PermissionReplied:
            emit permissionReplied(fromUtf8(event.permissionId), fromUtf8(event.detail));
            return;

        case MaiEventType::QuestionAsked:
            emit questionAsked(fromUtf8(event.questionId), sessionId, fromUtf8(event.partId));
            return;

        case MaiEventType::QuestionAnswered:
            emit questionAnswered(fromUtf8(event.questionId), sessionId);
            return;

        default:
            // SessionCreated / SessionDeleted / MessageUpdated / MessageRemoved /
            // MessagePartRemoved / SessionStatus：界面拿不到新信息。
            // 建会话和发消息的返回值里已经有 id 了，删除由发起那一侧自己知道。
            return;
    }
}
