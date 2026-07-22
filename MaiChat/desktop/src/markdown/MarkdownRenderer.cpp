#include "markdown/MarkdownRenderer.h"

#include <QByteArray>
#include <QRegularExpression>

#include "md4c-html.h"

// Markdown 解析交给 md4c（0.5.2，与 Qt 官方内嵌同源），GFM 方言与 Electron 端
// react-markdown + remark-gfm 对齐：表格、删除线、任务列表、自动链接、软换行
// 折叠为空格（CommonMark 行为，两端一致）。本文件只负责两件事：
//   1) 调 md4c 产出标准 HTML；
//   2) 针对 QTextDocument 的 CSS 子集限制做后处理（行内 padding、块级圆角/
//      内边距、<input> 复选框都不支持），并套上与 .remote-im-markdown 对齐的样式。

namespace {

void appendHtmlChunk(const MD_CHAR* data, MD_SIZE size, void* userdata) {
    static_cast<QByteArray*>(userdata)->append(data, static_cast<int>(size));
}

// md4c 不过滤链接协议；除 http/https/mailto/锚点外一律摘掉 href（保留链接文字），
// 与旧实现的安全策略一致。
QString sanitizeLinks(QString html) {
    static const QRegularExpression unsafeHref(
        QStringLiteral("<a href=\"(?!https?:|mailto:|#)[^\"]*\""),
        QRegularExpression::CaseInsensitiveOption);
    html.replace(unsafeHref, QStringLiteral("<a"));
    return html;
}

QString renderBody(const QString& markdown) {
    const QByteArray utf8 = markdown.toUtf8();
    QByteArray out;
    out.reserve(utf8.size() * 2);
    md_html(utf8.constData(), static_cast<MD_SIZE>(utf8.size()), appendHtmlChunk, &out,
            MD_DIALECT_GITHUB | MD_FLAG_NOHTML, MD_HTML_FLAG_XHTML);
    QString html = QString::fromUtf8(out);

    html = sanitizeLinks(html);

    // 任务列表：QTextDocument 渲染不了 <input type="checkbox">，换成字符。
    html.replace(QStringLiteral("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled checked>"),
                 QStringLiteral("☑ "));
    html.replace(QStringLiteral("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled>"),
                 QStringLiteral("☐ "));

    // GFM 表格：Qt 不吃 CSS 的 td/th border，用表格属性画边框。
    // （我们自己包代码块的 <table 带 width 属性，不会被这条命中。）
    html.replace(QStringLiteral("<table>"),
                 QStringLiteral("<table border=\"1\" cellspacing=\"0\" cellpadding=\"5\""
                                " style=\"border-collapse:collapse;margin-bottom:8px;\">"));

    // 代码块与行内代码的后处理需要区分两者：先给块级 code 打占位，
    // 再给剩余（行内）code 加 thin space 模拟 1px 5px 内边距，最后还原块级。
    html.replace(QStringLiteral("<pre><code"), QStringLiteral("<pre><mdblockcode"));
    html.replace(QStringLiteral("</code></pre>"), QStringLiteral("</mdblockcode></pre>"));

    html.replace(QStringLiteral("<code>"), QStringLiteral("<code>&#8201;"));
    html.replace(QStringLiteral("</code>"), QStringLiteral("&#8201;</code>"));

    // Qt 不支持块级 padding/border-radius，pre 直接铺背景会变成贴边黑条；
    // 用单元格表格（cellpadding + bgcolor）模拟 Electron 深色代码卡片。
    html.replace(QStringLiteral("<pre><mdblockcode"),
                 QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"10\""
                                " bgcolor=\"#0f172a\" style=\"margin-top:0;margin-bottom:8px;\">"
                                "<tr><td><pre><code"));
    html.replace(QStringLiteral("</mdblockcode></pre>"),
                 QStringLiteral("</code></pre></td></tr></table>"));

    return html;
}

}  // namespace

QString MarkdownRenderer::renderToHtml(const QString& markdown) {
    // 样式对齐 Electron 端远程 IM 抽屉的 .remote-im-markdown（src/styles.css）：
    // 正文 14px/#0f172a，h1 22/h2 18/h3 16，行内代码浅灰底 + Cascadia/Consolas，
    // 代码块深底浅字（#0f172a/#e2e8f0）。rgba(15,23,42,.08) 在白底上换算为 #ececee。
    return QStringLiteral(R"(<!doctype html><html><head><meta charset="utf-8"><style>
body{margin:0;color:#0f172a;font-size:14px;}
p{margin:0 0 8px 0;}
h1,h2,h3,h4,h5,h6{margin:0 0 10px 0;font-weight:700;color:#0f172a;}
h1{font-size:22px;}h2{font-size:18px;}h3{font-size:16px;}h4,h5,h6{font-size:14px;}
ul,ol{margin:0 0 8px 0;padding:0;}
li{margin:2px 0;}
pre{margin:0;color:#e2e8f0;white-space:pre-wrap;}
code{font-family:'Cascadia Code','Cascadia Mono',Consolas,monospace;background:#ececee;font-size:13px;}
pre code{background:transparent;color:#e2e8f0;}
a{color:#2563eb;text-decoration:none;}
del{text-decoration:line-through;}
th{background:#f8fafc;}
blockquote{margin:0 0 8px 0;padding-left:10px;border-left:3px solid #e2e8f0;color:#475569;}
</style></head><body>)") + renderBody(markdown) + QStringLiteral("</body></html>");
}
