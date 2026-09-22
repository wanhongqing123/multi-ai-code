#include "agent/AgentChatPanel.h"

#include <QAbstractTextDocumentLayout>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QDebug>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <variant>

#include "agent/AgentController.h"
#include "markdown/MarkdownView.h"
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

// 助手的回答不再有自己的部件。
//
// 原来每个回答是一个 QTextBrowser，走 MarkdownRenderer 出 HTML 交给 QTextDocument 排。
// 那条路受 CSS 子集限制（行内 padding、块级圆角、border-left、复选框都不支持），
// 而且每条消息一个部件，几百条之后滚动会卡。现在整个展示区是一个 MarkdownView，
// 自己排自己画，按可见区裁剪。

// ── 思考条 ──────────────────────────────────────────────────────
//
// 一行淡色文字，不是灰色药丸——对齐 Codex 那个「用时 1m 10s ›」。
// 它是回答的注脚，不该有自己的容器和背景，那会让它看起来像一条独立消息。
// ── 子任务 ──────────────────────────────────────────────────────
//
// 一个子 Agent 一张卡，**原地更新**，不往下刷新行。
//
// 为什么不把子任务的正文铺进主对话流：一屏里三个 agent 同时说话，谁也读不下去。
// 而且那本来就不是父在说话——父要的是结论，结论会通过 wait_agent 回到父这边，
// 正常出现在它的回答里。这张卡只回答一个问题：**它现在在干什么、干完没有。**
class AgentChatPanel::SubAgentCard final : public QFrame {
public:
    explicit SubAgentCard(QWidget* parent = nullptr) : QFrame(parent) {
        // 和 ToolCard 一样按 id 选：QLabel 本身是 QFrame 的子类，
        // 按类型选会把卡里每个标签也套上边框。
        setObjectName(QStringLiteral("agentSubTaskCard"));
        setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(UiZoom::s(11), UiZoom::s(7), UiZoom::s(11), UiZoom::s(7));
        column->setSpacing(UiZoom::s(4));

        auto* head = new QHBoxLayout;
        head->setSpacing(UiZoom::s(8));
        name_ = makeLabel(QString(), 12, kInk, true);
        name_->setWordWrap(false);
        state_ = makeLabel(QString(), 11, kInkFaint);
        state_->setWordWrap(false);
        head->addWidget(name_);
        head->addStretch(1);
        head->addWidget(state_);
        column->addLayout(head);

        // 它最近说的一句。**只留一行**：这是个进度指示，不是第二个对话框。
        latest_ = makeLabel(QString(), 12, kInkSoft);
        latest_->setWordWrap(false);
        column->addWidget(latest_);

        setStyleSheet(UiZoom::scaleQss(
            QStringLiteral("QFrame#agentSubTaskCard{background:#fbfcfe;border:1px solid %1;"
                           "border-radius:8px;}")
                .arg(kLine)));
    }

    void setTask(const QString& name) {
        name_->setText(QStringLiteral("子任务 · ") + name);
    }

    void setState(const QString& state) {
        state_->setText(state);
    }

    // 只显示最后一行，而且截断。子 Agent 可能吐几千字，
    // 全塞进来的话这张卡会把主对话流挤没。
    void setLatest(const QString& text) {
        QString line = text.trimmed();
        const int newline = line.lastIndexOf(QLatin1Char('\n'));
        if (newline >= 0) line = line.mid(newline + 1).trimmed();
        if (line.size() > 60) line = line.left(60) + QStringLiteral("…");
        latest_->setText(line);
        latest_->setVisible(!line.isEmpty());
    }

private:
    QLabel* name_ = nullptr;
    QLabel* state_ = nullptr;
    QLabel* latest_ = nullptr;
};

class AgentChatPanel::ThinkingLine final : public QFrame {
public:
    explicit ThinkingLine(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName(QStringLiteral("agentThinkingCard"));
        setCursor(Qt::PointingHandCursor);
        // 和工具卡同一副面孔（白底、细边、左侧色条、圆角）——它是同一列里的
        // 元素，裸文字夹在一堆卡片中间会显得像没画完。
        setStyleSheet(UiZoom::scaleQss(QStringLiteral(
            "QFrame#agentThinkingCard{background:#ffffff;border:1px solid %1;"
            "border-left:3px solid %2;border-radius:7px;}").arg(kLine, kInkFaint)));
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(UiZoom::s(10), UiZoom::s(6), UiZoom::s(10), UiZoom::s(6));
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

    // 历史恢复的思考条：时长没有落库，无从知晓——显示「思考过程」而不是
    // 编一个「思考了 0 秒」（刚建好就 settle，计时器走的永远是 0）。
    void settleRestored() {
        ticker_->stop();
        restored_ = true;
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
        if (restored_) {
            caption_->setText(QStringLiteral("思考过程") + caret);
            return;
        }
        caption_->setText(QStringLiteral("思考了 %1 秒").arg(seconds_) + caret);
    }

    QLabel* caption_ = nullptr;
    QLabel* body_ = nullptr;
    QTimer* ticker_ = nullptr;
    QElapsedTimer elapsed_;
    QString text_;
    bool expanded_ = false;
    bool restored_ = false;
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
    MarkdownView* view = nullptr;
    PromptEdit* editor = nullptr;
    QPushButton* send = nullptr;
    QLabel* hint = nullptr;

    // partId -> 思考条 / 工具卡。这两样要能点，画不出来，所以还是部件，
    // 由 MarkdownView 负责摆位置和跟着滚。
    QHash<QString, ThinkingLine*> thinking;
    QHash<QString, ToolCard*> toolCards;
    // 子会话 id -> 那张子任务卡。
    QHash<QString, SubAgentCard*> subAgentCards;

    // partId -> 已经攒到的 Markdown 原文。正文没有部件了，源在这儿。
    QHash<QString, QString> answers;
    // 攒着还没刷进视图的那些。
    QSet<QString> dirtyAnswers;
    // 每来一个 delta 就重排一次是 O(n^2)：一段 3000 字的回答会重排几百次。
    // 攒一小会儿再刷，肉眼看不出延迟，CPU 差一个数量级。
    // **定时器只有一个**，不是每条回答一个——原来那版是每个 AnswerView 自带一个。
    QTimer* flushTimer = nullptr;

    // 提示行（错误、说明）的 id 要唯一，它们不对应任何消息。
    int noticeSerial = 0;

    // 模型正在等回答的那次提问。空表示没有——输入框据此决定回车是发 prompt
    // 还是发答案。
    QString pendingQuestionId;

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

    // ---- 对话流：整片就是一个 MarkdownView ----
    //
    // 阅读列的居中和限宽由视图自己做。原来是 addStretch(1)/addWidget(列,20)/
    // addStretch(1) 这种权重摆出来的，权重给错一次列就只剩三分之一宽。
    runtime_->view = new MarkdownView(this);
    runtime_->view->setTheme(MarkdownTheme::standard(UiZoom::factor()));
    runtime_->view->setMaxContentWidth(UiZoom::s(kColumnWidth));
    root->addWidget(runtime_->view, 1);

    runtime_->flushTimer = new QTimer(this);
    runtime_->flushTimer->setSingleShot(true);
    runtime_->flushTimer->setInterval(90);
    connect(runtime_->flushTimer, &QTimer::timeout, this, [this] { flushAnswers(); });

    connect(runtime_->view, &MarkdownView::linkActivated, this,
            [](const QString& href) { QDesktopServices::openUrl(QUrl(href)); });

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
    // **每一条都要先认会话。**
    //
    // 子 Agent 是并发跑在另一个会话里的。不认的话它的正文会接进父的回答里、
    // 它跑完会把发送按钮从「停止」翻回「发送」、它的标题会顶掉头部那一行——
    // 而用户看到的是「AI 自己说了些我没问的话」。
    //
    // 以前只有一个会话在跑，忽略 sessionId 是无害的；现在不是了。
    connect(&controller, &AgentController::textDelta, this,
            [this](const QString& sessionId, const QString&, const QString& partId,
                   const QString& delta) {
                if (!isCurrentSession(sessionId)) {
                    noteOtherSession(sessionId, delta);
                    return;
                }
                appendAnswerDelta(partId, delta);
            });
    connect(&controller, &AgentController::reasoningDelta, this,
            [this](const QString& sessionId, const QString&, const QString& partId,
                   const QString& delta) {
                if (!isCurrentSession(sessionId)) return;
                thinkingLineFor(partId)->append(delta);
                scrollToBottom();
            });
    connect(&controller, &AgentController::toolPartChanged, this,
            [this](const QString& sessionId, const QString& messageId, const QString& partId) {
                if (!isCurrentSession(sessionId)) return;
                refreshToolCard(messageId, partId);
            });
    connect(&controller, &AgentController::permissionAsked, this,
            [this](const QString& permissionId, const QString& sessionId) {
                // 授权**不按会话过滤**：子 Agent 也要用户点头，而它没有自己的界面。
                // 滤掉的话它会永远挂在闸门上，用户只看到「一直在跑」。
                (void)sessionId;
                showApproval(permissionId);
            });
    connect(&controller, &AgentController::questionAsked, this,
            [this](const QString& questionId, const QString&, const QString&) {
                // 同授权：子 Agent 问的话也得有人答。
                showQuestion(questionId);
            });
    connect(&controller, &AgentController::questionAnswered, this,
            [this](const QString&, const QString&) { clearQuestion(); });
    connect(&controller, &AgentController::turnFinished, this, [this](const QString& sessionId) {
        if (!isCurrentSession(sessionId)) {
            noteOtherSession(sessionId, QString());
            return;
        }
        setRunning(false);
        emit sessionListChanged();
        for (ThinkingLine* line : runtime_->thinking) line->settle();
        flushAnswers();
        refreshContextSize();
        scrollToBottom();
    });
    connect(&controller, &AgentController::turnFailed, this,
            [this](const QString& sessionId, const QString& message) {
                if (!isCurrentSession(sessionId)) {
                    noteOtherSession(sessionId, message);
                    return;
                }
                setRunning(false);
                for (ThinkingLine* line : runtime_->thinking) line->settle();
                flushAnswers();
                appendNotice(message, true);
            });
    connect(&controller, &AgentController::sessionTitleChanged, this,
            [this](const QString& sessionId, const QString& title) {
                if (!isCurrentSession(sessionId)) return;
                runtime_->title->setText(title.isEmpty() ? QStringLiteral("AI 助手") : title);
                emit sessionListChanged();
            });
}

AgentChatPanel::~AgentChatPanel() = default;

bool AgentChatPanel::isCurrentSession(const QString& sessionId) const {
    return sessionId == runtime_->sessionId;
}

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
    // 视图自己会把嵌进去的部件删掉，这儿只要把索引清干净。
    runtime_->flushTimer->stop();
    runtime_->view->clear();
    runtime_->answers.clear();
    runtime_->dirtyAnswers.clear();
    runtime_->thinking.clear();
    runtime_->toolCards.clear();
    runtime_->subAgentCards.clear();

    for (const MaiMessage& message :
         runtime_->controller->agent().listMessages(toUtf8(runtime_->sessionId))) {
        if (message.role == MaiRole::User) {
            // 历史里的气泡用消息 id：重开会话再画一遍时 id 要稳定，
            // 不然同一条消息会被当成两条。
            runtime_->view->addItem(fromUtf8(message.id), MarkdownView::Style::Bubble,
                                    fromUtf8(message.text()));
            continue;
        }
        for (const MaiMessagePart& part : message.parts) {
            const QString partId = fromUtf8(part.id);
            if (const auto* text = std::get_if<MaiTextPart>(&part.body)) {
                runtime_->answers.insert(partId, fromUtf8(text->text));
                runtime_->view->addItem(partId, MarkdownView::Style::Document,
                                        fromUtf8(text->text));
            } else if (const auto* reasoning = std::get_if<MaiReasoningPart>(&part.body)) {
                // 空文本的思考片段没有可展开的内容，画出来就是个点不开的空壳。
                if (reasoning->text.empty()) continue;
                ThinkingLine* line = thinkingLineFor(partId);
                line->append(fromUtf8(reasoning->text));
                line->settleRestored();  // 历史里的思考早就结束了；时长没落库，别编秒数
            } else if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                ToolCard* card = toolCardFor(partId);
                card->setCall(fromUtf8(tool->tool), fromUtf8(tool->input));
                card->apply(tool->state, false);
            }
        }
    }
    refreshContextSize();
    scrollToBottom();
}

// 流式期间往某条回答后面追加。第一段到的时候先把条目建出来，
// 用户立刻看得见有东西在长；后面的靠定时器攒着批量刷。
void AgentChatPanel::appendAnswerDelta(const QString& partId, const QString& delta) {
    QString& source = runtime_->answers[partId];
    const bool isNew = source.isEmpty() && !runtime_->view->contains(partId);
    source += delta;
    if (isNew) {
        runtime_->view->addItem(partId, MarkdownView::Style::Document, source);
        scrollToBottom();
        return;
    }
    runtime_->dirtyAnswers.insert(partId);
    if (!runtime_->flushTimer->isActive()) runtime_->flushTimer->start();
}

void AgentChatPanel::flushAnswers() {
    runtime_->flushTimer->stop();
    if (runtime_->dirtyAnswers.isEmpty()) return;
    const bool wasAtBottom = runtime_->view->isAtBottom();
    for (const QString& partId : runtime_->dirtyAnswers) {
        runtime_->view->updateItem(partId, runtime_->answers.value(partId));
    }
    runtime_->dirtyAnswers.clear();
    // 正在看历史的时候不要把人拽回底部。
    if (wasAtBottom) runtime_->view->scrollToBottom();
}

void AgentChatPanel::appendUserBubble(const QString& text) {
    // 只有用户这一侧保留气泡。人发的消息短，气泡合适；
    // 而且右侧那块底色让"谁说的"一眼可辨，不用头像也不用名字。
    //
    // 宽度不用自己量了：视图先按上限排一遍、再按**量出来的**自然宽度收窄。
    // 原来是拿 QFontMetrics 估的，估窄了最后一个字会被挤到下一行
    //（"你好"两个字排成两行就是这么来的）。
    runtime_->view->addItem(QStringLiteral("local-%1").arg(++runtime_->noticeSerial),
                            MarkdownView::Style::Bubble, text);
    scrollToBottom();
}

void AgentChatPanel::appendNotice(const QString& text, bool isError) {
    runtime_->view->addItem(
        QStringLiteral("notice-%1").arg(++runtime_->noticeSerial),
        isError ? MarkdownView::Style::Error : MarkdownView::Style::Notice, text);
    scrollToBottom();
}

AgentChatPanel::ThinkingLine* AgentChatPanel::thinkingLineFor(const QString& partId) {
    auto found = runtime_->thinking.constFind(partId);
    if (found != runtime_->thinking.constEnd()) return found.value();

    auto* line = new ThinkingLine;
    runtime_->view->addWidget(partId, line);
    runtime_->thinking.insert(partId, line);
    return line;
}

AgentChatPanel::ToolCard* AgentChatPanel::toolCardFor(const QString& partId) {
    auto found = runtime_->toolCards.constFind(partId);
    if (found != runtime_->toolCards.constEnd()) return found.value();

    auto* card = new ToolCard;
    // 不撑满整列：它是一条注记，不是正文。撑满会让它看起来比回答还重要。
    // MarkdownView 认这个策略，按 sizeHint 给宽度并靠左摆。
    card->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    runtime_->view->addWidget(partId, card);
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

void AgentChatPanel::noteOtherSession(const QString& sessionId, const QString& text) {
    // 只认自己的孩子。别的根会话（用户在另一个标签页里开的）不关这儿的事。
    if (!runtime_->controller->isChildOf(sessionId, runtime_->sessionId)) return;

    SubAgentCard* card = subAgentCardFor(sessionId);
    if (card == nullptr) return;
    if (!text.isEmpty()) card->setLatest(text);

    // 状态每次都从核心现取，不在这边推算：多端同时开着的时候，
    // 推算出来的状态会和真实情况岔开。
    for (const MaiSubAgentInfo& info : runtime_->controller->subAgents(runtime_->sessionId)) {
        if (fromUtf8(info.sessionId) != sessionId) continue;
        card->setTask(fromUtf8(info.taskName));
        const std::string& status = info.status;
        card->setState(status == "running"  ? QStringLiteral("在跑")
                       : status == "closed" ? QStringLiteral("已收")
                                            : QStringLiteral("完成"));
        return;
    }
}

AgentChatPanel::SubAgentCard* AgentChatPanel::subAgentCardFor(const QString& sessionId) {
    auto found = runtime_->subAgentCards.constFind(sessionId);
    if (found != runtime_->subAgentCards.constEnd()) return found.value();

    auto* card = new SubAgentCard;
    runtime_->view->addWidget(QStringLiteral("sub-") + sessionId, card);
    runtime_->subAgentCards.insert(sessionId, card);
    scrollToBottom();
    return card;
}

void AgentChatPanel::showQuestion(const QString& questionId) {
    // 问题文本不在事件里（它在那次工具调用的参数上，重复一份就有两个真相），
    // 所以回核心取。
    for (const MaiQuestionRequest& pending : runtime_->controller->pendingQuestions()) {
        if (fromUtf8(pending.id) != questionId) continue;

        QString text = fromUtf8(pending.question);
        if (!pending.options.empty()) {
            // 选项只是提示，用户照样可以回别的，所以摆成一行字而不是按钮——
            // 做成按钮会让人以为只能选这几个。
            QStringList options;
            for (const std::string& option : pending.options) options << fromUtf8(option);
            text += QStringLiteral("\n\n") + options.join(QStringLiteral(" · "));
        }
        runtime_->pendingQuestionId = questionId;
        appendNotice(text, false);
        runtime_->editor->setPlaceholderText(QStringLiteral("回答它…"));
        runtime_->send->setText(QStringLiteral("回答"));
        runtime_->hint->setText(QStringLiteral("它在等你回答"));
        runtime_->editor->setFocus();
        scrollToBottom();
        return;
    }
}

void AgentChatPanel::clearQuestion() {
    if (runtime_->pendingQuestionId.isEmpty()) return;
    runtime_->pendingQuestionId.clear();
    runtime_->editor->setPlaceholderText(QStringLiteral("交给它做点什么…"));
    // 按钮和提示交回给 setRunning 管：那一轮多半还在跑，回答完接着跑。
    setRunning(runtime_->running);
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
    // 正在等回答时，**先把这一句当答案送出去**。
    //
    // 这个判断要放在"跑着就是停止"前面：等回答的时候那一轮确实还在跑，
    // 但用户按回车的意思显然是回答，不是中断。
    if (!runtime_->pendingQuestionId.isEmpty()) {
        const QString answer = runtime_->editor->toPlainText().trimmed();
        if (answer.isEmpty()) return;
        if (!runtime_->controller->answerQuestion(runtime_->pendingQuestionId, answer)) {
            appendNotice(runtime_->controller->lastError(), true);
            return;
        }
        runtime_->editor->clear();
        appendUserBubble(answer);
        clearQuestion();
        scrollToBottom();
        return;
    }

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
    runtime_->view->scrollToBottom();
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
