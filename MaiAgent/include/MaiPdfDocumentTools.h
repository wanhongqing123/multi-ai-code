// 文件作用：为各宿主提供同一份 PDF 密码保护核心，不依赖模型参数或平台排版器。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "MaiError.h"
#include "MaiFilePath.h"

// 模型工具只能请求密码保护，由宿主私有安全表单取得打开密码。
// 在工作线程调用；宿主须切到 UI 线程展示双密码框，并观察 cancel 精确关闭自己的表单。
// 返回值仅在本次普通调用里传给加密核心，不能放入工具结果、聊天历史或诊断文本。
using MaiPdfPasswordProvider =
    std::function<MaiResult<std::string>(const std::atomic<bool>* cancel)>;

// 普通代码调用的加密选项。模型工具只传保护开关，密码须由宿主安全输入回调提供。
// 本对象不应序列化到工具输入、聊天历史或诊断；核心不会保存或回显密码。
struct MaiPdfEncryptionOptions {
    // PDF 打开密码，非空有效 UTF-8、无 NUL，最多 127 字节。
    // 不做 trim 或静默截断，避免生成后的实际密码与用户输入不同。
    // 调用期间必须保持有效；返回后调用方负责其自己的临时输入生命周期。
    std::string userPassword;

    // 默认只发布到新文件。true 才允许同目录原子替换已有普通文件；
    // 可用于原地保护，失败或提交前取消时保留旧目标。符号链接和目录一律拒绝。
    bool replaceExisting = false;
};

struct MaiPdfEncryptionResult {
    MaiFilePath path;
    std::uint64_t bytes = 0;
    std::uint64_t pageCount = 0;
};

// 通过进程内 qpdf 作业为未加密的 PDF 设置 AES-256 / 标准安全处理器 R6 打开密码。
// 排版保持由现有 Qt / UIKit / Android 排版器负责；此入口仅重写 PDF 对象和加密信息，
// 不把 PDF 栅格化，也不启动外部命令。普通调用与 generate_pdf 工具复用本函数。
//
// 输入和输出均为绝对路径。输入须是普通、未加密的有效 PDF，大小为 1 字节至 100 MiB；
// 输出父目录须已存在。路径访问授权由调用方限定，核心仍拒绝末级符号链接和目录。
// 所有中间文件以排他的随机同目录名称创建，加密和验证完成后才发布到 outputPath；
// 最终目标从不会短暂包含明文。失败只清理本次持有的临时文件，不删除调用方输入或旧目标。
//
// 本函数会阻塞，只能在允许文件 I/O 的工作线程调用。每次调用持有独立 qpdf 作业和日志器，
// 可由多个会话并发调用；不更改进程 stdout/stderr、全局密码或加密 provider。
// cancel 为 nullptr 表示没有取消源；解析前后和写入期间检查取消，原子发布是提交点。
// 提交前观察到取消则不发布；提交成功后返回成功，不因随后到达的取消删除已提交目标。
// 上游异常和日志不直接回灌，防止密码出现在错误文本中；错误通过 MaiResult 返回，不跨 ABI 抛出。
MaiResult<MaiPdfEncryptionResult> maiEncryptPdfFile(const MaiFilePath& sourcePath,
                                                    const MaiFilePath& outputPath,
                                                    const MaiPdfEncryptionOptions& options,
                                                    const std::atomic<bool>* cancel = nullptr);
