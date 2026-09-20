#include "markdown/MarkdownLayout.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>
#include <QVector>
#include <algorithm>
#include <vector>

namespace {

// 一段排好版的文字。位置相对内容左上角。
struct TextRun {
    std::unique_ptr<QTextLayout> layout;
    QPointF position;
    qreal height = 0;
    // 这段文字在 selectableText 里的起点，选区和复制都按这个换算。
    int textStart = 0;
    // 行内代码的胶囊底。**自己画**：QTextCharFormat 的背景是个贴着字的方块，
    // 没有内边距也没有圆角，看起来很糙——这正是换掉 QTextDocument 的理由之一。
    QVector<QRectF> chips;

    struct Link {
        int start = 0;
        int length = 0;
        QString href;
    };
    QVector<Link> links;
};

// 文字底下的那些东西：底色、竖条、分割线、项目符号、复选框、表格线。
// 一种结构覆盖全部，比每样开一个类型省事得多。
struct Shape {
    QRectF rect;
    QColor fill;
    QColor stroke;
    qreal strokeWidth = 1;
    qreal radius = 0;
    // 任务列表打的勾。用路径画而不是字符，字体里有没有 ✓ 不该影响显示。
    bool check = false;
};

struct RunMetrics {
    qreal height = 0;
    qreal firstLineHeight = 0;
};

}  // namespace

struct MarkdownLayout::Impl {
    MarkdownTheme theme;
    qreal width = 0;
    qreal height = 0;
    std::vector<TextRun> runs;
    QVector<Shape> shapes;
    QString text;
    int selectionFrom = 0;
    int selectionTo = 0;
};

namespace {

// 排版的全部逻辑。一次性对象：构造、run()、丢掉。
class Builder {
public:
    Builder(MarkdownLayout::Impl& out, qreal width) : out_(out), theme_(out.theme), width_(width) {}

    void run(const QVector<MarkdownBlock>& blocks) {
        const qreal bottom = layoutRange(blocks, 0, blocks.size(), 0, 0, width_, 0);
        out_.height = bottom;
    }

private:
    // ── 字体 ────────────────────────────────────────────────

    QFont bodyFont() const {
        QFont font;
        if (!theme_.bodyFamily.isEmpty()) font.setFamily(theme_.bodyFamily);
        font.setPixelSize(theme_.bodyPixelSize);
        return font;
    }

    QFont headingFont(int level) const {
        QFont font = bodyFont();
        font.setPixelSize(theme_.headingPixelSize[qBound(1, level, 6) - 1]);
        // CSS 里标题是 font-weight:500。QTextDocument 把它映射到 Qt 的 0..99，
        // 500 就是 DemiBold(63)——不能照搬浏览器的数值，700 在 Qt 里已经接近 Black。
        font.setWeight(QFont::DemiBold);
        return font;
    }

    // 换等宽字体。
    //
    // **setFamily 一个人不够。** 应用字体是用 setFamilies({Segoe UI, 微软雅黑, ...})
    // 设的，而 QFont 默认构造会把那份列表带过来；列表非空时 Qt 按列表解析，
    // setFamily 设的那个名字直接被忽略——代码块看起来和正文一模一样。
    // 两个都设才盖得住。
    QFont monospace(int pixelSize) const {
        QFont font;
        font.setFamilies({theme_.codeFamily});
        font.setFamily(theme_.codeFamily);
        font.setPixelSize(pixelSize);
        return font;
    }

    QFont codeFont() const {
        return monospace(theme_.codePixelSize);
    }

    // 一行的实际行距。Qt 给的 line.height() 偏挤，按字号乘一个倍率抬一下；
    // 取大值是为了照顾行里混了大字号（比如行内代码之外的标题）的情况。
    qreal lineAdvance(const QTextLine& line, int pixelSize) const {
        return qMax<qreal>(line.height(), pixelSize * theme_.lineHeightRatio);
    }

    // ── 文字 ────────────────────────────────────────────────

    // 把一串片段排成一段。返回高度；文字为空时不产生任何东西。
    RunMetrics addSpans(const QVector<MarkdownSpan>& spans, const QFont& base, const QColor& color,
                        qreal x, qreal y, qreal avail, Qt::Alignment alignment = Qt::AlignLeft) {
        QString text;
        QVector<QTextLayout::FormatRange> formats;
        QVector<TextRun::Link> links;
        QVector<QPair<int, int>> codeRanges;

        for (const MarkdownSpan& span : spans) {
            if (span.text.isEmpty()) continue;
            const int start = text.size();
            text += span.text;

            QTextCharFormat format;
            QFont font = base;
            const bool isCode = span.styles.testFlag(MarkdownStyle::Code);
            if (isCode) {
                // 行内代码整只换成等宽字体，粗细和斜体的标志还留着。
                const QFont mono = monospace(theme_.codePixelSize);
                font.setFamilies(mono.families());
                font.setFamily(mono.family());
                font.setPixelSize(theme_.codePixelSize);
            }
            if (span.styles.testFlag(MarkdownStyle::Bold)) font.setWeight(QFont::Bold);
            if (span.styles.testFlag(MarkdownStyle::Italic)) font.setItalic(true);
            if (span.styles.testFlag(MarkdownStyle::Strike)) font.setStrikeOut(true);
            const bool isLink =
                span.styles.testFlag(MarkdownStyle::Link) && !span.href.isEmpty();
            if (isLink) font.setUnderline(true);
            format.setFont(font);

            // 颜色只能有一个，按「越具体越优先」排：行内代码 > 链接 > 删除线 >
            // 加粗 > 强调 > 正文色。加粗的链接要看起来像链接，不是像加粗。
            if (isCode) {
                format.setForeground(theme_.inlineCodeText);
            } else if (isLink) {
                format.setForeground(theme_.link);
            } else if (span.styles.testFlag(MarkdownStyle::Strike)) {
                format.setForeground(theme_.strike);
            } else if (span.styles.testFlag(MarkdownStyle::Bold)) {
                format.setForeground(theme_.strong);
            } else if (span.styles.testFlag(MarkdownStyle::Italic)) {
                format.setForeground(theme_.emphasis);
            } else {
                format.setForeground(color);
            }

            QTextLayout::FormatRange range;
            range.start = start;
            range.length = span.text.size();
            range.format = format;
            formats.push_back(range);

            if (isLink) links.push_back({start, span.text.size(), span.href});
            if (isCode) codeRanges.push_back({start, span.text.size()});
        }

        if (text.isEmpty()) return {};
        return addLaidOutText(text, base, formats, links, codeRanges, x, y, avail, alignment);
    }

    // 纯文字（代码行、列表序号、提示框标题）。没有片段，整段一个格式。
    RunMetrics addPlainText(const QString& text, const QFont& font, const QColor& color, qreal x,
                            qreal y, qreal avail, Qt::Alignment alignment = Qt::AlignLeft) {
        if (text.isEmpty()) return {};
        QTextCharFormat format;
        format.setFont(font);
        format.setForeground(color);
        QTextLayout::FormatRange range;
        range.start = 0;
        range.length = text.size();
        range.format = format;
        return addLaidOutText(text, font, {range}, {}, {}, x, y, avail, alignment);
    }

    RunMetrics addLaidOutText(const QString& text, const QFont& base,
                              const QVector<QTextLayout::FormatRange>& formats,
                              const QVector<TextRun::Link>& links,
                              const QVector<QPair<int, int>>& codeRanges, qreal x, qreal y,
                              qreal avail, Qt::Alignment alignment) {
        TextRun run;
        run.layout = std::make_unique<QTextLayout>(text, base);
        run.position = QPointF(x, y);
        run.textStart = out_.text.size();
        run.links = links;

        QTextOption option;
        // 长 URL、长代码 token 没有词边界，只按词断会把它整条甩出可视区。
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setAlignment(alignment);
        run.layout->setTextOption(option);
        run.layout->setFormats(formats);

        const int pixelSize = base.pixelSize() > 0 ? base.pixelSize() : theme_.bodyPixelSize;
        qreal height = 0;
        qreal firstLineHeight = 0;
        run.layout->beginLayout();
        while (true) {
            QTextLine line = run.layout->createLine();
            if (!line.isValid()) break;
            line.setLineWidth(qMax<qreal>(avail, 1));
            line.setPosition(QPointF(0, height));
            const qreal advance = lineAdvance(line, pixelSize);
            if (firstLineHeight <= 0) firstLineHeight = advance;
            height += advance;
        }
        run.layout->endLayout();
        run.height = height;

        for (const QPair<int, int>& range : codeRanges) {
            appendChips(run, range.first, range.second);
        }

        // 块之间补一个换行，复制出来才是分段的，不会糊成一行。
        out_.text += text;
        out_.text += QLatin1Char('\n');

        out_.runs.push_back(std::move(run));
        return {height, firstLineHeight};
    }

    // 行内代码的胶囊底。一段代码可能被折到两行，所以要逐行算 x 区间。
    void appendChips(TextRun& run, int start, int length) {
        const qreal padX = qMax(1, theme_.codePixelSize / 4);
        for (int index = 0; index < run.layout->lineCount(); ++index) {
            const QTextLine line = run.layout->lineAt(index);
            const int from = qMax(start, line.textStart());
            const int to = qMin(start + length, line.textStart() + line.textLength());
            if (from >= to) continue;
            const qreal left = line.cursorToX(from);
            const qreal right = line.cursorToX(to);
            run.chips.push_back(QRectF(qMin(left, right) - padX, line.y() + 1,
                                       qAbs(right - left) + 2 * padX,
                                       qMax<qreal>(line.height() - 2, 2)));
        }
    }

    qreal measureSpans(const QVector<MarkdownSpan>& spans, const QFont& base) const {
        qreal total = 0;
        for (const MarkdownSpan& span : spans) {
            QFont font = base;
            if (span.styles.testFlag(MarkdownStyle::Code)) {
                font.setFamilies({theme_.codeFamily});
                font.setFamily(theme_.codeFamily);
                font.setPixelSize(theme_.codePixelSize);
            }
            if (span.styles.testFlag(MarkdownStyle::Bold)) font.setWeight(QFont::Bold);
            // 宽度用 horizontalAdvance，不用 boundingRect——后者会少算，
            // 表格列会窄一截，里面的字被迫提前折行。
            total += QFontMetricsF(font).horizontalAdvance(span.text);
        }
        return total;
    }

    // ── 块之间的留白 ────────────────────────────────────────

    int spacingAbove(const MarkdownBlock& block) const {
        if (block.kind == MarkdownBlockKind::Heading) {
            return theme_.headingSpacingAbove[qBound(1, block.headingLevel, 6) - 1];
        }
        if (block.kind == MarkdownBlockKind::Divider) return theme_.dividerSpacing;
        return theme_.blockSpacing;
    }

    int spacingBelow(const MarkdownBlock& block) const {
        if (block.kind == MarkdownBlockKind::Heading) {
            return theme_.headingSpacingBelow[qBound(1, block.headingLevel, 6) - 1];
        }
        if (block.kind == MarkdownBlockKind::Divider) return theme_.dividerSpacing;
        return theme_.blockSpacing;
    }

    // ── 块 ──────────────────────────────────────────────────

    // [from, to) 这一段块，排在 depth 层引用里。返回排完之后的 y。
    qreal layoutRange(const QVector<MarkdownBlock>& blocks, int from, int to, int depth, qreal x,
                      qreal avail, qreal y) {
        bool first = true;
        int previousBelow = 0;

        int index = from;
        while (index < to) {
            const MarkdownBlock& block = blocks[index];

            // 更深一层的引用：把**同属一个引用**的块收进一个框里。
            // 比编号不是比深度：相邻的两个 `>` 深度一样却是两个框。
            if (block.quoteDepth() > depth) {
                const int group = block.quoteIds[depth];
                int end = index;
                while (end < to && blocks[end].quoteDepth() > depth &&
                       blocks[end].quoteIds[depth] == group) {
                    ++end;
                }
                if (!first) y += qMax(previousBelow, theme_.blockSpacing);
                y = layoutQuote(blocks, index, end, depth + 1, x, avail, y);
                previousBelow = theme_.blockSpacing;
                first = false;
                index = end;
                continue;
            }

            if (!first) y += qMax(previousBelow, spacingAbove(block));
            y = layoutBlock(block, x, avail, y);
            previousBelow = spacingBelow(block);
            first = false;
            ++index;
        }
        return y;
    }

    qreal layoutQuote(const QVector<MarkdownBlock>& blocks, int from, int to, int depth, qreal x,
                      qreal avail, qreal y) {
        const MarkdownCalloutStyle style = theme_.calloutStyle(blocks[from].callout);

        // 背景和竖条要画在里面的东西**底下**，而里面可能还有代码块的底色。
        // shapes 是按加入顺序画的，所以先占两个位，尺寸等内容排完再回填。
        const int backgroundIndex = out_.shapes.size();
        out_.shapes.push_back(Shape{});
        const int barIndex = out_.shapes.size();
        out_.shapes.push_back(Shape{});

        const qreal top = y;
        const qreal innerX = x + theme_.quoteBarWidth + theme_.quotePadding;
        const qreal innerAvail =
            qMax<qreal>(avail - theme_.quoteBarWidth - 2 * theme_.quotePadding, 1);
        qreal inner = y + theme_.quotePadding;

        if (!style.title.isEmpty()) {
            QFont font = bodyFont();
            font.setPixelSize(theme_.calloutTitlePixelSize);
            font.setWeight(QFont::Bold);
            inner += addPlainText(style.title, font, style.accent, innerX, inner, innerAvail).height;
            inner += theme_.blockSpacing / 2;
        }

        inner = layoutRange(blocks, from, to, depth, innerX, innerAvail, inner);
        const qreal bottom = inner + theme_.quotePadding;

        Shape background;
        background.rect = QRectF(x, top, avail, bottom - top);
        background.fill = style.background;
        background.radius = theme_.quoteRadius;
        out_.shapes[backgroundIndex] = background;

        Shape bar;
        // 竖条压在圆角背景左边，所以它自己不能有圆角，否则两层弧线对不齐。
        bar.rect = QRectF(x, top, theme_.quoteBarWidth, bottom - top);
        bar.fill = style.accent;
        out_.shapes[barIndex] = bar;

        return bottom;
    }

    qreal layoutBlock(const MarkdownBlock& block, qreal x, qreal avail, qreal y) {
        const QColor color = block.quoteDepth() > 0 ? theme_.quoteText : theme_.text;
        switch (block.kind) {
            case MarkdownBlockKind::Paragraph:
                return y + addSpans(block.spans, bodyFont(), color, x, y, avail).height;

            case MarkdownBlockKind::Heading: {
                const int level = qBound(1, block.headingLevel, 6);
                return y + addSpans(block.spans, headingFont(level),
                                    theme_.headingColor[level - 1], x, y, avail)
                               .height;
            }

            case MarkdownBlockKind::Code:
                return layoutCode(block, x, avail, y);

            case MarkdownBlockKind::List:
                return layoutList(block, color, x, avail, y);

            case MarkdownBlockKind::Divider: {
                Shape line;
                line.rect = QRectF(x, y, avail, 1);
                line.fill = theme_.divider;
                out_.shapes.push_back(line);
                return y + 1;
            }

            case MarkdownBlockKind::Table:
                return layoutTable(block, x, avail, y);
        }
        return y;
    }

    qreal layoutCode(const MarkdownBlock& block, qreal x, qreal avail, qreal y) {
        const int backgroundIndex = out_.shapes.size();
        out_.shapes.push_back(Shape{});

        const qreal top = y;
        const qreal innerX = x + theme_.codePadding;
        const qreal innerAvail = qMax<qreal>(avail - 2 * theme_.codePadding, 1);
        y += theme_.codePadding;

        const QFont font = codeFont();
        const qreal emptyLineHeight = theme_.codePixelSize * theme_.lineHeightRatio;
        QString code = block.code;
        // 围栏结束前的那个换行是语法的一部分，不是空行——留着会多出一行空白。
        if (code.endsWith(QLatin1Char('\n'))) code.chop(1);

        const QStringList lines = code.split(QLatin1Char('\n'));
        for (const QString& line : lines) {
            if (line.isEmpty()) {
                // 空行也要占位，否则代码里的分段全被压掉。
                out_.text += QLatin1Char('\n');
                y += emptyLineHeight;
                continue;
            }
            y += addPlainText(line, font, theme_.codeText, innerX, y, innerAvail).height;
        }
        y += theme_.codePadding;

        Shape background;
        background.rect = QRectF(x, top, avail, y - top);
        background.fill = theme_.codeBackground;
        // 圆角——QTextDocument 的 CSS 子集画不出来，这是换掉它最直观的那一项。
        background.radius = theme_.codeRadius;
        out_.shapes[backgroundIndex] = background;
        return y;
    }

    qreal layoutList(const MarkdownBlock& block, const QColor& color, qreal x, qreal avail,
                     qreal y) {
        const QFont font = bodyFont();
        bool first = true;
        for (const MarkdownListItem& item : block.items) {
            if (!first) y += theme_.listItemSpacing;
            first = false;

            const qreal itemX = x + item.depth * theme_.listIndent;
            const qreal textX = itemX + theme_.listIndent;
            const qreal textAvail = qMax<qreal>(avail - (textX - x), 1);
            const qreal top = y;

            const RunMetrics metrics = addSpans(item.spans, font, color, textX, y, textAvail);
            const qreal firstLine =
                metrics.firstLineHeight > 0 ? metrics.firstLineHeight
                                            : theme_.bodyPixelSize * theme_.lineHeightRatio;

            if (item.hasCheckbox) {
                Shape box;
                box.rect = QRectF(itemX + 2, top + (firstLine - theme_.checkboxSize) / 2,
                                  theme_.checkboxSize, theme_.checkboxSize);
                box.radius = qMax(2, theme_.checkboxSize / 4);
                box.check = item.checked;
                if (item.checked) {
                    box.fill = theme_.checkboxOn;
                    box.stroke = theme_.checkboxOn;
                } else {
                    box.stroke = theme_.checkboxOff;
                }
                out_.shapes.push_back(box);
            } else if (item.number > 0) {
                // 序号右对齐贴着正文，列表才是一条竖线对齐的。
                const qreal gap = qMax<qreal>(6, theme_.listIndent / 4);
                addPlainText(QStringLiteral("%1.").arg(item.number), font, theme_.emphasis, itemX,
                             top, theme_.listIndent - gap, Qt::AlignRight);
            } else {
                Shape dot;
                const qreal radius = theme_.bulletRadius;
                dot.rect = QRectF(itemX + 4, top + firstLine / 2 - radius, radius * 2, radius * 2);
                dot.fill = theme_.bullet;
                dot.radius = radius;
                out_.shapes.push_back(dot);
            }

            y = top + qMax(metrics.height, firstLine);
        }
        return y;
    }

    qreal layoutTable(const MarkdownBlock& block, qreal x, qreal avail, qreal y) {
        if (block.rows.isEmpty()) return y;

        int columns = 0;
        for (const QVector<QVector<MarkdownSpan>>& row : block.rows) {
            columns = qMax(columns, row.size());
        }
        if (columns == 0) return y;

        QFont font = bodyFont();
        font.setPixelSize(theme_.tablePixelSize);

        // 先量每列的自然宽度，再按可用宽度等比缩——直接平分的话，
        // 一列两个字、一列一整句时会难看到没法读。
        QVector<qreal> natural(columns, 0);
        for (const QVector<QVector<MarkdownSpan>>& row : block.rows) {
            for (int column = 0; column < row.size(); ++column) {
                natural[column] = qMax(natural[column], measureSpans(row[column], font));
            }
        }

        const qreal padding = theme_.tableCellPadding;
        qreal total = 0;
        for (int column = 0; column < columns; ++column) total += natural[column] + 2 * padding;

        QVector<qreal> widths(columns, 0);
        const qreal minimum = qMin<qreal>(avail / columns, theme_.tablePixelSize * 4);
        if (total <= avail || total <= 0) {
            const qreal extra = (avail - total) / columns;
            for (int column = 0; column < columns; ++column) {
                widths[column] = natural[column] + 2 * padding + qMax<qreal>(extra, 0);
            }
        } else {
            const qreal scale = avail / total;
            for (int column = 0; column < columns; ++column) {
                widths[column] = qMax(minimum, (natural[column] + 2 * padding) * scale);
            }
            qreal scaled = 0;
            for (qreal value : widths) scaled += value;
            if (scaled > avail) {
                for (int column = 0; column < columns; ++column) widths[column] *= avail / scaled;
            }
        }

        const qreal top = y;
        for (int rowIndex = 0; rowIndex < block.rows.size(); ++rowIndex) {
            const QVector<QVector<MarkdownSpan>>& row = block.rows[rowIndex];
            const bool header = rowIndex == 0;
            const int backgroundIndex = out_.shapes.size();
            out_.shapes.push_back(Shape{});

            qreal cellX = x;
            qreal rowHeight = 0;
            for (int column = 0; column < columns; ++column) {
                const qreal cellWidth = widths[column];
                if (column < row.size()) {
                    QFont cellFont = font;
                    if (header) cellFont.setWeight(QFont::DemiBold);
                    const RunMetrics metrics =
                        addSpans(row[column], cellFont,
                                 header ? theme_.tableHeaderText : theme_.text, cellX + padding,
                                 y + padding, qMax<qreal>(cellWidth - 2 * padding, 1));
                    rowHeight = qMax(rowHeight, metrics.height);
                }
                cellX += cellWidth;
            }
            if (rowHeight <= 0) rowHeight = theme_.tablePixelSize * theme_.lineHeightRatio;
            const qreal fullHeight = rowHeight + 2 * padding;

            Shape background;
            background.rect = QRectF(x, y, cellX - x, fullHeight);
            // 斑马纹用整行底色。原来靠给每个单元格上色模拟，因为
            // QTextDocument 不支持 nth-child；自己画就没这个问题了。
            background.fill = header ? theme_.tableHeaderBackground
                                     : (rowIndex % 2 == 1 ? theme_.tableRow
                                                          : theme_.tableRowAlternate);
            out_.shapes[backgroundIndex] = background;

            if (!header) {
                Shape separator;
                separator.rect = QRectF(x, y, cellX - x, 1);
                separator.fill = theme_.tableLine;
                out_.shapes.push_back(separator);
            }
            y += fullHeight;
        }

        Shape border;
        qreal tableWidth = 0;
        for (qreal value : widths) tableWidth += value;
        border.rect = QRectF(x, top, tableWidth, y - top);
        border.stroke = theme_.tableLine;
        out_.shapes.push_back(border);
        return y;
    }

    MarkdownLayout::Impl& out_;
    const MarkdownTheme& theme_;
    qreal width_;
};

}  // namespace

MarkdownLayout::MarkdownLayout() : impl_(std::make_unique<Impl>()) {}
MarkdownLayout::~MarkdownLayout() = default;
MarkdownLayout::MarkdownLayout(MarkdownLayout&&) noexcept = default;
MarkdownLayout& MarkdownLayout::operator=(MarkdownLayout&&) noexcept = default;

void MarkdownLayout::layout(const MarkdownDocument& document, const MarkdownTheme& theme,
                            qreal width) {
    impl_->theme = theme;
    impl_->width = width;
    impl_->height = 0;
    impl_->runs.clear();
    impl_->shapes.clear();
    impl_->text.clear();
    impl_->selectionFrom = 0;
    impl_->selectionTo = 0;
    if (width <= 0 || document.isEmpty()) return;

    // 流式输出时每来一段就整篇重排，长回答下是 O(n²)。调用方拿节流挡着够用，
    // 真成瓶颈时再改成只重排最后一个块——那需要按块记住 y 偏移。
    Builder builder(*impl_, width);
    builder.run(document.blocks());
}

qreal MarkdownLayout::width() const {
    return impl_->width;
}

qreal MarkdownLayout::height() const {
    return impl_->height;
}

qreal MarkdownLayout::naturalWidth() const {
    qreal widest = 0;
    for (const TextRun& run : impl_->runs) {
        // 加上 x：列表项、引用里的内容有缩进，只看文字宽度会少算缩进那一截。
        widest = qMax(widest, run.position.x() + run.layout->maximumWidth());
    }
    for (const Shape& shape : impl_->shapes) {
        widest = qMax(widest, shape.rect.right());
    }
    return widest;
}

bool MarkdownLayout::isEmpty() const {
    return impl_->runs.empty() && impl_->shapes.isEmpty();
}

void MarkdownLayout::paint(QPainter* painter, const QPointF& origin, const QRectF& clip) const {
    if (painter == nullptr) return;
    const bool clipped = clip.isValid();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    // clip 有两个作用：跳过整段看不见的内容（省下的是排版好的几十个 QTextLayout
    // 的绘制），以及真的把边界外裁掉。只做前者不够——一段文字是整体绘制的，
    // 它只要有一行露在可见区里就会被整段画出来，尾巴会溢到边界外面。
    if (clipped) painter->setClipRect(clip, Qt::IntersectClip);

    for (const Shape& shape : impl_->shapes) {
        const QRectF rect = shape.rect.translated(origin);
        if (clipped && !clip.intersects(rect)) continue;
        painter->setPen(shape.stroke.isValid() ? QPen(shape.stroke, shape.strokeWidth)
                                               : QPen(Qt::NoPen));
        painter->setBrush(shape.fill.isValid() ? QBrush(shape.fill) : QBrush(Qt::NoBrush));
        if (shape.radius > 0) {
            painter->drawRoundedRect(rect, shape.radius, shape.radius);
        } else {
            painter->drawRect(rect);
        }
        if (!shape.check) continue;
        // 勾用路径画，不用字符：字体里有没有 ✓ 不该决定复选框长什么样。
        QPainterPath path;
        path.moveTo(rect.left() + rect.width() * 0.24, rect.top() + rect.height() * 0.52);
        path.lineTo(rect.left() + rect.width() * 0.43, rect.top() + rect.height() * 0.72);
        path.lineTo(rect.left() + rect.width() * 0.78, rect.top() + rect.height() * 0.29);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(Qt::white, qMax<qreal>(1.4, rect.width() / 8.0), Qt::SolidLine,
                             Qt::RoundCap, Qt::RoundJoin));
        painter->drawPath(path);
    }

    const int from = qMin(impl_->selectionFrom, impl_->selectionTo);
    const int to = qMax(impl_->selectionFrom, impl_->selectionTo);

    painter->setPen(impl_->theme.text);
    for (const TextRun& run : impl_->runs) {
        const QPointF position = origin + run.position;
        const QRectF bounds(position, QSizeF(impl_->width, run.height));
        if (clipped && !clip.intersects(bounds)) continue;

        if (!run.chips.isEmpty()) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(impl_->theme.inlineCodeBackground);
            const qreal radius = qMax(3, impl_->theme.codePixelSize / 3);
            for (const QRectF& chip : run.chips) {
                painter->drawRoundedRect(chip.translated(position), radius, radius);
            }
        }

        QVector<QTextLayout::FormatRange> selections;
        if (to > from) {
            const int length = run.layout->text().size();
            const int start = qMax(0, from - run.textStart);
            const int end = qMin(length, to - run.textStart);
            if (end > start) {
                QTextLayout::FormatRange range;
                range.start = start;
                range.length = end - start;
                range.format.setBackground(impl_->theme.selection);
                range.format.setForeground(impl_->theme.selectionText);
                selections.push_back(range);
            }
        }
        run.layout->draw(painter, position, selections);
    }
    painter->restore();
}

QString MarkdownLayout::linkAt(const QPointF& point) const {
    for (const TextRun& run : impl_->runs) {
        if (run.links.isEmpty()) continue;
        const QRectF bounds(run.position, QSizeF(impl_->width, run.height));
        if (!bounds.contains(point)) continue;
        const QPointF local = point - run.position;

        for (int index = 0; index < run.layout->lineCount(); ++index) {
            const QTextLine line = run.layout->lineAt(index);
            if (local.y() < line.y() || local.y() > line.y() + line.height()) continue;
            // 超出行尾的位置也会被 xToCursor 映射到最后一个字符上，
            // 那会让行尾右边一整片空白都变成可点的链接。先把行宽挡住。
            if (local.x() > line.naturalTextWidth()) return QString();
            const int cursor = line.xToCursor(local.x());
            for (const TextRun::Link& link : run.links) {
                if (cursor >= link.start && cursor < link.start + link.length) return link.href;
            }
        }
    }
    return QString();
}

int MarkdownLayout::positionAt(const QPointF& point) const {
    const TextRun* best = nullptr;
    qreal bestDistance = 0;
    for (const TextRun& run : impl_->runs) {
        const qreal top = run.position.y();
        const qreal bottom = top + run.height;
        if (point.y() >= top && point.y() <= bottom) {
            best = &run;
            break;
        }
        const qreal distance = point.y() < top ? top - point.y() : point.y() - bottom;
        if (best == nullptr || distance < bestDistance) {
            best = &run;
            bestDistance = distance;
        }
    }
    if (best == nullptr) return 0;

    const QPointF local = point - best->position;
    for (int index = 0; index < best->layout->lineCount(); ++index) {
        const QTextLine line = best->layout->lineAt(index);
        const bool last = index == best->layout->lineCount() - 1;
        if (!last && local.y() > line.y() + line.height()) continue;
        return best->textStart + line.xToCursor(local.x());
    }
    return best->textStart;
}

int MarkdownLayout::textLength() const {
    return impl_->text.size();
}

const QString& MarkdownLayout::selectableText() const {
    return impl_->text;
}

void MarkdownLayout::setSelection(int from, int to) {
    impl_->selectionFrom = qBound(0, from, impl_->text.size());
    impl_->selectionTo = qBound(0, to, impl_->text.size());
}

void MarkdownLayout::clearSelection() {
    impl_->selectionFrom = 0;
    impl_->selectionTo = 0;
}

QString MarkdownLayout::selectedText() const {
    const int from = qMin(impl_->selectionFrom, impl_->selectionTo);
    const int to = qMax(impl_->selectionFrom, impl_->selectionTo);
    if (to <= from) return QString();
    return impl_->text.mid(from, to - from);
}
