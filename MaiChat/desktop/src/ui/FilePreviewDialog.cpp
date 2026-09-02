#include <QHash>
#include <QHeaderView>
#include <QSplitter>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QAbstractItemView>

#include "ui/FilePreviewDialog.h"

#include <QApplication>
#include <QAbstractTextDocumentLayout>
#include <QFontMetrics>
#include <QFrame>
#include <QList>
#include <QPair>
#include <QTextDocument>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSizeGrip>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QtMath>

#include "ui/UiZoom.h"

QString FilePreviewDialog::normalizeGitDiffHtmlForQt(QString html) {
    // QTextDocument 不执行媒体查询，会把桌面 split 与手机 unified 两份都画出来。
    // HTML 中的显式边界只用于选择表现形式，不执行脚本；Qt 固定保留左右对比。
    //
    // 必须是**非贪婪**（.*?）：多文件 Diff 每个文件各有一对 START/END 标记，
    // 贪婪的 .* 会从第一个 START 一路吃到最后一个 END，把中间所有文件的 split
    // 表格连同它们的锚点一起删掉——21 个文件的报告实测被吞掉 97.2% 的内容，
    // 桌面端只剩第一个文件可看，且不报错、不空白，看起来就像「这次只改了一个文件」。
    const QRegularExpression unifiedBlock(
        QStringLiteral("<!-- MAICHAT_UNIFIED_START -->.*?<!-- MAICHAT_UNIFIED_END -->"),
        QRegularExpression::DotMatchesEverythingOption);
    html.remove(unifiedBlock);

    // 浏览器用 CSS gap 分隔 pill；Qt 的富文本引擎会把 gap、圆角和背景静默丢掉。
    // 生成器放入了真实文本分隔符，浏览器隐藏、Qt 则在这里解除隐藏。
    html.replace(QStringLiteral(".qt-separator{display:none}"),
                 QStringLiteral(".qt-separator{}"));

    // QTextDocument 不支持 CSS 自定义属性。统一展开成浅色字面值，确保边框、次要文字
    // 和增删背景不会静默退回平台默认色；预览面板本身也是浅色主题。
    const QList<QPair<QString, QString>> colors = {
        {QStringLiteral("var(--bg)"), QStringLiteral("#f6f8fa")},
        {QStringLiteral("var(--panel)"), QStringLiteral("#ffffff")},
        {QStringLiteral("var(--border)"), QStringLiteral("#d0d7de")},
        {QStringLiteral("var(--text)"), QStringLiteral("#1f2328")},
        {QStringLiteral("var(--muted)"), QStringLiteral("#656d76")},
        {QStringLiteral("var(--add)"), QStringLiteral("#dafbe1")},
        {QStringLiteral("var(--del)"), QStringLiteral("#ffebe9")},
        {QStringLiteral("var(--hunk)"), QStringLiteral("#ddf4ff")},
        {QStringLiteral("var(--add-strong)"), QStringLiteral("#aceebb")},
        {QStringLiteral("var(--del-strong)"), QStringLiteral("#ffcecb")},
    };
    for (const auto& color : colors) {
        html.replace(color.first, color.second);
    }
    return html;
}

FilePreviewDialog::FilePreviewDialog(const QString& displayName, const QString& html, QWidget* parent)
    : QDialog(parent) {
    buildUi(displayName, html);
    applyStyle();
    resize(contentAwareInitialSize());
}

void FilePreviewDialog::buildUi(const QString& displayName, const QString& html) {
    fullTitle_ = displayName.trimmed().isEmpty() ? QStringLiteral("文件预览") : displayName.trimmed();

    setObjectName(QStringLiteral("filePreviewDialog"));
    setWindowTitle(fullTitle_);
    setModal(true);
    // 去掉系统标题栏，文件名改由面板内的标签承担（与 AppMessageDialog 一致）。
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 18, 18, 18);
    rootLayout->setSpacing(0);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("filePreviewPanel"));
    rootLayout->addWidget(panel);
    panel_ = panel;
    // 缩放判定带跨在面板边线两侧：外圈是对话框自己的透明区，内侧几像素落在
    // 面板上。不开鼠标跟踪的话，没按下按钮时收不到 MouseMove，光标就不会变，
    // 用户根本看不出这里可以拖。
    setMouseTracking(true);
    panel_->setMouseTracking(true);
    panel_->installEventFilter(this);

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(24, 20, 24, 18);
    layout->setSpacing(UiZoom::s(14));

    // 自制标题栏：显示文件名 + 承担窗口拖动 + 右侧关闭。
    header_ = new QWidget(panel);
    header_->setObjectName(QStringLiteral("filePreviewHeader"));
    header_->installEventFilter(this);
    auto* headerRow = new QHBoxLayout(header_);
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(UiZoom::s(10));

    title_ = new QLabel(fullTitle_, header_);
    title_->setObjectName(QStringLiteral("filePreviewTitle"));
    // 让标题不决定窗口宽度：过长的文件名在 resizeEvent 里省略，而不是把窗口撑开。
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // 鼠标事件要落到 header_ 上才能拖动，标签本身不吃事件。
    title_->setAttribute(Qt::WA_TransparentForMouseEvents);
    // 省略必须跟着标签自己的宽度走：对话框 resize 时布局还没把拉伸空间分给标签，
    // 那一刻算出来的可用宽度极小，会把文件名截成一两个字符再也不恢复。
    title_->installEventFilter(this);
    headerRow->addWidget(title_, 1);

    // 标题栏不再放 ✕：底部已有「关闭」，两个出口并排反而让人犹豫该点哪个
    // （远程观看窗那次也是同样的取舍）。关闭入口只留底部按钮 + Esc。
    layout->addWidget(header_);

    content_ = new QTextBrowser(panel);
    content_->setObjectName(QStringLiteral("filePreviewContent"));
    content_->setOpenExternalLinks(true);
    content_->setReadOnly(true);
    content_->setFrameShape(QFrame::NoFrame);
    // 字体必须显式跟随应用全局字体：MarkdownRenderer 的 CSS 只声明字号、不声明
    // font-family（见 MarkdownRenderer.cpp），字体族完全由文档默认字体决定。
    // 少了这一步就会落到 Qt 在中文 Windows 上的默认宋体，衬线观感与界面其余部分脱节。
    // 取 QApplication::font() 而不是写死字体名：Windows 是 Segoe UI + 微软雅黑
    // （main.cpp 里按平台设的），macOS 用系统默认，且能跟上 MainWindow 的缩放倍率。
    content_->document()->setDefaultFont(QApplication::font());
    // 多文件 Diff 用「左目录 / 右正文」两栏：索引在文档顶部时，看第 20 个文件
    // 得先滚回最上面再点一次，来回找位置。左栏常驻才是这类报告该有的样子。
    const QList<FileIndexEntry> entries = parseFileIndex(html);
    if (entries.size() >= 2) {
        auto* splitter = new QSplitter(Qt::Horizontal, panel);
        splitter->setObjectName(QStringLiteral("filePreviewSplitter"));
        // 不允许把任一栏拖没：拖没了没有恢复入口，等于把功能弄丢。
        splitter->setChildrenCollapsible(false);
        fileList_ = qobject_cast<QTreeWidget*>(buildFileListPane(entries, splitter));
        splitter->addWidget(fileList_);
        splitter->addWidget(content_);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes({UiZoom::s(340), UiZoom::s(900)});
        layout->addWidget(splitter, 1);
        // 左栏顶替了文档顶部那份索引，两份并排只会让人怀疑哪份是准的。
        // 手机端没有左栏，所以索引只在这里、只对 Qt 剥掉。
        static const QRegularExpression indexBlock(
            QStringLiteral("<div class=\"file-index\">.*?</ul></div>"),
            QRegularExpression::DotMatchesEverythingOption);
        QString withoutIndex = html;
        withoutIndex.remove(indexBlock);
        content_->setHtml(withoutIndex);
    } else {
        content_->setHtml(html);
        layout->addWidget(content_, 1);
    }

    auto* footerRow = new QHBoxLayout();
    footerRow->setContentsMargins(0, 0, 0, 0);
    footerRow->setSpacing(UiZoom::s(10));
    footerRow->addStretch(1);

    auto* close = new QPushButton(QStringLiteral("关闭"), panel);
    close->setObjectName(QStringLiteral("filePreviewClose"));
    close->setCursor(Qt::PointingHandCursor);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    footerRow->addWidget(close);

    // 无边框窗口没有系统缩放边框，靠右下角的 grip 补回来。
    auto* grip = new QSizeGrip(panel);
    grip->setObjectName(QStringLiteral("filePreviewGrip"));
    grip->setFixedSize(UiZoom::s(22), UiZoom::s(22));
    grip->setCursor(Qt::SizeFDiagCursor);
    grip->setToolTip(QStringLiteral("拖动调整窗口大小"));
    grip->setAccessibleName(QStringLiteral("调整预览窗口大小"));
    footerRow->addWidget(grip, 0, Qt::AlignBottom);

    layout->addLayout(footerRow);

    setMinimumSize(UiZoom::s(480), UiZoom::s(360));
}

void FilePreviewDialog::applyStyle() {
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #filePreviewPanel {
            background: #ffffff;
            border-radius: 16px;
        }
        #filePreviewTitle {
            color: #0f172a;
            font-size: 17px;
            font-weight: 800;
        }
        /* 不在这里设 font-size：正文字号由 MarkdownRenderer 的 CSS 给出，
           并已被 UiZoom::scaleQss 按缩放倍率换算过，这里再设会把它顶掉。 */
        #filePreviewContent {
            background: #f8fafc;
            border: 1px solid #e6edf5;
            border-radius: 12px;
            color: #172033;
            padding: 16px;
        }
        #filePreviewFileList {
            background: #ffffff;
            border: 1px solid #e6edf5;
            border-radius: 12px;
            color: #172033;
            font-size: 12px;
        }
        #filePreviewFileList::item {
            padding: 3px 2px;
        }
        #filePreviewFileList QHeaderView::section {
            background: #f1f5f9;
            border: 0;
            border-bottom: 1px solid #e6edf5;
            color: #475569;
            font-weight: 700;
            padding: 6px 8px;
        }
        #filePreviewSplitter::handle {
            background: transparent;
            width: 10px;
        }
        #filePreviewClose {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                        stop:0 #5b9bff, stop:1 #1e40af);
            border: 0;
            border-radius: 10px;
            color: #ffffff;
            font-size: 14px;
            font-weight: 700;
            padding: 10px 22px;
        }
        #filePreviewClose:hover {
            background: #1e40af;
        }
    )")));
}

QSize FilePreviewDialog::contentAwareInitialSize() {
    // 初始尺寸由内容与当前屏幕共同决定：短文档不应占满父窗口，长 Diff 则要保留
    // 足够的左右对比宽度，但无论内容多大都不能越过工作区，超出的部分交给滚动条。
    ensurePolished();
    if (layout()) layout()->activate();

    QScreen* screen = QApplication::primaryScreen();
    if (parentWidget()) {
        const QPoint parentCenter = parentWidget()->mapToGlobal(parentWidget()->rect().center());
        if (QScreen* parentScreen = QApplication::screenAt(parentCenter)) screen = parentScreen;
    }
    const QSize available = screen ? screen->availableGeometry().size() : QSize(1440, 900);
    const int maximumWidth = qMax(
        1,
        qMin(UiZoom::s(1280), qFloor(available.width() * 0.90)));
    const int maximumHeight = qMax(1, qFloor(available.height() * 0.90));
    const int minimumWidth = qMin(UiZoom::s(480), maximumWidth);
    const int minimumHeight = qMin(UiZoom::s(360), maximumHeight);
    setMinimumSize(minimumWidth, minimumHeight);

    const bool isGitDiff = fullTitle_.startsWith(QStringLiteral("remote-im-diff-"),
                                                  Qt::CaseInsensitive)
        && fullTitle_.endsWith(QStringLiteral(".html"), Qt::CaseInsensitive);
    int preferredWidth = UiZoom::s(620);
    if (isGitDiff) {
        const int parentBasedWidth = parentWidget()
            ? parentWidget()->width() * 2 / 3
            : UiZoom::s(1000);
        const int readableDiffWidth = qMin(UiZoom::s(900), maximumWidth);
        preferredWidth = qBound(readableDiffWidth, parentBasedWidth, maximumWidth);
    } else {
        const QFontMetrics metrics(content_->document()->defaultFont());
        int longestLineWidth = 0;
        const QStringList lines = content_->toPlainText().split(QLatin1Char('\n'));
        for (const QString& line : lines) {
            longestLineWidth = qMax(longestLineWidth, metrics.horizontalAdvance(line));
        }
        const int naturalWidth = longestLineWidth + UiZoom::s(150);
        const int parentCeiling = parentWidget()
            ? qMax(minimumWidth, parentWidget()->width() * 2 / 3)
            : UiZoom::s(900);
        preferredWidth = qMin(naturalWidth, parentCeiling);
    }
    const int targetWidth = qBound(minimumWidth, preferredWidth, maximumWidth);

    // 用已经 polish 的真实控件测 chrome，而不是把标题、按钮、QSS padding 写死。
    // probe 文档避免为了量高度去改变屏幕上 QTextBrowser 的真实 page size。
    const int chromeWidth = qMax(UiZoom::s(96), width() - content_->viewport()->width());
    const int chromeHeight = qMax(UiZoom::s(150), height() - content_->viewport()->height());
    const int documentWidth = qMax(1, targetWidth - chromeWidth);
    QTextDocument probe;
    probe.setDefaultFont(content_->document()->defaultFont());
    probe.setHtml(content_->toHtml());
    probe.setTextWidth(documentWidth);
    const int documentHeight = qCeil(probe.documentLayout()->documentSize().height());
    const int preferredHeight = documentHeight + chromeHeight;
    const int targetHeight = qBound(minimumHeight, preferredHeight, maximumHeight);
    return QSize(targetWidth, targetHeight);
}

void FilePreviewDialog::updateElidedTitle() {
    if (!title_) return;
    const int available = title_->width();
    if (available <= 0) return;
    // 中间省略：文件名的扩展名往往比中段更重要，末尾省略会把 .md 吃掉。
    title_->setText(title_->fontMetrics().elidedText(fullTitle_, Qt::ElideMiddle, available));
}

namespace {

// 判定带在面板边线内侧留出的宽度。太窄了用户得像穿针一样瞄，太宽了会把
// 靠边的正常点击也吃掉。
//
// 这个内侧带必须自己就够用，不能指望面板外那圈透明区：窗口开了
// WA_TranslucentBackground，在 Windows 上是分层窗口，**完全透明的像素会被系统
// 判为点击穿透**，那圈里的按下根本到不了我们手上。外圈的处理留着（有的平台
// 收得到，收到就是白赚），但可用性只能押在这条内侧带上。
constexpr int kResizeInnerBand = 8;

Qt::CursorShape cursorForEdges(Qt::Edges edges) {
    const bool left = edges & Qt::LeftEdge;
    const bool right = edges & Qt::RightEdge;
    const bool top = edges & Qt::TopEdge;
    const bool bottom = edges & Qt::BottomEdge;
    if ((left && top) || (right && bottom)) return Qt::SizeFDiagCursor;
    if ((right && top) || (left && bottom)) return Qt::SizeBDiagCursor;
    if (left || right) return Qt::SizeHorCursor;
    if (top || bottom) return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

}  // namespace

namespace {

bool isDirectoryRow(const QTreeWidgetItem* item) {
    // 叶子行才带锚点；目录行没有可跳转的目标。
    return item->data(0, Qt::UserRole).toString().isEmpty();
}

// 把「只有一个子目录」的目录节点与那个子节点并成一行：
// electron/ > remote-im/ 各占一行、每层只有一个孩子，纯粹是缩进噪声，
// 合成 electron/remote-im/ 一行既短又不丢信息。
void collapseSingleChildDirectories(QTreeWidgetItem* item) {
    while (isDirectoryRow(item) && item->childCount() == 1 && isDirectoryRow(item->child(0))) {
        QTreeWidgetItem* only = item->takeChild(0);
        item->setText(0, item->text(0) + only->text(0));
        item->addChildren(only->takeChildren());
        delete only;
    }
    for (int i = 0; i < item->childCount(); ++i) {
        collapseSingleChildDirectories(item->child(i));
    }
}

}  // namespace

QList<FilePreviewDialog::FileIndexEntry> FilePreviewDialog::parseFileIndex(const QString& html) {
    // 直接读生成器写出的那段索引标记，而不是自己再解析一遍 diff：
    // 索引与正文锚点由同一处编号产出，跟着它走就不会与正文对不上。
    static const QRegularExpression row(
        QStringLiteral("<li><a href=\"#(f\\d+)\">(.*?)</a>"
                       ".*?<span class=\"idx-add\">\\+(\\d+)</span>"
                       ".*?<span class=\"idx-del\">-(\\d+)</span>"),
        QRegularExpression::DotMatchesEverythingOption);

    QList<FileIndexEntry> entries;
    QRegularExpressionMatchIterator it = row.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        FileIndexEntry entry;
        entry.anchor = match.captured(1);
        // 生成器对路径做过 HTML 转义，这里还原回来再显示。
        entry.label = match.captured(2)
                          .replace(QStringLiteral("&amp;"), QStringLiteral("&"))
                          .replace(QStringLiteral("&lt;"), QStringLiteral("<"))
                          .replace(QStringLiteral("&gt;"), QStringLiteral(">"))
                          .replace(QStringLiteral("&quot;"), QStringLiteral("\""))
                          .replace(QStringLiteral("&#39;"), QStringLiteral("'"));
        entry.additions = match.captured(3).toInt();
        entry.deletions = match.captured(4).toInt();
        entries.append(entry);
    }
    return entries;
}

QWidget* FilePreviewDialog::buildFileListPane(const QList<FileIndexEntry>& entries,
                                              QWidget* parent) {
    auto* tree = new QTreeWidget(parent);
    tree->setObjectName(QStringLiteral("filePreviewFileList"));
    tree->setColumnCount(2);
    tree->setHeaderLabels({QStringLiteral("变更文件（%1）").arg(entries.size()),
                           QStringLiteral("")});
    tree->setRootIsDecorated(true);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(false);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 叶子只放文件名，短，右侧省略即可。
    tree->setTextElideMode(Qt::ElideRight);
    // 默认缩进按一层 20px 算，java 包名那种深目录到第四层就把文件名挤没了。
    // 这里的层级已经被折叠压过一轮，缩进再给小一点即可读。
    tree->setIndentation(UiZoom::s(12));

    // 按目录建树，而不是平铺整条路径：仓库根下的完整路径动辄七八层，
    // 平铺时每行都在重复同样的前缀，真正要看的文件名反而被挤没了。
    QHash<QString, QTreeWidgetItem*> directories;
    for (const FileIndexEntry& entry : entries) {
        const QStringList parts = entry.label.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QTreeWidgetItem* parent = nullptr;
        QString path;
        for (int i = 0; i + 1 < parts.size(); ++i) {
            path += (path.isEmpty() ? QString() : QStringLiteral("/")) + parts.at(i);
            QTreeWidgetItem*& directory = directories[path];
            if (!directory) {
                directory = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
                directory->setText(0, parts.at(i) + QLatin1Char('/'));
                // 目录行没有增删数，让名字占满两列，省得右边空一格。
                directory->setFirstColumnSpanned(true);
            }
            parent = directory;
        }
        auto* leaf = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        leaf->setText(0, parts.isEmpty() ? entry.label : parts.last());
        // 完整路径进 tooltip：树里看到的是文件名，偶尔仍需要确认它在哪。
        leaf->setToolTip(0, entry.label);
        leaf->setText(1, QStringLiteral("+%1 -%2").arg(entry.additions).arg(entry.deletions));
        leaf->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        leaf->setData(0, Qt::UserRole, entry.anchor);
    }
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        collapseSingleChildDirectories(tree->topLevelItem(i));
    }
    tree->expandAll();
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    const auto jump = [this](QTreeWidgetItem* item) {
        if (!item || !content_) return;
        const QString anchor = item->data(0, Qt::UserRole).toString();
        if (anchor.isEmpty()) return;
        content_->scrollToAnchor(anchor);
    };
    // 单击就跳：这是一个导航列表，不是需要「确认」的选择器。
    connect(tree, &QTreeWidget::itemClicked, this,
            [jump](QTreeWidgetItem* item, int) { jump(item); });
    connect(tree, &QTreeWidget::currentItemChanged, this,
            [jump](QTreeWidgetItem* current, QTreeWidgetItem*) { jump(current); });
    return tree;
}

Qt::Edges FilePreviewDialog::resizeEdgesAt(const QPoint& pos) const {
    if (!rect().contains(pos)) return {};
    // 用面板的边线而不是对话框的：对话框外圈是透明阴影区，用户瞄的是
    // 看得见的那条白色边。
    const QRect panel = panel_ ? panel_->geometry() : rect();
    const int band = UiZoom::s(kResizeInnerBand);
    Qt::Edges edges;
    if (pos.x() <= panel.left() + band) edges |= Qt::LeftEdge;
    if (pos.x() >= panel.right() - band) edges |= Qt::RightEdge;
    if (pos.y() <= panel.top() + band) edges |= Qt::TopEdge;
    if (pos.y() >= panel.bottom() - band) edges |= Qt::BottomEdge;
    return edges;
}

bool FilePreviewDialog::beginResize(const QPoint& globalPos, const QPoint& localPos) {
    const Qt::Edges edges = resizeEdgesAt(localPos);
    if (!edges) return false;
    resizeEdges_ = edges;
    resizeStartGeometry_ = geometry();
    resizeStartGlobal_ = globalPos;
    return true;
}

void FilePreviewDialog::updateResize(const QPoint& globalPos) {
    const QPoint delta = globalPos - resizeStartGlobal_;
    QRect target = resizeStartGeometry_;
    const QSize floor = minimumSize();
    // QRect 的 right()/bottom() 是闭区间，所以夹取时要 -1/+1。
    // 右/下两边其实 Qt 的 setGeometry 自己就会按 minimumSize 夹住尺寸；
    // 必须我们自己夹的是左/上：只夹尺寸的话，窗口会一边保持最小尺寸、
    // 一边让 topLeft 跟着鼠标继续滑走，整个窗口就飘出去了。
    if (resizeEdges_ & Qt::LeftEdge) {
        target.setLeft(qMin(target.left() + delta.x(), target.right() - floor.width() + 1));
    }
    if (resizeEdges_ & Qt::RightEdge) {
        target.setRight(qMax(target.right() + delta.x(), target.left() + floor.width() - 1));
    }
    if (resizeEdges_ & Qt::TopEdge) {
        target.setTop(qMin(target.top() + delta.y(), target.bottom() - floor.height() + 1));
    }
    if (resizeEdges_ & Qt::BottomEdge) {
        target.setBottom(qMax(target.bottom() + delta.y(), target.top() + floor.height() - 1));
    }
    setGeometry(target);
}

void FilePreviewDialog::applyResizeCursor(const QPoint& localPos) {
    const Qt::CursorShape shape = cursorForEdges(resizeEdgesAt(localPos));
    if (shape == Qt::ArrowCursor) {
        unsetCursor();
        return;
    }
    setCursor(shape);
}

// 面板盖住了对话框的绝大部分，靠内那几像素的判定带落在它身上，
// 所以这些事件得转回来按对话框坐标处理。
bool FilePreviewDialog::handlePanelMouseEvent(QEvent* event) {
    if (!panel_) return false;
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove
        && type != QEvent::MouseButtonRelease) {
        return false;
    }
    auto* mouse = static_cast<QMouseEvent*>(event);
    const QPoint local = panel_->mapTo(this, mouse->pos());
    if (type == QEvent::MouseButtonPress) {
        return mouse->button() == Qt::LeftButton && beginResize(mouse->globalPos(), local);
    }
    if (type == QEvent::MouseMove) {
        if (resizeEdges_ && (mouse->buttons() & Qt::LeftButton)) {
            updateResize(mouse->globalPos());
            return true;
        }
        applyResizeCursor(local);
        return false;
    }
    if (resizeEdges_) {
        resizeEdges_ = {};
        unsetCursor();
        return true;
    }
    return false;
}

void FilePreviewDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && beginResize(event->globalPos(), event->pos())) {
        event->accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

void FilePreviewDialog::mouseMoveEvent(QMouseEvent* event) {
    if (resizeEdges_ && (event->buttons() & Qt::LeftButton)) {
        updateResize(event->globalPos());
        event->accept();
        return;
    }
    applyResizeCursor(event->pos());
    QDialog::mouseMoveEvent(event);
}

void FilePreviewDialog::mouseReleaseEvent(QMouseEvent* event) {
    if (resizeEdges_) {
        resizeEdges_ = {};
        unsetCursor();
        event->accept();
        return;
    }
    QDialog::mouseReleaseEvent(event);
}

void FilePreviewDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    updateElidedTitle();
}

bool FilePreviewDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == title_ && event->type() == QEvent::Resize) {
        updateElidedTitle();
        return false;
    }
    if (watched == panel_ && handlePanelMouseEvent(event)) return true;
    if (watched == header_) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                dragging_ = true;
                dragOffset_ = mouse->globalPos() - frameGeometry().topLeft();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (dragging_ && (mouse->buttons() & Qt::LeftButton)) {
                move(mouse->globalPos() - dragOffset_);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            dragging_ = false;
        }
    }
    return QDialog::eventFilter(watched, event);
}
