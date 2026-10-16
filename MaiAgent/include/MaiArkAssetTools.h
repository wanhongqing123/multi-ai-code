// 文件作用：管理火山方舟素材上传和云端素材引用。
// 对外类型：MaiArkAssetServiceSettings；线程、失败和生命周期以类型附近注释为准。
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

// 签发目的必须显式传给服务端。三个值分别对应 shared-media-inputs、wan-inputs 和
// ark-asset-inputs 分区，调用方不能自行覆盖对象前缀。
enum class MaiOssUploadPurpose { SharedMediaInput, WanInput, ArkAssetInput };

// Ark CreateAsset 在素材处理完成前就返回 ID。接口未提供状态时 status 为空；
// 调用方必须用 get_asset 查到 Active 后才能使用 asset://<id>，Failed 表示不可用。
struct MaiArkVideoAsset {
    // CreateAsset 返回的素材 ID；必须再查状态，不能仅凭非空就用于生成。
    std::string id;
    // CreateAsset 可选的处理状态；空串表示本次响应未提供状态。
    std::string status;
};

// 将工作区内 1–200 MB 的 MP4/MOV 经 provider 的私有 OSS 签发服务直传，
// 再登记到指定组，类型为 Ark Video。expectedGroupType 仅接受 AIGC/LivenessFace；
// 先用 get_group 核对它，
// 类型不匹配或查询失败时不上传。项目与 AK/SK 由 provider 配置，组所在项目
// 必须与配置项目及视频 API Key 项目一致。调用方提供已有的 AIGC 或真人认证组；
// 真人视频须传 LivenessFace，且由本人完成认证。此函数不执行真人授权或像素识别。
// 调用会阻塞工作线程，支持 context 取消。路径无效、缺少组、上传失败或 Ark 未
// 返回有效 ID 时返回错误；本地文件不会被修改。返回的 ID 要等 get_asset 报
// Active 后才能使用。这里只校验文件格式与大小；时长、尺寸、帧率和人脸一致性
// 由 Ark 异步审核。provider 与 context 必须在调用完成前保持有效。
MaiResult<MaiArkVideoAsset> uploadMaiArkVideoAsset(
    const MaiArkAssetProvider& provider, const std::string& groupId, const std::string& videoPath,
    const std::string& name, const std::string& expectedGroupType, const MaiToolContext& context);

// 每次调用时由宿主从安全存储提供私有服务配置，返回的 provider 将资产动作发往该服务。
// 本地图片和视频先申请短期上传凭证，再直接上传对象存储，最后把读取地址登记到方舟。
// 服务令牌和带签名的链接都不得回灌给模型。
MaiArkAssetProvider makeMaiArkAssetServiceProvider(
    std::function<MaiResult<MaiArkAssetServiceSettings>()> settings, std::string caBundlePath = {});

// 图片（JPG/PNG）、视频（MP4/MOV）及 Wan3 支持的文档统一走签发服务直传 OSS，
// 返回 24 小时有效的签名 HTTPS 读取地址。纯音频和其他格式不在签发范围内。
// 上传失败不会触发付费模型请求；创作工具不得回退 Base64 或厂商临时上传。
// purpose 总会写入签发请求。默认 SharedMediaInput 写入 shared-media-inputs；WanInput 写入
// wan-inputs；ArkAssetInput 写入 ark-asset-inputs。已有调用方使用默认参数仍会发出明确目的。
MaiCreativeMediaUploadProvider makeMaiPrivateOssMediaUploader(
    std::function<MaiResult<MaiArkAssetServiceSettings>()> settings, std::string caBundlePath = {},
    MaiOssUploadPurpose purpose = MaiOssUploadPurpose::SharedMediaInput);

// 创建、查询素材组和素材，并上传图片/视频及检查异步审核状态。只有 Active 素材能以
// asset://<ID> 交给 Seedance；上传不会修改本地原图。
std::unique_ptr<MaiTool> makeMaiArkAssetTool(MaiArkAssetProvider provider = {});
