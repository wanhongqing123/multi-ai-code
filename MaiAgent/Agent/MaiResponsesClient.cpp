#include "MaiResponsesClient.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"

namespace {

using Json = nlohmann::json;

std::string stringField(const Json& object, const char* field) {
    return object.is_object() && object.contains(field) && object[field].is_string()
               ? object[field].get<std::string>()
               : std::string{};
}

class LineBuffer {
public:
    void append(const char* bytes, std::size_t size) {
        mBytes.append(bytes, size);
    }

    bool next(std::string& line) {
        const std::size_t end = mBytes.find('\n', mScanned);
        if (end == std::string::npos) {
            mScanned = mBytes.size();
            return false;
        }
        line.assign(mBytes, 0, end);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        mBytes.erase(0, end + 1);
        mScanned = 0;
        return true;
    }

    bool takeRemainder(std::string& line) {
        if (mBytes.empty()) return false;
        line = std::move(mBytes);
        mBytes.clear();
        mScanned = 0;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    }

private:
    std::string mBytes;
    std::size_t mScanned = 0;
};

std::string base64Encode(const std::string& input) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto first = static_cast<unsigned char>(input[offset]);
        const auto second =
            offset + 1 < input.size() ? static_cast<unsigned char>(input[offset + 1]) : 0;
        const auto third =
            offset + 2 < input.size() ? static_cast<unsigned char>(input[offset + 2]) : 0;
        output.push_back(alphabet[first >> 2]);
        output.push_back(alphabet[((first & 0x03) << 4) | (second >> 4)]);
        output.push_back(offset + 1 < input.size() ? alphabet[((second & 0x0f) << 2) | (third >> 6)]
                                                   : '=');
        output.push_back(offset + 2 < input.size() ? alphabet[third & 0x3f] : '=');
    }
    return output;
}

MaiResult<std::string> imageDataUrl(const MaiModelRequest& request, const MaiModelImage& image,
                                    const MaiModelImagePreparer& prepareImage) {
    if (request.workingDirectory.empty())
        return {MaiErrorCode::InvalidInput, "an image prompt requires a working directory"};
    if (image.mimeType != "image/jpeg" && image.mimeType != "image/png" &&
        image.mimeType != "image/gif" && image.mimeType != "image/webp")
        return {MaiErrorCode::InvalidInput, "unsupported prompt image type: " + image.mimeType};
    const MaiFilePath candidate = MaiFilePath::fromUtf8(image.path);
    const std::string resolved =
        candidate.isAbsolute() ? MaiFileSystem::resolve(candidate).toUtf8()
                               : maiResolvePathWithinRoot(request.workingDirectory, image.path);
    if (resolved.empty())
        return {MaiErrorCode::InvalidInput,
                "prompt image is outside the working directory: " + image.path};
    constexpr std::uint64_t kMaxSourceBytes = 20u * 1024u * 1024u;
    constexpr std::uint64_t kMaxRequestBytes = 2u * 1024u * 1024u;
    const MaiFilePath path = MaiFilePath::fromUtf8(resolved);
    std::uint64_t sourceSize = 0;
    const bool knownSize = MaiFileSystem::fileSize(path, sourceSize);
    if (knownSize && sourceSize > kMaxSourceBytes)
        return {MaiErrorCode::InvalidInput, "prompt image exceeds the 20 MB limit: " + image.path};
    std::string bytes;
    bool truncated = false;
    const bool largeSource = knownSize && sourceSize > kMaxRequestBytes;
    if (!largeSource) {
        const MaiError readError =
            MaiFileSystem::readFile(path, bytes, kMaxSourceBytes, &truncated);
        if (readError) return readError;
        if (truncated || bytes.empty())
            return {MaiErrorCode::InvalidInput,
                    "prompt image is empty or exceeds the 20 MB limit: " + image.path};
    }
    std::string mime = image.mimeType;
    if (largeSource || bytes.size() > kMaxRequestBytes) {
        if (!prepareImage)
            return {MaiErrorCode::InvalidInput,
                    "image exceeds the 2 MB model request budget; resize it before retrying"};
        MaiResult<std::string> preview = prepareImage(resolved, request.workingDirectory);
        if (!preview) return preview.error();
        if (preview.value() == resolved)
            return {MaiErrorCode::Internal, "the model preview must not reuse the original image"};
        bytes.clear();
        truncated = false;
        const MaiError readError = MaiFileSystem::readFile(MaiFilePath::fromUtf8(preview.value()),
                                                           bytes, kMaxRequestBytes, &truncated);
        if (readError) return readError;
        if (truncated || bytes.empty())
            return {MaiErrorCode::InvalidInput,
                    "the prepared model image is empty or exceeds the 2 MB request budget"};
        mime = "image/jpeg";
    }
    return "data:" + mime + ";base64," + base64Encode(bytes);
}

const char* roleName(MaiModelRole role) {
    switch (role) {
        case MaiModelRole::System: return "system";
        case MaiModelRole::User: return "user";
        case MaiModelRole::Assistant: return "assistant";
        case MaiModelRole::ToolResult: return "user";
    }
    return "user";
}

MaiResult<std::string> buildBody(const MaiModelRequest& request,
                                 const MaiModelImagePreparer& prepareImage) {
    Json input = Json::array();
    std::size_t latestUser = request.messages.size();
    for (std::size_t index = 0; index < request.messages.size(); ++index)
        if (request.messages[index].role == MaiModelRole::User) latestUser = index;
    for (std::size_t index = 0; index < request.messages.size(); ++index) {
        const MaiModelMessage& message = request.messages[index];
        if (message.role == MaiModelRole::ToolResult && message.toolCallId.empty())
            return {MaiErrorCode::Protocol, "a tool result needs its call ID"};
        if (!message.images.empty() && message.role != MaiModelRole::User &&
            message.role != MaiModelRole::ToolResult)
            return {MaiErrorCode::InvalidInput,
                    "Responses accepts image parts only in user or tool-result input"};

        Json content;
        if (!message.images.empty()) {
            content = Json::array();
            std::string text = message.content;
            Json images = Json::array();
            for (const MaiModelImage& image : message.images) {
                MaiResult<std::string> encoded = imageDataUrl(request, image, prepareImage);
                if (!encoded) {
                    if (message.role == MaiModelRole::ToolResult || index == latestUser ||
                        encoded.error().code() != MaiErrorCode::NotFound)
                        return encoded.error();
                    if (!text.empty()) text += "\n";
                    text += "[A previously attached image is no longer available.]";
                    continue;
                }
                images.push_back(Json{{"type", "input_image"}, {"image_url", encoded.value()}});
            }
            if (!text.empty()) content.push_back(Json{{"type", "input_text"}, {"text", text}});
            for (Json& image : images) content.push_back(std::move(image));
        } else {
            content = message.content;
        }

        if (message.role == MaiModelRole::ToolResult) {
            input.push_back(Json{{"type", "function_call_output"},
                                 {"call_id", message.toolCallId},
                                 {"output", std::move(content)}});
            continue;
        }
        if (message.role == MaiModelRole::Assistant && !message.reasoning.empty()) {
            input.push_back(Json{{"type", "reasoning"},
                                 {"content", Json::array({Json{{"type", "reasoning_text"},
                                                               {"text", message.reasoning}}})}});
        }
        if (!message.content.empty() || message.invocations.empty() || !message.images.empty())
            input.push_back(Json{{"type", "message"},
                                 {"role", roleName(message.role)},
                                 {"content", std::move(content)}});
        for (const MaiToolInvocation& call : message.invocations) {
            if (call.id.empty() || call.name.empty())
                return {MaiErrorCode::Protocol, "a function call needs an ID and name"};
            input.push_back(Json{{"type", "function_call"},
                                 {"call_id", call.id},
                                 {"name", call.name},
                                 {"arguments", call.arguments}});
        }
    }

    Json body{{"model", request.model}, {"input", std::move(input)}, {"stream", true}};
    if (!request.baseInstructions.empty()) body["instructions"] = request.baseInstructions;
    if (request.temperature >= 0.0) body["temperature"] = request.temperature;
    if (!request.tools.empty()) {
        Json tools = Json::array();
        for (const MaiToolSpec& tool : request.tools) {
            Json parameters = Json::parse(tool.parametersJson, nullptr, false);
            if (parameters.is_discarded()) parameters = Json::object();
            tools.push_back(Json{{"type", "function"},
                                 {"name", tool.name},
                                 {"description", tool.description},
                                 {"parameters", std::move(parameters)}});
        }
        body["tools"] = std::move(tools);
    }
    return body.dump(-1, ' ', false, Json::error_handler_t::replace);
}

struct PendingCall {
    std::string id;
    std::string name;
    std::string arguments;
    bool complete = false;
};

struct StreamContext {
    const MaiStreamSink* sink = nullptr;
    const std::atomic<bool>* cancel = nullptr;
    LineBuffer lines;
    std::map<int, PendingCall> calls;
    std::set<std::pair<int, int>> textSeen;
    std::set<std::pair<int, int>> reasoningSeen;
    std::string rawBody;
    std::string terminalError;
    bool sawOutput = false;
    bool done = false;
    bool inactive = false;
    long inactivityTimeoutSeconds = 0;
    std::chrono::steady_clock::time_point lastModelProgress = std::chrono::steady_clock::now();
    std::size_t responseBytes = 0;
    curl_off_t uploadedBytes = 0;
};

void mergeCall(PendingCall& call, const Json& item) {
    if (!item.is_object()) return;
    const std::string id = stringField(item, "call_id");
    const std::string name = stringField(item, "name");
    if (!id.empty()) call.id = id;
    if (!name.empty()) call.name = name;
    if (item.contains("arguments") && item["arguments"].is_string()) {
        const std::string full = item["arguments"].get<std::string>();
        if (!full.empty() || call.arguments.empty()) call.arguments = full;
    }
}

void emitFullOutput(StreamContext& context, const Json& response) {
    if (!response.is_object() || !response.value("output", Json{}).is_array()) return;
    const Json& output = response["output"];
    for (std::size_t index = 0; index < output.size(); ++index) {
        const Json& item = output[index];
        const std::string type = stringField(item, "type");
        if (type == "function_call") {
            mergeCall(context.calls[static_cast<int>(index)], item);
            context.calls[static_cast<int>(index)].complete = true;
            continue;
        }
        if ((type != "message" && type != "reasoning") || !item.value("content", Json{}).is_array())
            continue;
        const Json& parts = item["content"];
        for (std::size_t partIndex = 0; partIndex < parts.size(); ++partIndex) {
            const std::string text = stringField(parts[partIndex], "text");
            if (text.empty()) continue;
            const auto key = std::make_pair(static_cast<int>(index), static_cast<int>(partIndex));
            if (type == "message" && context.textSeen.insert(key).second) {
                context.sawOutput = true;
                if (context.sink->onText) context.sink->onText(text);
            } else if (type == "reasoning" && context.reasoningSeen.insert(key).second) {
                context.sawOutput = true;
                if (context.sink->onReasoning) context.sink->onReasoning(text);
            }
        }
    }
}

void handleEvent(StreamContext& context, const Json& event) {
    const std::string type = stringField(event, "type");
    if (type.empty()) return;
    const int outputIndex = event.value("output_index", -1);
    const int contentIndex = event.value("content_index", 0);
    const auto key = std::make_pair(outputIndex, contentIndex);
    if (type == "response.output_text.delta" || type == "response.reasoning_text.delta") {
        const std::string delta = stringField(event, "delta");
        if (delta.empty()) return;
        context.sawOutput = true;
        context.lastModelProgress = std::chrono::steady_clock::now();
        if (type == "response.output_text.delta") {
            context.textSeen.insert(key);
            if (context.sink->onText) context.sink->onText(delta);
        } else {
            context.reasoningSeen.insert(key);
            if (context.sink->onReasoning) context.sink->onReasoning(delta);
        }
        return;
    }
    if (type == "response.output_text.done" || type == "response.reasoning_text.done") {
        const std::string text = stringField(event, "text");
        if (text.empty()) return;
        if (type == "response.output_text.done" && context.textSeen.insert(key).second) {
            context.sawOutput = true;
            if (context.sink->onText) context.sink->onText(text);
        } else if (type == "response.reasoning_text.done" &&
                   context.reasoningSeen.insert(key).second) {
            context.sawOutput = true;
            if (context.sink->onReasoning) context.sink->onReasoning(text);
        }
        context.lastModelProgress = std::chrono::steady_clock::now();
        return;
    }
    if (type == "response.output_item.added" || type == "response.output_item.done") {
        const Json item = event.value("item", Json::object());
        if (stringField(item, "type") == "function_call" && outputIndex >= 0) {
            PendingCall& call = context.calls[outputIndex];
            mergeCall(call, item);
            if (type == "response.output_item.done") call.complete = true;
            context.sawOutput = true;
            context.lastModelProgress = std::chrono::steady_clock::now();
        }
        return;
    }
    if (type == "response.function_call_arguments.delta" && outputIndex >= 0) {
        PendingCall& call = context.calls[outputIndex];
        if (!call.complete) call.arguments += stringField(event, "delta");
        context.sawOutput = true;
        context.lastModelProgress = std::chrono::steady_clock::now();
        return;
    }
    if (type == "response.function_call_arguments.done" && outputIndex >= 0) {
        PendingCall& call = context.calls[outputIndex];
        const std::string full = stringField(event, "arguments");
        if (!full.empty() || call.arguments.empty()) call.arguments = full;
        call.complete = true;
        context.sawOutput = true;
        context.lastModelProgress = std::chrono::steady_clock::now();
        return;
    }
    if (type == "response.completed" || type == "response.incomplete" ||
        type == "response.failed") {
        const Json response = event.value("response", Json::object());
        emitFullOutput(context, response);
        if (type != "response.completed") {
            const Json error = response.value("error", Json::object());
            const Json details = response.value("incomplete_details", Json::object());
            context.terminalError = stringField(error, "message");
            if (context.terminalError.empty())
                context.terminalError = stringField(details, "reason");
            if (context.terminalError.empty()) context.terminalError = type;
        }
        context.done = true;
        context.lastModelProgress = std::chrono::steady_clock::now();
    }
}

void handleLine(StreamContext& context, const std::string& line) {
    if (line.empty() || line[0] == ':' || line.rfind("data:", 0) != 0) return;
    std::string payload = line.substr(5);
    if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
    const Json event = Json::parse(payload, nullptr, false);
    if (event.is_object()) handleEvent(context, event);
}

std::size_t writeCallback(char* bytes, std::size_t size, std::size_t count, void* user) {
    auto& context = *static_cast<StreamContext*>(user);
    if (count != 0 && size > SIZE_MAX / count) return 0;
    const std::size_t length = size * count;
    context.responseBytes += length;
    if (context.cancel->load(std::memory_order_relaxed)) return 0;
    constexpr std::size_t kMaxErrorBody = 64 * 1024;
    if (context.rawBody.size() < kMaxErrorBody)
        context.rawBody.append(bytes, std::min(length, kMaxErrorBody - context.rawBody.size()));
    context.lines.append(bytes, length);
    std::string line;
    while (context.lines.next(line)) handleLine(context, line);
    return context.done ? 0 : length;
}

int progressCallback(void* user, curl_off_t, curl_off_t uploadTotal, curl_off_t uploadNow) {
    auto& context = *static_cast<StreamContext*>(user);
    if (context.cancel->load(std::memory_order_relaxed)) return 1;
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

bool isRetryable(CURLcode code) {
    return code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
           code == CURLE_OPERATION_TIMEDOUT || code == CURLE_RECV_ERROR ||
           code == CURLE_SEND_ERROR || code == CURLE_GOT_NOTHING;
}

std::string providerError(const std::string& body) {
    const Json parsed = Json::parse(body, nullptr, false);
    if (!parsed.is_object()) return {};
    const Json error = parsed.value("error", Json{});
    if (error.is_string()) return error.get<std::string>();
    const std::string message = stringField(error, "message");
    const std::string code = stringField(error, "code");
    return message.empty() ? code : code.empty() ? message : message + " (" + code + ")";
}

class ResponsesClient final : public MaiModelClient {
public:
    explicit ResponsesClient(MaiModelConfig config) : mConfig(std::move(config)) {}

    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        MaiResult<std::string> built =
            buildBody(request, [this](const std::string& source, const std::string& workspace) {
                return prepareImage(source, workspace);
            });
        if (!built) return built.error();
        const std::string body = std::move(built.value());
        std::string url = mConfig.baseUrl;
        if (!url.empty() && url.back() == '/') url.pop_back();
        url += "/responses";
        for (int attempt = 0;; ++attempt) {
            CURL* curl = curl_easy_init();
            if (!curl) return MaiError::make(MaiErrorCode::Internal, "curl_easy_init failed");
            curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers, "Accept: text/event-stream");
            headers = curl_slist_append(headers, "Cache-Control: no-cache");
            if (!mConfig.apiKey.empty())
                headers =
                    curl_slist_append(headers, ("Authorization: Bearer " + mConfig.apiKey).c_str());
            StreamContext context;
            context.sink = &sink;
            context.cancel = &cancel;
            context.inactivityTimeoutSeconds = mConfig.inactivityTimeoutSeconds;
            char transportError[CURL_ERROR_SIZE] = {};
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, transportError);
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            if (!mConfig.caBundlePath.empty())
                curl_easy_setopt(curl, CURLOPT_CAINFO, mConfig.caBundlePath.c_str());
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(body.size()));
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
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
            const CURLcode transfer = curl_easy_perform(curl);
            std::string remainder;
            if (context.lines.takeRemainder(remainder)) handleLine(context, remainder);
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            if (cancel.load(std::memory_order_relaxed))
                return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
            if (context.inactive)
                return MaiError::make(MaiErrorCode::Network, "model stream inactive");
            const bool transient =
                transfer != CURLE_OK ? isRetryable(transfer) : status == 408 || status >= 500;
            if (transient && !context.sawOutput && attempt < mConfig.maxRetries) {
                const long delay = mConfig.retryInitialDelayMs * (1L << attempt);
                for (long waited = 0; waited < delay; waited += 25) {
                    if (cancel.load(std::memory_order_relaxed))
                        return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(std::min(25L, delay - waited)));
                }
                continue;
            }
            if (transfer != CURLE_OK && !(context.done && transfer == CURLE_WRITE_ERROR))
                return MaiError::make(
                    MaiErrorCode::Network,
                    std::string("curl: ") +
                        (transfer == CURLE_PEER_FAILED_VERIFICATION && transportError[0]
                             ? transportError
                             : curl_easy_strerror(transfer)));
            if (status >= 400) {
                const std::string detail = providerError(context.rawBody);
                const std::string message =
                    "HTTP " + std::to_string(status) + (detail.empty() ? "" : ": " + detail);
                const MaiErrorCode code = status == 401 || status == 403
                                              ? MaiErrorCode::NotConfigured
                                          : status == 429 ? MaiErrorCode::RateLimited
                                          : status >= 500 ? MaiErrorCode::Network
                                                          : MaiErrorCode::Protocol;
                return MaiError::make(code, message);
            }
            if (!context.done)
                return MaiError::make(MaiErrorCode::Protocol,
                                      "Responses stream ended without a terminal event");
            if (!context.terminalError.empty())
                return MaiError::make(MaiErrorCode::Protocol, context.terminalError);
            for (const auto& [_, call] : context.calls) {
                if (call.id.empty() || call.name.empty())
                    return MaiError::make(MaiErrorCode::Protocol,
                                          "Responses returned an incomplete function call");
                if (sink.onToolCall)
                    sink.onToolCall(MaiToolInvocation{call.id, call.name, call.arguments});
            }
            return {};
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

}  // namespace

std::unique_ptr<MaiModelClient> makeMaiResponsesClient(MaiModelConfig config) {
    return std::make_unique<ResponsesClient>(std::move(config));
}
