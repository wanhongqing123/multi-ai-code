// 文件作用：声明包装 ffplay 的启动、停止、跳转和播放器状态，把宿主命令交给播放循环所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#include "MaiFfplayHost.h"

#ifdef __cplusplus
extern "C" {
#endif

// 在调用方的播放线程阻塞，直到宿主 ffplay 会话关闭。
// 实现包含的 ffplay.c 与上游字节一致；进程退出和信号处理在此层拦截。
MAI_FFPLAY_EXPORT int maiFfplayRun(const MaiFfplayHost* host, int argc, char** argv);
// 以新倍速重新打开同一宿主播放器。调用方先停止并等待旧会话退出，
// 再把原媒体位置作为 start_position_us 传入。视频 PTS 和不变调音频一起重定时，
// 不创建输出文件。支持 0.25、0.5、1、1.5、2 倍速；上游 ffplay.c 保持原样。
MAI_FFPLAY_EXPORT int maiFfplayRunAtSpeed(const MaiFfplayHost* host, int argc, char** argv,
                                         double speed, int64_t start_position_us);
MAI_FFPLAY_EXPORT void maiFfplayRequestQuit(void);
// 将 Agent/MaiChat 指令映射到 ffplay 现有控制逻辑。
// 入队返回 1，播放器未运行返回 0，未知指令返回 -1。
MAI_FFPLAY_EXPORT int maiFfplaySendCommand(const char* command);
// 音频 -nodisp 模式没有视频窗口；由 FFplay 事件线程直接切换暂停状态。
MAI_FFPLAY_EXPORT int maiFfplayRequestPauseToggle(void);
// 请求源媒体的绝对时间戳，由 ffplay 播放线程直接执行，
// 不根据窗口宽度伪造鼠标点击事件。
MAI_FFPLAY_EXPORT int maiFfplaySeekAbsoluteUs(int64_t position_us);
MAI_FFPLAY_EXPORT int maiFfplaySeekPercent(double fraction);
typedef struct MaiFfplayPlaybackStatus {
    int64_t position_us;
    int64_t duration_us;
    int running;
    int paused;
    int speed_percent;
} MaiFfplayPlaybackStatus;
// 在 ffplay 播放／事件线程采样的线程安全状态快照。
MAI_FFPLAY_EXPORT int maiFfplayGetPlaybackStatus(MaiFfplayPlaybackStatus* status);
// 接收播放工作线程的诊断信息。user_data 由调用方持有；
// maiFfplayRun 返回后必须清除诊断接收回调。
MAI_FFPLAY_EXPORT void maiFfplaySetDiagnosticSink(
    void (*sink)(void* user_data, int level, const char* line), void* user_data);

#ifdef __cplusplus
}
#endif
