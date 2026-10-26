// 文件作用：桌面端创作中心和 Agent 会话；AgentSessionList 负责列表内容、选择状态和滚动行为，本文件给出其实现。
// 主要入口：refresh、setCurrent。
// 直接依赖：agent/AgentController.h、ui/UiZoom.h；实现与这些接口协作。
#include "agent/AgentSessionList.h"

#include <QDir>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include "agent/AgentController.h"
#include "ui/UiZoom.h"

namespace {

// 配色取自 MainWindow.cpp，别在这儿另起一套。
const char* const kInk = "#172033";
const char* const kInkFaint = "#98a2b3";
const char* const kLine = "#e2e8f0";
const char* const kAccent = "#0b67b7";
const char* const kAccentWash = "#edf8ff";

QRect sessionMenuRect(const QRect& itemRect) {
    const QRect row = itemRect.adjusted(UiZoom::s(30), UiZoom::s(2),
                                       -UiZoom::s(7), -UiZoom::s(2));
    return QRect(row.right() - UiZoom::s(32), row.center().y() - UiZoom::s(13),
                 UiZoom::s(27), UiZoom::s(27));
}

QRect deviceCreateRect(const QRect& itemRect) {
    const QRect card = itemRect.adjusted(UiZoom::s(7), UiZoom::s(3),
                                        -UiZoom::s(7), -UiZoom::s(3));
    return QRect(card.right() - UiZoom::s(110), card.center().y() - UiZoom::s(13),
                 UiZoom::s(24), UiZoom::s(27));
}

class SessionItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const bool header = !index.data(Qt::UserRole + 3).toString().isEmpty();
        if (header) {
            const QRect card = option.rect.adjusted(UiZoom::s(7), UiZoom::s(3),
                                                     -UiZoom::s(7), -UiZoom::s(3));
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#eaf1f7")));
            painter->drawRoundedRect(card, UiZoom::s(7), UiZoom::s(7));
            const bool collapsed = index.data(Qt::UserRole + 8).toBool();
            const int arrowX = card.left() + UiZoom::s(14);
            const int centerY = card.center().y();
            painter->setPen(QPen(QColor(QStringLiteral("#587187")), UiZoom::s(2)));
            if (collapsed) {
                painter->drawLine(arrowX - UiZoom::s(2), centerY - UiZoom::s(3),
                                  arrowX + UiZoom::s(2), centerY);
                painter->drawLine(arrowX + UiZoom::s(2), centerY,
                                  arrowX - UiZoom::s(2), centerY + UiZoom::s(3));
            } else {
                painter->drawLine(arrowX - UiZoom::s(3), centerY - UiZoom::s(2),
                                  arrowX, centerY + UiZoom::s(2));
                painter->drawLine(arrowX, centerY + UiZoom::s(2),
                                  arrowX + UiZoom::s(3), centerY - UiZoom::s(2));
            }
            const int count = index.data(Qt::UserRole + 5).toInt();
            const bool remote = index.data(Qt::UserRole + 7).toBool();
            QFont font = option.font;
            if (remote && index.data(Qt::UserRole + 6).toBool()) {
                const QRect add = deviceCreateRect(option.rect);
                painter->setPen(QColor(QStringLiteral("#0b67b7")));
                font.setPixelSize(UiZoom::s(19));
                font.setBold(false);
                painter->setFont(font);
                painter->drawText(add, Qt::AlignCenter, QStringLiteral("+"));
            }
            const int countWidth = UiZoom::s(count >= 10 ? 26 : 22);
            const QRect badge(card.right() - countWidth - UiZoom::s(8),
                              centerY - UiZoom::s(10), countWidth, UiZoom::s(20));
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#dbe7f1")));
            painter->drawRoundedRect(badge, UiZoom::s(10), UiZoom::s(10));
            font.setPixelSize(UiZoom::s(11));
            font.setBold(true);
            painter->setFont(font);
            painter->setPen(QColor(QStringLiteral("#516a82")));
            painter->drawText(badge, Qt::AlignCenter, QString::number(count));
            int textRight = badge.left() - UiZoom::s(8);
            if (remote) {
                const bool online = index.data(Qt::UserRole + 6).toBool();
                const QRect status(textRight - UiZoom::s(38), card.top(),
                                   UiZoom::s(38), card.height());
                painter->setPen(online ? QColor(QStringLiteral("#2b8060")) :
                                         QColor(QStringLiteral("#98a2b3")));
                font.setPixelSize(UiZoom::s(10));
                font.setBold(false);
                painter->setFont(font);
                painter->drawText(status, Qt::AlignVCenter | Qt::AlignRight,
                                  online ? QStringLiteral("在线") : QStringLiteral("离线"));
                textRight = status.left() - UiZoom::s(5);
                if (online) textRight = deviceCreateRect(option.rect).left() - UiZoom::s(5);
            }
            const QRect title(card.left() + UiZoom::s(28), card.top(),
                              textRight - card.left() - UiZoom::s(28), card.height());
            font.setPixelSize(UiZoom::s(12));
            font.setBold(true);
            painter->setFont(font);
            painter->setPen(QColor(QStringLiteral("#344e67")));
            painter->drawText(title, Qt::AlignVCenter | Qt::AlignLeft,
                              painter->fontMetrics().elidedText(
                                  index.data(Qt::UserRole + 4).toString(), Qt::ElideRight,
                                  title.width()));
        } else {
            const bool selected = option.state & QStyle::State_Selected;
            const bool hovered = option.state & QStyle::State_MouseOver;
            const QRect row = option.rect.adjusted(UiZoom::s(30), UiZoom::s(2),
                                                   -UiZoom::s(7), -UiZoom::s(2));
            painter->setPen(Qt::NoPen);
            if (selected || hovered) {
                painter->setBrush(QColor(selected ? QStringLiteral("#dff1fc") :
                                             QStringLiteral("#f0f5f9")));
                painter->drawRoundedRect(row, UiZoom::s(7), UiZoom::s(7));
            }
            painter->setPen(QPen(QColor(QStringLiteral("#dbe5ed")), UiZoom::s(1)));
            const int railX = option.rect.left() + UiZoom::s(22);
            painter->drawLine(railX, option.rect.top(), railX, option.rect.bottom());
            QFont font = option.font;
            font.setPixelSize(UiZoom::s(13));
            font.setBold(selected);
            painter->setFont(font);
            painter->setPen(QColor(selected ? QStringLiteral("#0b466f") :
                                       QStringLiteral("#26384b")));
            const bool hasMenu = !index.data(Qt::UserRole).toString().isEmpty() ||
                                 index.data(Qt::UserRole + 1).toBool();
            const QRect title = row.adjusted(UiZoom::s(11), 0,
                                             -UiZoom::s(hasMenu ? 39 : 9), 0);
            painter->drawText(title, Qt::AlignVCenter | Qt::AlignLeft,
                              painter->fontMetrics().elidedText(index.data().toString(),
                                                                 Qt::ElideRight, title.width()));
            if (hasMenu) {
                const QRect menu = sessionMenuRect(option.rect);
                painter->setPen(QColor(QStringLiteral("#607589")));
                font.setPixelSize(UiZoom::s(18));
                font.setBold(true);
                painter->setFont(font);
                painter->drawText(menu, Qt::AlignCenter, QStringLiteral("⋯"));
            }
        }
        painter->restore();
    }
};

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
    QJsonObject device;
    QList<QJsonObject> sessions;
};

}  // namespace

struct AgentSessionList::Runtime {
    AgentController* controller = nullptr;
    QListWidget* list = nullptr;
    QString current;
    QString currentRemote;
    QJsonArray remoteSessions;
    QJsonArray remoteDevices;
    QSet<QString> collapsedDevices;
};

AgentSessionList::AgentSessionList(AgentController& controller, QWidget* parent)
    : QWidget(parent), runtime_(std::make_unique<Runtime>()) {
    runtime_->controller = &controller;
    setObjectName(QStringLiteral("agentSessionList"));
    setMinimumWidth(UiZoom::s(210));
    setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QWidget#agentSessionList{background:#fafbfd;"
                       "border-right:1px solid %1;}").arg(kLine)));

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
    runtime_->list->setItemDelegate(new SessionItemDelegate(runtime_->list));
    runtime_->list->setMouseTracking(true);
    runtime_->list->viewport()->installEventFilter(this);
    runtime_->list->viewport()->setContextMenuPolicy(Qt::CustomContextMenu);
    runtime_->list->setStyleSheet(QStringLiteral(
        "QListWidget{background:transparent;border:none;outline:none;}"
        "QListWidget::item{background:transparent;}"));
    column->addWidget(runtime_->list, 1);

    connect(newButton, &QPushButton::clicked, this,
            [this] { emit newSessionRequested(); });
    connect(runtime_->list->viewport(), &QWidget::customContextMenuRequested, this,
            [this](const QPoint& position) {
                QListWidgetItem* item = runtime_->list->itemAt(position);
                if (!item) return;
                const QString sessionId = item->data(Qt::UserRole).toString();
                if (!sessionId.isEmpty())
                    showLocalSessionMenu(sessionId,
                        runtime_->list->viewport()->mapToGlobal(position));
                else if (item->data(Qt::UserRole + 1).toBool())
                    showRemoteSessionMenu(item->data(Qt::UserRole + 2).toJsonObject(),
                        runtime_->list->viewport()->mapToGlobal(position));
                else if (item->data(Qt::UserRole + 7).toBool())
                    showRemoteDeviceMenu(item->data(Qt::UserRole + 9).toJsonObject(),
                        runtime_->list->viewport()->mapToGlobal(position));
            });
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

bool AgentSessionList::eventFilter(QObject* watched, QEvent* event) {
    if (watched == runtime_->list->viewport() &&
        (event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseButtonRelease)) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            QListWidgetItem* item = runtime_->list->itemAt(mouse->pos());
            if (item) {
                const QString sessionId = item->data(Qt::UserRole).toString();
                const bool remote = item->data(Qt::UserRole + 1).toBool();
                if ((!sessionId.isEmpty() || remote) &&
                    sessionMenuRect(runtime_->list->visualItemRect(item)).contains(mouse->pos())) {
                    if (event->type() == QEvent::MouseButtonRelease) {
                        const QPoint position = runtime_->list->viewport()->mapToGlobal(mouse->pos());
                        if (remote)
                            showRemoteSessionMenu(item->data(Qt::UserRole + 2).toJsonObject(), position);
                        else
                            showLocalSessionMenu(sessionId, position);
                    }
                    return true;
                }
                if (item->data(Qt::UserRole + 7).toBool() &&
                    item->data(Qt::UserRole + 6).toBool() &&
                    deviceCreateRect(runtime_->list->visualItemRect(item)).contains(mouse->pos())) {
                    if (event->type() == QEvent::MouseButtonRelease)
                        emit remoteCreateRequested(item->data(Qt::UserRole + 9).toJsonObject());
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AgentSessionList::showRemoteSessionMenu(const QJsonObject& session, const QPoint& position) {
    if (!session.value(QStringLiteral("online")).toBool()) return;
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("agentRemoteSessionMenu"));
    menu->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
        "QMenu{background:#ffffff;border:1px solid #e2e8f0;border-radius:8px;padding:4px;}"
        "QMenu::item{color:#b42318;padding:8px 16px;border-radius:5px;}"
        "QMenu::item:selected{background:#fff1f2;}")));
    QAction* remove = menu->addAction(QStringLiteral("删除对话"));
    remove->setObjectName(QStringLiteral("agentRemoteDeleteSessionAction"));
    connect(remove, &QAction::triggered, this,
            [this, session] { emit remoteDeleteRequested(session); });
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(position);
}

void AgentSessionList::showRemoteDeviceMenu(const QJsonObject& device, const QPoint& position) {
    if (!device.value(QStringLiteral("online")).toBool()) return;
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("agentRemoteDeviceMenu"));
    menu->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
        "QMenu{background:#ffffff;border:1px solid #e2e8f0;border-radius:8px;padding:4px;}"
        "QMenu::item{color:#172033;padding:8px 16px;border-radius:5px;}"
        "QMenu::item:selected{background:#edf8ff;color:#0b67b7;}")));
    QAction* create = menu->addAction(QStringLiteral("在此设备新建对话"));
    create->setObjectName(QStringLiteral("agentRemoteCreateSessionAction"));
    connect(create, &QAction::triggered, this,
            [this, device] { emit remoteCreateRequested(device); });
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(position);
}

void AgentSessionList::showLocalSessionMenu(const QString& sessionId,
                                            const QPoint& position) {
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("agentSessionMenu"));
    menu->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
        "QMenu{background:#ffffff;border:1px solid #e2e8f0;border-radius:8px;padding:4px;}"
        "QMenu::item{color:#b42318;padding:8px 16px;border-radius:5px;}"
        "QMenu::item:selected{background:#fff1f2;}")));
    QAction* remove = menu->addAction(QStringLiteral("删除对话"));
    remove->setObjectName(QStringLiteral("agentDeleteSessionAction"));
    connect(remove, &QAction::triggered, this,
            [this, sessionId] { emit deleteSessionRequested(sessionId); });
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(position);
}

void AgentSessionList::refresh() {
    const int scroll = runtime_->list->verticalScrollBar()->value();
    runtime_->list->clear();
    const auto addHeader = [this](const QString& key, const QString& label,
                                  int count, bool remote, bool online = false,
                                  const QJsonObject& device = {}) {
        const bool collapsed = runtime_->collapsedDevices.contains(key);
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 · %2").arg(label).arg(count));
        item->setData(Qt::UserRole + 3, key);
        item->setData(Qt::UserRole + 4, label);
        item->setData(Qt::UserRole + 5, count);
        item->setData(Qt::UserRole + 6, online);
        item->setData(Qt::UserRole + 7, remote);
        item->setData(Qt::UserRole + 8, collapsed);
        item->setData(Qt::UserRole + 9, device);
        if (remote && online) item->setToolTip(QStringLiteral("点 + 可在此设备新建对话"));
        item->setFlags(Qt::ItemIsEnabled);
        item->setSizeHint(QSize(0, UiZoom::s(42)));
        runtime_->list->addItem(item);
    };
    // listSessions 已经按 updated 倒序，最近动过的在最上面——和 IM 那边的会话列表一致。
    const auto localSessions = runtime_->controller->agent().listSessions();
    const QString localGroup = QStringLiteral("local");
    if (!localSessions.empty()) {
#ifdef Q_OS_WIN
        addHeader(localGroup, QStringLiteral("这台 Windows 电脑"), int(localSessions.size()), false);
#else
        addHeader(localGroup, QStringLiteral("这台 Mac"), int(localSessions.size()), false);
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
            item->setSizeHint(QSize(0, UiZoom::s(42)));
            runtime_->list->addItem(item);
            if (sessionId == runtime_->current && runtime_->currentRemote.isEmpty())
                runtime_->list->setCurrentItem(item);
        }
    }
    QList<DeviceGroup> groups;
    QHash<QString, int> groupIndex;
    for (const QJsonValue& value : runtime_->remoteDevices) {
        const QJsonObject device = value.toObject();
        const QString key = deviceGroupKey(device);
        if (device.value(QStringLiteral("device_slot")).toString().isEmpty() ||
            device.value(QStringLiteral("device_id")).toString().isEmpty() ||
            groupIndex.contains(key)) continue;
        groupIndex.insert(key, groups.size());
        groups.append({key, deviceLabel(device), device.value(QStringLiteral("online")).toBool(),
                       device, {}});
    }
    for (const QJsonValue& value : runtime_->remoteSessions) {
        const QJsonObject session = value.toObject();
        const QString id = session.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        const QString key = deviceGroupKey(session);
        if (!groupIndex.contains(key)) {
            groupIndex.insert(key, groups.size());
            groups.append({key, deviceLabel(session), false, session, {}});
        }
        DeviceGroup& group = groups[groupIndex.value(key)];
        group.online |= session.value(QStringLiteral("online")).toBool();
        if (group.device.isEmpty()) group.device = session;
        group.sessions.append(session);
    }
    QHash<QString, int> labelTotals;
    for (const DeviceGroup& group : groups) ++labelTotals[group.label];
    QHash<QString, int> labelNumbers;
    for (const DeviceGroup& group : groups) {
        QString label = group.label;
        if (labelTotals.value(group.label) > 1)
            label += QStringLiteral("（设备 %1）").arg(++labelNumbers[group.label]);
        addHeader(group.key, label, group.sessions.size(), true, group.online, group.device);
        if (runtime_->collapsedDevices.contains(group.key)) continue;
        for (const QJsonObject& session : group.sessions) {
            QString title = session.value(QStringLiteral("title")).toString();
            if (title == QStringLiteral("New session")) title = QStringLiteral("新对话");
            auto* item = new QListWidgetItem(title);
            item->setData(Qt::UserRole + 1, true);
            item->setData(Qt::UserRole + 2, session);
            item->setToolTip(label);
            item->setSizeHint(QSize(0, UiZoom::s(42)));
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

void AgentSessionList::setRemoteDevices(const QJsonArray& devices) {
    if (runtime_->remoteDevices == devices) return;
    runtime_->remoteDevices = devices;
    refresh();
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
