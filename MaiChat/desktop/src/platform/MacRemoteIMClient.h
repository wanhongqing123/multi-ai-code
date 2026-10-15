// 文件作用：桌面端操作系统能力适配；MacRemoteIMClient 负责外部接口请求与结果转换，本文件给出其声明。
// 类型关系：MacRemoteIMClient final : public TimSdkRemoteIMClient；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 MacRemoteIMClient 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 直接依赖：im/TimSdkRemoteIMClient.h；实现与这些接口协作。
#pragma once

#include "im/TimSdkRemoteIMClient.h"

class MacRemoteIMClient final : public TimSdkRemoteIMClient {
public:
    explicit MacRemoteIMClient(QObject* parent = nullptr);
};
