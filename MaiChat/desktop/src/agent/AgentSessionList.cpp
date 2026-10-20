// 文件作用：桌面端创作中心和 Agent 会话；AgentSessionList 负责列表内容、选择状态和滚动行为，本文件给出其实现。
// 主要入口：refresh、setCurrent。
// 直接依赖：agent/AgentController.h、ui/UiZoom.h；实现与这些接口协作。
#include "agent/AgentSessionList.h"

#include <QDir>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
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

QString remoteKey(const QJsonObject& session) {
    return session.value(QStringLiteral("device_slot")).toString() + QLatin1Char(':') +
           session.value(QStringLiteral("device_id")).toString() + QLatin1Char(':') +
           session.value(QStringLiteral("id")).toString();
}

QString deviceGroupKey(const QJsonObject& session) {
    return QStringLiteral("device:") + session.value(QStringLiteral("device_slot")).toString() +
           QLatin1Char(':') + session.value(QStringLiteral("device_id")).toString();
}

QString slotLabel(const QString& slot) {
    if (slot == QStringLiteral("ios_phone")) return QStringLiteral("iPhone");
    if (slot == QStringLiteral("android_phone")) return QStringLiteral("Android 手机");
    if (slot == QStringLiteral("mac")) return QStringLiteral("Mac");
    if (slot == QStringLiteral("windows")) return QStringLiteral("Windows 电脑");
    if (slot == QStringLiteral("tablet")) return QStringLiteral("平板");
    return QStringLiteral("其他设备");
}

QString deviceLabel(const QJsonObject& session) {
    const QString slot = session.value(QStringLiteral("device_slot")).toString();
    const QString type = slotLabel(slot);
    const QString name = session.value(QStringLiteral("device_name")).toString().trimmed();
    if (name.isEmpty()) return type;
    return name.contains(type, Qt::CaseInsensitive) ? name : type + QStringLiteral(" · ") + name;
}

struct DeviceGroup {
    QString key;
    QString label;
    bool online = false;
    QList<QJsonObject> sessions;
};

}  // namespace

struct AgentSessionList::Runtime {
    AgentController* controller = nullptr;
    QListWidget* list = nullptr;
    QString current;
    QString currentRemote;
    QJsonArray remoteSessions;
    QSet<QString> collapsedDevices;
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
        const QString group = item->data(Qt::UserRole + 3).toString();
        if (!group.isEmpty()) {
            if (runtime_->collapsedDevices.contains(group)) runtime_->collapsedDevices.remove(group);
            else runtime_->collapsedDevices.insert(group);
            refresh();
            return;
        }
        if (item->data(Qt::UserRole + 1).toBool()) {
            const QJsonObject session = item->data(Qt::UserRole + 2).toJsonObject();
            const QString key = remoteKey(session);
            if (key.isEmpty() || key == runtime_->currentRemote) return;
            runtime_->currentRemote = key;
            runtime_->current.clear();
            emit remoteSelected(session);
            return;
        }
        const QString sessionId = item->data(Qt::UserRole).toString();
        if (sessionId.isEmpty() || (sessionId == runtime_->current && runtime_->currentRemote.isEmpty())) return;
        runtime_->current = sessionId;
        runtime_->currentRemote.clear();
        emit selected(sessionId);
    });
}

AgentSessionList::~AgentSessionList() = default;

void AgentSessionList::refresh() {
    const int scroll = runtime_->list->verticalScrollBar()->value();
    runtime_->list->clear();
    const auto addHeader = [this](const QString& key, const QString& label, int count) {
        const bool collapsed = runtime_->collapsedDevices.contains(key);
        auto* item = new QListWidgetItem(
            QStringLiteral("%1  %2  ·  %3").arg(collapsed ? QStringLiteral("›") :
                                                       QStringLiteral("⌄"), label).arg(count));
        item->setData(Qt::UserRole + 3, key);
        item->setFlags(Qt::ItemIsEnabled);
        item->setForeground(QColor(QStringLiteral("#52677d")));
        item->setBackground(QColor(QStringLiteral("#eef3f8")));
        item->setSizeHint(QSize(0, UiZoom::s(34)));
        QFont font = item->font();
        font.setPixelSize(UiZoom::s(12));
        font.setBold(true);
        item->setFont(font);
        runtime_->list->addItem(item);
    };
    // listSessions 已经按 updated 倒序，最近动过的在最上面——和 IM 那边的会话列表一致。
    const auto localSessions = runtime_->controller->agent().listSessions();
    const QString localGroup = QStringLiteral("local");
    if (!localSessions.empty()) {
#ifdef Q_OS_WIN
        addHeader(localGroup, QStringLiteral("这台 Windows 电脑"), int(localSessions.size()));
#else
        addHeader(localGroup, QStringLiteral("这台 Mac"), int(localSessions.size()));
#endif
    }
    if (!runtime_->collapsedDevices.contains(localGroup)) {
        for (const MaiSession& session : localSessions) {
            const QString sessionId = fromUtf8(session.id);
            const QString directory = fromUtf8(session.directory);
            // 未命名的会话核心会填一个占位标题，第一轮结束后换成用户那句话。
            const QString title =
                session.isUntitled() ? QStringLiteral("新对话") : fromUtf8(session.title);

            auto* item = new QListWidgetItem(title);
            item->setData(Qt::UserRole, sessionId);
            // 工作目录留在 tooltip，避免几十条会话都变成两行。
            item->setToolTip(directory);
            QFont font = item->font();
            font.setPixelSize(UiZoom::s(13));
            item->setFont(font);
            runtime_->list->addItem(item);
            if (sessionId == runtime_->current && runtime_->currentRemote.isEmpty())
                runtime_->list->setCurrentItem(item);
        }
    }
    QList<DeviceGroup> groups;
    QHash<QString, int> groupIndex;
    for (const QJsonValue& value : runtime_->remoteSessions) {
        const QJsonObject session = value.toObject();
        const QString id = session.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        const QString key = deviceGroupKey(session);
        if (!groupIndex.contains(key)) {
            groupIndex.insert(key, groups.size());
            groups.append({key, deviceLabel(session), false, {}});
        }
        DeviceGroup& group = groups[groupIndex.value(key)];
        group.online |= session.value(QStringLiteral("online")).toBool();
        group.sessions.append(session);
    }
    QHash<QString, int> labelTotals;
    for (const DeviceGroup& group : groups) ++labelTotals[group.label];
    QHash<QString, int> labelNumbers;
    for (const DeviceGroup& group : groups) {
        QString label = group.label;
        if (labelTotals.value(group.label) > 1)
            label += QStringLiteral("（设备 %1）").arg(++labelNumbers[group.label]);
        label += group.online ? QStringLiteral(" · 在线") : QStringLiteral(" · 离线");
        addHeader(group.key, label, group.sessions.size());
        if (runtime_->collapsedDevices.contains(group.key)) continue;
        for (const QJsonObject& session : group.sessions) {
            QString title = session.value(QStringLiteral("title")).toString();
            if (title == QStringLiteral("New session")) title = QStringLiteral("新对话");
            auto* item = new QListWidgetItem(title);
            item->setData(Qt::UserRole + 1, true);
            item->setData(Qt::UserRole + 2, session);
            item->setToolTip(label);
            QFont font = item->font();
            font.setPixelSize(UiZoom::s(13));
            item->setFont(font);
            runtime_->list->addItem(item);
            if (remoteKey(session) == runtime_->currentRemote) runtime_->list->setCurrentItem(item);
        }
    }

    if (runtime_->list->count() == 0) {
        auto* empty = new QListWidgetItem(QStringLiteral("还没有对话"));
        empty->setFlags(Qt::NoItemFlags);
        empty->setForeground(QColor(kInkFaint));
        runtime_->list->addItem(empty);
    }
    runtime_->list->verticalScrollBar()->setValue(scroll);
}

void AgentSessionList::setRemoteSessions(const QJsonArray& sessions) {
    if (runtime_->remoteSessions == sessions) return;
    runtime_->remoteSessions = sessions;
    refresh();
}

void AgentSessionList::setCurrent(const QString& sessionId) {
    runtime_->current = sessionId;
    runtime_->currentRemote.clear();
    if (!sessionId.isEmpty() && runtime_->collapsedDevices.remove(QStringLiteral("local"))) refresh();
    if (sessionId.isEmpty()) {
        runtime_->list->clearSelection();
        return;
    }
    for (int row = 0; row < runtime_->list->count(); ++row) {
        QListWidgetItem* item = runtime_->list->item(row);
        if (item->data(Qt::UserRole).toString() != sessionId) continue;
        runtime_->list->setCurrentItem(item);
        return;
    }
}

void AgentSessionList::setCurrentRemote(const QJsonObject& session) {
    runtime_->current.clear();
    runtime_->currentRemote = remoteKey(session);
    if (runtime_->collapsedDevices.remove(deviceGroupKey(session))) refresh();
    for (int row = 0; row < runtime_->list->count(); ++row) {
        QListWidgetItem* item = runtime_->list->item(row);
        if (item->data(Qt::UserRole + 1).toBool() &&
            remoteKey(item->data(Qt::UserRole + 2).toJsonObject()) == runtime_->currentRemote) {
            runtime_->list->setCurrentItem(item);
            return;
        }
    }
}
