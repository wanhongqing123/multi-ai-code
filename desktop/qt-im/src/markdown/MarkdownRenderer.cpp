#include "markdown/MarkdownRenderer.h"

#include <QChar>
#include <QStringList>
#include <QUrl>

namespace {

QString escapeHtml(const QString& text) {
    return text.toHtmlEscaped();
}

QString escapeAttribute(const QString& text) {
    QString escaped = text.toHtmlEscaped();
    escaped.replace(QLatin1Char('\''), QStringLiteral("&#39;"));
    return escaped;
}

bool isSafeUrl(const QString& value) {
    if (value.startsWith(QLatin1Char('#'))) return true;
    const QUrl url(value);
    if (!url.isValid() || url.scheme().isEmpty()) return false;
    const QString scheme = url.scheme().toLower();
    return scheme == QStringLiteral("http") ||
           scheme == QStringLiteral("https") ||
           scheme == QStringLiteral("mailto");
}

bool startsWithAt(const QString& text, int index, const QString& token) {
    return index >= 0 && index + token.size() <= text.size() && text.mid(index, token.size()) == token;
}

QString renderInline(const QString& text);

QString renderInlineDelimited(const QString& text, int& index, const QString& token, const QString& tag) {
    const int end = text.indexOf(token, index + token.size());
    if (end < 0) return {};
    const QString inner = text.mid(index + token.size(), end - index - token.size());
    index = end + token.size();
    return QStringLiteral("<%1>%2</%1>").arg(tag, renderInline(inner));
}

QString renderInlineCode(const QString& text, int& index) {
    const int end = text.indexOf(QLatin1Char('`'), index + 1);
    if (end < 0) return {};
    const QString inner = text.mid(index + 1, end - index - 1);
    index = end + 1;
    return QStringLiteral("<code>%1</code>").arg(escapeHtml(inner));
}

QString renderInlineLink(const QString& text, int& index) {
    const int labelEnd = text.indexOf(QLatin1Char(']'), index + 1);
    if (labelEnd < 0 || labelEnd + 1 >= text.size() || text[labelEnd + 1] != QLatin1Char('(')) return {};
    const int urlEnd = text.indexOf(QLatin1Char(')'), labelEnd + 2);
    if (urlEnd < 0) return {};

    const QString label = text.mid(index + 1, labelEnd - index - 1);
    const QString url = text.mid(labelEnd + 2, urlEnd - labelEnd - 2).trimmed();
    index = urlEnd + 1;

    if (!isSafeUrl(url)) return renderInline(label);
    return QStringLiteral("<a href=\"%1\">%2</a>").arg(escapeAttribute(url), renderInline(label));
}

QString renderInline(const QString& text) {
    QString html;
    QString plain;

    auto flushPlain = [&] {
        if (plain.isEmpty()) return;
        html += escapeHtml(plain);
        plain.clear();
    };

    int index = 0;
    while (index < text.size()) {
        if (text[index] == QLatin1Char('`')) {
            int nextIndex = index;
            const QString rendered = renderInlineCode(text, nextIndex);
            if (!rendered.isEmpty()) {
                flushPlain();
                html += rendered;
                index = nextIndex;
                continue;
            }
        }

        if (startsWithAt(text, index, QStringLiteral("**"))) {
            int nextIndex = index;
            const QString rendered = renderInlineDelimited(text, nextIndex, QStringLiteral("**"), QStringLiteral("strong"));
            if (!rendered.isEmpty()) {
                flushPlain();
                html += rendered;
                index = nextIndex;
                continue;
            }
        }

        if (startsWithAt(text, index, QStringLiteral("__"))) {
            int nextIndex = index;
            const QString rendered = renderInlineDelimited(text, nextIndex, QStringLiteral("__"), QStringLiteral("strong"));
            if (!rendered.isEmpty()) {
                flushPlain();
                html += rendered;
                index = nextIndex;
                continue;
            }
        }

        if (text[index] == QLatin1Char('*')) {
            int nextIndex = index;
            const QString rendered = renderInlineDelimited(text, nextIndex, QStringLiteral("*"), QStringLiteral("em"));
            if (!rendered.isEmpty()) {
                flushPlain();
                html += rendered;
                index = nextIndex;
                continue;
            }
        }

        if (text[index] == QLatin1Char('[')) {
            int nextIndex = index;
            const QString rendered = renderInlineLink(text, nextIndex);
            if (!rendered.isEmpty()) {
                flushPlain();
                html += rendered;
                index = nextIndex;
                continue;
            }
        }

        plain += text[index];
        ++index;
    }

    flushPlain();
    return html;
}

bool isFenceStart(const QString& trimmed, QString* marker) {
    if (trimmed.startsWith(QStringLiteral("```"))) {
        if (marker) *marker = QStringLiteral("```");
        return true;
    }
    if (trimmed.startsWith(QStringLiteral("~~~"))) {
        if (marker) *marker = QStringLiteral("~~~");
        return true;
    }
    return false;
}

int headingLevel(const QString& trimmed) {
    int level = 0;
    while (level < trimmed.size() && level < 6 && trimmed[level] == QLatin1Char('#')) ++level;
    if (level == 0 || level >= trimmed.size() || !trimmed[level].isSpace()) return 0;
    return level;
}

bool unorderedListContent(const QString& trimmed, QString* content) {
    if (trimmed.size() < 3) return false;
    const QChar marker = trimmed[0];
    if (marker != QLatin1Char('-') && marker != QLatin1Char('*') && marker != QLatin1Char('+')) return false;
    if (!trimmed[1].isSpace()) return false;
    if (content) *content = trimmed.mid(2).trimmed();
    return true;
}

bool orderedListContent(const QString& trimmed, QString* content) {
    int index = 0;
    while (index < trimmed.size() && trimmed[index].isDigit()) ++index;
    if (index == 0 || index + 1 >= trimmed.size()) return false;
    if (trimmed[index] != QLatin1Char('.') && trimmed[index] != QLatin1Char(')')) return false;
    if (!trimmed[index + 1].isSpace()) return false;
    if (content) *content = trimmed.mid(index + 2).trimmed();
    return true;
}

bool isSpecialBlockStart(const QString& line) {
    const QString trimmed = line.trimmed();
    QString ignored;
    QString content;
    return trimmed.isEmpty() ||
           isFenceStart(trimmed, &ignored) ||
           headingLevel(trimmed) > 0 ||
           unorderedListContent(trimmed, &content) ||
           orderedListContent(trimmed, &content) ||
           trimmed.startsWith(QStringLiteral("> "));
}

QString renderParagraph(const QStringList& lines) {
    QStringList renderedLines;
    for (const QString& line : lines) {
        renderedLines.append(renderInline(line.trimmed()));
    }
    return QStringLiteral("<p>%1</p>").arg(renderedLines.join(QStringLiteral("<br/>")));
}

QString renderCodeBlock(const QStringList& lines, int& index) {
    QString marker;
    isFenceStart(lines[index].trimmed(), &marker);
    ++index;

    QStringList codeLines;
    while (index < lines.size()) {
        if (lines[index].trimmed().startsWith(marker)) {
            ++index;
            break;
        }
        codeLines.append(lines[index]);
        ++index;
    }

    return QStringLiteral("<pre><code>%1</code></pre>").arg(escapeHtml(codeLines.join(QLatin1Char('\n'))));
}

QString renderUnorderedList(const QStringList& lines, int& index) {
    QString html = QStringLiteral("<ul>");
    QString content;
    while (index < lines.size() && unorderedListContent(lines[index].trimmed(), &content)) {
        html += QStringLiteral("<li>%1</li>").arg(renderInline(content));
        ++index;
    }
    html += QStringLiteral("</ul>");
    return html;
}

QString renderOrderedList(const QStringList& lines, int& index) {
    QString html = QStringLiteral("<ol>");
    QString content;
    while (index < lines.size() && orderedListContent(lines[index].trimmed(), &content)) {
        html += QStringLiteral("<li>%1</li>").arg(renderInline(content));
        ++index;
    }
    html += QStringLiteral("</ol>");
    return html;
}

QString renderBlockQuote(const QStringList& lines, int& index) {
    QStringList quoteLines;
    while (index < lines.size()) {
        const QString trimmed = lines[index].trimmed();
        if (!trimmed.startsWith(QStringLiteral("> "))) break;
        quoteLines.append(trimmed.mid(2));
        ++index;
    }
    return QStringLiteral("<blockquote>%1</blockquote>").arg(renderParagraph(quoteLines));
}

QString renderBody(const QString& markdown) {
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    QString html;
    int index = 0;
    while (index < lines.size()) {
        const QString trimmed = lines[index].trimmed();
        if (trimmed.isEmpty()) {
            ++index;
            continue;
        }

        QString marker;
        if (isFenceStart(trimmed, &marker)) {
            html += renderCodeBlock(lines, index);
            continue;
        }

        const int level = headingLevel(trimmed);
        if (level > 0) {
            html += QStringLiteral("<h%1>%2</h%1>").arg(level).arg(renderInline(trimmed.mid(level).trimmed()));
            ++index;
            continue;
        }

        QString listContent;
        if (unorderedListContent(trimmed, &listContent)) {
            html += renderUnorderedList(lines, index);
            continue;
        }

        if (orderedListContent(trimmed, &listContent)) {
            html += renderOrderedList(lines, index);
            continue;
        }

        if (trimmed.startsWith(QStringLiteral("> "))) {
            html += renderBlockQuote(lines, index);
            continue;
        }

        QStringList paragraphLines;
        while (index < lines.size() && !isSpecialBlockStart(lines[index])) {
            paragraphLines.append(lines[index]);
            ++index;
        }
        html += renderParagraph(paragraphLines);
    }
    return html;
}

}  // namespace

QString MarkdownRenderer::renderToHtml(const QString& markdown) {
    return QStringLiteral(R"(<!doctype html><html><head><meta charset="utf-8"><style>
body{margin:0;color:#172033;font-size:13px;}
p{margin:0 0 8px 0;}
h1,h2,h3,h4,h5,h6{margin:0 0 8px 0;font-weight:800;color:#101828;}
h1{font-size:20px;}h2{font-size:18px;}h3{font-size:16px;}h4,h5,h6{font-size:14px;}
ul,ol{margin:0 0 8px 18px;padding:0;}
li{margin:2px 0;}
pre{margin:0 0 8px 0;padding:8px;border-radius:6px;background:#f2f4f7;white-space:pre-wrap;}
code{font-family:Menlo,Consolas,monospace;background:#f2f4f7;border-radius:4px;padding:1px 4px;}
pre code{padding:0;background:transparent;border-radius:0;}
a{color:#0b67b7;text-decoration:none;}
blockquote{margin:0 0 8px 0;padding-left:10px;border-left:3px solid #dae4f0;color:#475467;}
</style></head><body>)") + renderBody(markdown) + QStringLiteral("</body></html>");
}
