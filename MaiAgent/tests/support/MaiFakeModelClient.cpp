#include "MaiFakeModelClient.h"

#include <chrono>
#include <thread>

MaiFakeModelClient::MaiFakeModelClient(std::vector<Turn> script) : mScript(std::move(script)) {}

void MaiFakeModelClient::setRepeatingTurn(Turn turn) {
    std::lock_guard<std::mutex> lock(mMutex);
    mRepeatingTurn = std::move(turn);
    mRepeat = true;
}

MaiError MaiFakeModelClient::stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                                    const std::atomic<bool>& cancel) {
    Turn turn;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const std::size_t index = mRequests.size();
        mRequests.push_back(request);
        // 剧本用完之后一律回空应答。让它静默收场而不是断言失败，是因为
        // "模型比预期多问了一轮"这件事该由用例自己去查 requestCount()，
        // 在这里炸掉只会把失败现场变成一个看不懂的崩溃。
        if (mRepeat) {
            turn = mRepeatingTurn;
        } else if (index < mScript.size()) {
            turn = mScript[index];
        }
    }

    if (turn.error) return turn.error;

    if (!turn.reasoning.empty() && sink.onReasoning) sink.onReasoning(turn.reasoning);

    for (const auto& chunk : turn.textChunks) {
        // 每片之前查一次取消。真实客户端是在 curl 的写回调里查的，
        // 时机一样：正在往外吐的时候能被叫停。
        if (cancel.load(std::memory_order_relaxed))
            return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
        if (turn.chunkDelayMilliseconds > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(turn.chunkDelayMilliseconds));
        if (sink.onText) sink.onText(chunk);
    }

    if (cancel.load(std::memory_order_relaxed))
        return MaiError::make(MaiErrorCode::Canceled, "canceled by user");

    // 工具调用攒完整才交付，和真实客户端的契约一致（见 MaiStreamSink）。
    for (const auto& invocation : turn.invocations) {
        if (sink.onToolCall) sink.onToolCall(invocation);
    }
    return {};
}

MaiWireApi MaiFakeModelClient::wireApi() const {
    return MaiWireApi::ChatCompletions;
}

std::size_t MaiFakeModelClient::requestCount() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mRequests.size();
}

MaiModelRequest MaiFakeModelClient::request(std::size_t index) const {
    std::lock_guard<std::mutex> lock(mMutex);
    if (index >= mRequests.size()) return {};
    return mRequests[index];
}

MaiModelRequest MaiFakeModelClient::lastRequest() const {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mRequests.empty()) return {};
    return mRequests.back();
}
