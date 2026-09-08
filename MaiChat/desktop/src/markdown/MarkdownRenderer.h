#pragma once

#include <QString>

class MarkdownRenderer final {
public:
    static QString renderToHtml(const QString& markdown);
    // Semantic HTML for plain previews: no decorative code labels or padding.
    static QString renderPreviewHtml(const QString& markdown);
};
