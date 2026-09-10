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

QString quoteOpening(const QString& accent, const QString& background, const QString& title = {}) {
    QString result = QStringLiteral(
        "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" style=\"margin-bottom:8px;\">"
        "<tr><td width=\"3\" bgcolor=\"%1\"></td><td>"
        "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"8\" bgcolor=\"%2\">"
        "<tr><td style=\"color:#475569;\">").arg(accent, background);
    if (!title.isEmpty()) {
        result += QStringLiteral("<p style=\"margin:0 0 6px 0;color:%1;font-size:13px;\"><strong>%2</strong></p>")
            .arg(accent, title);
    }
    return result;
}

QString parseHtml(const QString& markdown) {
    const QByteArray utf8 = markdown.toUtf8();
    QByteArray out;
    out.reserve(utf8.size() * 2);
    md_html(utf8.constData(), static_cast<MD_SIZE>(utf8.size()), appendHtmlChunk, &out,
            MD_DIALECT_GITHUB | MD_FLAG_NOHTML, MD_HTML_FLAG_XHTML);
    return sanitizeLinks(QString::fromUtf8(out));
}

struct Callout { const char* name; const char* accent; const char* background; QString title; };
const Callout callouts[] = {
    {"NOTE", "#1f64b0", "#f0f5fb", QStringLiteral("提示")},
    {"TIP", "#16836b", "#eff7f4", QStringLiteral("建议")},
    {"IMPORTANT", "#7547a8", "#f5f1fa", QStringLiteral("重要")},
    {"WARNING", "#9c640f", "#fbf6ec", QStringLiteral("注意")},
    {"CAUTION", "#ba3d40", "#fbf1f2", QStringLiteral("警告")}
};

QRegularExpression calloutMarker(const char* name) {
    return QRegularExpression(QStringLiteral("<blockquote>\\s*<p>\\[!%1\\][ \\t]*(?:\\n|<br\\s*/?>\\n?|(?=</p>))")
        .arg(QString::fromLatin1(name)), QRegularExpression::CaseInsensitiveOption);
}

QString renderBody(const QString& markdown) {
    QString html = parseHtml(markdown);

    // 任务列表：QTextDocument 渲染不了 <input type="checkbox">，换成字符。
    html.replace(QStringLiteral("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled checked>"),
                 QStringLiteral("<span style=\"color:#16836b;\">☑</span> "));
    html.replace(QStringLiteral("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled>"),
                 QStringLiteral("<span style=\"color:#64748b;\">☐</span> "));

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
            const QString color = header ? QStringLiteral("#245995")
                : (rowIndex++ % 2 == 0 ? QStringLiteral("#ffffff") : QStringLiteral("#f7f7f7"));
            cells.replace(QStringLiteral("<td"), QStringLiteral("<td bgcolor=\"%1\"").arg(color));
            cells.replace(QStringLiteral("<th"), QStringLiteral("<th bgcolor=\"%1\"").arg(color));
            const QString replacement = QStringLiteral("<tr>") + cells + QStringLiteral("</tr>");
            body.replace(row.capturedStart(), row.capturedLength(), replacement);
            rowOffset = row.capturedStart() + replacement.size();
        }
        const QString replacement = QStringLiteral("<table border=\"0\" cellspacing=\"0\" cellpadding=\"8\" style=\"border-collapse:collapse;margin-bottom:8px;\">")
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

    // D 明晰蓝：代码块只保留浅底与内边距，减少装饰性标题栏。
    static const QRegularExpression codePattern(
        QStringLiteral("<pre><mdblockcode([^>]*)>([\\s\\S]*?)</mdblockcode></pre>"));
    int codeOffset = 0;
    while (true) {
        const auto match = codePattern.match(html, codeOffset);
        if (!match.hasMatch()) break;
        QString code = match.captured(2);
        if (code.endsWith(QLatin1Char('\n'))) code.chop(1);
        const QString replacement = QStringLiteral(
            "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"10\" bgcolor=\"#f7f7f7\" style=\"margin-top:0;margin-bottom:8px;\">"
            "<tr><td><pre><code%1>%2</code></pre></td></tr></table>")
            .arg(match.captured(1), code);
        html.replace(match.capturedStart(), match.capturedLength(), replacement);
        codeOffset = match.capturedStart() + replacement.size();
    }

    // Recognize only a standalone first-line GFM callout marker. Escaped code,
    // unknown markers, and markers appearing within ordinary prose stay literal.
    for (const auto& callout : callouts) {
        html.replace(calloutMarker(callout.name), quoteOpening(QString::fromLatin1(callout.accent),
            QString::fromLatin1(callout.background), callout.title) + QStringLiteral("<p>"));
    }
    // Qt 不支持 CSS border-left/padding，用窄色条和带内边距的内表格呈现引用。
    html.replace(QStringLiteral("<blockquote>"), quoteOpening(QStringLiteral("#2873c7"), QStringLiteral("#eaf3fd")));
    html.replace(QStringLiteral("</blockquote>"),
        QStringLiteral("</td></tr></table></td></tr></table>"));

    return html;
}

}  // namespace

QString MarkdownRenderer::renderPreviewHtml(const QString& markdown) {
    QString html = parseHtml(markdown);
    // Plain previews do not need emphasis tags, but keep code/link tags until
    // after callout recognition so their literal labels remain protected.
    static const QRegularExpression emphasis(QStringLiteral("</?(?:strong|em|del)>"));
    html.remove(emphasis);
    // Recognize callouts before Qt discards inline-code semantics into font
    // properties. <p><code>[!TIP]</code> can never match the plain marker.
    for (const auto& callout : callouts) {
        // Preserve the existing preview policy: a leading plain callout token
        // may also precede same-line text. Rich message rendering stays strict.
        const QRegularExpression marker(QStringLiteral("<blockquote>\\s*<p>\\[!%1\\](?:[ \\t]*(?:\\n|<br\\s*/?>\\n?|(?=</p>))|[ \\t]+)")
            .arg(QString::fromLatin1(callout.name)), QRegularExpression::CaseInsensitiveOption);
        html.replace(marker, QStringLiteral("<blockquote><p>") + callout.title + QStringLiteral("："));
    }
    html.replace(QStringLiteral("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled checked>"), QStringLiteral("☑ "));
    html.replace(QStringLiteral("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled>"), QStringLiteral("☐ "));
    // Preview extraction never needs image resources, only their escaped alt text.
    static const QRegularExpression image(QStringLiteral("<img\\b[^>]*\\balt=\"([^\"]*)\"[^>]*>"));
    html.replace(image, QStringLiteral("\\1"));
    return html;
}

QString MarkdownRenderer::renderToHtml(const QString& markdown) {
    // 保留 D 的颜色层级，字号与 iOS 的 22/18/16/14 对齐。
    // QTextDocument 的 CSS 数值字重映射到 Qt 0..99：500 为 DemiBold(63)，
    // 600 为 Bold(75)，700 已接近 Black(88)，不能照搬浏览器的字重数值。
    QString styledHtml = QStringLiteral(R"(<!doctype html><html><head><meta charset="utf-8"><style>
body{margin:0;color:#334155;font-size:14px;font-weight:400;}
p{margin:0 0 10px 0;font-weight:400;}
h1,h2,h3,h4,h5,h6{font-weight:500;}
h1{font-size:22px;color:#142b4a;margin:0 0 12px 0;}
h2{font-size:18px;color:#1769be;margin:16px 0 10px 0;}
h3{font-size:16px;color:#176e83;margin:12px 0 8px 0;}
h4{font-size:14px;color:#37465c;margin:10px 0 8px 0;}
h5{font-size:14px;color:#37465c;margin:10px 0 8px 0;}
h6{font-size:14px;color:#54657a;margin:10px 0 6px 0;}
ul,ol{margin:0 0 8px 0;padding:0;}
li{margin:4px 0;font-weight:400;}
strong{font-weight:600;color:#1e293b;}
em{font-style:italic;color:#54657a;}
pre{margin:0;color:#454545;white-space:pre-wrap;}
code{font-family:'MAICHAT_CODE_FONT';background:#e6efff;color:#164eac;font-size:13px;}
pre code{background:transparent;color:#454545;}
a{color:#075fd1;text-decoration:underline;}
del{color:#7c8798;text-decoration:line-through;}
th{background:#245995;color:#ffffff;font-weight:500;font-size:13px;}
td{font-size:13px;}
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
