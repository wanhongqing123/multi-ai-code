#pragma once

#include <QString>

// 应用日志文件。默认开启，无需任何开关。
//
// 存在的理由：这个应用的问题大多发生在"用户那台机器上、装完的正式包里"——
// 跨机远程桌面、IM 连接、TRTC 进房。而 Windows 上 exe 是 GUI 子系统
// （/SUBSYSTEM:WINDOWS），双击启动时 stderr 没有去处，qWarning 写了等于没写；
// macOS 双击 .app 同理。没有落盘的日志，排障就只能靠反复追问用户现象。
//
// install() 会接管 Qt 的消息处理器，因此**所有** qDebug / qInfo / qWarning /
// qCritical（包括 Qt 自身发出的）都会自动进这个文件，不需要改调用方。
namespace AppLog {

// 在 main() 里尽早调用，且必须在 setApplicationName / setOrganizationName
// 之后——日志目录是按这两个名字推导出来的。重复调用无副作用。
void install();

// 当前日志文件的完整路径。轮转后仍指向正在写的那个。
QString filePath();

// 日志目录。出问题时让用户整个目录打包发过来最省事。
QString directoryPath();

}  // namespace AppLog
