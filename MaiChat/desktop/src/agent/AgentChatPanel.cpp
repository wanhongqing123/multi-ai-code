#include "agent/AgentChatPanel.h"

#include <QAbstractTextDocumentLayout>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <variant>

#include "agent/AgentController.h"
#include "markdown/MarkdownView.h"
#include "ui/ComposerTextEdit.h"
#include "ui/UiZoom.h"

namespace {

// 配色取自 MainWindow.cpp，别在这儿另起一套。
const char* const kInk = "#172033";
const char* const kInkSoft = "#667085";
const char* const kInkFaint = "#98a2b3";
const char* const kLine = "#e2e8f0";
const char* const kLineSoft = "#f1f5f9";
const char* const kAccent = "#0b67b7";
const char* const kDanger = "#b42318";

std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString fromUtf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

QString toolPrimaryArgument(const QString& tool, const QString& arguments) {
    const QJsonDocument document = QJsonDocument::fromJson(arguments.toUtf8());
    if (!document.isObject()) return arguments.simplified();
    const QJsonObject object = document.object();
    const QString key = tool == QStringLiteral("shell")      ? QStringLiteral("command")
                        : tool == QStringLiteral("glob")     ? QStringLiteral("pattern")
                        : tool == QStringLiteral("grep")     ? QStringLiteral("pattern")
                        : tool == QStringLiteral("webfetch") ? QStringLiteral("url")
                                                               : QStringLiteral("path");
    return object.value(key).toString().simplified();
}

QString toolActionText(const QString& tool, const QString& argument, MaiToolState state,
                       bool waitingForUser) {
    QString action;
    if (waitingForUser) {
        action = QStringLiteral("等待批准");
    } else if (state == MaiToolState::Running || state == MaiToolState::Pending) {
        action = QStringLiteral("正在运行");
    } else if (state == MaiToolState::Error) {
        action = QStringLiteral("运行失败");
    } else {
        action = QStringLiteral("已运行");
    }

    const QString target = argument.isEmpty() ? tool : argument;
    QString text = action + QLatin1Char(' ') + target;
    if (text.size() > 120) text = text.left(117) + QStringLiteral("…");
    return text;
}

QLabel* makeLabel(const QString& text, int pixelSize, const char* color, bool bold = false) {
    auto* label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
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

void applyAgentMenuStyle(QMenu* menu) {
    if (menu == nullptr) return;
    menu->setWindowFlags(menu->windowFlags() | Qt::FramelessWindowHint |
                         Qt::NoDropShadowWindowHint);
    menu->setAttribute(Qt::WA_TranslucentBackground);
    menu->setToolTipsVisible(true);
    menu->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QMenu {
            background: #ffffff;
            border: 1px solid #dbe5f0;
            border-radius: 10px;
            padding: 6px;
        }
        QMenu::item {
            background: transparent;
            color: #344054;
            font-size: 12px;
            padding: 8px 34px 8px 12px;
            border-radius: 6px;
        }
        QMenu::item:selected {
            background: #eef6ff;
            color: #0b67b7;
        }
        QMenu::item:checked {
            color: #0b67b7;
            font-weight: 700;
        }
        QMenu::indicator {
            width: 8px;
            height: 8px;
            margin-left: 9px;
        }
        QMenu::indicator:checked {
            background: #0b67b7;
            border-radius: 4px;
        }
    )")));
}

QIcon makeComposerActionIcon(bool stop, const QColor& color) {
    constexpr int kRender = 48;
    QPixmap pixmap(kRender, kRender);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    if (stop) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(14, 14, 20, 20), 3, 3);
    } else {
        painter.drawPolygon(QPolygonF({QPointF(8, 22), QPointF(40, 8), QPointF(30, 40),
                                       QPointF(23, 28)}));
        painter.drawLine(QPointF(8, 22), QPointF(23, 28));
        painter.drawLine(QPointF(23, 28), QPointF(40, 8));
    }
    painter.end();
    return QIcon(pixmap);
}

// Enter 发送，Shift+Enter 换行。
//
// 得自己拦 keyPressEvent：QTextEdit 默认把 Enter 当换行，而这个框是多行的，
// 不拦的话用户每轮都得去点发送按钮。
class PromptEdit final : public ComposerTextEdit {
public:
    explicit PromptEdit(QWidget* parent = nullptr) : ComposerTextEdit(parent) {}
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
        setStyleSheet(QStringLiteral(
            "QFrame#agentThinkingCard{background:transparent;border:none;}"));
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(0, UiZoom::s(2), 0, UiZoom::s(2));
        column->setSpacing(UiZoom::s(4));

        caption_ = makeLabel(QStringLiteral("正在思考"), 12, kInkFaint);
        caption_->setWordWrap(false);
        column->addWidget(caption_);

        body_ = makeLabel(QString(), 12, kInkFaint);
        body_->setObjectName(QStringLiteral("agentThinkingBody"));
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
        // 默认收起时不让 QLabel 每个增量都重排一遍完整推理文本。
        // 推理可以有几万字，这条旧路径是明显的 O(n²) 主线程开销。
        if (expanded_) body_->setText(text_);
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
        if (expanded_) body_->setText(text_);
        body_->setVisible(expanded_ && !text_.isEmpty());
        refreshCaption();
    }

private:
    void refreshCaption() {
        if (ticker_->isActive()) {
            // 收着也要动：那段时间一个正文字都不会来，完全没反应和卡死分不开。
            dots_ = (dots_ + 1) % 4;
            caption_->setText(QStringLiteral("正在思考") + QString(dots_, QChar('.')));
            return;
        }
        const QString caret = expanded_ ? QStringLiteral(" ⌄") : QStringLiteral(" ›");
        if (restored_) {
            caption_->setText(QStringLiteral("思考过程") + caret);
            return;
        }
        caption_->setText(QStringLiteral("已处理 %1 秒").arg(seconds_) + caret);
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
        column->setContentsMargins(0, UiZoom::s(2), 0, UiZoom::s(2));
        column->setSpacing(UiZoom::s(5));

        auto* head = new QHBoxLayout;
        head->setSpacing(UiZoom::s(8));
        icon_ = makeLabel(QStringLiteral("⌘"), 12, kInkFaint);
        icon_->setWordWrap(false);
        name_ = makeLabel(QString(), 12, kInkSoft);
        name_->setWordWrap(false);
        state_ = makeLabel(QString(), 11, kInkFaint);
        state_->setWordWrap(false);
        expand_ = new QPushButton(QStringLiteral("›"));
        expand_->setObjectName(QStringLiteral("agentToolDisclosure"));
        expand_->setCursor(Qt::PointingHandCursor);
        expand_->setFixedSize(UiZoom::s(22), UiZoom::s(22));
        expand_->setStyleSheet(QStringLiteral(
            "QPushButton{background:transparent;border:none;color:#98a2b3;padding:0;}"));
        expand_->hide();
        head->addWidget(icon_);
        head->addWidget(name_);
        head->addStretch(1);
        head->addWidget(state_);
        head->addWidget(expand_);
        column->addLayout(head);

        detail_ = makeLabel(QString(), 11, kInkSoft);
        detail_->setObjectName(QStringLiteral("agentToolDetail"));
        detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont detailFont = detail_->font();
        detailFont.setStyleHint(QFont::Monospace);
        detailFont.setFamily(QStringLiteral("Menlo"));
        detail_->setFont(detailFont);
        detail_->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
            "QLabel#agentToolDetail{background:#f7f8fa;border:1px solid %1;"
            "border-radius:7px;color:%2;padding:9px;}").arg(kLine, kInkSoft)));
        detail_->hide();
        column->addWidget(detail_);

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

        QObject::connect(expand_, &QPushButton::clicked, this, [this] {
            expanded_ = !expanded_;
            detail_->setVisible(expanded_ && !detailText_.isEmpty());
            expand_->setText(expanded_ ? QStringLiteral("⌄") : QStringLiteral("›"));
        });

        apply(MaiToolState::Pending, false);
    }

    void setCall(const QString& tool, const QString& arguments) {
        tool_ = tool;
        arguments_ = arguments;
        primaryArgument_ = toolPrimaryArgument(tool, arguments);
        refreshSummary();
    }

    void apply(MaiToolState toolState, bool waitingForUser) {
        const char* edge = kInkFaint;
        QString label;
        stateValue_ = toolState;
        waiting_ = waitingForUser;
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
        setStyleSheet(QStringLiteral(
            "QFrame#agentToolCard{background:transparent;border:none;}"));
        icon_->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(edge));
        approval_->setVisible(waitingForUser);
        refreshSummary();
    }

    void setDetail(const QString& text) {
        QString rendered;
        if (tool_ == QStringLiteral("shell") && !primaryArgument_.isEmpty()) {
            rendered = QStringLiteral("$ ") + primaryArgument_;
            if (!text.trimmed().isEmpty()) rendered += QStringLiteral("\n\n") + text.trimmed();
        } else {
            rendered = arguments_.trimmed();
            if (!text.trimmed().isEmpty()) rendered += QStringLiteral("\n\n") + text.trimmed();
        }
        constexpr int kMaxDetailCharacters = 12000;
        if (rendered.size() > kMaxDetailCharacters) {
            rendered = rendered.left(kMaxDetailCharacters) +
                       QStringLiteral("\n\n…（输出过长，已截断显示）");
        }
        detailText_ = rendered;
        detail_->setText(detailText_);
        expand_->setVisible(!detailText_.isEmpty());
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
    void refreshSummary() {
        name_->setText(toolActionText(tool_, primaryArgument_, stateValue_, waiting_));
    }

    QLabel* icon_ = nullptr;
    QLabel* name_ = nullptr;
    QLabel* state_ = nullptr;
    QLabel* detail_ = nullptr;
    QPushButton* expand_ = nullptr;
    QWidget* approval_ = nullptr;
    QPushButton* allow_ = nullptr;
    QPushButton* deny_ = nullptr;
    QPushButton* always_ = nullptr;
    QString tool_;
    QString arguments_;
    QString primaryArgument_;
    QString detailText_;
    MaiToolState stateValue_ = MaiToolState::Pending;
    bool waiting_ = false;
    bool expanded_ = false;
};

struct AgentChatPanel::Runtime {
    AgentController* controller = nullptr;
    QString sessionId;

    QLabel* title = nullptr;
    QLabel* contextSize = nullptr;
    QPushButton* modelChip = nullptr;
    MarkdownView* view = nullptr;
    PromptEdit* editor = nullptr;
    QPushButton* send = nullptr;
    QPushButton* hint = nullptr;

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
    bool modelConfigured = true;
};

AgentChatPanel::AgentChatPanel(AgentController& controller, QWidget* parent)
    : QWidget(parent), runtime_(std::make_unique<Runtime>()) {
    runtime_->controller = &controller;
    setStyleSheet(QStringLiteral("background:#ffffff;"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 头部：标题 + 模型/权限 + 上下文 + 更多 ----
    // 模型、权限和低频配置属于页面级设置，不应挤在消息编辑框里。
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
    auto* moreButton = new QPushButton(QStringLiteral("•••"));
    moreButton->setObjectName(QStringLiteral("agentMoreActions"));
    moreButton->setAccessibleName(QStringLiteral("更多"));
    moreButton->setCursor(Qt::PointingHandCursor);
    moreButton->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QPushButton{background:#f5f7fa;border:none;border-radius:8px;"
                       "color:%1;padding:5px 9px;}QPushButton:hover{background:#eef2f6;}")
            .arg(kInkSoft)));
    auto* moreMenu = new QMenu(moreButton);
    applyAgentMenuStyle(moreMenu);
    QAction* configureAction = moreMenu->addAction(QStringLiteral("模型配置"));
    QAction* clearAction = moreMenu->addAction(QStringLiteral("清空当前对话"));
    moreButton->setMenu(moreMenu);
    headRow->addWidget(moreButton);
    root->addWidget(head);

    // ---- 对话流：整片就是一个 MarkdownView ----
    // AI 页面与 IM 一样使用窗口可用宽度，只保留 MarkdownView 自己的页边距。
    runtime_->view = new MarkdownView(this);
    runtime_->view->setTheme(MarkdownTheme::standard(UiZoom::factor()));
    root->addWidget(runtime_->view, 1);

    runtime_->flushTimer = new QTimer(this);
    runtime_->flushTimer->setSingleShot(true);
    runtime_->flushTimer->setInterval(90);
    connect(runtime_->flushTimer, &QTimer::timeout, this, [this] { flushAnswers(); });

    connect(runtime_->view, &MarkdownView::linkActivated, this,
            [](const QString& href) { QDesktopServices::openUrl(QUrl(href)); });

    // ---- 输入区：与普通 IM 共用同一个 ComposerTextEdit 形状 ----
    auto* composerHost = new QWidget(this);
    composerHost->setObjectName(QStringLiteral("agentComposerPanel"));
    composerHost->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
        "QWidget#agentComposerPanel{background:#ffffff;border-top:1px solid %1;}"))
                                    .arg(kLine));
    auto* composerLayout = new QVBoxLayout(composerHost);
    composerLayout->setContentsMargins(UiZoom::s(24), UiZoom::s(12), UiZoom::s(24),
                                       UiZoom::s(14));
    composerLayout->setSpacing(0);

    runtime_->editor = new PromptEdit(composerHost);
    runtime_->editor->setObjectName(QStringLiteral("agentPromptEditor"));
    runtime_->editor->setPlaceholderText(QStringLiteral("交给它做点什么…"));
    runtime_->editor->setFixedHeight(UiZoom::s(112));
    runtime_->editor->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QTextEdit#agentPromptEditor {
            border:1px solid %1;
            border-radius:14px;
            background:#ffffff;
            color:%2;
            padding:10px 52px 46px 13px;
        }
        QTextEdit#agentPromptEditor:focus { border-color:#58b7ff; }
    )").arg(kLine, kInk)));
    QFont editorFont = runtime_->editor->font();
    editorFont.setPixelSize(UiZoom::s(14));
    runtime_->editor->setFont(editorFont);
    runtime_->editor->onSubmit = [this] { onSend(); };
    runtime_->editor->setMimeHandler(
        [this](const QMimeData* mime) { return insertComposerMimeData(mime); });
    composerLayout->addWidget(runtime_->editor);

    runtime_->modelChip = new QPushButton;
    runtime_->modelChip->setObjectName(QStringLiteral("agentModelChip"));
    runtime_->modelChip->setCursor(Qt::PointingHandCursor);
    runtime_->modelChip->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QPushButton#agentModelChip {
            background: #eef6ff;
            border: 1px solid #d5e8fb;
            border-radius: 7px;
            color: #0b67b7;
            padding: 4px 8px;
        }
        QPushButton#agentModelChip:hover {
            background: #e2f0ff;
            border-color: #b9daf8;
        }
        QPushButton#agentModelChip:pressed {
            background: #d5e8fb;
        }
        QPushButton#agentModelChip::menu-indicator {
            image: none;
            width: 0;
        }
    )")));
    QFont modelFont = runtime_->modelChip->font();
    modelFont.setPixelSize(UiZoom::s(11));
    runtime_->modelChip->setFont(modelFont);
    auto* modelMenu = new QMenu(runtime_->modelChip);
    applyAgentMenuStyle(modelMenu);
    for (const QString& model : {QStringLiteral("glm-5.3"),
                                 QStringLiteral("glm-5.3-flash")}) {
        QAction* action = modelMenu->addAction(model);
        action->setData(model);
        action->setCheckable(true);
    }
    connect(modelMenu, &QMenu::aboutToShow, this, [this, modelMenu] {
        for (QAction* action : modelMenu->actions()) {
            action->setChecked(action->data().toString().compare(
                                   runtime_->modelChip->text(), Qt::CaseInsensitive) == 0);
        }
    });
    connect(modelMenu, &QMenu::triggered, this, [this](QAction* action) {
        const QString selected = action->data().toString();
        if (!runtime_->controller->setModel(runtime_->sessionId, selected)) {
            appendNotice(runtime_->controller->lastError(), true);
            return;
        }
        setModelLabel(selected);
        emit modelSelected(selected);
    });
    runtime_->modelChip->setMenu(modelMenu);
    headRow->insertWidget(1, runtime_->modelChip);
    // 这句常驻。它是这套东西最重要的一句承诺，写在文档里没人看。
    runtime_->hint = new QPushButton;
    runtime_->hint->setObjectName(QStringLiteral("agentApprovalPolicy"));
    runtime_->hint->setCursor(Qt::PointingHandCursor);
    runtime_->hint->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QPushButton#agentApprovalPolicy {
            background: #fff7ed;
            border: 1px solid #fed7aa;
            border-radius: 7px;
            color: #b54708;
            padding: 4px 9px;
        }
        QPushButton#agentApprovalPolicy:hover {
            background: #ffedd5;
            border-color: #fdba74;
        }
        QPushButton#agentApprovalPolicy:disabled {
            background: #f8fafc;
            border-color: #e2e8f0;
            color: #98a2b3;
        }
        QPushButton#agentApprovalPolicy::menu-indicator {
            image: none;
            width: 0;
        }
    )")));
    QFont policyFont = runtime_->hint->font();
    policyFont.setPixelSize(UiZoom::s(11));
    runtime_->hint->setFont(policyFont);
    auto* policyMenu = new QMenu(runtime_->hint);
    applyAgentMenuStyle(policyMenu);
    auto addPolicy = [policyMenu](const QString& title, const QString& detail,
                                  MaiApprovalPolicy policy) {
        QAction* action = policyMenu->addAction(title);
        action->setData(static_cast<int>(policy));
        action->setCheckable(true);
        action->setToolTip(detail);
    };
    addPolicy(QStringLiteral("请求批准"),
              QStringLiteral("修改文件、访问网络或执行高风险命令时询问"),
              MaiApprovalPolicy::OnRequest);
    addPolicy(QStringLiteral("帮我批准"),
              QStringLiteral("工作区文件修改自动批准；网络和高风险命令仍询问"),
              MaiApprovalPolicy::UnlessTrusted);
    addPolicy(QStringLiteral("完全访问"),
              QStringLiteral("不逐次询问；终端和网络调用会直接执行"),
              MaiApprovalPolicy::Never);
    runtime_->hint->setMenu(policyMenu);
    connect(policyMenu, &QMenu::triggered, this, [this](QAction* action) {
        selectApprovalPolicy(static_cast<MaiApprovalPolicy>(action->data().toInt()));
    });
    updateApprovalPolicyUi();
    headRow->insertWidget(2, runtime_->hint);
    runtime_->send = new QPushButton(runtime_->editor);
    runtime_->send->setObjectName(QStringLiteral("agentSendButton"));
    runtime_->send->setCursor(Qt::PointingHandCursor);
    runtime_->send->setFixedSize(UiZoom::s(36), UiZoom::s(36));
    runtime_->send->setIconSize(QSize(UiZoom::s(18), UiZoom::s(18)));
    runtime_->editor->setCornerAction(runtime_->send);
    root->addWidget(composerHost);

    setRunning(false);
    connect(runtime_->send, &QPushButton::clicked, this, &AgentChatPanel::onSend);
    connect(configureAction, &QAction::triggered, this,
            [this] { emit modelConfigurationRequested(); });
    connect(clearAction, &QAction::triggered, this, &AgentChatPanel::onClear);

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
    runtime_->modelConfigured = model != QStringLiteral("未配置模型") && !model.trimmed().isEmpty();
    runtime_->modelChip->setEnabled(runtime_->modelConfigured && !runtime_->running);
}

void AgentChatPanel::selectApprovalPolicy(MaiApprovalPolicy policy) {
    if (!runtime_->controller->setApprovalPolicy(policy)) {
        appendNotice(runtime_->controller->lastError(), true);
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("agent/approvalPolicy"),
                      policy == MaiApprovalPolicy::Never
                          ? QStringLiteral("never")
                      : policy == MaiApprovalPolicy::UnlessTrusted
                          ? QStringLiteral("unless_trusted")
                          : QStringLiteral("on_request"));
    settings.sync();
    updateApprovalPolicyUi();
}

void AgentChatPanel::updateApprovalPolicyUi() {
    const MaiApprovalPolicy policy = runtime_->controller->approvalPolicy();
    for (QAction* action : runtime_->hint->menu()->actions()) {
        action->setChecked(action->data().toInt() == static_cast<int>(policy));
    }
    if (policy == MaiApprovalPolicy::Never) {
        runtime_->hint->setText(QStringLiteral("完全访问"));
        runtime_->hint->setToolTip(
            QStringLiteral("不逐次询问；终端和网络调用会直接执行。"));
    } else if (policy == MaiApprovalPolicy::UnlessTrusted) {
        runtime_->hint->setText(QStringLiteral("帮我批准"));
        runtime_->hint->setToolTip(
            QStringLiteral("工作区文件修改自动批准；访问网络和高风险命令仍会询问。"));
    } else {
        runtime_->hint->setText(QStringLiteral("请求批准"));
        runtime_->hint->setToolTip(
            QStringLiteral("修改文件、访问网络或执行高风险命令前都会询问。"));
    }
}

void AgentChatPanel::openSession(const QString& sessionId) {
    runtime_->sessionId =
        sessionId.isEmpty() ? runtime_->controller->createSession(QDir::currentPath()) : sessionId;

    MaiSession session;
    if (runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), session)) {
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

    MaiSession selectedSession;
    runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), selectedSession);

    for (const MaiMessage& message :
         runtime_->controller->agent().listMessages(toUtf8(runtime_->sessionId))) {
        if (message.role == MaiRole::User) {
            // 历史里的气泡用消息 id：重开会话再画一遍时 id 要稳定，
            // 不然同一条消息会被当成两条。
            runtime_->view->addItem(fromUtf8(message.id), MarkdownView::Style::Bubble,
                                    fromUtf8(message.text()));
            QStringList images;
            for (const MaiMessagePart& part : message.parts) {
                const auto* image = std::get_if<MaiImagePart>(&part.body);
                if (!image) continue;
                const QString stored = fromUtf8(image->path);
                images.push_back(QFileInfo(stored).isAbsolute()
                                     ? stored
                                     : QDir(fromUtf8(selectedSession.directory)).filePath(stored));
            }
            appendUserImages(images);
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
                card->setDetail(tool->state == MaiToolState::Error ? fromUtf8(tool->error)
                                                                   : fromUtf8(tool->output));
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
    scrollToBottom();
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
                card->setDetail(fromUtf8(tool->output));
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

bool AgentChatPanel::insertComposerMimeData(const QMimeData* mime) {
    if (!mime) return false;
    bool inserted = false;
    if (mime->hasUrls()) {
        for (const QUrl& url : mime->urls()) {
            if (!url.isLocalFile()) continue;
            const QString path = url.toLocalFile();
            if (!QFileInfo(path).isFile() || QImageReader::imageFormat(path).isEmpty()) continue;
            insertComposerImageFile(path);
            inserted = true;
        }
        if (inserted) return true;
    }
    if (mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) {
            insertComposerImage(image);
            return true;
        }
    }
    return false;
}

void AgentChatPanel::insertComposerImage(const QImage& image) {
    const QString directory = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                  .filePath(QStringLiteral("maichat-ai-paste"));
    if (!QDir().mkpath(directory)) return;
    const QString path = QDir(directory).filePath(
        QStringLiteral("paste-%1.png").arg(QDateTime::currentMSecsSinceEpoch()));
    if (!image.save(path, "PNG")) return;
    insertComposerImageFile(path);
}

void AgentChatPanel::insertComposerImageFile(const QString& path) {
    QImageReader reader(path);
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty()) {
        appendNotice(QStringLiteral("图片无法读取：%1").arg(QFileInfo(path).fileName()), true);
        return;
    }
    QTextImageFormat format;
    format.setName(QFileInfo(path).absoluteFilePath());
    int width = size.width();
    int height = size.height();
    constexpr int kMaxWidth = 240;
    if (width > kMaxWidth && width > 0) {
        height = height * kMaxWidth / width;
        width = kMaxWidth;
    }
    format.setWidth(width);
    format.setHeight(height);
    QTextCursor cursor = runtime_->editor->textCursor();
    cursor.insertImage(format);
    runtime_->editor->setTextCursor(cursor);
    runtime_->editor->setFocus();
}

QStringList AgentChatPanel::composerImagePaths() const {
    QStringList paths;
    const QTextDocument* document = runtime_->editor->document();
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) continue;
            const QString path = fragment.charFormat().toImageFormat().name();
            if (QFileInfo(path).isFile() && !paths.contains(path)) paths.push_back(path);
        }
    }
    return paths;
}

void AgentChatPanel::appendUserImages(const QStringList& paths) {
    for (const QString& path : paths) {
        QPixmap pixmap(path);
        if (pixmap.isNull()) continue;
        auto* preview = new QLabel;
        preview->setPixmap(pixmap.scaled(UiZoom::s(280), UiZoom::s(210), Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation));
        preview->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        preview->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        preview->setToolTip(QFileInfo(path).fileName());
        runtime_->view->addWidget(QStringLiteral("local-image-%1").arg(++runtime_->noticeSerial),
                                  preview);
    }
}

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
    QString text = runtime_->editor->toPlainText();
    text.remove(QChar(0xFFFC));
    text = text.trimmed();
    const QStringList imagePaths = composerImagePaths();
    if (text.isEmpty() && imagePaths.isEmpty()) return;
    if (text.isEmpty()) text = QStringLiteral("请查看这些图片。");
    if (!runtime_->modelConfigured) {
        emit modelConfigurationRequested();
        return;
    }
    if (!imagePaths.isEmpty() && runtime_->modelChip->text().compare(
                                         QStringLiteral("glm-5.3"), Qt::CaseInsensitive) == 0) {
        appendNotice(QStringLiteral("glm-5.3 仅支持文本，请先切换到 glm-5.3-flash。"), true);
        return;
    }

    if (!runtime_->controller->sendPrompt(runtime_->sessionId, text, imagePaths)) {
        appendNotice(runtime_->controller->lastError(), true);
        return;
    }
    runtime_->editor->clear();
    appendUserBubble(text);
    appendUserImages(imagePaths);
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
    runtime_->send->setText(QString());
    runtime_->send->setIcon(makeComposerActionIcon(
        running, QColor(QString::fromLatin1(running ? kDanger : "#ffffff"))));
    runtime_->send->setToolTip(running ? QStringLiteral("停止任务") : QStringLiteral("发送消息"));
    runtime_->send->setAccessibleName(runtime_->send->toolTip());
    runtime_->send->setStyleSheet(UiZoom::scaleQss(
        running ? QStringLiteral("QPushButton#agentSendButton{background:#fff5f3;color:%1;"
                                 "border:1px solid #f0c3bd;border-radius:8px;padding:0;}"
                                 "QPushButton#agentSendButton:hover{background:#fee4e2;}")
                      .arg(kDanger)
                : QStringLiteral("QPushButton#agentSendButton{background:%1;color:#ffffff;"
                                 "border:none;border-radius:8px;padding:0;}"
                                 "QPushButton#agentSendButton:hover{background:#095a9f;}"
                                 "QPushButton#agentSendButton:pressed{background:#084d87;}"
                                 "QPushButton#agentSendButton:disabled{background:#b8d3e8;"
                                 "color:#eef6ff;}")
                      .arg(kAccent)));
    runtime_->hint->setEnabled(!running);
    runtime_->modelChip->setEnabled(!running && runtime_->modelConfigured);
    if (running) {
        runtime_->hint->setText(QStringLiteral("正在执行"));
    } else {
        updateApprovalPolicyUi();
    }
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
