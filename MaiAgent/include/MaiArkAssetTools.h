#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiCreativeMediaSupport.h"
#include "MaiTool.h"

// 宿主保管方舟 Assets 凭据并处理私有存储上传。工具在工作线程传入已校验的 JSON 参数，
// 回调返回一份 Assets 服务的 JSON 对象；本地图路径已限制在工具上下文允许的范围内。
// 网络调用可能阻塞，不能在 UI 线程运行。空回调只保留能力查询，远端动作不可用；
// 回调所依赖资源的生命周期由调用方保证。
using MaiArkAssetProvider =
    std::function<MaiResult<std::string>(const std::string&, const MaiToolContext&)>;

struct MaiArkAssetServiceSettings {
    std::string baseUrl;
    std::string token;
};

// 每次调用时由宿主从安全存储提供私有服务配置，返回的 provider 将资产动作发往该服务。
// 本地图像先申请短期上传凭证，再直接上传对象存储，最后把读取地址登记到方舟。
// 服务令牌和带签名的链接都不得回灌给模型。
MaiArkAssetProvider makeMaiArkAssetServiceProvider(
    std::function<MaiResult<MaiArkAssetServiceSettings>()> settings, std::string caBundlePath = {});

// 把工作区内的 MP4/MOV 直接上传私有 OSS，并返回只供视频请求使用的临时 HTTPS 读取链接。
// 先向配置中的签发服务申请短期 PUT/GET 链接，再用内嵌 curl 流式上传；视频字节不经过
// 签发服务。settings 每次调用时读取，便于服务令牌轮换；调用会阻塞 Agent 工作线程，
// 不可放在 UI 线程。路径必须位于 ToolContext 允许的范围，大小为 1–200 MB。
// 失败返回具体的签名或上传错误；取消时停止传输。返回链接的有效期由服务端控制，
// 调用方应立即交给 Seedance，不应保存或回显给模型。回调本身不持有 settings 的资源。
std::function<MaiResult<std::string>(const std::string&, const MaiToolContext&)>
makeMaiPrivateOssVideoUploader(std::function<MaiResult<MaiArkAssetServiceSettings>()> settings,
                               std::string caBundlePath = {});

// 图片（JPG/PNG）、视频（MP4/MOV）及 Wan3 支持的文档统一走签发服务直传 OSS，
// 返回 24 小时有效的签名 HTTPS 读取地址。纯音频和其他格式不在签发范围内。
// 上传失败不会触发付费模型请求；创作工具不得回退 Base64 或厂商临时上传。
MaiCreativeMediaUploadProvider makeMaiPrivateOssMediaUploader(
    std::function<MaiResult<MaiArkAssetServiceSettings>()> settings, std::string caBundlePath = {});

// 创建、查询素材组和素材，并上传图片及检查异步审核状态。只有 Active 素材能以
// asset://<ID> 交给 Seedance；上传不会修改本地原图。
std::unique_ptr<MaiTool> makeMaiArkAssetTool(MaiArkAssetProvider provider = {});
