// 文件作用：桌面端Markdown 内容解析与呈现；MarkdownRenderer 负责内容渲染和资源生命周期，本文件给出其声明。
// 类型关系：MarkdownRenderer final；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 MarkdownRenderer 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 公开入口：renderToHtml、renderPreviewHtml。
#pragma once

#include <QString>

class MarkdownRenderer final {
public:
    static QString renderToHtml(const QString& markdown);
    // Semantic HTML for plain previews: no decorative code labels or padding.
    static QString renderPreviewHtml(const QString& markdown);
};
