#include "MaiError.h"

const char* maiErrorCodeToString(MaiErrorCode code) {
    switch (code) {
        case MaiErrorCode::Ok: return "ok";
        case MaiErrorCode::NotFound: return "not_found";
        case MaiErrorCode::Busy: return "busy";
        case MaiErrorCode::InvalidInput: return "invalid_input";
        case MaiErrorCode::NotConfigured: return "not_configured";
        case MaiErrorCode::Network: return "network";
        case MaiErrorCode::Protocol: return "protocol";
        case MaiErrorCode::Canceled: return "canceled";
        case MaiErrorCode::NotSupported: return "not_supported";
        case MaiErrorCode::Internal: return "internal";
    }
    return "internal";
}

MaiError::MaiError(MaiErrorCode code, std::string message)
    : mCode(code), mMessage(std::move(message)) {}

MaiError MaiError::ok() {
    return MaiError{};
}

MaiError MaiError::make(MaiErrorCode code, std::string message) {
    return MaiError{code, std::move(message)};
}

MaiErrorCode MaiError::code() const {
    return mCode;
}

const std::string& MaiError::message() const {
    return mMessage;
}

bool MaiError::hasError() const {
    return mCode != MaiErrorCode::Ok;
}

MaiError::operator bool() const {
    return hasError();
}
