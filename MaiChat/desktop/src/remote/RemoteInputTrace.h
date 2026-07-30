#pragma once

#include <QString>

// 远程输入链路的诊断日志。
//
// 存在的理由：这条链路跨两台机器（控制端采集 → TRTC → 被控端注入），中间任何
// 一跳断了，表面现象都是同一个"远端鼠标不动"。没有日志时只能靠猜，而两台机器
// 又没法同时上断点。所以每一跳都留一条记录，出问题时两边各自看自己的日志，
// 一眼就能定位是"没发出去"还是"没收到"还是"收到了被门禁挡了"。
//
// 默认关闭，零开销。打开方式：把环境变量 MAICHAT_REMOTE_INPUT_TRACE 设为
// 1 / true / on（大小写不限）后启动程序。
namespace RemoteInput {

// 进程内只读一次环境变量，之后是纯 bool 判断。
bool traceEnabled();

// 日志落盘位置，同时也会写一份到 stderr。关闭时返回空串。
QString traceFilePath();

// 追加一行（自动带时间戳与换行）。未开启时直接返回。
void trace(const QString& line);

}  // namespace RemoteInput
