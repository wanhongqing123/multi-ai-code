#include "MaiSessionTitler.h"

MaiSessionTitler::MaiSessionTitler(Options options) : mOptions(options) {}

std::string MaiSessionTitler::makeTitle(const std::string& text) const {
    if (text.size() <= mOptions.maxBytes) return text;
    // 退到 UTF-8 字符边界。按字节硬切会切出半个汉字，后面 JSON 序列化会失败或者在界面上显示成乱码。
    std::size_t cut = mOptions.maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + "…";
}

std::string MaiSessionTitler::apply(MaiSessionStore& store, const std::string& sessionId) const {
    MaiSession snapshot;
    if (!store.getSession(sessionId, snapshot)) return {};
    if (!snapshot.isUntitled()) return {};

    std::string title;
    for (const auto& message : store.listMessages(sessionId)) {
        if (message.role != MaiRole::User) continue;
        title = makeTitle(message.text());
        break;
    }
    if (title.empty()) return {};

    // 通过 mutateSession 而不是"读出来改完写回去"：
    // 那样会把这期间别人对 model/agent 的修改盖掉（lost update）。
    std::string applied;
    store.mutateSession(sessionId, [&](MaiSession& session) {
        if (!session.isUntitled()) return;  // 锁内再查一次，可能已经被别人命名了
        session.title = title;
        session.updated = MaiTime::getCurrentTime();
        applied = title;
    });
    return applied;
}
