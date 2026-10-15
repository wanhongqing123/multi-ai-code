// 文件作用：桌面端操作系统能力适配；MacRemoteIMClient 负责外部接口请求与结果转换，本文件给出其实现。
// 实现边界：与 MacRemoteIMClient 的公开声明配套，调用方通过相应接口进入。
#include "platform/MacRemoteIMClient.h"

MacRemoteIMClient::MacRemoteIMClient(QObject* parent) : TimSdkRemoteIMClient(parent) {}
