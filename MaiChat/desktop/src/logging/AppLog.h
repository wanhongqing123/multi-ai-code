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
//
// 文件命名与留存：
//
//   maichat-20260731.log         今天，正在写的那个
//   maichat-20260731.1.log       今天写满 5MB 滚出来的第一段（序号按时间递增）
//   maichat-20260730.log         昨天
//   maichat-20260731-p8124.log   同机第二个实例（见下）
//
// 按天分文件是因为用户描述问题时给的是时间——"昨天下午连不上"，能直接定位到
// 文件，而不是在一个大文件里翻。单个文件超过 5MB 再按序号滚，保留 7 天。
//
// 同机双开是这个产品的正常用法（--login 就是为远程桌面同机联调加的）。两个
// 进程往同一个文件里追加会交错，轮转还会互相改名对方的文件——一份两个账号
// 混在一起的日志比没有日志更误导人。所以第一个实例用干净的文件名，之后的
// 实例各自带 -p<pid> 后缀。
namespace AppLog {

// 在 main() 里尽早调用，且必须在 setApplicationName / setOrganizationName
// 之后——日志目录是按这两个名字推导出来的。重复调用无副作用。
void install();

// 当前日志文件的完整路径。跨天或按大小滚动后仍指向正在写的那个。
QString filePath();

// 日志目录。出问题时让用户整个目录打包发过来最省事。
QString directoryPath();

}  // namespace AppLog
