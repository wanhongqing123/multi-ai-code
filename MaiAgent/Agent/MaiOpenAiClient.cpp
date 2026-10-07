#include "MaiOpenAiClient.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"
#include "MaiResponsesClient.h"

namespace {

using json = nlohmann::json;

// ── 按行切分流式字节 ────────────────────────────────────────────
// curl 的写回调给的是任意大小的字节块，一个 SSE 事件可能被劈成几块，也可能一块里塞了好几个事件。
// 所以必须自己缓冲按 \n 切。
//
// mScanned 记住"已扫描过、确认不含换行"的前缀长度，
// 避免每来一块就把整个缓冲区重扫一遍——流式期间这个回调每秒被调几十次。
// 思路取自 codex 的 ollama/src/line_buffer.rs（那边 32 行）。
class LineBuffer {
public:
    void append(const char* data, std::size_t length) {
        mBuffer.append(data, length);
    }

    bool nextLine(std::string& out) {
        const std::size_t newlineAt = mBuffer.find('\n', mScanned);
        if (newlineAt == std::string::npos) {
            mScanned = mBuffer.size();
            return false;
        }
        out.assign(mBuffer, 0, newlineAt);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        mBuffer.erase(0, newlineAt + 1);
        mScanned = 0;
        return true;
    }

    bool takeRemainder(std::string& out) {
        if (mBuffer.empty()) return false;
        out = std::move(mBuffer);
        mBuffer.clear();
        mScanned = 0;
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
    }

private:
    std::string mBuffer;
    std::size_t mScanned = 0;
};

// ── 工具调用的分片聚合 ──────────────────────────────────────────
// 这是 Chat Completions 最容易写错的一处。arguments 不是一次给全的，是 JSON 字符串的碎片，
// 按 index 分批到达：
//   {index:0, id:"call_x", function:{name:"bash", arguments:""}}
//   {index:0,             function:{arguments:"{\"comm"}}
//   {index:0,             function:{arguments:"and\":\"npm"}}
//   {index:0,             function:{arguments:" test\"}"}}
// 不同服务端分片时机不一样——有的整块给，有的一个字符一个字符给，两种都要能处理，
// 所以只能按 index 攒，等流结束再交付。
class InvocationAccumulator {
public:
    void feed(const json& delta_tool_calls) {
        if (!delta_tool_calls.is_array()) return;
        for (const auto& toolCallNode : delta_tool_calls) {
            // index 缺省当 0：个别服务端在只有一个调用时会省掉它。
            const int index = toolCallNode.value("index", 0);
            auto& slot = mSlots[index];
            if (toolCallNode.contains("id") && toolCallNode["id"].is_string())
                slot.id = toolCallNode["id"].get<std::string>();
            if (!toolCallNode.contains("function")) continue;
            const auto& functionNode = toolCallNode["function"];
            if (functionNode.contains("name") && functionNode["name"].is_string()) {
                // name 也可能分片，所以是 append 不是赋值。
                slot.name += functionNode["name"].get<std::string>();
            }
            if (functionNode.contains("arguments") && functionNode["arguments"].is_string()) {
                slot.arguments += functionNode["arguments"].get<std::string>();
            }
        }
    }

    std::vector<MaiToolInvocation> take() {
        std::vector<MaiToolInvocation> out;
        out.reserve(mSlots.size());
        for (auto& [_, choice] : mSlots) {  // map 保证按 index 有序
            if (choice.name.empty()) continue;
            out.push_back(std::move(choice));
        }
        mSlots.clear();
        return out;
    }

    bool empty() const {
        return mSlots.empty();
    }

private:
    std::map<int, MaiToolInvocation> mSlots;
};

// 中立的 MaiModelRole -> OpenAI 的 role 字符串。
// 这个映射是**这一层的职责**：上层用自己的词汇，翻译只发生在边界。
const char* toWireRole(MaiModelRole role) {
    switch (role) {
        case MaiModelRole::System: return "system";
        case MaiModelRole::User: return "user";
        case MaiModelRole::Assistant: return "assistant";
        case MaiModelRole::ToolResult: return "tool";
    }
    return "user";
}

constexpr std::uint64_t kMaxPromptImageBytes = 20u * 1024 * 1024;

bool isSupportedImageMimeType(const std::string& value) {
    return value == "image/jpeg" || value == "image/png" || value == "image/webp" ||
           value == "image/gif";
}

std::string base64Encode(const std::string& input) {
    constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto first = static_cast<unsigned char>(input[offset]);
        const auto second =
            offset + 1 < input.size() ? static_cast<unsigned char>(input[offset + 1]) : 0;
        const auto third =
            offset + 2 < input.size() ? static_cast<unsigned char>(input[offset + 2]) : 0;
        output.push_back(kAlphabet[first >> 2]);
        output.push_back(kAlphabet[((first & 0x03) << 4) | (second >> 4)]);
        output.push_back(
            offset + 1 < input.size() ? kAlphabet[((second & 0x0F) << 2) | (third >> 6)] : '=');
        output.push_back(offset + 2 < input.size() ? kAlphabet[third & 0x3F] : '=');
    }
    return output;
}

using ImagePreparer = std::function<MaiResult<std::string>(const std::string&, const std::string&)>;

MaiResult<std::string> imageDataUrl(const MaiModelRequest& request, const MaiModelImage& image,
                                    const ImagePreparer& prepareImage) {
    if (request.workingDirectory.empty())
        return {MaiErrorCode::InvalidInput,
                "an image prompt requires a non-empty working directory"};
    if (!isSupportedImageMimeType(image.mimeType))
        return {MaiErrorCode::InvalidInput, "unsupported prompt image type: " + image.mimeType};
    const MaiFilePath candidate = MaiFilePath::fromUtf8(image.path);
    const std::string resolved =
        candidate.isAbsolute() ? MaiFileSystem::resolve(candidate).toUtf8()
                               : maiResolvePathWithinRoot(request.workingDirectory, image.path);
    if (resolved.empty())
        return {MaiErrorCode::InvalidInput,
                "prompt image is outside the working directory: " + image.path};

    constexpr std::uint64_t kMaximumModelPreviewBytes = 2u * 1024u * 1024u;
    const MaiFilePath sourcePath = MaiFilePath::fromUtf8(resolved);
    std::uint64_t sourceSize = 0;
    const bool sizeKnown = MaiFileSystem::fileSize(sourcePath, sourceSize);
    if (sizeKnown && sourceSize > kMaxPromptImageBytes)
        return {MaiErrorCode::InvalidInput, "prompt image exceeds the 20 MB limit: " + image.path};
    std::string bytes;
    bool truncated = false;
    const bool largeSource = sizeKnown && sourceSize > kMaximumModelPreviewBytes;
    if (!largeSource) {
        const MaiError readError =
            MaiFileSystem::readFile(sourcePath, bytes, kMaxPromptImageBytes, &truncated);
        if (readError) return readError;
        if (truncated)
            return {MaiErrorCode::InvalidInput,
                    "prompt image exceeds the 20 MB limit: " + image.path};
        if (bytes.empty())
            return {MaiErrorCode::InvalidInput, "prompt image is empty: " + image.path};
    }
    std::string mimeType = image.mimeType;
    if (largeSource || bytes.size() > kMaximumModelPreviewBytes) {
        if (!prepareImage)
            return {MaiErrorCode::InvalidInput,
                    "image exceeds the 2 MB model request budget; resize it before retrying"};
        MaiResult<std::string> preview = prepareImage(resolved, request.workingDirectory);
        if (!preview) return preview.error();
        if (preview.value() == resolved)
            return {MaiErrorCode::Internal, "the model preview must not reuse the original image"};
        bytes.clear();
        truncated = false;
        const MaiError previewRead = MaiFileSystem::readFile(
            MaiFilePath::fromUtf8(preview.value()), bytes, kMaximumModelPreviewBytes, &truncated);
        if (previewRead) return previewRead;
        if (truncated || bytes.empty())
            return {MaiErrorCode::InvalidInput,
                    "the prepared model image is empty or exceeds the 2 MB request budget"};
        mimeType = "image/jpeg";
    }
    return "data:" + mimeType + ";base64," + base64Encode(bytes);
}

// ── 请求体构造：中立结构 -> OpenAI 线格式 ─────────────────────
MaiResult<std::string> buildRequestBody(const MaiModelRequest& request,
                                        const ImagePreparer& prepareImage) {
    json msgs = json::array();
    if (!request.baseInstructions.empty()) {
        msgs.push_back({{"role", "system"}, {"content", request.baseInstructions}});
    }
    std::size_t latestUserMessage = request.messages.size();
    for (std::size_t index = 0; index < request.messages.size(); ++index) {
        if (request.messages[index].role == MaiModelRole::User) latestUserMessage = index;
    }
    for (std::size_t index = 0; index < request.messages.size(); ++index) {
        const auto& message = request.messages[index];
        json messageNode{{"role", toWireRole(message.role)}};
        // assistant 发起调用的那条，content 可以是 null，但必须带 tool_calls。
        if (!message.images.empty()) {
            json content = json::array();
            std::string text = message.content;
            bool skippedUnavailableHistoryImage = false;
            json images = json::array();
            for (const auto& image : message.images) {
                MaiResult<std::string> dataUrl = imageDataUrl(request, image, prepareImage);
                if (!dataUrl) {
                    // 当前输入的图片丢失必须明确失败，不能悄悄降级成纯文本。
                    // 旧历史里的图片则可能因为移动端容器迁移或用户清理缓存而失效；
                    // 它不应让这个会话此后的每一轮都永久失败。
                    if (index == latestUserMessage ||
                        dataUrl.error().code() != MaiErrorCode::NotFound)
                        return dataUrl.error();
                    skippedUnavailableHistoryImage = true;
                    continue;
                }
                images.push_back(
                    {{"type", "image_url"}, {"image_url", {{"url", dataUrl.value()}}}});
            }
            if (skippedUnavailableHistoryImage) {
                if (!text.empty()) text += "\n";
                text += "[One or more previously attached images are no longer available.]";
            }
            if (!text.empty()) content.push_back({{"type", "text"}, {"text", std::move(text)}});
            for (auto& image : images) content.push_back(std::move(image));
            messageNode["content"] = std::move(content);
        } else if (!message.content.empty() || message.invocations.empty()) {
            messageNode["content"] = message.content;
        }
        // 线上字段名是 tool_call_id（snake_case），不是我们结构体里那个 toolCallId。
        // 写错的话工具结果和它对应的调用就对不上——服务端要么直接 400，
        // 要么模型认不出这是哪次调用的结果，下一轮把同样的工具再调一遍。
        if (!message.toolCallId.empty()) messageNode["tool_call_id"] = message.toolCallId;
        if (!message.invocations.empty()) {
            json calls = json::array();
            for (const auto& choice : message.invocations) {
                calls.push_back(
                    {{"id", choice.id},
                     {"type", "function"},
                     {"function", {{"name", choice.name}, {"arguments", choice.arguments}}}});
            }
            messageNode["tool_calls"] = std::move(calls);
        }
        msgs.push_back(std::move(messageNode));
    }

    json body{{"model", request.model}, {"messages", std::move(msgs)}, {"stream", true}};
    if (request.temperature >= 0.0) body["temperature"] = request.temperature;

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& toolCall : request.tools) {
            json params = json::parse(toolCall.parametersJson, nullptr, /*allow_exceptions=*/false);
            if (params.is_discarded()) params = json::object();
            tools.push_back({{"type", "function"},
                             {"function",
                              {{"name", toolCall.name},
                               {"description", toolCall.description},
                               {"parameters", std::move(params)}}}});
        }
        body["tools"] = std::move(tools);
    }
    // 旧版本在 Windows 上曾把 cmd.exe 的本地代码页字节直接存进工具输出。那些历史记录
    // 不是合法 UTF-8；默认 dump 会抛 type_error，而异常逃出轮次线程会终止整个进程。
    // 边界层必须能读取旧数据：非法序列替换成 U+FFFD，保证发出去的 JSON 始终合法。
    return body.dump(-1, ' ', false, json::error_handler_t::replace);
}

// ── curl 回调的上下文 ───────────────────────────────────────────
struct StreamCtx {
    const MaiStreamSink* sink = nullptr;
    const std::atomic<bool>* cancel = nullptr;
    LineBuffer lines;
    InvocationAccumulator tools;
    std::string error;
    std::string rawBody;
    std::string finishReason;
    bool sawOutput = false;
    bool done = false;
    long inactivityTimeoutSeconds = 0;
    std::chrono::steady_clock::time_point lastModelProgress = std::chrono::steady_clock::now();
    bool inactive = false;
    std::size_t responseBytes = 0;
    curl_off_t uploadedBytes = 0;
};

std::string providerErrorMessage(const std::string& body) {
    const json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return {};
    if (!parsed.contains("error")) return {};
    const json& error = parsed["error"];
    if (error.is_string()) return error.get<std::string>();
    if (!error.is_object()) return error.dump();
    const std::string message = error.value("message", std::string{});
    const std::string code = error.value("code", std::string{});
    if (message.empty()) return code;
    return code.empty() ? message : message + " (" + code + ")";
}

void handleSseLine(StreamCtx& context, const std::string& line) {
    if (line.empty()) return;
    if (line[0] == ':') return;               // 注释 / 心跳
    if (line.rfind("data:", 0) != 0) return;  // 只关心 data 行

    std::string payload = line.substr(5);
    if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
    if (payload == "[DONE]") {
        context.done = true;
        context.lastModelProgress = std::chrono::steady_clock::now();
        return;
    }

    // 服务端可能推来半截或畸形 JSON，不能让它把整轮搞崩。
    const json parsed = json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return;

    // 有的服务端把错误塞在正常流里而不是用 HTTP 状态码。
    if (parsed.contains("error")) {
        context.error = parsed["error"].is_string() ? parsed["error"].get<std::string>()
                                                    : parsed["error"].dump();
        return;
    }

    if (!parsed.contains("choices") || !parsed["choices"].is_array() || parsed["choices"].empty())
        return;
    const auto& choice = parsed["choices"][0];
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
        context.finishReason = choice["finish_reason"].get<std::string>();
    if (!choice.contains("delta")) return;
    const auto& delta = choice["delta"];

    if (delta.contains("content") && delta["content"].is_string()) {
        const auto slot = delta["content"].get<std::string>();
        if (!slot.empty()) {
            context.sawOutput = true;
            context.lastModelProgress = std::chrono::steady_clock::now();
            if (context.sink->onText) context.sink->onText(slot);
        }
    }
    // 推理增量各家字段名不统一，这两个是见得最多的。
    for (const char* key : {"reasoning_content", "reasoning"}) {
        if (delta.contains(key) && delta[key].is_string()) {
            const auto slot = delta[key].get<std::string>();
            if (!slot.empty()) {
                context.sawOutput = true;
                context.lastModelProgress = std::chrono::steady_clock::now();
                if (context.sink->onReasoning) context.sink->onReasoning(slot);
            }
        }
    }
    if (delta.contains("tool_calls") && delta["tool_calls"].is_array() &&
        !delta["tool_calls"].empty()) {
        context.sawOutput = true;
        context.lastModelProgress = std::chrono::steady_clock::now();
        context.tools.feed(delta["tool_calls"]);
    }
}

std::size_t writeCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto& context = *static_cast<StreamCtx*>(userdata);
    const std::size_t total = size * nmemb;
    context.responseBytes += total;
    // 返回不等于 total 的值会让 curl 以 CURLE_WRITE_ERROR 中断传输。取消和收到完整的
    // [DONE] 都在这里结束，不等服务端继续保持连接。
    if (context.cancel->load(std::memory_order_relaxed)) return 0;

    constexpr std::size_t kMaxCapturedBody = 64 * 1024;
    if (context.rawBody.size() < kMaxCapturedBody) {
        const std::size_t remaining = kMaxCapturedBody - context.rawBody.size();
        context.rawBody.append(ptr, std::min(total, remaining));
    }
    context.lines.append(ptr, total);
    std::string line;
    while (context.lines.nextLine(line)) handleSseLine(context, line);
    return context.done ? 0 : total;
}

int progressCallback(void* userdata, curl_off_t, curl_off_t uploadTotal, curl_off_t uploadNow) {
    auto& context = *static_cast<StreamCtx*>(userdata);
    if (context.cancel->load(std::memory_order_relaxed)) return 1;
    // A large multimodal POST can spend time uploading before the model can respond.
    // Count actual upload progress as activity, then enforce the response idle deadline.
    if (uploadTotal > 0 && uploadNow > context.uploadedBytes) {
        context.uploadedBytes = uploadNow;
        context.lastModelProgress = std::chrono::steady_clock::now();
    }
    if (context.inactivityTimeoutSeconds > 0 &&
        std::chrono::steady_clock::now() - context.lastModelProgress >=
            std::chrono::seconds(context.inactivityTimeoutSeconds)) {
        context.inactive = true;
        return 1;
    }
    return 0;
}

bool isRetryableCurlError(CURLcode code) {
    return code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
           code == CURLE_OPERATION_TIMEDOUT || code == CURLE_RECV_ERROR ||
           code == CURLE_SEND_ERROR || code == CURLE_GOT_NOTHING;
}

bool waitBeforeRetry(long delayMs, const std::atomic<bool>& cancel) {
    constexpr long kSliceMs = 25;
    long waited = 0;
    while (waited < delayMs) {
        if (cancel.load(std::memory_order_relaxed)) return false;
        const long slice = std::min(kSliceMs, delayMs - waited);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        waited += slice;
    }
    return !cancel.load(std::memory_order_relaxed);
}

// Chat Completions 的实现。Responses 在独立的 wire 文件里，
// 上层通过同一个 MaiModelClient 接口使用。
class ChatCompletionsClient final : public MaiModelClient {
public:
    explicit ChatCompletionsClient(MaiModelConfig config) : mConfig(std::move(config)) {}

    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        std::string url = mConfig.baseUrl;
        if (!url.empty() && url.back() == '/') url.pop_back();
        url += "/chat/completions";

        MaiResult<std::string> builtBody = buildRequestBody(
            request, [this](const std::string& source, const std::string& workspace) {
                return prepareImage(source, workspace);
            });
        if (!builtBody) return builtBody.error();
        const std::string body = std::move(builtBody.value());
        std::size_t imageCount = 0;
        for (const auto& message : request.messages) imageCount += message.images.size();
        for (int attempt = 0;; ++attempt) {
            CURL* curl = curl_easy_init();
            if (!curl) return MaiError::make(MaiErrorCode::Internal, "curl_easy_init failed");

            curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers, "Accept: text/event-stream");
            headers = curl_slist_append(headers, "Cache-Control: no-cache");
            std::string auth;
            if (!mConfig.apiKey.empty()) {
                auth = "Authorization: Bearer " + mConfig.apiKey;
                headers = curl_slist_append(headers, auth.c_str());
            }

            char transportError[CURL_ERROR_SIZE] = {};
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, transportError);
            StreamCtx context;
            context.sink = &sink;
            context.cancel = &cancel;
            context.inactivityTimeoutSeconds = mConfig.inactivityTimeoutSeconds;
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            if (!mConfig.caBundlePath.empty())
                curl_easy_setopt(curl, CURLOPT_CAINFO, mConfig.caBundlePath.c_str());
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
            // The progress callback also runs while the connection is silent. Write callbacks
            // cannot enforce cancellation or an idle deadline until another byte arrives.
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, mConfig.connectTimeoutSeconds);
            if (mConfig.totalTimeoutSeconds > 0)
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, mConfig.totalTimeoutSeconds);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "MaiAgent/0.1");

            const CURLcode curlResult = curl_easy_perform(curl);
            std::string finalLine;
            if (context.lines.takeRemainder(finalLine)) handleSseLine(context, finalLine);
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            if (cancel.load(std::memory_order_relaxed))
                return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
            if (context.inactive)
                return MaiError::make(
                    MaiErrorCode::Network,
                    "model stream inactive for " +
                        std::to_string(mConfig.inactivityTimeoutSeconds) + " seconds (request " +
                        std::to_string(body.size()) + " bytes, " + std::to_string(imageCount) +
                        " images; received " + std::to_string(context.responseBytes) +
                        " bytes). The provider did not start a model response; retry later or "
                        "choose another available model.");

            const bool transient = curlResult != CURLE_OK ? isRetryableCurlError(curlResult)
                                                          : status == 408 || status >= 500;
            if (transient && !context.sawOutput && attempt < mConfig.maxRetries) {
                const long delay = mConfig.retryInitialDelayMs * (1L << attempt);
                if (!waitBeforeRetry(delay, cancel))
                    return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
                continue;
            }

            if (curlResult != CURLE_OK && !(context.done && curlResult == CURLE_WRITE_ERROR))
                return MaiError::make(
                    MaiErrorCode::Network,
                    std::string("curl: ") +
                        (curlResult == CURLE_PEER_FAILED_VERIFICATION && transportError[0]
                             ? transportError
                             : curl_easy_strerror(curlResult)));
            if (status >= 400) {
                const std::string provider = providerErrorMessage(context.rawBody);
                std::string message = "HTTP " + std::to_string(status);
                if (!provider.empty()) message += ": " + provider;
                const MaiErrorCode code = status == 401 || status == 403
                                              ? MaiErrorCode::NotConfigured
                                          : status == 429 ? MaiErrorCode::RateLimited
                                          : status >= 500 ? MaiErrorCode::Network
                                                          : MaiErrorCode::Protocol;
                return MaiError::make(code, std::move(message));
            }
            if (!context.error.empty())
                return MaiError::make(MaiErrorCode::Protocol, context.error);
            if (context.finishReason == "length")
                return MaiError::make(MaiErrorCode::Protocol,
                                      "模型输出达到长度上限，回答可能不完整。");
            if (context.finishReason == "content_filter")
                return MaiError::make(MaiErrorCode::Protocol,
                                      "模型供应商拦截了这次输出（content_filter）。");

            if (sink.onToolCall) {
                for (const auto& choice : context.tools.take()) sink.onToolCall(choice);
            }
            return MaiError::ok();
        }
    }

private:
    MaiResult<std::string> prepareImage(const std::string& source, const std::string& workspace) {
        if (!mConfig.prepareImage)
            return {MaiErrorCode::InvalidInput,
                    "image exceeds the 2 MB model request budget; resize it before retrying"};
        const std::string key = workspace + "\n" + source;
        std::lock_guard<std::mutex> lock(mPreviewMutex);
        const auto existing = mPreviewCache.find(key);
        if (existing != mPreviewCache.end() &&
            MaiFileSystem::exists(MaiFilePath::fromUtf8(existing->second)))
            return existing->second;
        MaiResult<std::string> result = mConfig.prepareImage(source, workspace);
        if (result) mPreviewCache[key] = result.value();
        return result;
    }

    MaiModelConfig mConfig;
    std::mutex mPreviewMutex;
    std::unordered_map<std::string, std::string> mPreviewCache;
};

struct CurlGlobal {
    CurlGlobal() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~CurlGlobal() {
        curl_global_cleanup();
    }
};

}  // namespace

std::unique_ptr<MaiModelClient> makeMaiModelClient(MaiModelConfig config) {
    // curl_global_init 不是线程安全的，用函数内静态保证只跑一次。
    static CurlGlobal once;
    (void)once;
    switch (config.wire) {
        case MaiWireApi::ChatCompletions:
            return std::make_unique<ChatCompletionsClient>(std::move(config));
        case MaiWireApi::Responses: return makeMaiResponsesClient(std::move(config));
    }
    return nullptr;
}

const char* maiWireApiToString(MaiWireApi wireApi) {
    switch (wireApi) {
        case MaiWireApi::ChatCompletions: return "chat_completions";
        case MaiWireApi::Responses: return "responses";
    }
    return "chat_completions";
}
