#include "agent/session_titler.h"

namespace mai::internal {

std::string SessionTitler::make_title(const std::string& text) const {
  if (text.size() <= options_.max_bytes) return text;
  // 退到 UTF-8 字符边界。按字节硬切会切出半个汉字，
  // 后面 JSON 序列化会失败或者在界面上显示成乱码。
  std::size_t cut = options_.max_bytes;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  return text.substr(0, cut) + "…";
}

std::string SessionTitler::apply(SessionStore& store,
                                 const std::string& session_id) const {
  Session snapshot;
  if (!store.get_session(session_id, snapshot)) return {};
  if (!snapshot.untitled()) return {};

  std::string title;
  for (const auto& m : store.list_messages(session_id)) {
    if (m.role != Role::User) continue;
    title = make_title(m.text());
    break;
  }
  if (title.empty()) return {};

  // 通过 mutate_session 而不是"读出来改完写回去"：
  // 那样会把这期间别人对 model/agent 的修改盖掉（lost update）。
  std::string applied;
  store.mutate_session(session_id, [&](Session& s) {
    if (!s.untitled()) return;  // 锁内再查一次，可能已经被别人命名了
    s.title = title;
    s.updated = now_ms();
    applied = title;
  });
  return applied;
}

}  // namespace mai::internal
