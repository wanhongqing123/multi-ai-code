#include "MaiIdGenerator.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>

namespace {

// Crockford base32 的字母表（去掉 I L O U，避免和 1 0 混淆）。和 ULID 用的是同一套，
// 以后真要换成标准 ULID 时不用动已有数据。
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::mt19937_64& rng() {
    // thread_local：每个线程一个，避免加锁。agent 是多会话并发的，
    // 生成 id 在流式期间每秒会被调用几十次，不值得为它争一把全局锁。
    static thread_local std::mt19937_64 gen(std::random_device{}());
    return gen;
}

void encode(std::string& out, std::uint64_t value, int chars) {
    const std::size_t start = out.size();
    out.append(static_cast<std::size_t>(chars), '0');
    for (int i = chars - 1; i >= 0; --i) {
        out[start + static_cast<std::size_t>(i)] = kAlphabet[value & 31u];
        value >>= 5;
    }
}

}  // namespace

std::string MaiIdGenerator::generate(const char* prefix) {
    using namespace std::chrono;
    const auto now = static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());

    // 同一毫秒内可能生成多个 id，用一个计数器保证单调，否则排序会打架。
    static std::atomic<std::uint64_t> counter{0};
    static std::atomic<std::uint64_t> last_ms{0};
    std::uint64_t seq = 0;
    std::uint64_t previous = last_ms.load(std::memory_order_relaxed);
    if (previous == now) {
        seq = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    } else {
        last_ms.store(now, std::memory_order_relaxed);
        counter.store(0, std::memory_order_relaxed);
    }

    std::string out;
    out.reserve(24);
    out += prefix;
    encode(out, now, 10);           // 50 bit 时间戳，够用到公元 10889 年
    encode(out, seq & 0xFFFFu, 3);  // 同毫秒序号
    encode(out, rng()(), 8);        // 随机尾巴
    return out;
}

std::string MaiIdGenerator::newSessionId() {
    return generate("ses_");
}

std::string MaiIdGenerator::newMessageId() {
    return generate("msg_");
}

std::string MaiIdGenerator::newPartId() {
    return generate("prt_");
}

std::string MaiIdGenerator::newEventId() {
    return generate("evt_");
}

std::string MaiIdGenerator::newPermissionId() {
    return generate("per_");
}

std::string MaiIdGenerator::newQuestionId() {
    return generate("qst_");
}
