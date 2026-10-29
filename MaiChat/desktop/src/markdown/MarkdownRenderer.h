// 文件作用：桌面端Markdown 内容解析与呈现；MarkdownRenderer 负责内容渲染和资源生命周期，本文件给出其声明。
// 类型关系：MarkdownRenderer final；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 MarkdownRenderer 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 公开入口：renderToHtml、renderPreviewHtml。
#pragma once

#include <QString>

class MarkdownRenderer final {
public:
    // 聊天正文保留原文中的软换行；由 md4c 判定语义，不对 Markdown 全文替换换行。
    static QString renderToHtml(const QString& markdown);
// 纯文本预览使用语义化 HTML，不添加装饰性代码标签或内边距。
    static QString renderPreviewHtml(const QString& markdown);
};
