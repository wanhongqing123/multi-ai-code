#include "mai/message.h"

namespace mai {

const char* to_string(ToolState s) {
  switch (s) {
    case ToolState::Pending:   return "pending";
    case ToolState::Running:   return "running";
    case ToolState::Completed: return "completed";
    case ToolState::Error:     return "error";
  }
  return "pending";
}

const char* to_string(Role r) { return r == Role::User ? "user" : "assistant"; }

std::string Message::text() const {
  std::string out;
  for (const auto& p : parts) {
    if (const auto* t = std::get_if<TextPart>(&p.body)) {
      if (!out.empty()) out += "\n";
      out += t->text;
    }
  }
  return out;
}

}  // namespace mai
