#include "agent/AgentChatPanel.h"

#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <variant>

#include "agent/AgentController.h"
#include "markdown/MarkdownRenderer.h"
#include "ui/UiZoom.h"

namespace {

// 配色取自 MainWindow.cpp，别在这儿另起一套。
const char* const kInk = "#172033";
const char* const kInkSoft = "#667085";
const char* const kInkFaint = "#98a2b3";
const char* const kLine = "#e2e8f0";
const char* const kLineSoft = "#f1f5f9";
const char* const kAccent = "#0b67b7";
const char* const kGoldWash = "#fdf8ea";
const char* const kDanger = "#b42318";

// 正文的阅读宽度上限。窗口拉到 2000px 时正文不该跟着拉那么宽——
// 一行太长，眼睛从行尾回到下一行行首会丢行。
constexpr int kColumnWidth = 760;

std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString fromUtf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

QLabel* makeLabel(const QString& text, int pixelSize, const char* color, bool bold = false) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    // 换行标签必须让布局按"给定宽度算高度"来量，否则它只按一行高算。
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont font = label->font();
    font.setPixelSize(UiZoom::s(pixelSize));
    font.setBold(bold);
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(color));
    return label;
}

// Enter 发送，Shift+Enter 换行。
//
// 得自己拦 keyPressEvent：QTextEdit 默认把 Enter 当换行，而这个框是多行的，
// 不拦的话用户每轮都得去点发送按钮。
class PromptEdit final : public QTextEdit {
public:
    explicit PromptEdit(QWidget* parent = nullptr) : QTextEdit(parent) {}
    std::function<void()> onSubmit;

protected:
    void keyPressEvent(QKeyEvent* event) override {
        const bool isEnter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
        if (isEnter && !(event->modifiers() & Qt::ShiftModifier)) {
            if (onSubmit) onSubmit();
            return;
        }
        QTextEdit::keyPressEvent(event);
    }
};

}  // namespace

// ── 助手的回答 ──────────────────────────────────────────────────
//
// **不是气泡，是整列宽的正文。** 模型的回答动辄几百字带列表和代码，
// 塞进窄气泡里就是一根面条。
//
// Markdown 走 MaiChat 自己的 MarkdownRenderer，和 IM 那边渲染出来的东西一致——
// 另起一套的话同一个应用里会有两种列表样式、两种代码块。
class AgentChatPanel::AnswerView final : public QTextBrowser {
public:
    explicit AnswerView(QWidget* parent = nullptr) : QTextBrowser(parent) {
        setFrameShape(QFrame::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setOpenExternalLinks(true);
        setStyleSheet(QStringLiteral("QTextBrowser{background:transparent;border:none;}"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        // 每来一个 delta 就重渲一次 Markdown 是 O(n²)：一段 3000 字的回答会重渲几百次。
        // 攒一小会儿再渲，肉眼看不出延迟，CPU 差一个数量级。
        repaintTimer_ = new QTimer(this);
        repaintTimer_->setSingleShot(true);
        repaintTimer_->setInterval(90);
        QObject::connect(repaintTimer_, &QTimer::timeout, this, [this] { render(); });

        QObject::connect(document()->documentLayout(),
                         &QAbstractTextDocumentLayout::documentSizeChanged, this,
                         [this](const QSizeF&) { fitHeight(); });
    }

    void append(const QString& delta) {
        source_ += delta;
        if (!repaintTimer_->isActive()) repaintTimer_->start();
    }

    void setMarkdown(const QString& markdown) {
        source_ = markdown;
        render();
    }

    // 流式结束：把攒着没渲的那一点补上。
    void flush() {
        repaintTimer_->stop();
        render();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QTextBrowser::resizeEvent(event);
        fitHeight();
    }

private:
    void render() {
        // 渲染器输出的 HTML 内嵌固定 px 字号，整体缩放时要一并按倍率缩放——
        // 和 MainWindow 里 MarkdownMessageView 的做法一致。
        QString html = MarkdownRenderer::renderToHtml(source_);
        // 在渲染器自己那段 CSS **之后**追加覆盖规则。同级选择器后来者胜，
        // 所以插在 </style> 之前就能盖掉，而 MarkdownRenderer 一个字都不用改——
        // 那份 CSS 是 IM 气泡在用的，动它会连带改掉聊天界面。
        html.replace(QStringLiteral("</style>"), agentOverrides() + QStringLiteral("</style>"));
        setHtml(UiZoom::scaleQss(html));
        fitHeight();
    }

    // 给长答案调的排版。
    //
    // 共用那份 CSS 是按 **IM 气泡**调的：一两句话、标题几乎用不上。
    // 拿它排一篇带三级标题、列表和代码的回答就会露怯：
    //
    //   - **没有设行高**。中文在默认行距下挤成一坨，长段落读起来很累。
    //   - 标题是蓝色和青色的（h2 #1769be / h3 #176e83）。气泡里偶尔出现一个还行，
    //     一屏五个就成了圣诞树，而且抢了正文的注意力。
    //   - 行内代码是蓝底蓝字。一段话里出现七八个 `PRAGMA xxx` 时整段都在闪。
    //
    // 所以这里只改三件事：把行距放开、标题收回墨色、代码块改成中性灰。
    static QString agentOverrides() {
        return QStringLiteral(
            "body{font-size:14px;line-height:175%;color:#172033;}"
            "p{margin:0 0 13px 0;}"
            "h1{font-size:20px;color:#172033;margin:22px 0 10px 0;}"
            "h2{font-size:17px;color:#172033;margin:22px 0 9px 0;}"
            "h3{font-size:15px;color:#172033;margin:18px 0 8px 0;}"
            "h4{font-size:14px;color:#475569;margin:14px 0 6px 0;}"
            "h5{font-size:14px;color:#475569;margin:14px 0 6px 0;}"
            "h6{font-size:14px;color:#667085;margin:14px 0 6px 0;}"
            "ul{margin:0 0 13px 0;}"
            "ol{margin:0 0 13px 0;}"
            "li{margin:7px 0;}"
            "strong{color:#0f172a;font-weight:600;}"
            "em{color:#475569;}"
            "code{background:#f1f5f9;color:#475569;font-size:13px;}"
            "pre{margin:0 0 13px 0;}"
            "a{color:#0b67b7;}");
    }

    // QTextBrowser 默认自己滚动。这里它嵌在外层的滚动区里，必须长到和内容一样高，
    // 否则长回答会出现一个框里再套一个滚动条。
    void fitHeight() {
        document()->setTextWidth(viewport()->width());
        const int wanted = static_cast<int>(document()->size().height()) + UiZoom::s(4);
        if (wanted != height_) {
            height_ = wanted;
            setFixedHeight(wanted);
        }
    }

    QString source_;
    QTimer* repaintTimer_ = nullptr;
    int height_ = 0;
};

// ── 思考条 ──────────────────────────────────────────────────────
//
// 一行淡色文字，不是灰色药丸——对齐 Codex 那个「用时 1m 10s ›」。
// 它是回答的注脚，不该有自己的容器和背景，那会让它看起来像一条独立消息。
class AgentChatPanel::ThinkingLine final : public QWidget {
public:
    explicit ThinkingLine(QWidget* parent = nullptr) : QWidget(parent) {
        setCursor(Qt::PointingHandCursor);
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(UiZoom::s(4));

        caption_ = makeLabel(QStringLiteral("思考中"), 12, kInkFaint);
        caption_->setWordWrap(false);
        column->addWidget(caption_);

        body_ = makeLabel(QString(), 12, kInkFaint);
        body_->hide();
        column->addWidget(body_);

        elapsed_.start();
        ticker_ = new QTimer(this);
        QObject::connect(ticker_, &QTimer::timeout, this, [this] { refreshCaption(); });
        ticker_->start(450);
        refreshCaption();
    }

    void append(const QString& delta) {
        text_ += delta;
        body_->setText(text_);
    }

    // 这一轮结束：停下动画，塌成"思考了 N 秒"。
    void settle() {
        if (!ticker_->isActive()) return;
        ticker_->stop();
        seconds_ = static_cast<int>((elapsed_.elapsed() + 500) / 1000);
        refreshCaption();
    }

protected:
    void mousePressEvent(QMouseEvent*) override {
        expanded_ = !expanded_;
        body_->setVisible(expanded_ && !text_.isEmpty());
        refreshCaption();
    }

private:
    void refreshCaption() {
        if (ticker_->isActive()) {
            // 收着也要动：那段时间一个正文字都不会来，完全没反应和卡死分不开。
            dots_ = (dots_ + 1) % 4;
            caption_->setText(QStringLiteral("思考中") + QString(dots_, QChar('.')));
            return;
        }
        const QString caret = expanded_ ? QStringLiteral(" ⌄") : QStringLiteral(" ›");
        caption_->setText(QStringLiteral("思考了 %1 秒").arg(seconds_) + caret);
    }

    QLabel* caption_ = nullptr;
    QLabel* body_ = nullptr;
    QTimer* ticker_ = nullptr;
    QElapsedTimer elapsed_;
    QString text_;
    bool expanded_ = false;
    int dots_ = 0;
    int seconds_ = 0;
};

// ── 工具卡 ──────────────────────────────────────────────────────
//
// 它表示"发生了一件事"，不是"谁说了句话"。做成气泡的话，一轮对话读起来像三个人在说话。
//
// 等授权时它**就地**长出按钮，不弹对话框：弹窗会打断阅读，
// 而且会训练用户条件反射点"允许"——那正是这道闸门要防的事。
class AgentChatPanel::ToolCard final : public QFrame {
public:
    explicit ToolCard(QWidget* parent = nullptr) : QFrame(parent) {
        // **必须给它一个 objectName，样式表按 id 选。**
        //
        // QLabel 自己就是 QFrame 的子类，所以 "QFrame{border:...}" 会把卡片里
        // 每一个标签也套上边框和圆角——真机上看到的就是一堆框里套框。
        // 按 id 选只命中这张卡本身。
        setObjectName(QStringLiteral("agentToolCard"));
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(UiZoom::s(11), UiZoom::s(7), UiZoom::s(11), UiZoom::s(7));
        column->setSpacing(UiZoom::s(6));

        auto* head = new QHBoxLayout;
        head->setSpacing(UiZoom::s(8));
        name_ = makeLabel(QString(), 12, kInk, true);
        name_->setWordWrap(false);
        args_ = makeLabel(QString(), 12, kInkSoft);
        args_->setWordWrap(false);
        state_ = makeLabel(QString(), 11, kInkFaint);
        state_->setWordWrap(false);
        head->addWidget(name_);
        head->addWidget(args_);
        // 伸缩放在参数和状态之间：参数按内容长，状态贴右边，
        // 给参数 stretch 会让它撑满整行，看起来像个空输入框。
        head->addStretch(1);
        head->addWidget(state_);
        column->addLayout(head);

        approval_ = new QWidget(this);
        auto* buttons = new QHBoxLayout(approval_);
        buttons->setContentsMargins(0, 0, 0, 0);
        buttons->setSpacing(UiZoom::s(7));
        allow_ = new QPushButton(QStringLiteral("允许"));
        deny_ = new QPushButton(QStringLiteral("拒绝"));
        always_ = new QPushButton(QStringLiteral("本会话都允许"));
        allow_->setStyleSheet(UiZoom::scaleQss(
            QStringLiteral("QPushButton{background:%1;color:#fff;border:none;border-radius:6px;"
                           "padding:4px 14px;}")
                .arg(kAccent)));
        deny_->setStyleSheet(UiZoom::scaleQss(
            QStringLiteral("QPushButton{background:#fff;color:%1;border:1px solid %2;"
                           "border-radius:6px;padding:4px 14px;}")
                .arg(kInk, kLine)));
        always_->setStyleSheet(UiZoom::scaleQss(
            QStringLiteral("QPushButton{background:transparent;color:%1;border:none;"
                           "padding:4px 6px;text-decoration:underline;}")
                .arg(kInkSoft)));
        for (QPushButton* button : {allow_, deny_, always_}) {
            QFont font = button->font();
            font.setPixelSize(UiZoom::s(12));
            button->setFont(font);
            button->setCursor(Qt::PointingHandCursor);
            buttons->addWidget(button);
        }
        buttons->addStretch(1);
        approval_->hide();
        column->addWidget(approval_);

        apply(MaiToolState::Pending, false);
    }

    void setCall(const QString& tool, const QString& arguments) {
        name_->setText(tool);
        // 参数可能很长（write 会带整篇内容）。截一段够用户判断该不该批就行，
        // 塞全文会把卡片撑到半屏。
        QString brief = arguments.simplified();
        if (brief.size() > 88) brief = brief.left(85) + QStringLiteral("…");
        args_->setText(brief);
    }

    void apply(MaiToolState toolState, bool waitingForUser) {
        const char* edge = kInkFaint;
        QString label;
        switch (toolState) {
            case MaiToolState::Pending:
                edge = waitingForUser ? "#d9a441" : kInkFaint;
                label = waitingForUser ? QStringLiteral("等你点头") : QStringLiteral("排队中");
                break;
            case MaiToolState::Running:
                edge = kAccent;
                label = QStringLiteral("执行中…");
                break;
            case MaiToolState::Completed:
                edge = "#12b76a";
                label = QStringLiteral("完成");
                break;
            case MaiToolState::Error:
                edge = kDanger;
                label = QStringLiteral("失败");
                break;
        }
        state_->setText(label);
        state_->setStyleSheet(QStringLiteral("color:%1;background:transparent;")
                                  .arg(toolState == MaiToolState::Error ? kDanger : kInkFaint));
        setStyleSheet(UiZoom::scaleQss(
            QStringLiteral("QFrame#agentToolCard{background:%1;border:1px solid %2;"
                           "border-left:3px solid %3;border-radius:7px;}")
                .arg(waitingForUser ? kGoldWash : "#ffffff", kLine, edge)));
        approval_->setVisible(waitingForUser);
        waiting_ = waitingForUser;
    }

    void setDetail(const QString& text) {
        state_->setText(text);
    }

    // 卡片自己记着在不在等人点头。
    //
    // 需要它是因为两条消息分别到：permissionAsked 打开授权态，
    // 而紧接着的 message.part.updated 只说"这个片段变了"、不说在等人。
    // 刷新时不问一句就会把刚亮起来的按钮关掉。
    bool isWaitingForUser() const {
        return waiting_;
    }

    QPushButton* allowButton() const {
        return allow_;
    }
    QPushButton* denyButton() const {
        return deny_;
    }
    QPushButton* alwaysButton() const {
        return always_;
    }

private:
    QLabel* name_ = nullptr;
    QLabel* args_ = nullptr;
    QLabel* state_ = nullptr;
    QWidget* approval_ = nullptr;
    QPushButton* allow_ = nullptr;
    QPushButton* deny_ = nullptr;
    QPushButton* always_ = nullptr;
    bool waiting_ = false;
};

struct AgentChatPanel::Runtime {
    AgentController* controller = nullptr;
    QString sessionId;

    QLabel* title = nullptr;
    QLabel* contextSize = nullptr;
    QLabel* dirChip = nullptr;
    QLabel* modelChip = nullptr;
    QScrollArea* scroll = nullptr;
    QVBoxLayout* stream = nullptr;
    PromptEdit* editor = nullptr;
    QPushButton* send = nullptr;
    QLabel* hint = nullptr;

    // partId -> 部件。流式期间靠它找到要追加的地方，而不是把整个对话重画一遍
    //（重画会让正在长的正文闪）。
    QHash<QString, AnswerView*> answers;
    QHash<QString, ThinkingLine*> thinking;
    QHash<QString, ToolCard*> toolCards;

    bool running = false;
};

AgentChatPanel::AgentChatPanel(AgentController& controller, QWidget* parent)
    : QWidget(parent), runtime_(std::make_unique<Runtime>()) {
    runtime_->controller = &controller;
    setStyleSheet(QStringLiteral("background:#ffffff;"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 头部：标题 + 攒了多少 + 清空 ----
    // 合成一条，不再占两行：清空是低频动作，"攒了多少"是它的理由，
    // 两个都该靠边站，别抢正文的位置。
    auto* head = new QWidget(this);
    head->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QWidget{background:#ffffff;border-bottom:1px solid %1;}").arg(kLine)));
    auto* headRow = new QHBoxLayout(head);
    headRow->setContentsMargins(UiZoom::s(20), UiZoom::s(10), UiZoom::s(20), UiZoom::s(10));
    headRow->setSpacing(UiZoom::s(12));
    runtime_->title = makeLabel(QStringLiteral("AI 助手"), 14, kInk, true);
    runtime_->title->setWordWrap(false);
    headRow->addWidget(runtime_->title, 1);

    // 把"攒了多少字"摆出来是有理由的：协议是无状态的，历史每一轮都要全量重发，
    // 聊得越久每句话越贵越慢。藏进菜单的话用户不会主动去点——
    // 他感觉不到自己在为一个月前的对话付钱。
    runtime_->contextSize = makeLabel(QString(), 11, kInkFaint);
    runtime_->contextSize->setWordWrap(false);
    headRow->addWidget(runtime_->contextSize);
    auto* clearButton = new QPushButton(QStringLiteral("清空重来"));
    clearButton->setCursor(Qt::PointingHandCursor);
    clearButton->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QPushButton{background:transparent;border:none;color:%1;padding:0 4px;}")
            .arg(kInkFaint)));
    QFont clearFont = clearButton->font();
    clearFont.setPixelSize(UiZoom::s(11));
    clearButton->setFont(clearFont);
    headRow->addWidget(clearButton);
    root->addWidget(head);

    // ---- 对话流：一个居中的阅读列 ----
    runtime_->scroll = new QScrollArea(this);
    runtime_->scroll->setWidgetResizable(true);
    runtime_->scroll->setFrameShape(QFrame::NoFrame);
    runtime_->scroll->setStyleSheet(QStringLiteral("QScrollArea{background:#ffffff;border:none;}"));
    auto* streamHost = new QWidget;
    streamHost->setStyleSheet(QStringLiteral("background:#ffffff;"));
    auto* hostRow = new QHBoxLayout(streamHost);
    hostRow->setContentsMargins(UiZoom::s(20), UiZoom::s(18), UiZoom::s(20), UiZoom::s(18));
    // 两侧的伸缩权重要**远小于**中间那一列，否则三个 1 会把宽度三等分——
    // 阅读列只能拿到三分之一，两边空出一大片。给列一个大权重，
    // 它先长到 maximumWidth，剩下的才分给两侧。
    hostRow->addStretch(1);
    auto* column = new QWidget;
    column->setStyleSheet(QStringLiteral("background:transparent;"));
    column->setMaximumWidth(UiZoom::s(kColumnWidth));
    runtime_->stream = new QVBoxLayout(column);
    runtime_->stream->setContentsMargins(0, 0, 0, 0);
    runtime_->stream->setSpacing(UiZoom::s(14));
    runtime_->stream->addStretch(1);
    hostRow->addWidget(column, 20);
    hostRow->addStretch(1);
    runtime_->scroll->setWidget(streamHost);
    root->addWidget(runtime_->scroll, 1);

    // ---- 输入区：一张卡，控件都在卡里 ----
    auto* composerHost = new QWidget(this);
    composerHost->setStyleSheet(QStringLiteral("background:#ffffff;"));
    auto* composerRow = new QHBoxLayout(composerHost);
    composerRow->setContentsMargins(UiZoom::s(20), 0, UiZoom::s(20), UiZoom::s(16));
    composerRow->addStretch(1);

    auto* card = new QFrame;
    card->setMaximumWidth(UiZoom::s(kColumnWidth));
    card->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QFrame{background:#ffffff;border:1px solid %1;border-radius:12px;}")
            .arg(kLine)));
    auto* cardColumn = new QVBoxLayout(card);
    cardColumn->setContentsMargins(UiZoom::s(14), UiZoom::s(10), UiZoom::s(10), UiZoom::s(8));
    cardColumn->setSpacing(UiZoom::s(6));

    // 输入框本身不画边框——边框是外面那张卡的。两层框会看起来像输入框里套输入框。
    runtime_->editor = new PromptEdit(card);
    runtime_->editor->setPlaceholderText(QStringLiteral("交给它做点什么…"));
    runtime_->editor->setFixedHeight(UiZoom::s(62));
    runtime_->editor->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QTextEdit{border:none;background:transparent;color:%1;}").arg(kInk)));
    QFont editorFont = runtime_->editor->font();
    editorFont.setPixelSize(UiZoom::s(13));
    runtime_->editor->setFont(editorFont);
    runtime_->editor->onSubmit = [this] { onSend(); };
    cardColumn->addWidget(runtime_->editor);

    // 卡片底边：左边是工作目录和模型（agent 能碰到什么的边界，必须一直看得见），
    // 右边是那句常驻承诺和发送。
    auto* foot = new QHBoxLayout;
    foot->setContentsMargins(0, 0, 0, 0);
    foot->setSpacing(UiZoom::s(10));
    runtime_->dirChip = makeLabel(QString(), 11, kInkSoft);
    runtime_->dirChip->setWordWrap(false);
    runtime_->modelChip = makeLabel(QString(), 11, kAccent);
    runtime_->modelChip->setWordWrap(false);
    foot->addWidget(runtime_->dirChip);
    foot->addWidget(runtime_->modelChip);
    foot->addStretch(1);
    // 这句常驻。它是这套东西最重要的一句承诺，写在文档里没人看。
    runtime_->hint = makeLabel(QStringLiteral("改东西前会先问你"), 11, kInkFaint);
    runtime_->hint->setWordWrap(false);
    foot->addWidget(runtime_->hint);
    runtime_->send = new QPushButton(QStringLiteral("发送"));
    runtime_->send->setCursor(Qt::PointingHandCursor);
    QFont sendFont = runtime_->send->font();
    sendFont.setPixelSize(UiZoom::s(12));
    runtime_->send->setFont(sendFont);
    foot->addWidget(runtime_->send);
    cardColumn->addLayout(foot);

    composerRow->addWidget(card, 20);
    composerRow->addStretch(1);
    root->addWidget(composerHost);

    setRunning(false);
    connect(runtime_->send, &QPushButton::clicked, this, &AgentChatPanel::onSend);
    connect(clearButton, &QPushButton::clicked, this, &AgentChatPanel::onClear);

    // ---- 接事件 ----
    // 这些信号全部已经在主线程上了（AgentController 负责把它们从核心的线程搬过来），
    // 所以这里查存储、改部件都随意。
    connect(&controller, &AgentController::textDelta, this,
            [this](const QString&, const QString&, const QString& partId, const QString& delta) {
                answerViewFor(partId)->append(delta);
                scrollToBottom();
            });
    connect(&controller, &AgentController::reasoningDelta, this,
            [this](const QString&, const QString&, const QString& partId, const QString& delta) {
                thinkingLineFor(partId)->append(delta);
                scrollToBottom();
            });
    connect(&controller, &AgentController::toolPartChanged, this,
            [this](const QString&, const QString& messageId, const QString& partId) {
                refreshToolCard(messageId, partId);
            });
    connect(&controller, &AgentController::permissionAsked, this,
            [this](const QString& permissionId, const QString&) { showApproval(permissionId); });
    connect(&controller, &AgentController::turnFinished, this, [this](const QString&) {
        setRunning(false);
        emit sessionListChanged();
        for (ThinkingLine* line : runtime_->thinking) line->settle();
        for (AnswerView* answer : runtime_->answers) answer->flush();
        refreshContextSize();
        scrollToBottom();
    });
    connect(&controller, &AgentController::turnFailed, this,
            [this](const QString&, const QString& message) {
                setRunning(false);
                for (ThinkingLine* line : runtime_->thinking) line->settle();
                for (AnswerView* answer : runtime_->answers) answer->flush();
                appendNotice(message, true);
            });
    connect(&controller, &AgentController::sessionTitleChanged, this,
            [this](const QString&, const QString& title) {
                runtime_->title->setText(title.isEmpty() ? QStringLiteral("AI 助手") : title);
                emit sessionListChanged();
            });
}

AgentChatPanel::~AgentChatPanel() = default;

QString AgentChatPanel::sessionId() const {
    return runtime_->sessionId;
}

void AgentChatPanel::setModelLabel(const QString& model) {
    runtime_->modelChip->setText(model);
}

void AgentChatPanel::openSession(const QString& sessionId) {
    runtime_->sessionId =
        sessionId.isEmpty() ? runtime_->controller->createSession(QDir::currentPath()) : sessionId;

    MaiSession session;
    if (runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), session)) {
        const QString directory = fromUtf8(session.directory);
        // 不用文件夹 emoji：这台机器的界面字体里不一定有那个字形，渲染出来是个豆腐块。
        runtime_->dirChip->setText(QDir(directory).dirName());
        runtime_->dirChip->setToolTip(directory);
        // 用 isUntitled() 而不是 title.isEmpty()：核心给新会话填的是一个占位标题
        //（"New session"），不是空串。判空的话那串占位符会直接显示在头部。
        runtime_->title->setText(session.isUntitled() ? QStringLiteral("AI 助手")
                                                      : fromUtf8(session.title));
    }
    reloadFromStore();
    emit sessionListChanged();
}

// ── 重画 ────────────────────────────────────────────────────────

void AgentChatPanel::reloadFromStore() {
    // 先把旧部件全拆掉。注意 takeAt(0) 会把最后那个 stretch 也取出来，所以后面要补回去。
    while (QLayoutItem* item = runtime_->stream->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
    runtime_->answers.clear();
    runtime_->thinking.clear();
    runtime_->toolCards.clear();

    for (const MaiMessage& message :
         runtime_->controller->agent().listMessages(toUtf8(runtime_->sessionId))) {
        if (message.role == MaiRole::User) {
            appendUserBubble(fromUtf8(message.text()));
            continue;
        }
        for (const MaiMessagePart& part : message.parts) {
            const QString partId = fromUtf8(part.id);
            if (const auto* text = std::get_if<MaiTextPart>(&part.body)) {
                answerViewFor(partId)->setMarkdown(fromUtf8(text->text));
            } else if (const auto* reasoning = std::get_if<MaiReasoningPart>(&part.body)) {
                ThinkingLine* line = thinkingLineFor(partId);
                line->append(fromUtf8(reasoning->text));
                line->settle();  // 历史里的思考早就结束了，别让它转圈
            } else if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                ToolCard* card = toolCardFor(partId);
                card->setCall(fromUtf8(tool->tool), fromUtf8(tool->input));
                card->apply(tool->state, false);
            }
        }
    }
    runtime_->stream->addStretch(1);
    refreshContextSize();
    scrollToBottom();
}

void AgentChatPanel::addToStream(QWidget* widget, Qt::Alignment alignment) {
    // **不能直接用 alignment 把部件塞进竖直布局。**
    //
    // 带对齐标志时 QVBoxLayout 按 sizeHint 给尺寸，而一个开了 wordWrap 的 QLabel
    // 的 sizeHint 是**不换行**那一行的宽度；再被 maximumWidth 卡住，高度却还是一行——
    // 结果就是长文本被硬生生截断。踩过一次，两个气泡都只显示半句。
    //
    // 正确做法是给每条消息包一层横向行：行本身撑满列宽，内容在行里靠左或靠右。
    auto* row = new QWidget;
    row->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(0);
    if (alignment.testFlag(Qt::AlignRight)) {
        rowLayout->addStretch(1);
        rowLayout->addWidget(widget);
    } else if (widget->sizePolicy().horizontalPolicy() == QSizePolicy::Maximum) {
        // 不想撑满的东西（工具卡）：贴左，右边留白。
        // 不补这条 stretch 的话 Qt 会把它**居中**——一张注记卡浮在正文中间很奇怪。
        rowLayout->addWidget(widget);
        rowLayout->addStretch(1);
    } else {
        // 助手的正文要占满整个阅读列，那正是它和气泡的区别。
        rowLayout->addWidget(widget, 1);
    }

    const int insertAt = std::max(0, runtime_->stream->count() - 1);
    runtime_->stream->insertWidget(insertAt, row);
}

void AgentChatPanel::appendUserBubble(const QString& text) {
    // 只有用户这一侧保留气泡。人发的消息短，气泡合适；
    // 而且右侧那块蓝色让"谁说的"一眼可辨，不用头像也不用名字。
    auto* bubble = makeLabel(text, 13, "#ffffff");
    const int padding = UiZoom::s(14) * 2;
    bubble->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QLabel{background:%1;color:#ffffff;border-radius:12px;padding:9px 14px;}")
            .arg(kAccent)));

    // **宽度得自己量，不能交给 sizeHint。**
    //
    // 开了 wordWrap 的 QLabel，sizeHint 给的是一个偏窄的方块（Qt 想让它接近正方），
    // 所以一句二十来字的话会被折成三行、右边空出一大片。
    //
    // 量法是"这句话排成一行要多宽"：放得下就给它那么宽，一个换行都不要；
    // 放不下才用满上限，让它在上限处折。
    //
    // 最后那点余量不是玄学：QLabel 自己还有 contentsMargins 和边框，
    // 样式表里的 padding 也是按整数像素缩放的。少算几个像素的后果不是"挤一点"，
    // 而是**最后一个字被挤到下一行**——"你好"两个字排成两行就是这么来的。
    const int cap = UiZoom::s(kColumnWidth * 3 / 4);
    const QFontMetrics metrics(bubble->font());
    const int oneLine = metrics.horizontalAdvance(text) + padding + UiZoom::s(8);
    bubble->setFixedWidth(std::min(oneLine, cap));
    addToStream(bubble, Qt::AlignRight);
}

void AgentChatPanel::appendNotice(const QString& text, bool isError) {
    auto* notice = makeLabel(text, 12, isError ? kDanger : kInkSoft);
    notice->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QLabel{background:%1;color:%2;border-radius:6px;padding:6px 10px;}")
            .arg(isError ? "#fef3f2" : kLineSoft, isError ? kDanger : kInkSoft)));
    addToStream(notice, Qt::AlignLeft);
}

AgentChatPanel::AnswerView* AgentChatPanel::answerViewFor(const QString& partId) {
    auto found = runtime_->answers.constFind(partId);
    if (found != runtime_->answers.constEnd()) return found.value();

    auto* answer = new AnswerView;
    addToStream(answer, Qt::AlignLeft);
    runtime_->answers.insert(partId, answer);
    return answer;
}

AgentChatPanel::ThinkingLine* AgentChatPanel::thinkingLineFor(const QString& partId) {
    auto found = runtime_->thinking.constFind(partId);
    if (found != runtime_->thinking.constEnd()) return found.value();

    auto* line = new ThinkingLine;
    addToStream(line, Qt::AlignLeft);
    runtime_->thinking.insert(partId, line);
    return line;
}

AgentChatPanel::ToolCard* AgentChatPanel::toolCardFor(const QString& partId) {
    auto found = runtime_->toolCards.constFind(partId);
    if (found != runtime_->toolCards.constEnd()) return found.value();

    auto* card = new ToolCard;
    // 不撑满整列：它是一条注记，不是正文。撑满会让它看起来比回答还重要。
    card->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    addToStream(card, Qt::AlignLeft);
    runtime_->toolCards.insert(partId, card);

    connect(card->allowButton(), &QPushButton::clicked, this,
            [this, partId] { replyForPart(partId, false, true); });
    connect(card->alwaysButton(), &QPushButton::clicked, this,
            [this, partId] { replyForPart(partId, true, true); });
    connect(card->denyButton(), &QPushButton::clicked, this,
            [this, partId] { replyForPart(partId, false, false); });
    return card;
}

void AgentChatPanel::replyForPart(const QString& partId, bool forSession, bool approve) {
    for (const MaiPermissionRequest& pending :
         runtime_->controller->agent().listPendingPermissions()) {
        if (fromUtf8(pending.partId) != partId) continue;
        const QString permissionId = fromUtf8(pending.id);
        if (approve) {
            runtime_->controller->approvePermission(permissionId, forSession);
        } else {
            runtime_->controller->denyPermission(permissionId);
        }
        if (ToolCard* card = runtime_->toolCards.value(partId, nullptr)) {
            card->apply(MaiToolState::Running, false);
        }
        return;
    }
}

void AgentChatPanel::refreshToolCard(const QString& messageId, const QString& partId) {
    for (const MaiMessage& message :
         runtime_->controller->agent().listMessages(toUtf8(runtime_->sessionId))) {
        if (fromUtf8(message.id) != messageId) continue;
        for (const MaiMessagePart& part : message.parts) {
            if (fromUtf8(part.id) != partId) continue;
            const auto* tool = std::get_if<MaiToolPart>(&part.body);
            if (tool == nullptr) return;

            ToolCard* card = toolCardFor(partId);
            card->setCall(fromUtf8(tool->tool), fromUtf8(tool->input));
            // 还在等人点头的话，卡片保持授权态——那个状态由 permissionAsked 打开，
            // 这里不要把它关掉。
            const bool waiting = card->isWaitingForUser();
            card->apply(tool->state, waiting && tool->state == MaiToolState::Pending);
            if (tool->state == MaiToolState::Completed && !tool->output.empty()) {
                card->setDetail(
                    QStringLiteral("完成 · %1 字节").arg(static_cast<int>(tool->output.size())));
            } else if (tool->state == MaiToolState::Error) {
                card->setDetail(fromUtf8(tool->error));
            }
            scrollToBottom();
            return;
        }
        return;
    }
}

void AgentChatPanel::showApproval(const QString& permissionId) {
    for (const MaiPermissionRequest& pending :
         runtime_->controller->agent().listPendingPermissions()) {
        if (fromUtf8(pending.id) != permissionId) continue;
        ToolCard* card = toolCardFor(fromUtf8(pending.partId));
        card->setCall(fromUtf8(pending.toolName), fromUtf8(pending.arguments));
        card->apply(MaiToolState::Pending, true);
        scrollToBottom();
        return;
    }
}

// ── 动作 ────────────────────────────────────────────────────────

void AgentChatPanel::onSend() {
    // 跑着的时候这个按钮是"停止"。
    if (runtime_->running) {
        runtime_->controller->interrupt(runtime_->sessionId);
        return;
    }
    const QString text = runtime_->editor->toPlainText().trimmed();
    if (text.isEmpty()) return;

    if (!runtime_->controller->sendPrompt(runtime_->sessionId, text)) {
        appendNotice(runtime_->controller->lastError(), true);
        return;
    }
    runtime_->editor->clear();
    appendUserBubble(text);
    setRunning(true);
    scrollToBottom();
}

void AgentChatPanel::onClear() {
    MaiResult<std::string> cleared =
        runtime_->controller->agent().submit(MaiClearMessages{toUtf8(runtime_->sessionId)});
    if (!cleared) {
        // 最常见的是 Busy：一轮还在跑。说清楚能怎么办，别只报错。
        appendNotice(fromUtf8(cleared.error().message()) + QStringLiteral("（先按停止，再清空）"),
                     true);
        return;
    }
    runtime_->title->setText(QStringLiteral("AI 助手"));
    reloadFromStore();
}

void AgentChatPanel::setRunning(bool running) {
    runtime_->running = running;
    runtime_->send->setText(running ? QStringLiteral("停止") : QStringLiteral("发送"));
    runtime_->send->setStyleSheet(UiZoom::scaleQss(
        running ? QStringLiteral("QPushButton{background:#ffffff;color:%1;"
                                 "border:1px solid #f0c3bd;border-radius:6px;padding:5px 17px;}")
                      .arg(kDanger)
                : QStringLiteral("QPushButton{background:%1;color:#ffffff;border:none;"
                                 "border-radius:6px;padding:5px 17px;}")
                      .arg(kAccent)));
    runtime_->hint->setText(running ? QStringLiteral("在跑 · 随时可以停")
                                    : QStringLiteral("改东西前会先问你"));
}

void AgentChatPanel::scrollToBottom() {
    // 部件是这一拍刚加进去的，布局还没算完高度，直接滚会滚到旧的底部。
    QTimer::singleShot(0, this, [this] {
        QScrollBar* bar = runtime_->scroll->verticalScrollBar();
        bar->setValue(bar->maximum());
    });
}

void AgentChatPanel::refreshContextSize() {
    int characters = 0;
    for (const MaiMessage& message :
         runtime_->controller->agent().listMessages(toUtf8(runtime_->sessionId))) {
        characters += fromUtf8(message.text()).size();
    }
    // 这是个**估数**，不是 token 数：核心现在不往外报 usage。
    // 给用户的信号是"越来越长"，这一点估数就够了。
    runtime_->contextSize->setText(
        characters == 0 ? QString() : QStringLiteral("已攒约 %1 字").arg(characters));
}
