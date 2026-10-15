// 文件作用：声明声明 ffplay 兼容层内部共享的播放器状态与音视频桥接入口所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void maiFfplaySetError(const char* message);
int maiFfplayWindowWidth(void);

#ifdef __cplusplus
}
#endif
