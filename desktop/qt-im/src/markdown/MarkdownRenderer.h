#pragma once

#include <QString>

class MarkdownRenderer final {
public:
    static QString renderToHtml(const QString& markdown);
};
