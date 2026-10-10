#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiCreativeMediaSupport.h"
#include "MaiTool.h"

// 每次执行时从宿主获取方舟 API Key；密钥仍由宿主保管，可随时轮换或撤销。
// 回调在工具工作线程上调用，必须保证线程安全。返回空值时仍可查询能力，但不能提交任务。
using MaiArkApiKeyProvider = std::function<std::string()>;

// 将已经校验的本地视频上传到宿主管理的存储，返回方舟可读取的 HTTPS 地址。
// 宿主负责密钥和传输策略；回调可能阻塞，须尽量响应 context 取消。
// 存储密钥不得放进模型可见的工具参数。
using MaiArkVideoUploadProvider = MaiCreativeMediaUploadProvider;

// 跨平台 Seedance 视频工具：提交任务、轮询状态并将成片下载到工作区。
// 只有宿主提供安全上传通道时才接受本地视频。网络调用可能阻塞，不能在 UI 线程执行；
// 取消请求通过 MaiToolContext 传入。
std::unique_ptr<MaiTool> makeMaiSeedanceVideoTool(MaiArkApiKeyProvider apiKey,
                                                  std::string caBundlePath = {},
                                                  MaiCreativeMediaUploadProvider uploadMedia = {});

// 跨平台 Seedream 图像工具：读取可访问的输入图，调用方舟，在工作区创建新的 PNG。
// 原图不修改；线程、取消和密钥生命周期与 Seedance 视频工具相同。
std::unique_ptr<MaiTool> makeMaiSeedreamImageTool(MaiArkApiKeyProvider apiKey,
                                                  std::string caBundlePath = {},
                                                  MaiCreativeMediaUploadProvider uploadMedia = {});
