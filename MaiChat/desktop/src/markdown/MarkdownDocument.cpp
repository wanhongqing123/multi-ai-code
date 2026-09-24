#include "markdown/MarkdownDocument.h"

#include <QByteArray>
#include <QRegularExpression>

#include "md4c.h"

// 用 md4c 的**回调 API**（md_parse），不是 md_html。
//
// md_html 直接吐 HTML，那是给 QTextDocument 用的老路线。这里要的是一棵能自己排版的树，
// 所以接 enter_block / leave_block / enter_span / leave_span / text 这五个回调，
// 自己攒结构。
//
// 方言和老路线保持一致（MD_DIALECT_GITHUB + MD_FLAG_NOHTML），否则同一段文字在
// agent 和 IM 里会渲染成两样。

namespace {

// md4c 的回调是 C 函数指针，状态只能靠 userdata 传。所有活都在这个结构里做。
struct Builder {
    QVector<MarkdownBlock> blocks;

    // 正在攒的块。leave_block 时才 push 进 blocks——
    // 因为要等它的所有 span 和 text 都到齐。
    MarkdownBlock current;
    bool inBlock = false;

    // 行内样式是**栈**：md4c 按 enter_span / leave_span 成对给，中间可以嵌套。
    // 用计数而不是布尔：`**粗体里的**粗体**` 这种会嵌两层，用布尔的话内层一退就全没了。
    int bold = 0;
    int italic = 0;
    int code = 0;
    int strike = 0;
    int listCodeBlockDepth = 0;
    QVector<QString> linkStack;

    // 列表状态。md4c 的列表是嵌套块，深度靠 enter/leave 配对数出来。
    int listDepth = -1;
    QVector<int> orderedCounters;  // 每层的下一个序号；0 表示这层是无序列表
    // **正在填的列表项要用栈，不能用单个槽。**
    //
    // 嵌套列表里子列表是长在父 LI **内部**的，md4c 的回调顺序是
    // enter LI(父) → 父的文字 → enter UL → enter LI(子) → … → leave LI(父)。
    // 用一个槽的话，内层 LI 一开始就把父项的文字盖掉了，
    // 最后 leave LI(父) 收的是最后一个子项——父项彻底丢失，深度也全错。
    //
    // 存的是 current.items 里的下标而不是对象：**进入时就先占位**，
    // 这样父项天然排在子项前面，不用事后再排序。
    QVector<int> itemStack;

    // 引用。**只记深度，不建块**：引用里能装段落、列表、代码，
    // 做成「一种块」就得递归，扁平打标记更好画。
    int quoteDepth = 0;
    // 每层引用的编号。给块打标记用，同一个引用里的块拿到同一个编号。
    QVector<int> quoteIdStack;
    int nextQuoteId = 0;
    // 每层引用是从 blocks 的哪一条开始的——退出这层时要回头给这些块盖上提示框类型。
    QVector<int> quoteStart;
    QVector<MarkdownCallout> quoteCallout;
    // 提示框标记先扣着不发：`[!TIP]` 必须**独占一行**才算标记，
    // 而 md4c 把它当普通文字给出来，得等下一个事件才知道后面是不是换行。
    MarkdownCallout pendingCallout = MarkdownCallout::None;
    QString pendingCalloutText;

    // 表格。
    bool inTable = false;
    bool inTableHeadRow = false;
    QVector<QVector<MarkdownSpan>> currentRow;
    QVector<MarkdownSpan> currentCell;
    bool inTableCell = false;

    MarkdownStyles currentStyles() const {
        MarkdownStyles styles = MarkdownStyle::Normal;
        if (bold > 0) styles |= MarkdownStyle::Bold;
        if (italic > 0) styles |= MarkdownStyle::Italic;
        if (code > 0) styles |= MarkdownStyle::Code;
        if (strike > 0) styles |= MarkdownStyle::Strike;
        if (!linkStack.isEmpty()) styles |= MarkdownStyle::Link;
        return styles;
    }

    // 文字往哪儿落，取决于当前在什么结构里。
    QVector<MarkdownSpan>* sink() {
        if (inTableCell) return &currentCell;
        if (!itemStack.isEmpty()) {
            const int index = itemStack.last();
            if (index < 0 || index >= current.items.size()) return nullptr;
            return &current.items[index].spans;
        }
        if (inBlock) return &current.spans;
        return nullptr;
    }

    bool inListItem() const {
        return !itemStack.isEmpty();
    }

    // 所有块都从这儿进 blocks，这样引用深度只需要盖一个地方。
    void emitBlock(MarkdownBlock block) {
        block.quoteIds = quoteIdStack;
        blocks.push_back(block);
    }

    // 标记后面又来了别的东西 —— 那它就不是标记，把原文补回去。
    void flushPendingCallout() {
        if (pendingCallout == MarkdownCallout::None) return;
        pendingCallout = MarkdownCallout::None;
        const QString literal = pendingCalloutText;
        pendingCalloutText.clear();
        addText(literal);
    }

    // 标记后面是换行 —— 成立，记在当前这层引用上。
    void commitPendingCallout() {
        if (pendingCallout == MarkdownCallout::None) return;
        if (!quoteCallout.isEmpty()) quoteCallout.last() = pendingCallout;
        pendingCallout = MarkdownCallout::None;
        pendingCalloutText.clear();
    }

    // 只在引用里、段落刚开头的位置认标记。别处出现的 `[!TIP]` 是普通文字。
    bool tryHoldCallout(const QString& piece);

    // 标记和正文在**同一段文字**里 —— `> [!TIP] 正文`。md4c 不会为了我们把它
    // 拆成两段，整行就是一个 MD_TEXT_NORMAL，所以得自己在开头找标记。
    bool tryStartCalloutInline(QString* piece);

    // 认不认「标记后面紧跟正文」这件事，两条路共用一个判据。
    bool calloutPositionAllowed() const;

    // 标记后面在**同一行**上还有字 —— `> [!TIP] 正文`。GFM 要求标记独占一行，
    // 但人就是会这么写，老的 HTML 渲染器也一直认这种形式，换成自绘不能退化。
    //
    // 判据是「后面那段字以空白开头」：`[!TIP] 正文` 认，`[!TIP]正文` 不认——
    // 后者更像是有人在写一段以方括号开头的普通话。
    bool tryFinishCalloutInline(QString* piece) {
        if (pendingCallout == MarkdownCallout::None) return false;
        if (piece->isEmpty() || !piece->at(0).isSpace()) return false;
        commitPendingCallout();
        int start = 0;
        while (start < piece->size() && piece->at(start).isSpace()) ++start;
        *piece = piece->mid(start);
        return true;
    }

    void addText(const QString& text) {
        QVector<MarkdownSpan>* target = sink();
        if (target == nullptr || text.isEmpty()) return;

        const MarkdownStyles styles = currentStyles();
        const QString href = linkStack.isEmpty() ? QString() : linkStack.last();
        // 和前一个片段样式相同就直接接上去，不要每个字符一个片段——
        // 绘制层按片段建 QTextLayout 的格式区间，片段越碎排版越慢。
        if (!target->isEmpty() && target->last().styles == styles && target->last().href == href) {
            target->last().text += text;
            return;
        }
        target->push_back(MarkdownSpan{text, styles, href});
    }
};

// `[!NOTE]` 这些。名字和 GFM 一致，大小写不敏感。
MarkdownCallout calloutFor(const QString& marker) {
    static const struct {
        const char* name;
        MarkdownCallout kind;
    } table[] = {
        {"NOTE", MarkdownCallout::Note},
        {"TIP", MarkdownCallout::Tip},
        {"IMPORTANT", MarkdownCallout::Important},
        {"WARNING", MarkdownCallout::Warning},
        {"CAUTION", MarkdownCallout::Caution},
    };
    for (const auto& entry : table) {
        if (marker.compare(QLatin1String(entry.name), Qt::CaseInsensitive) == 0) return entry.kind;
    }
    return MarkdownCallout::None;
}

// 只在引用里、当前段落还一个字都没有的时候认。别处的 `[!TIP]` 是普通文字。
bool Builder::calloutPositionAllowed() const {
    if (pendingCallout != MarkdownCallout::None) return false;
    if (quoteDepth == 0 || quoteCallout.isEmpty()) return false;
    if (quoteCallout.last() != MarkdownCallout::None) return false;
    if (!inBlock || current.kind != MarkdownBlockKind::Paragraph) return false;
    if (!current.spans.isEmpty()) return false;
    // 链接里的、行内代码里的 `[!TIP]` 是**写给人看的字面量**，不是标记。
    // 没有这两条，`> [[!TIP]](url)` 和 `` > `[!TIP]` `` 会被当成提示框。
    if (!linkStack.isEmpty()) return false;
    if (currentStyles().testFlag(MarkdownStyle::Code)) return false;
    return true;
}

bool Builder::tryHoldCallout(const QString& piece) {
    if (!calloutPositionAllowed()) return false;
    if (!piece.startsWith(QLatin1String("[!")) || !piece.endsWith(QLatin1Char(']'))) return false;

    const MarkdownCallout kind = calloutFor(piece.mid(2, piece.size() - 3));
    if (kind == MarkdownCallout::None) return false;
    pendingCallout = kind;
    pendingCalloutText = piece;
    return true;
}

bool Builder::tryStartCalloutInline(QString* piece) {
    if (!calloutPositionAllowed()) return false;
    if (!piece->startsWith(QLatin1String("[!"))) return false;
    const int close = piece->indexOf(QLatin1Char(']'));
    if (close < 0) return false;
    // 标记后面必须**隔着空白**才算 —— `[!TIP] 正文` 认，`[!TIP]正文` 不认，
    // 后者更像有人在写一段以方括号开头的普通话。
    if (close + 1 >= piece->size() || !piece->at(close + 1).isSpace()) return false;

    const MarkdownCallout kind = calloutFor(piece->mid(2, close - 2));
    if (kind == MarkdownCallout::None) return false;
    quoteCallout.last() = kind;
    int start = close + 1;
    while (start < piece->size() && piece->at(start).isSpace()) ++start;
    *piece = piece->mid(start);
    return true;
}

QString fromMd(const MD_CHAR* text, MD_SIZE size) {
    return QString::fromUtf8(text, static_cast<int>(size));
}

QString attributeText(const MD_ATTRIBUTE& attribute) {
    return attribute.text == nullptr ? QString() : fromMd(attribute.text, attribute.size);
}

// md4c 不管链接协议。除 http/https/mailto/锚点外一律丢掉目标（文字保留），
// 和老渲染器的策略一致。
//
// **在解析阶段做**，不是在绘制阶段：绘制层只该管怎么画，不该承担安全判断——
// 将来多一个绘制端（比如导出 HTML），那边一忘就漏了。
QString sanitizeHref(const QString& href) {
    static const QRegularExpression safe(QStringLiteral("^(https?:|mailto:|#)"),
                                         QRegularExpression::CaseInsensitiveOption);
    return safe.match(href).hasMatch() ? href : QString();
}

int enterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto* builder = static_cast<Builder*>(userdata);
    switch (type) {
        case MD_BLOCK_DOC:
            break;

        case MD_BLOCK_P:
            // 列表项和表格单元格里也会有 P。那种情况不另起块，文字直接落进外层。
            if (builder->inListItem() || builder->inTableCell) break;
            builder->current = MarkdownBlock{};
            builder->current.kind = MarkdownBlockKind::Paragraph;
            builder->inBlock = true;
            break;

        case MD_BLOCK_H: {
            if (builder->inListItem()) break;
            builder->current = MarkdownBlock{};
            builder->current.kind = MarkdownBlockKind::Heading;
            builder->current.headingLevel = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
            builder->inBlock = true;
            break;
        }

        case MD_BLOCK_CODE: {
            if (builder->inListItem()) {
                ++builder->listCodeBlockDepth;
                ++builder->code;
                break;
            }
            builder->current = MarkdownBlock{};
            builder->current.kind = MarkdownBlockKind::Code;
            auto* codeDetail = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
            builder->current.language = attributeText(codeDetail->lang);
            builder->inBlock = true;
            break;
        }

        case MD_BLOCK_QUOTE:
            ++builder->quoteDepth;
            builder->quoteIdStack.push_back(builder->nextQuoteId++);
            builder->quoteStart.push_back(builder->blocks.size());
            builder->quoteCallout.push_back(MarkdownCallout::None);
            break;

        case MD_BLOCK_HR: {
            MarkdownBlock divider;
            divider.kind = MarkdownBlockKind::Divider;
            builder->emitBlock(divider);
            break;
        }

        case MD_BLOCK_UL:
        case MD_BLOCK_OL: {
            // 最外层的列表才新建块；嵌套的并进同一个块，靠 depth 区分。
            if (builder->listDepth < 0) {
                builder->current = MarkdownBlock{};
                builder->current.kind = MarkdownBlockKind::List;
                builder->inBlock = true;
            }
            ++builder->listDepth;
            const int start =
                type == MD_BLOCK_OL ? static_cast<int>(static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start)
                                    : 0;
            builder->orderedCounters.push_back(start);
            break;
        }

        case MD_BLOCK_LI: {
            auto* itemDetail = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
            MarkdownListItem item;
            item.depth = builder->listDepth;
            item.hasCheckbox = itemDetail->is_task != 0;
            item.checked = itemDetail->is_task != 0 &&
                           (itemDetail->task_mark == 'x' || itemDetail->task_mark == 'X');
            if (!builder->orderedCounters.isEmpty() && builder->orderedCounters.last() > 0) {
                item.number = builder->orderedCounters.last();
                ++builder->orderedCounters.last();
            }
            builder->current.items.push_back(item);
            builder->itemStack.push_back(builder->current.items.size() - 1);
            break;
        }

        case MD_BLOCK_TABLE:
            builder->current = MarkdownBlock{};
            builder->current.kind = MarkdownBlockKind::Table;
            builder->inBlock = true;
            builder->inTable = true;
            break;

        case MD_BLOCK_THEAD:
            builder->inTableHeadRow = true;
            break;

        case MD_BLOCK_TR:
            builder->currentRow.clear();
            break;

        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            builder->currentCell.clear();
            builder->inTableCell = true;
            break;

        default:
            break;
    }
    return 0;
}

int leaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
    auto* builder = static_cast<Builder*>(userdata);
    switch (type) {
        case MD_BLOCK_P:
            if (builder->inListItem() || builder->inTableCell) break;
            // 空段落不进树。md4c 在松散列表和某些嵌套结构里会给出空的 P，
            // 留着会在界面上变成一段莫名其妙的空行。
            builder->flushPendingCallout();
            if (!builder->current.spans.isEmpty()) builder->emitBlock(builder->current);
            builder->inBlock = false;
            break;

        case MD_BLOCK_H:
            if (builder->inListItem()) break;
            builder->emitBlock(builder->current);
            builder->inBlock = false;
            break;

        case MD_BLOCK_CODE:
            if (builder->listCodeBlockDepth > 0) {
                --builder->listCodeBlockDepth;
                if (builder->code > 0) --builder->code;
                break;
            }
            builder->emitBlock(builder->current);
            builder->inBlock = false;
            break;

        case MD_BLOCK_QUOTE: {
            // 走到这儿标记还扣着，说明这段引用只有 `> [!TIP]` 一行、后面什么都没有。
            // 老渲染器也认这种写法。
            builder->commitPendingCallout();
            const int start = builder->quoteStart.isEmpty() ? 0 : builder->quoteStart.takeLast();
            const MarkdownCallout callout = builder->quoteCallout.isEmpty()
                                                ? MarkdownCallout::None
                                                : builder->quoteCallout.takeLast();
            // 内层引用的类型优先：已经盖过的不再覆盖。
            if (callout != MarkdownCallout::None) {
                for (int index = start; index < builder->blocks.size(); ++index) {
                    if (builder->blocks[index].callout == MarkdownCallout::None) {
                        builder->blocks[index].callout = callout;
                    }
                }
            }
            --builder->quoteDepth;
            if (!builder->quoteIdStack.isEmpty()) builder->quoteIdStack.pop_back();
            break;
        }

        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            --builder->listDepth;
            if (!builder->orderedCounters.isEmpty()) builder->orderedCounters.pop_back();
            // 退到最外层才收口。
            if (builder->listDepth < 0) {
                // 只有嵌套列表、自己一个字都没有的占位项要去掉——
                // 那种项在界面上是一个孤零零的空项目符号。
                QVector<MarkdownListItem> kept;
                for (const MarkdownListItem& item : builder->current.items) {
                    if (!item.spans.isEmpty()) kept.push_back(item);
                }
                builder->current.items = kept;
                if (!builder->current.items.isEmpty()) builder->emitBlock(builder->current);
                builder->inBlock = false;
            }
            break;

        case MD_BLOCK_LI:
            if (!builder->itemStack.isEmpty()) builder->itemStack.pop_back();
            break;

        case MD_BLOCK_TABLE:
            builder->inTable = false;
            builder->emitBlock(builder->current);
            builder->inBlock = false;
            break;

        case MD_BLOCK_THEAD:
            builder->inTableHeadRow = false;
            break;

        case MD_BLOCK_TR:
            builder->current.rows.push_back(builder->currentRow);
            break;

        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            builder->currentRow.push_back(builder->currentCell);
            builder->inTableCell = false;
            break;

        default:
            break;
    }
    return 0;
}

int enterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    auto* builder = static_cast<Builder*>(userdata);
    switch (type) {
        case MD_SPAN_STRONG: ++builder->bold; break;
        case MD_SPAN_EM: ++builder->italic; break;
        case MD_SPAN_CODE: ++builder->code; break;
        case MD_SPAN_DEL: ++builder->strike; break;
        case MD_SPAN_A:
            builder->linkStack.push_back(
                sanitizeHref(attributeText(static_cast<MD_SPAN_A_DETAIL*>(detail)->href)));
            break;
        default:
            // 图片、LaTeX、wikilink 现在不支持：**不静默吞掉**，
            // 里面的文字照常落下去，用户至少能看见内容。
            break;
    }
    return 0;
}

int leaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto* builder = static_cast<Builder*>(userdata);
    switch (type) {
        case MD_SPAN_STRONG: --builder->bold; break;
        case MD_SPAN_EM: --builder->italic; break;
        case MD_SPAN_CODE: --builder->code; break;
        case MD_SPAN_DEL: --builder->strike; break;
        case MD_SPAN_A:
            if (!builder->linkStack.isEmpty()) builder->linkStack.pop_back();
            break;
        default:
            break;
    }
    return 0;
}

int onText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto* builder = static_cast<Builder*>(userdata);
    switch (type) {
        case MD_TEXT_CODE:
            // 代码块的正文按行给，行尾是 '\n'。**原样存**，一个字符都不能改——
            // 复制按钮复制的就是它。
            if (builder->inBlock && builder->current.kind == MarkdownBlockKind::Code) {
                builder->current.code += fromMd(text, size);
                break;
            }
            if (builder->listCodeBlockDepth > 0) {
                builder->addText(fromMd(text, size));
                break;
            }
            {
                QString piece = fromMd(text, size);
                if (!builder->tryFinishCalloutInline(&piece)) {
                    builder->flushPendingCallout();
                    builder->tryStartCalloutInline(&piece);
                }
                builder->addText(piece);
            }
            break;

        case MD_TEXT_BR:
            if (builder->pendingCallout != MarkdownCallout::None) {
                builder->commitPendingCallout();
                break;
            }
            builder->addText(QStringLiteral("\n"));
            break;

        case MD_TEXT_SOFTBR:
            // 标记独占一行时，这个换行连同标记一起吞掉，不然正文会以一个空格开头。
            if (builder->pendingCallout != MarkdownCallout::None) {
                builder->commitPendingCallout();
                break;
            }
            // 软换行折成空格——CommonMark 的行为，和老渲染器、Electron 端一致。
            builder->addText(QStringLiteral(" "));
            break;

        case MD_TEXT_NULLCHAR:
            builder->flushPendingCallout();
            builder->addText(QStringLiteral("�"));
            break;

        case MD_TEXT_HTML:
            // MD_FLAG_NOHTML 开着，走到这里的是被当成字面量的尖括号之类，照常显示。
            builder->flushPendingCallout();
            builder->addText(fromMd(text, size));
            break;

        default: {
            QString piece = fromMd(text, size);
            if (builder->tryHoldCallout(piece)) break;
            if (!builder->tryFinishCalloutInline(&piece)) {
                builder->flushPendingCallout();
                builder->tryStartCalloutInline(&piece);
            }
            builder->addText(piece);
            break;
        }
    }
    return 0;
}

}  // namespace

MarkdownDocument MarkdownDocument::parse(const QString& markdown) {
    MarkdownDocument document;
    document.mSource = markdown;
    if (markdown.isEmpty()) return document;

    Builder builder;
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
    parser.enter_block = enterBlock;
    parser.leave_block = leaveBlock;
    parser.enter_span = enterSpan;
    parser.leave_span = leaveSpan;
    parser.text = onText;

    const QByteArray utf8 = markdown.toUtf8();
    md_parse(utf8.constData(), static_cast<MD_SIZE>(utf8.size()), &parser, &builder);

    document.mBlocks = builder.blocks;
    return document;
}

QString markdownCalloutTitle(MarkdownCallout callout) {
    switch (callout) {
        case MarkdownCallout::Note:
            return QStringLiteral("提示");
        case MarkdownCallout::Tip:
            return QStringLiteral("建议");
        case MarkdownCallout::Important:
            return QStringLiteral("重要");
        case MarkdownCallout::Warning:
            return QStringLiteral("注意");
        case MarkdownCallout::Caution:
            return QStringLiteral("警告");
        case MarkdownCallout::None:
            break;
    }
    return QString();
}

QString MarkdownDocument::plainText() const {
    QStringList pieces;
    // 提示框的标题只在一组的**头一个块**前面加一次。同一个提示框里有三段的话，
    // 「建议：」出现三遍会把这一行本来就不多的位置占满。
    //
    // 标题不单独成段，而是粘在后面第一段的前头：段与段之间是拿空格拼的，
    // 单独成段会出来「建议： 正文」，冒号后面多一个空格。
    MarkdownCallout openCallout = MarkdownCallout::None;
    QVector<int> openQuote;
    QString pendingTitle;
    const auto push = [&pieces, &pendingTitle](const QString& piece) {
        pieces.push_back(pendingTitle + piece);
        pendingTitle.clear();
    };
    for (const MarkdownBlock& block : mBlocks) {
        if (block.callout != openCallout || block.quoteIds != openQuote) {
            openCallout = block.callout;
            openQuote = block.quoteIds;
            const QString title = markdownCalloutTitle(block.callout);
            pendingTitle = title.isEmpty() ? QString() : title + QStringLiteral("：");
        }
        switch (block.kind) {
            case MarkdownBlockKind::Code:
                push(block.code.trimmed());
                break;
            case MarkdownBlockKind::Divider:
                break;
            case MarkdownBlockKind::List:
                for (const MarkdownListItem& item : block.items) {
                    QString line;
                    // 任务列表在预览行里也要能看出勾没勾上——只剩文字的话，
                    // 「完成 待办」读起来像两件都没做。
                    if (item.hasCheckbox) {
                        line += item.checked ? QStringLiteral("☑ ") : QStringLiteral("☐ ");
                    }
                    for (const MarkdownSpan& span : item.spans) line += span.text;
                    if (!line.trimmed().isEmpty()) push(line);
                }
                break;
            case MarkdownBlockKind::Table:
                for (const QVector<QVector<MarkdownSpan>>& row : block.rows) {
                    QStringList cells;
                    for (const QVector<MarkdownSpan>& cell : row) {
                        QString text;
                        for (const MarkdownSpan& span : cell) text += span.text;
                        cells.push_back(text);
                    }
                    push(cells.join(QStringLiteral(" ")));
                }
                break;
            default: {
                QString line;
                for (const MarkdownSpan& span : block.spans) line += span.text;
                if (!line.isEmpty()) push(line);
                break;
            }
        }
    }
    // 预览只有一行，换行一律折成空格——留着换行的话 QLabel 会把后面的内容截掉，
    // 看起来像消息只有半句。
    return pieces.join(QStringLiteral(" ")).simplified();
}
