#include "agent/AgentSessionList.h"

#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "agent/AgentController.h"
#include "ui/UiZoom.h"

namespace {

// 配色取自 MainWindow.cpp，别在这儿另起一套。
const char* const kInk = "#172033";
const char* const kInkFaint = "#98a2b3";
const char* const kLine = "#e2e8f0";
const char* const kLineSoft = "#f1f5f9";
const char* const kAccent = "#0b67b7";
const char* const kAccentWash = "#edf8ff";

QString fromUtf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

}  // namespace

struct AgentSessionList::Runtime {
    AgentController* controller = nullptr;
    QListWidget* list = nullptr;
    QString current;
};

AgentSessionList::AgentSessionList(AgentController& controller, QWidget* parent)
    : QWidget(parent), runtime_(std::make_unique<Runtime>()) {
    runtime_->controller = &controller;
    setMinimumWidth(UiZoom::s(210));
    setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QWidget{background:#fafbfd;border-right:1px solid %1;}").arg(kLine)));

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    // ---- 新对话 ----
    auto* head = new QWidget(this);
    head->setStyleSheet(QStringLiteral("QWidget{background:transparent;border:none;}"));
    auto* headRow = new QHBoxLayout(head);
    headRow->setContentsMargins(UiZoom::s(12), UiZoom::s(12), UiZoom::s(12), UiZoom::s(8));
    auto* newButton = new QPushButton(QStringLiteral("＋  新对话"));
    newButton->setCursor(Qt::PointingHandCursor);
    newButton->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QPushButton{background:#ffffff;color:%1;border:1px solid %2;"
                       "border-radius:8px;padding:7px 10px;text-align:left;}"
                       "QPushButton:hover{background:%3;border-color:%4;color:%4;}")
            .arg(kInk, kLine, kAccentWash, kAccent)));
    QFont newFont = newButton->font();
    newFont.setPixelSize(UiZoom::s(13));
    newButton->setFont(newFont);
    headRow->addWidget(newButton);
    column->addWidget(head);

    // ---- 会话 ----
    runtime_->list = new QListWidget(this);
    runtime_->list->setFrameShape(QFrame::NoFrame);
    runtime_->list->setSelectionMode(QAbstractItemView::SingleSelection);
    runtime_->list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    runtime_->list->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QListWidget{background:transparent;border:none;outline:none;}"
                       "QListWidget::item{padding:8px 12px;border-bottom:1px solid %1;}"
                       "QListWidget::item:selected{background:%2;color:%3;}"
                       "QListWidget::item:hover{background:#eef2f7;}")
            .arg(kLineSoft, kAccentWash, kInk)));
    column->addWidget(runtime_->list, 1);

    connect(newButton, &QPushButton::clicked, this,
            [this] { emit newSessionRequested(); });
    connect(runtime_->list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const QString sessionId = item->data(Qt::UserRole).toString();
        if (sessionId.isEmpty() || sessionId == runtime_->current) return;
        runtime_->current = sessionId;
        emit selected(sessionId);
    });
}

AgentSessionList::~AgentSessionList() = default;

void AgentSessionList::refresh() {
    runtime_->list->clear();
    // listSessions 已经按 updated 倒序，最近动过的在最上面——和 IM 那边的会话列表一致。
    for (const MaiSession& session : runtime_->controller->agent().listSessions()) {
        const QString sessionId = fromUtf8(session.id);
        const QString directory = fromUtf8(session.directory);
        // 未命名的会话核心会填一个占位标题，直接显示出来很难看；
        // 第一轮结束后它会被换成用户那句话。
        const QString title =
            session.isUntitled() ? QStringLiteral("新对话") : fromUtf8(session.title);

        auto* item = new QListWidgetItem(title);
        item->setData(Qt::UserRole, sessionId);
        // 工作目录挂在 tooltip 上而不是排在标题下面：一项两行会让列表在几十个会话时
        // 变得很难扫。真要看是哪个目录，停一下就有。
        item->setToolTip(directory);
        QFont font = item->font();
        font.setPixelSize(UiZoom::s(13));
        item->setFont(font);
        runtime_->list->addItem(item);
        if (sessionId == runtime_->current) runtime_->list->setCurrentItem(item);
    }

    if (runtime_->list->count() == 0) {
        auto* empty = new QListWidgetItem(QStringLiteral("还没有对话"));
        empty->setFlags(Qt::NoItemFlags);
        empty->setForeground(QColor(kInkFaint));
        runtime_->list->addItem(empty);
    }
}

void AgentSessionList::setCurrent(const QString& sessionId) {
    runtime_->current = sessionId;
    for (int row = 0; row < runtime_->list->count(); ++row) {
        QListWidgetItem* item = runtime_->list->item(row);
        if (item->data(Qt::UserRole).toString() != sessionId) continue;
        runtime_->list->setCurrentItem(item);
        return;
    }
}
