#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiCreativeMediaSupport.h"
#include "MaiTool.h"

// Credentials are read at call time so a host can update or revoke them without recreating the
// Agent. The workspace ID is the Model Studio workspace, not a local file path. Both values must
// belong to the same Beijing region. This callback runs on a tool worker, never on a UI thread.
struct MaiWanCredentials {
    std::string apiKey;
    std::string workspaceId;
};
using MaiWanCredentialsProvider = std::function<MaiWanCredentials()>;

// Wan3.0 根据文本、首尾帧或参考素材生成视频；本地素材同样经私有 OSS 上传。任务异步提交；
// the completed MP4 is downloaded to the Agent workspace by continue().
std::unique_ptr<MaiTool> makeMaiWanVideoTool(MaiWanCredentialsProvider credentials,
                                             std::string caBundlePath = {},
                                             MaiCreativeMediaUploadProvider uploadMedia = {});

// 百炼可灵视频与万相共用北京地域 Key/Workspace，但需单独开通可灵模型权限。
// 本地 JPEG/PNG 和 MP4/MOV 经宿主私有 OSS 直传，再提交异步任务；无上传通道时
// 文生视频仍可用，媒体输入会在付费提交前被拒绝。App 只注册此百炼可灵入口。
std::unique_ptr<MaiTool> makeMaiBailianKlingVideoTool(
    MaiWanCredentialsProvider credentials, std::string caBundlePath = {},
    MaiCreativeMediaUploadProvider uploadMedia = {});

// Wan2.7-Image 接受文本及最多九张本地参考图；图片先上传私有 OSS，模型只收到 HTTPS
// 地址。同步生成一张工作区 PNG，原图不修改；调用可能阻塞，必须在工具工作线程执行。
std::unique_ptr<MaiTool> makeMaiWanImageTool(MaiWanCredentialsProvider credentials,
                                             std::string caBundlePath = {},
                                             MaiCreativeMediaUploadProvider uploadMedia = {});

// 百炼可灵图像：标准版文生/单图，Omni 支持多图。异步任务保存 ID 并由 continue
// 下载 PNG；只生成一张，原图不修改。本地参考图使用与万相一致的私有 OSS 回调。
std::unique_ptr<MaiTool> makeMaiBailianKlingImageTool(
    MaiWanCredentialsProvider credentials, std::string caBundlePath = {},
    MaiCreativeMediaUploadProvider uploadMedia = {});
