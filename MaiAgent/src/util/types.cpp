#include "mai/types.h"

#include <chrono>

namespace mai {

Millis now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

const char* to_string(ErrorCode c) {
  switch (c) {
    case ErrorCode::Ok:            return "ok";
    case ErrorCode::NotFound:      return "not_found";
    case ErrorCode::Busy:          return "busy";
    case ErrorCode::InvalidInput:  return "invalid_input";
    case ErrorCode::NotConfigured: return "not_configured";
    case ErrorCode::Network:       return "network";
    case ErrorCode::Protocol:      return "protocol";
    case ErrorCode::Canceled:      return "canceled";
    case ErrorCode::Internal:      return "internal";
  }
  return "internal";
}

}  // namespace mai
