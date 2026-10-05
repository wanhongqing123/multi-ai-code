#pragma once

#include <string>
#include <vector>

// Availability of one capability on this device with the current account. Model and API support
// alone do not make a capability callable: the tool must implement and validate the full path.
enum class MaiSpecialistCapabilityStatus {
    Available,
    ImplementedUnverified,
    NotConfigured,
    NotImplemented,
    UploadNotConfigured,
};

// Stable capability metadata for a model-backed tool. These are domain values, not a provider's
// HTTP fields or the model-facing JSON schema. A specialist must not claim Available based only
// on a vendor feature list; runtime configuration and the implemented path also matter.
struct MaiSpecialistCapability {
    std::string id;
    bool modelSupported = false;
    bool apiSupported = false;
    MaiSpecialistCapabilityStatus status = MaiSpecialistCapabilityStatus::NotImplemented;
    std::string limitation;
};

// One registered expert, bound to a specific model. The key and any user media bytes must never
// appear here. Query it on a tool worker: credential providers may cross a host language boundary.
// Results reflect current credentials and may change without rebuilding the Agent. Tool names and
// capability IDs are stable across platforms.
struct MaiSpecialistInfo {
    std::string toolName;
    std::string modelId;
    bool configured = false;
    std::vector<MaiSpecialistCapability> capabilities;
};

const char* maiSpecialistCapabilityStatusToString(MaiSpecialistCapabilityStatus status);
