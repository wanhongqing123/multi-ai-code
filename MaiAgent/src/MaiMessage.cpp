#include "MaiMessage.h"

const char* maiToolStateToString(MaiToolState state) {
    switch (state) {
        case MaiToolState::Pending: return "pending";
        case MaiToolState::Running: return "running";
        case MaiToolState::Completed: return "completed";
        case MaiToolState::Error: return "error";
    }
    return "pending";
}

const char* maiRoleToString(MaiRole role) {
    return role == MaiRole::User ? "user" : "assistant";
}

bool MaiMessage::isInProgress() const {
    return completed == 0;
}

std::string MaiMessage::text() const {
    std::string out;
    for (const auto& part : parts) {
        if (const auto* text = std::get_if<MaiTextPart>(&part.body)) {
            if (!out.empty()) out += "\n";
            out += text->text;
        }
    }
    return out;
}
