#pragma once

#include <functional>
#include <optional>
#include <string>

#include "MaiTool.h"

// 创作工具可选的私有素材上传入口。传入工作区路径，返回厂商能够读取的短期 HTTPS
// 地址；回调只在工具工作线程调用，可阻塞且必须响应取消。空回调表示素材上传未配置，
// 有本地图片或视频输入的付费任务必须在提交前失败。带签名地址不能写进任务记录或回灌给主模型。
using MaiCreativeMediaUploadProvider =
    std::function<MaiResult<std::string>(const std::string&, const MaiToolContext&)>;

// Internal transport helpers for model-backed media tools. Hosts must call these only on a worker
// thread. The bearer key stays inside the HTTP request and is never placed in returned text.
struct MaiCreativeHttpResult {
    std::string body;
    std::optional<MaiToolResult> error;
};

MaiToolResult maiCreativeFailure(MaiErrorCode error, const char* code, const std::string& message);
MaiToolResult maiCreativeInvalid(const std::string& message);
bool maiCreativeValidId(const std::string& id);
// 上传回调应返回没有用户信息、带有效主机名的 HTTPS 地址；检查失败时不得提交付费任务。
bool maiCreativeHttpsUrl(const std::string& url);

// A null body sends GET; a non-null body sends JSON POST. Responses are bounded to 2 MB.
MaiCreativeHttpResult maiCreativeRequestJson(const std::string& url, const std::string& key,
                                             const std::string& caBundle, const std::string* body,
                                             const MaiToolContext& context);

// Upload an in-memory file as multipart/form-data. The caller validates the workspace path and
// file size before reading it. This helper does not persist or expose the bearer key.
MaiCreativeHttpResult maiCreativeUploadFile(const std::string& url, const std::string& key,
                                            const std::string& caBundle,
                                            const std::string& filename, const std::string& bytes,
                                            const std::string& purpose,
                                            const MaiToolContext& context);

// Download to a new workspace path, validate its media signature, and publish atomically.
MaiToolResult maiCreativeDownloadMedia(const std::string& url, bool video,
                                       const std::string& provider, const MaiToolContext& context,
                                       const std::string& caBundle);
