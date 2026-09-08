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

    // QTextDocument 不支持 nth-child，用每个单元格的底色形成完整斑马纹行。
    static const QRegularExpression tablePattern(QStringLiteral("<table>([\\s\\S]*?)</table>"));
    static const QRegularExpression rowPattern(QStringLiteral("<tr>([\\s\\S]*?)</tr>"));
    int tableOffset = 0;
    while (true) {
        const auto match = tablePattern.match(html, tableOffset);
        if (!match.hasMatch()) break;
        QString body = match.captured(1);
        int rowOffset = 0;
        int rowIndex = 0;
        while (true) {
            const auto row = rowPattern.match(body, rowOffset);
            if (!row.hasMatch()) break;
            QString cells = row.captured(1);
            const bool header = cells.contains(QStringLiteral("<th"));
            const QString color = header ? QStringLiteral("#e8f0f8")
                : (rowIndex++ % 2 == 0 ? QStringLiteral("#ffffff") : QStringLiteral("#f6f8fb"));
            cells.replace(QStringLiteral("<td"), QStringLiteral("<td bgcolor=\"%1\"").arg(color));
            cells.replace(QStringLiteral("<th"), QStringLiteral("<th bgcolor=\"%1\"").arg(color));
            const QString replacement = QStringLiteral("<tr>") + cells + QStringLiteral("</tr>");
            body.replace(row.capturedStart(), row.capturedLength(), replacement);
            rowOffset = row.capturedStart() + replacement.size();
        }
        const QString replacement = QStringLiteral("<table border=\"1\" bordercolor=\"#dce5ef\" cellspacing=\"0\" cellpadding=\"8\" style=\"border-collapse:collapse;margin-bottom:8px;\">")
            + body + QStringLiteral("</table>");
        html.replace(match.capturedStart(), match.capturedLength(), replacement);
        tableOffset = match.capturedStart() + replacement.size();
    }

    // 代码块与行内代码的后处理需要区分两者：先给块级 code 打占位，
    // 再给剩余（行内）code 加 thin space 模拟 1px 5px 内边距，最后还原块级。
    html.replace(QStringLiteral("<pre><code"), QStringLiteral("<pre><mdblockcode"));
    html.replace(QStringLiteral("</code></pre>"), QStringLiteral("</mdblockcode></pre>"));

    html.replace(QStringLiteral("<code>"), QStringLiteral("<code>&#8201;"));
    html.replace(QStringLiteral("</code>"), QStringLiteral("&#8201;</code>"));

    // Qt 的富文本不支持块级圆角；用浅色表格卡片保留内边距和语言标签。
    static const QRegularExpression codePattern(
        QStringLiteral("<pre><mdblockcode([^>]*)>([\\s\\S]*?)</mdblockcode></pre>"));
    static const QRegularExpression languagePattern(QStringLiteral("class=\"language-([^\"]*)\""));
    int codeOffset = 0;
    while (true) {
        const auto match = codePattern.match(html, codeOffset);
        if (!match.hasMatch()) break;
        const auto language = languagePattern.match(match.captured(1));
        const QString label = language.hasMatch() ? language.captured(1) : QStringLiteral("代码");
        QString code = match.captured(2);
        if (code.endsWith(QLatin1Char('\n'))) code.chop(1);
        const QString replacement = QStringLiteral(
            "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"10\" bgcolor=\"#f6f8fb\" style=\"margin-top:0;margin-bottom:8px;\">"
            "<tr><td bgcolor=\"#e9eff5\"><span style=\"font-size:11px;color:#64748b;\">%1</span></td></tr>"
            "<tr><td><pre><code%2>%3</code></pre></td></tr></table>")
            .arg(label, match.captured(1), code);
        html.replace(match.capturedStart(), match.capturedLength(), replacement);
        codeOffset = match.capturedStart() + replacement.size();
    }

    // Qt 不支持 CSS border-left/padding，用窄色条和带内边距的内表格呈现引用。
    html.replace(QStringLiteral("<blockquote>"), QStringLiteral(
        "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" style=\"margin-bottom:8px;\">"
        "<tr><td width=\"3\" bgcolor=\"#90c9ed\"></td><td>"
        "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"8\" bgcolor=\"#eef3f8\">"
        "<tr><td style=\"color:#475569;\">"));
    html.replace(QStringLiteral("</blockquote>"),
        QStringLiteral("</td></tr></table></td></tr></table>"));

    return html;
}

}  // namespace

QString MarkdownRenderer::renderToHtml(const QString& markdown) {
    // 与 iOS 使用同一套轻量排版：浅色代码卡片、清晰标题、淡色表头。
    QString styledHtml = QStringLiteral(R"(<!doctype html><html><head><meta charset="utf-8"><style>
body{margin:0;color:#0f172a;font-size:14px;}
p{margin:0 0 8px 0;}
h1,h2,h3,h4,h5,h6{margin:4px 0 10px 0;font-weight:600;color:#0f172a;}
h1{font-size:20px;}h2{font-size:17px;}h3{font-size:15px;}h4,h5,h6{font-size:14px;}
ul,ol{margin:0 0 8px 0;padding:0;}
li{margin:2px 0;}
pre{margin:0;color:#172033;white-space:pre-wrap;}
code{font-family:'MAICHAT_CODE_FONT';background:#eef2f7;font-size:13px;}
pre code{background:transparent;color:#172033;}
a{color:#2563eb;text-decoration:none;}
del{text-decoration:line-through;}
th{background:#e8f0f8;}
</style></head><body>)");
    // QTextDocument 的 font-family 不解析浏览器式的多字体回退列表。
#ifdef Q_OS_MAC
    styledHtml.replace(QStringLiteral("MAICHAT_CODE_FONT"), QStringLiteral("Menlo"));
#elif defined(Q_OS_WIN)
    styledHtml.replace(QStringLiteral("MAICHAT_CODE_FONT"), QStringLiteral("Consolas"));
#else
    styledHtml.replace(QStringLiteral("MAICHAT_CODE_FONT"), QStringLiteral("monospace"));
#endif
    return styledHtml + renderBody(markdown) + QStringLiteral("</body></html>");
}
