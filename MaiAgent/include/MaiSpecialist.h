#pragma once

#include <string>
#include <vector>

// 当前设备与账号对某项能力的可用程度。供应商模型或 API 理论上支持，
// 并不意味着 App 已经打通素材上传、参数校验、付费提交及结果交付整条链路。
enum class MaiSpecialistCapabilityStatus {
    Available,
    ImplementedUnverified,
    NotConfigured,
    NotImplemented,
    UploadNotConfigured,
};

// 带模型工具的稳定能力条目，不是供应商 HTTP 字段，也不是直接交给主模型的
// JSON Schema。modelSupported/apiSupported 说明厂商侧能力，status 说明本工具
// 实际接线与配置状态，limitation 要把数量、组合、费用或未实现限制写清楚。
// 不能仅凭厂商功能列表就标成 Available；必须考虑当前账号和端侧实现。
struct MaiSpecialistCapability {
    std::string id;
    bool modelSupported = false;
    bool apiSupported = false;
    MaiSpecialistCapabilityStatus status = MaiSpecialistCapabilityStatus::NotImplemented;
    std::string limitation;
};

// 一个已注册的专业模型工具及其能力快照。不得包含密钥或用户媒体字节。
// 需在工具工作线程查询，因为凭据提供器可能跨越宿主语言边界；结果反映当前
// 配置，运行期间可能变化，不必重新编译 Agent。工具名和能力 ID 跨平台保持稳定。
struct MaiSpecialistInfo {
    std::string toolName;
    std::string modelId;
    bool configured = false;
    std::vector<MaiSpecialistCapability> capabilities;
};

const char* maiSpecialistCapabilityStatusToString(MaiSpecialistCapabilityStatus status);
