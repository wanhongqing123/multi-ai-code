#include "MaiHttpAdapter.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <httplib.h>
#include <json.hpp>

#include "MaiIdGenerator.h"

namespace {

using json = nlohmann::json;

const char* maiRoleToWire(MaiRole role) {
    return role == MaiRole::User ? "user" : "assistant";
}

// 核心的错误码 -> HTTP 状态码。映射只在这一处，核心本身不知道 HTTP。
int toHttpStatus(MaiErrorCode code) {
    switch (code) {
        case MaiErrorCode::Ok: return 200;
        case MaiErrorCode::NotFound: return 404;
        case MaiErrorCode::Busy: return 409;
        case MaiErrorCode::InvalidInput: return 400;
        case MaiErrorCode::NotConfigured: return 503;
        case MaiErrorCode::Network:
        case MaiErrorCode::Protocol: return 502;
        case MaiErrorCode::Canceled: return 200;
        case MaiErrorCode::Internal: return 500;
    }
    return 500;
}

const char* maiToolStateToWire(MaiToolState state) {
    switch (state) {
        case MaiToolState::Pending: return "pending";
        case MaiToolState::Running: return "running";
        case MaiToolState::Completed: return "completed";
        case MaiToolState::Error: return "error";
    }
    return "pending";
}

// ── 序列化：只在这里把原生结构体变成 JSON ─────────────────────
json toJson(const MaiSession& session) {
    return json{
        {"id", session.id},
        {"title", session.title},
        {"directory", session.directory},
        {"model", session.model},
        {"agent", session.agent},
        {"time", {{"created", session.created}, {"updated", session.updated}}},
    };
}

json toJson(const MaiMessagePart& part) {
    json node{{"id", part.id}};
    std::visit(
        [&node](const auto& body) {
            using T = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<T, MaiTextPart>) {
                node["type"] = "text";
                node["text"] = body.text;
            } else if constexpr (std::is_same_v<T, MaiReasoningPart>) {
                node["type"] = "reasoning";
                node["text"] = body.text;
            } else if constexpr (std::is_same_v<T, MaiToolPart>) {
                node["type"] = "tool";
                node["tool"] = body.tool;
                node["callID"] = body.callId;
                node["state"] = {{"status", maiToolStateToWire(body.state)},
                                 {"input", body.input},
                                 {"output", body.output},
                                 {"error", body.error}};
            }
        },
        part.body);
    return node;
}

json toJson(const MaiMessage& message) {
    json parts = json::array();
    for (const auto& part : message.parts) parts.push_back(toJson(part));
    return json{
        {"id", message.id},
        {"role", maiRoleToWire(message.role)},
        {"parts", std::move(parts)},
        {"time", {{"created", message.created}, {"completed", message.completed}}},
    };
}

json toJson(const MaiPermissionRequest& permission) {
    return json{
        {"id", permission.id},
        {"sessionID", permission.sessionId},
        {"messageID", permission.messageId},
        {"partID", permission.partId},
        {"tool", permission.toolName},
        {"input", permission.arguments},
        {"time", {{"asked", permission.asked}}},
    };
}

// 事件的线上形状照 openapi.json 里的 MaiMessagePartDelta 等 schema：
// { id, type, data: { sessionID, messageID, partID, field, delta } }
std::string serializeEvent(const MaiEvent& event) {
    json data{{"sessionID", event.sessionId}};
    if (!event.messageId.empty()) data["messageID"] = event.messageId;
    if (!event.partId.empty()) data["partID"] = event.partId;
    if (!event.field.empty()) data["field"] = event.field;
    if (!event.delta.empty()) data["delta"] = event.delta;
    if (!event.permissionId.empty()) data["permissionID"] = event.permissionId;
    if (!event.detail.empty()) data["detail"] = event.detail;

    const json envelope{
        {"id", event.id}, {"type", maiEventTypeToString(event.type)}, {"data", std::move(data)}};

    // SSE 帧：data: <json>\n\n
    std::string frame = "data: ";
    frame += envelope.dump();
    frame += "\n\n";
    return frame;
}

// ── SSE 连接 ────────────────────────────────────────────────────
// 每个连接一个队列。事件在适配器层**只序列化一次**，再把同一份字符串
// 分发给所有连接——N 个客户端时不做 N 次 dump()。
struct SseConnection {
    std::mutex mutex;
    std::condition_variable hasWork;
    std::deque<std::string> queue;
    bool closed = false;

    void push(const std::string& frame) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed) return;
            // 背压：客户端卡住时不要无限堆积。丢最老的，UI 重连后会重新拉全量。
            if (queue.size() > 2048) queue.pop_front();
            queue.push_back(frame);
        }
        hasWork.notify_one();
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true;
        }
        hasWork.notify_all();
    }
};

}  // namespace

struct MaiHttpAdapter::Listener {
    MaiAgent& agent;
    MaiHttpAdapterOptions options;
    httplib::Server server;
    std::atomic<int> boundPort{0};

    std::mutex connectionsMutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<SseConnection>> connections;
    std::atomic<std::uint64_t> nextConnection{1};
    MaiEventBus::Token busToken = 0;

    explicit Listener(MaiAgent& agent, MaiHttpAdapterOptions options)
        : agent(agent), options(std::move(options)) {}

    void broadcast(const MaiEvent& event) {
        const std::string frame = serializeEvent(event);  // 只 dump 一次
        std::vector<std::shared_ptr<SseConnection>> targets;
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            targets.reserve(connections.size());
            for (const auto& [_, connection] : connections) targets.push_back(connection);
        }
        for (const auto& connection : targets) connection->push(frame);
    }

    void routes();
};

void MaiHttpAdapter::Listener::routes() {
    server.Get("/api/health", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(json{{"status", "ok"}, {"service", "maiagent"}}.dump(),
                             "application/json");
    });

    // UI 有几处会读它。M1 先给个能让 UI 跑起来的最小骨架。
    server.Get("/config", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(json{{"provider", json::object()}, {"model", ""}}.dump(),
                             "application/json");
    });

    server.Get("/api/session", [this](const httplib::Request&, httplib::Response& response) {
        json array = json::array();
        for (const auto& session : agent.listSessions()) array.push_back(toJson(session));
        response.set_content(array.dump(), "application/json");
    });

    server.Post(
        "/api/session", [this](const httplib::Request& request, httplib::Response& response) {
            MaiCreateSession operation;
            if (!request.body.empty()) {
                // 客户端给的 JSON 可能缺字段甚至不是合法 JSON，不能让它把服务端搞崩。
                const json body = json::parse(request.body, nullptr, /*allow_exceptions=*/false);
                if (body.is_object()) {
                    operation.directory = body.value("directory", std::string{});
                    operation.title = body.value("title", std::string{});
                    operation.model = body.value("model", std::string{});
                }
            }
            const auto created = agent.submit(operation);
            if (!created) {
                response.status = 400;
                response.set_content(json{{"error", created.error().message()},
                                          {"code", maiErrorCodeToString(created.error().code())}}
                                         .dump(),
                                     "application/json");
                return;
            }
            MaiSession session;
            agent.getSession(created.value(), session);
            response.set_content(toJson(session).dump(), "application/json");
        });

    server.Get(R"(/api/session/([^/]+))", [this](const httplib::Request& request,
                                                 httplib::Response& response) {
        MaiSession session;
        if (!agent.getSession(request.matches[1], session)) {
            response.status = 404;
            response.set_content(json{{"error", "session not found"}}.dump(), "application/json");
            return;
        }
        response.set_content(toJson(session).dump(), "application/json");
    });

    // 改会话：切模型、改标题、换 agent。M5 要的"切模型"落在这里。
    //
    // 空字段表示"不改"而不是"改成空"——界面通常只送它动过的那一个字段，
    // 把没送的当成清空会让用户改个模型就把标题弄丢了。
    server.Post(R"(/api/session/([^/]+))", [this](const httplib::Request& request,
                                                  httplib::Response& response) {
        const json body = json::parse(request.body, nullptr, /*allow_exceptions=*/false);
        if (!body.is_object()) {
            response.status = 400;
            response.set_content(json{{"error", "body must be a JSON object"}}.dump(),
                                 "application/json");
            return;
        }
        MaiUpdateSession operation;
        operation.sessionId = request.matches[1];
        operation.title = body.value("title", std::string{});
        operation.model = body.value("model", std::string{});
        operation.agent = body.value("agent", std::string{});

        const auto updated = agent.submit(operation);
        if (!updated) {
            response.status = toHttpStatus(updated.error().code());
            response.set_content(json{{"error", updated.error().message()},
                                      {"code", maiErrorCodeToString(updated.error().code())}}
                                     .dump(),
                                 "application/json");
            return;
        }
        MaiSession session;
        agent.getSession(operation.sessionId, session);
        response.set_content(toJson(session).dump(), "application/json");
    });

    server.Get(R"(/api/session/([^/]+)/message)",
               [this](const httplib::Request& request, httplib::Response& response) {
                   json array = json::array();
                   for (const auto& message : agent.listMessages(request.matches[1]))
                       array.push_back(toJson(message));
                   response.set_content(array.dump(), "application/json");
               });

    // 发一轮消息。**立刻返回**——真正的输出全部走 /api/event 的事件流。
    // 这里如果同步等 agent 跑完，HTTP 请求会挂几十秒，界面就卡死了。
    server.Post(R"(/api/session/([^/]+)/prompt)", [this](const httplib::Request& request,
                                                         httplib::Response& response) {
        const std::string sessionId = request.matches[1];
        std::string text;
        const json body = json::parse(request.body, nullptr, /*allow_exceptions=*/false);
        if (body.is_object()) {
            // UI 可能给 {"text": "..."}，也可能给 opencode 那种
            // {"parts":[{"type":"text","text":"..."}]}，两种都认。
            if (body.contains("text") && body["text"].is_string()) {
                text = body["text"].get<std::string>();
            } else if (body.contains("parts") && body["parts"].is_array()) {
                for (const auto& part : body["parts"]) {
                    if (part.is_object() && part.value("type", "") == "text") {
                        if (!text.empty()) text += "\n";
                        text += part.value("text", std::string{});
                    }
                }
            }
        }
        const auto sent = agent.submit(MaiSendPrompt{sessionId, text});
        if (!sent) {
            // 核心给的是结构化错误码，这里只做一次映射。
            // 之前核心只返回空字符串，适配器得靠 busy() 反猜是哪种失败——
            // 那是个竞态：猜的时候状态可能已经变了。
            response.status = toHttpStatus(sent.error().code());
            response.set_content(json{{"error", sent.error().message()},
                                      {"code", maiErrorCodeToString(sent.error().code())}}
                                     .dump(),
                                 "application/json");
            return;
        }
        response.set_content(json{{"messageID", sent.value()}}.dump(), "application/json");
    });

    server.Post(R"(/api/session/([^/]+)/interrupt)", [this](const httplib::Request& request,
                                                            httplib::Response& response) {
        const std::string sessionId = request.matches[1];
        const auto result = agent.submit(MaiInterrupt{sessionId});
        response.set_content(json{{"interrupted", result.isOk()}}.dump(), "application/json");
    });

    // ── 权限 ──────────────────────────────────────────────────────
    // 界面重连之后必须能补上这一份：SSE 断开的那个窗口期里发出的
    // permission.asked 是看不到的，只靠事件流会漏掉整整一次授权请求，
    // 那一轮就一直挂着而界面上什么都没有。
    server.Get("/api/permission", [this](const httplib::Request&, httplib::Response& response) {
        json array = json::array();
        for (const auto& result : agent.listPendingPermissions()) array.push_back(toJson(result));
        response.set_content(array.dump(), "application/json");
    });

    server.Post(R"(/api/permission/([^/]+))", [this](const httplib::Request& request,
                                                     httplib::Response& response) {
        const json body = json::parse(request.body, nullptr, /*allow_exceptions=*/false);
        const std::string raw =
            body.is_object() ? body.value("decision", std::string{}) : std::string{};

        MaiPermissionDecision decision = MaiPermissionDecision::Denied;
        if (!maiParsePermissionDecision(raw, decision)) {
            // 认不出来就 400，**不要兜底成允许**。把拼错的 decision
            // 当成放行，等于闸门被一个错别字拆掉，而且毫无痕迹。
            response.status = 400;
            response.set_content(
                json{{"error", "decision must be one of: approved, approved_for_session, denied"},
                     {"got", raw}}
                    .dump(),
                "application/json");
            return;
        }

        MaiReplyPermission operation;
        operation.permissionId = request.matches[1];
        operation.decision = decision;
        const auto replied = agent.submit(operation);
        if (!replied) {
            response.status = toHttpStatus(replied.error().code());
            response.set_content(json{{"error", replied.error().message()},
                                      {"code", maiErrorCodeToString(replied.error().code())}}
                                     .dump(),
                                 "application/json");
            return;
        }
        response.set_content(json{{"permissionID", replied.value()},
                                  {"decision", maiPermissionDecisionToString(decision)}}
                                 .dump(),
                             "application/json");
    });

    // ── SSE 事件流 ────────────────────────────────────────────────
    server.Get("/api/event", [this](const httplib::Request&, httplib::Response& response) {
        auto connection = std::make_shared<SseConnection>();
        const std::uint64_t cid = nextConnection.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            connections.emplace(cid, connection);
        }

        response.set_header("Cache-Control", "no-cache");
        response.set_header("Connection", "keep-alive");
        response.set_header("X-Accel-Buffering", "no");

        response.set_chunked_content_provider(
            "text/event-stream",
            [connection](std::size_t, httplib::DataSink& sink) {
                std::unique_lock<std::mutex> lock(connection->mutex);
                // 15 秒没事件就发一个注释行当心跳，防止中间的代理把连接掐了。
                const bool got = connection->hasWork.wait_for(lock, std::chrono::seconds(15), [&] {
                    return connection->closed || !connection->queue.empty();
                });
                if (connection->closed) {
                    sink.done();
                    return false;
                }
                if (!got) {
                    lock.unlock();
                    return sink.write(":\n\n", 3);
                }
                std::string batch;
                while (!connection->queue.empty()) {
                    batch += connection->queue.front();
                    connection->queue.pop_front();
                }
                lock.unlock();
                return sink.write(batch.data(), batch.size());
            },
            [this, cid, connection](bool) {
                connection->close();
                std::lock_guard<std::mutex> lock(connectionsMutex);
                connections.erase(cid);
            });
    });
}

MaiHttpAdapter::MaiHttpAdapter(MaiAgent& agent, MaiHttpAdapterOptions options)
    : mListener(std::make_unique<Listener>(agent, std::move(options))) {
    mListener->routes();
    mListener->busToken = mListener->agent.eventBus().subscribe(
        [this](const MaiEvent& event) { mListener->broadcast(event); });
}

MaiHttpAdapter::~MaiHttpAdapter() {
    if (mListener->busToken) mListener->agent.eventBus().unsubscribe(mListener->busToken);
    stop();
}

bool MaiHttpAdapter::bind() {
    // port=0 走 bind_to_any_port 让系统挑；指定端口就直接绑。
    const int boundPort = mListener->options.port > 0
                              ? (mListener->server.bind_to_port(mListener->options.host.c_str(),
                                                                mListener->options.port)
                                     ? mListener->options.port
                                     : 0)
                              : mListener->server.bind_to_any_port(mListener->options.host.c_str());
    if (boundPort <= 0) return false;
    mListener->boundPort.store(boundPort);
    return true;
}

bool MaiHttpAdapter::serve() {
    return mListener->server.listen_after_bind();
}

void MaiHttpAdapter::stop() {
    {
        std::lock_guard<std::mutex> lock(mListener->connectionsMutex);
        for (auto& [_, code] : mListener->connections) code->close();
    }
    mListener->server.stop();
}

int MaiHttpAdapter::port() const {
    return mListener->boundPort.load();
}

std::string MaiHttpAdapter::baseUrl() const {
    return "http://" + mListener->options.host + ":" + std::to_string(port());
}
