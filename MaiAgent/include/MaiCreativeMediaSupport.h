// 文件作用：复用创作媒体工具的路径、素材和响应处理规则。
// 对外类型：MaiCreativeHttpResult；线程、失败和生命周期以类型附近注释为准。
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

// 模型媒体工具内部使用的传输辅助入口，只能在宿主工作线程调用。
// Bearer 密钥只进入 HTTP 请求，不得写入返回文本。
struct MaiCreativeHttpResult {
    std::string body;
    std::optional<MaiToolResult> error;
};

MaiToolResult maiCreativeFailure(MaiErrorCode error, const char* code, const std::string& message);
MaiToolResult maiCreativeInvalid(const std::string& message);
bool maiCreativeValidId(const std::string& id);
// 上传回调应返回没有用户信息、带有效主机名的 HTTPS 地址；检查失败时不得提交付费任务。
bool maiCreativeHttpsUrl(const std::string& url);

// body 为空时发送 GET，否则发送 JSON POST；响应上限为 2 MB。
MaiCreativeHttpResult maiCreativeRequestJson(const std::string& url, const std::string& key,
                                             const std::string& caBundle, const std::string* body,
                                             const MaiToolContext& context);

// 以 multipart/form-data 上传内存文件；调用方须在读取前校验工作区路径与文件大小。
// 此入口不持久化或暴露 Bearer 密钥。
MaiCreativeHttpResult maiCreativeUploadFile(const std::string& url, const std::string& key,
                                            const std::string& caBundle,
                                            const std::string& filename, const std::string& bytes,
                                            const std::string& purpose,
                                            const MaiToolContext& context);

// 下载到新的工作区路径，校验媒体文件特征后原子交付。
MaiToolResult maiCreativeDownloadMedia(const std::string& url, bool video,
                                       const std::string& provider, const MaiToolContext& context,
                                       const std::string& caBundle);
