// 文件作用：封装火山方舟媒体任务的提交、查询和结果取回。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiArkAssetTools.h"
#include "MaiCreativeMediaSupport.h"
#include "MaiFfmpegTools.h"
#include "MaiTool.h"

// 每次执行时从宿主获取方舟 API Key；密钥仍由宿主保管，可随时轮换或撤销。
// 回调在工具工作线程上调用，必须保证线程安全。返回空值时仍可查询能力，但不能提交任务。
using MaiArkApiKeyProvider = std::function<std::string()>;

// 跨平台 Seedance 视频工具：提交任务、轮询状态并将成片下载到工作区。
// 本地图片/视频先由 assets 上传登记，等待 Active 后才以 asset:// ID 提交付费任务；本地视频
// 还必须由 videoMetadata 在上传前实测时长和帧率。assets 为空时只能文生视频或复用已完成
// Seedance 任务，资产核验与本地媒体准备不可用；videoMetadata 为空时拒绝本地视频。网络调用
// 可能阻塞，不能在 UI 线程执行；取消请求通过 MaiToolContext 传入。回调及 context 所依赖的
// 资源必须在调用完成前保持有效。
std::unique_ptr<MaiTool> makeMaiSeedanceVideoTool(MaiArkApiKeyProvider apiKey,
                                                  std::string caBundlePath = {},
                                                  MaiArkAssetProvider assets = {},
                                                  MaiVideoMetadataProvider videoMetadata = {});

// 跨平台 Seedream 图像工具：读取可访问的输入图，调用方舟，在工作区创建新的 PNG。
// 原图不修改；线程、取消和密钥生命周期与 Seedance 视频工具相同。
std::unique_ptr<MaiTool> makeMaiSeedreamImageTool(MaiArkApiKeyProvider apiKey,
                                                  std::string caBundlePath = {},
                                                  MaiCreativeMediaUploadProvider uploadMedia = {});
