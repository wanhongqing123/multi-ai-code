// 文件作用：桌面端共用界面；MainWindow 负责窗口呈现、输入处理和状态反馈，本文件给出其实现。
// 主要入口：event、updateWindowChromeBackground、closeEvent、eventFilter。
// 直接依赖：diagnostics/PerformanceLog.h、model/ContactGroups.h、model/MessageNotification.h、model/MessageQuote.h；实现与这些接口协作。
#include "ui/MainWindow.h"
#include "diagnostics/PerformanceLog.h"

#include <QMenu>

#include "model/ContactGroups.h"
#include "model/MessageNotification.h"
#include "model/MessageQuote.h"
#include "model/MessageSearch.h"
#include "ui/MessageImageLoader.h"
#include "ui/VideoCircleDialog.h"
#include "ui/CloudDriveDialog.h"
#include "ui/RemoteDiagnosticsDialog.h"
#if defined(MAICHAT_FFMPEG_IMAGE_DECODE)
#include "MaiImageDecode.h"
#endif
#if defined(MAICHAT_HAS_FFPLAY)
#include "MaiChatTools/DesktopMediaTools.h"
#include "MaiChatTools/DesktopRemoteSessionTools.h"
#include "ui/MaiFfplayVideoDialog.h"
#endif

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QPolygon>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSaveFile>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFormat>
#include <QTextFragment>
#include <QTextList>
#include <QTextOption>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtMath>
#include <algorithm>
#include <functional>
#include <limits>
#include <mutex>
#include <utility>

#include <QtConcurrent/QtConcurrentRun>
#include <curl/curl.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include "im/VideoFileMetadata.h"
#include "markdown/MarkdownDocument.h"
#include "markdown/MarkdownLabel.h"
#include "markdown/MarkdownTheme.h"
#include "markdown/PreviewText.h"
#include "ui/AddContactDialog.h"
#include "ui/AppMessageDialog.h"
#include "ui/AppTextInputDialog.h"
#include "ui/BroadcastDialog.h"
#include "ui/ComposerResizeSplitter.h"
#include "ui/ComposerTextEdit.h"
#include "ui/FilePreviewDialog.h"
#include "ui/ImagePreviewDialog.h"
#include "ui/PdfPreviewDialog.h"
#include "ui/VideoPreviewDialog.h"
#include <QButtonGroup>
#include <QCache>
#include <QCheckBox>
#include <QLoggingCategory>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QSystemTrayIcon>

#include "remote/TrtcEngine.h"
#include "ui/RemoteDesktopConsentDialog.h"
#include "ui/RemoteDesktopProxyDialog.h"
#include "ui/RemoteDesktopSessionCard.h"
#include "ui/RemoteDesktopViewPanel.h"
#include "ui/SharingIndicatorBar.h"
#include "ui/UiZoom.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

#include "MaiChatTools/MaiChatHostTools.h"
#include "MaiChatTools/DesktopCloudDriveTools.h"
#include "MaiSshTool.h"
#include "agent/AgentChatPanel.h"
#include "agent/AgentController.h"
#include "agent/AgentSessionList.h"
#include "agent/DesktopSessionSyncCoordinator.h"
#include "agent/RemoteAgentPanel.h"
#include "agent/ReplySuggestionController.h"

namespace {

constexpr int UserIdRole = Qt::UserRole;
constexpr char kActiveWindowChromeBackground[] = "#eef4f9";
constexpr char kInactiveWindowChromeBackground[] = "#f3f3f3";

void setSolidWindowBackground(QWidget *widget, const char *color) {
  QPalette palette = widget->palette();
  palette.setColor(QPalette::Window, QColor(QString::fromLatin1(color)));
  widget->setPalette(palette);
  widget->setAutoFillBackground(true);
}

// 切换会话时同步渲染的尾部消息条数。屏上可见的也就十几条；其余的排版纯是
// 切换延迟——200 条长 markdown 的会话首切实测排版占 3.8s。更早的消息由
// 「加载更早的消息」分批补渲染，布局缓存让补渲染接近免费。
constexpr int kMessageRenderWindow = 30;
constexpr int DisplayNameRole = Qt::UserRole + 1;
constexpr int PreviewRole = Qt::UserRole + 2;
constexpr int TimeRole = Qt::UserRole + 3;
constexpr int UnreadRole = Qt::UserRole + 4;
constexpr int AvatarUrlRole = Qt::UserRole + 5;
// 顶栏搜索结果项：记住它属于哪个会话、哪条消息，点开才能跳过去。
constexpr int SearchPeerRole = Qt::UserRole + 6;
constexpr int SearchMessageRole = Qt::UserRole + 7;
// 通讯录分组：分组表头和联系人共用一个 QListWidget（而不是换成 QTreeWidget），
// 表头就是一种特殊行。这样既不用重写已有的联系人绘制，也不必再套一层缩进。
constexpr int IsGroupHeaderRole = Qt::UserRole + 8;
constexpr int GroupNameRole = Qt::UserRole + 9;
constexpr int GroupCollapsedRole = Qt::UserRole + 10;
constexpr int GroupCountRole = Qt::UserRole + 11;
// 未分组表头在列表里的 GroupNameRole 用空串——和联系人的 groupName 表示法一致。
// 结果太多时列表本身就没用了，截断并提示收窄关键词。
constexpr int MaxGlobalSearchResults = 60;
// 导航栏与会话栏头部的图标统一用这个尺寸：混用会让一列图标看起来大小不一。
constexpr int kNavIconPixels = 20;
// 桌面端、iOS 和 Android 共用 Tabler 的 24px 视觉网格。
constexpr int kNavRailIconPixels = 24;
constexpr int MessageRowGap = 10;
constexpr int RemoteDesktopStopSendTimeoutMs = 800;

// 分隔线保留在现有行内，避免改变分页边界和消息索引。
void setMessageRowDivider(QWidget *row, bool visible) {
  if (row->property("showMessageDivider").isValid() &&
      row->property("showMessageDivider").toBool() == visible)
    return;
  row->setProperty("showMessageDivider", visible);
  row->layout()->setContentsMargins(0, visible ? UiZoom::s(10) : 0, 0, 0);
  row->style()->unpolish(row);
  row->style()->polish(row);
  row->update();
}

QString formatActivityDuration(qint64 seconds) {
  const qint64 value = qMax<qint64>(0, seconds);
  if (value < 60)
    return QStringLiteral("%1秒").arg(value);
  return QStringLiteral("%1分%2秒").arg(value / 60).arg(value % 60);
}

class RemoteIMActivityBubble final : public QWidget {
public:
  explicit RemoteIMActivityBubble(const RemoteIMActivitySignal &signal,
                                  QWidget *parent = nullptr)
      : QWidget(parent), signal_(signal) {
    setObjectName(QStringLiteral("remoteImActivityBubble"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(sizeHint());
    // 对齐 iOS ProgressView 的节奏：机器状态用十二段渐隐转圈；人工输入仍按
    // 每四帧推进一个圆点，约 360ms 一步。
    ticker_.setInterval(90);
    connect(&ticker_, &QTimer::timeout, this, [this] {
      phase_ = (phase_ + 1) % 24;
      setAccessibleName(statusText());
      update();
    });
    setAccessibleName(statusText());
    ticker_.start();
  }

  QSize sizeHint() const override {
    return signal_.kind == RemoteIMActivityKind::HumanTyping
               ? QSize(UiZoom::s(74), UiZoom::s(38))
               : QSize(UiZoom::s(300), UiZoom::s(38));
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (signal_.kind == RemoteIMActivityKind::HumanTyping) {
      for (int index = 0; index < 3; ++index) {
        QColor dot(QStringLiteral("#0b8fe3"));
        dot.setAlpha(index == (phase_ / 4) % 3 ? 255 : 100);
        painter.setBrush(dot);
        const qreal radius =
            (index == (phase_ / 4) % 3 ? 4.0 : 3.5) * UiZoom::factor();
        const QPointF center(UiZoom::s(25 + index * 13), height() / 2.0);
        painter.drawEllipse(center, radius, radius);
      }
      return;
    }

    const QPointF center(UiZoom::s(23), height() / 2.0);
    const qreal innerRadius = UiZoom::s(4);
    const qreal outerRadius = UiZoom::s(9);
    const int activeSpoke = phase_ % 12;
    for (int index = 0; index < 12; ++index) {
      const int trail = (index - activeSpoke + 12) % 12;
      QColor color(QStringLiteral("#0b8fe3"));
      color.setAlpha(255 - trail * 16);
      painter.setPen(
          QPen(color, 2.0 * UiZoom::factor(), Qt::SolidLine, Qt::RoundCap));
      const qreal angle =
          qDegreesToRadians(static_cast<qreal>(index * 30 - 90));
      const QPointF direction(qCos(angle), qSin(angle));
      painter.drawLine(center + direction * innerRadius,
                       center + direction * outerRadius);
    }

    QFont font = painter.font();
    font.setPixelSize(UiZoom::s(12));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(QColor(QStringLiteral("#0b8fe3")));
    painter.drawText(
        QRectF(UiZoom::s(42), 0, width() - UiZoom::s(50), height()),
        Qt::AlignVCenter | Qt::AlignLeft, statusText());
  }

private:
  QString statusText() const {
    const qint64 startedAt = signal_.startedAtMs > 0
                                 ? signal_.startedAtMs
                                 : QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsedSeconds = qMax<qint64>(
        0, (QDateTime::currentMSecsSinceEpoch() - startedAt) / 1000);
    const qint64 taskStartedAt =
        signal_.taskStartedAtMs > 0 ? signal_.taskStartedAtMs : startedAt;
    const qint64 taskElapsedSeconds =
        qMax(elapsedSeconds,
             (QDateTime::currentMSecsSinceEpoch() - taskStartedAt) / 1000);
    QString title;
    switch (signal_.kind) {
    case RemoteIMActivityKind::MachineThinking:
      title = QStringLiteral("思考中");
      break;
    case RemoteIMActivityKind::MachineTool:
      title = QStringLiteral("正在使用工具");
      break;
    case RemoteIMActivityKind::MachineWaiting:
      title = QStringLiteral("等待确认");
      break;
    case RemoteIMActivityKind::MachineWorking:
      title = QStringLiteral("正在执行");
      break;
    case RemoteIMActivityKind::HumanTyping:
      return QString();
    }
    return QStringLiteral("%1%2，任务总耗时%3")
        .arg(title, formatActivityDuration(elapsedSeconds),
             formatActivityDuration(taskElapsedSeconds));
  }

  RemoteIMActivitySignal signal_;
  QTimer ticker_;
  int phase_ = 0;
};

// IM 的消息正文。**和 AI 助手页是同一套渲染**：都走 MarkdownDocument →
// MarkdownLayout → 自己画，共用一份 MarkdownTheme。
//
// 原来这里是 QTextBrowser：MarkdownRenderer 出一段 HTML，交给 QTextDocument
// 排。 那条路有两个绕不过去的问题——
//
//   外观受限   QTextDocument 只认 CSS 的一个子集：行内 padding、块级圆角、
//              border-left、复选框全都没有。代码块和引用块只能拿表格去模拟。
//   两套皮肤   AI 页换成自绘之后，同一段 markdown 在两边长得不一样，
//              而且改一边另一边不会跟着变。
//
// 这个类现在只负责「IM 特有的那几件事」：右键菜单、回复/转发、复制原始数据、
// 把时间戳接在正文末尾、以及按宽度定高。画字的活全在 MarkdownLabel 里。
class MarkdownMessageView final : public MarkdownLabel {
public:
  explicit MarkdownMessageView(QWidget *parent = nullptr)
      : MarkdownLabel(parent) {
    setObjectName(QStringLiteral("messageMarkdownView"));
    setTheme(MarkdownTheme::standard(UiZoom::factor()));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // 链接点开走系统浏览器，和原来 setOpenExternalLinks(true) 一样。
    connect(this, &MarkdownLabel::linkActivated, this,
            [](const QString &href) { QDesktopServices::openUrl(QUrl(href)); });

    copyOriginalDataAction_ = new QAction(QStringLiteral("复制原始数据"), this);
    copyOriginalDataAction_->setObjectName(
        QStringLiteral("copyOriginalDataAction"));
    connect(copyOriginalDataAction_, &QAction::triggered, this,
            [this]() { QApplication::clipboard()->setText(markdown()); });
  }

  void setMessageMarkdown(const QString &markdown,
                          const QString &timestamp = {}) {
    setProperty("messageTimestamp", timestamp);
    // 时间戳接在正文最后一行的末尾，不另起一行——另起一行的话，
    // 一条「好」这样的单行消息会凭空多出一整行高。
    setTrailingNote(timestamp.isEmpty()
                        ? QString()
                        : QStringLiteral("  \u00b7 ") + timestamp,
                    QColor(QStringLiteral("#0f8ddd")), UiZoom::s(11));
    setMarkdown(markdown);
    updateContentHeight();
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    MarkdownLabel::resizeEvent(event);
    updateContentHeight();
  }

  // 替换原生英文右键菜单（Copy/Copy Link Location/Select All），
  // 换成与图片/文件气泡一致的飞书式中文菜单。定义在辅助函数之后（见文件下方）。
  void contextMenuEvent(QContextMenuEvent *event) override;

public:
  // 「回复」由 MainWindow 注入：这个视图本身不知道自己渲染的是哪条消息，
  // 把消息塞进来会让它依赖整个消息模型，不如只收一个回调。
  void setReplyHandler(std::function<void()> handler) {
    replyHandler_ = std::move(handler);
  }
  void setForwardHandler(std::function<void()> handler) {
    forwardHandler_ = std::move(handler);
  }

private:
  QAction *copyOriginalDataAction_ = nullptr;
  std::function<void()> replyHandler_;
  std::function<void()> forwardHandler_;

  void updateContentHeight() {
    // 按**自己的宽度**量。原来这里有个 120 的下限（从 QTextBrowser
    // 那版抄来的）， 后果是部件还没拿到真实宽度时先按 120
    // 量一次，拿到之后再排一次—— 每条消息白排一次版。MarkdownLabel
    // 自己有下限，这里不用再兜。
    const int wanted = qMax(UiZoom::s(20), heightForWidth(width()));
    if (height() == wanted && minimumHeight() == wanted)
      return;
    setFixedHeight(wanted);
    updateGeometry();
  }
};

// 连接状态点自己画，不靠 QSS 的 border-radius：缩放倍率不是整数时，
// 尺寸和圆角各自四舍五入，半径会小于半边长，圆就塌成圆角方块——
// 用户看到的就是「一个又大又丑的绿方块」。
QPixmap statusDotPixmap(bool connected, int logicalSize, int ringWidth,
                        qreal dpr) {
  const QString key = QStringLiteral("statusdot:%1:%2:%3:%4")
                          .arg(connected)
                          .arg(logicalSize)
                          .arg(ringWidth)
                          .arg(dpr);
  static QHash<QString, QPixmap> cache;
  const auto found = cache.constFind(key);
  if (found != cache.cend())
    return found.value();

  const int physical = qMax(1, qRound(logicalSize * dpr));
  QPixmap pixmap(physical, physical);
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);
  // 状态只在白色的个人资料卡片中显示。
  painter.setBrush(Qt::white);
  painter.drawEllipse(QRectF(0, 0, logicalSize, logicalSize));
  painter.setBrush(connected ? QColor(0x12, 0xb7, 0x6a)
                             : QColor(0x98, 0xa2, 0xb3));
  painter.drawEllipse(QRectF(ringWidth, ringWidth,
                             logicalSize - 2.0 * ringWidth,
                             logicalSize - 2.0 * ringWidth));
  painter.end();

  cache.insert(key, pixmap);
  return pixmap;
}

QPixmap defaultContactAvatar(const QString &userId, int size, qreal dpr) {
  quint32 hash = 2166136261u;
  for (const unsigned char byte : userId.toUtf8())
    hash = (hash ^ byte) * 16777619u;
  const int avatarIndex = static_cast<int>(hash % 26);
  const QString key = QStringLiteral("illustrated:%1:%2:%3")
                          .arg(avatarIndex)
                          .arg(size)
                          .arg(dpr);
  static QHash<QString, QPixmap> cache;
  const auto found = cache.constFind(key);
  if (found != cache.cend())
    return found.value();

  static const QPixmap atlas(
      QStringLiteral(":/maichat/avatars/chibi-atlas-v3.png"));
  const int tile = atlas.width() / 6;
  const QPixmap source = atlas.copy((avatarIndex % 6) * tile,
                                    (avatarIndex / 6) * tile, tile, tile);
  const int physical = qMax(1, qRound(size * dpr));
  QPixmap pixmap(physical, physical);
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  QPainterPath clip;
  clip.addRoundedRect(QRectF(0, 0, size, size), size * 0.22, size * 0.22);
  painter.setClipPath(clip);
  painter.drawPixmap(QRectF(0, 0, size, size), source, QRectF(source.rect()));
  cache.insert(key, pixmap);
  return pixmap;
}

QHash<QString, QPixmap> &avatarPixmapCache() {
  static QHash<QString, QPixmap> cache;
  return cache;
}

QHash<QString, QList<QPointer<QWidget>>> &avatarRepaintWaiters() {
  static QHash<QString, QList<QPointer<QWidget>>> waiters;
  return waiters;
}

QSet<QString> &pendingAvatarUrls() {
  static QSet<QString> urls;
  return urls;
}

QSet<QString> &failedAvatarUrls() {
  static QSet<QString> urls;
  return urls;
}

QString avatarCachePath(const QString &url) {
  const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  if (root.isEmpty()) return {};
  const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha256).toHex();
  return QDir(root).filePath(QStringLiteral("avatars/") + QString::fromLatin1(hash) +
                             QStringLiteral(".png"));
}

bool restoreAvatarPixmap(const QString &url) {
  const QString path = avatarCachePath(url);
  if (path.isEmpty()) return false;
  const QFileInfo file(path);
  if (!file.isFile() || file.size() <= 0 || file.size() > 4 * 1024 * 1024) return false;
  QPixmap source;
  if (!source.load(path, "PNG") || source.isNull() ||
      source.width() > 320 || source.height() > 320) return false;
  avatarPixmapCache().insert(url, source);
  return true;
}

void persistAvatarPixmap(const QString &url, const QPixmap &source) {
  const QString path = avatarCachePath(url);
  if (path.isEmpty() || source.isNull() ||
      !QDir().mkpath(QFileInfo(path).absolutePath())) return;
  QSaveFile output(path);
  if (!output.open(QIODevice::WriteOnly)) return;
  if (!source.save(&output, "PNG") || output.size() > 4 * 1024 * 1024) {
    output.cancelWriting();
    return;
  }
  output.commit();
}

void requestAvatarPixmap(const QString &avatarUrl, QWidget *repaintTarget);

void finishAvatarPixmap(const QString &url, const QByteArray &data) {
  QPixmap source;
  if (data.size() <= 4 * 1024 * 1024) {
    source.loadFromData(data);
    if (!source.isNull() && (source.width() > 320 || source.height() > 320))
      source = source.scaled(320, 320, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
  }
  pendingAvatarUrls().remove(url);
  if (source.isNull()) {
    failedAvatarUrls().insert(url);
  } else {
    avatarPixmapCache().insert(url, source);
    persistAvatarPixmap(url, source);
  }
  const QList<QPointer<QWidget>> targets = avatarRepaintWaiters().take(url);
  for (const QPointer<QWidget> &target : targets) {
    if (!target.isNull()) target->update();
  }
  if (source.isNull()) {
    QTimer::singleShot(30'000, qApp, [url, targets] {
      failedAvatarUrls().remove(url);
      for (const QPointer<QWidget> &target : targets)
        if (!target.isNull()) requestAvatarPixmap(url, target.data());
    });
  }
}

size_t receiveSignedAvatar(char *bytes, size_t size, size_t count, void *context) {
  auto *result = static_cast<QByteArray *>(context);
  if (size != 0 && count > SIZE_MAX / size) return 0;
  const size_t length = size * count;
  if (length > size_t(4 * 1024 * 1024 - result->size())) return 0;
  result->append(bytes, int(length));
  return length;
}

QByteArray downloadAvatar(const QUrl &url, const QString &proxyUrl) {
  const QString scheme = url.scheme().toLower();
  if (!url.isValid() || !url.userInfo().isEmpty() ||
      (scheme != QStringLiteral("https") && scheme != QStringLiteral("http"))) return {};
  static std::once_flag initialization;
  static CURLcode initializationCode = CURLE_FAILED_INIT;
  std::call_once(initialization, [] { initializationCode = curl_global_init(CURL_GLOBAL_DEFAULT); });
  if (initializationCode != CURLE_OK) return {};
  CURL *curl = curl_easy_init();
  if (!curl) return {};
  QByteArray data;
  const QByteArray encoded = url.toString(QUrl::FullyEncoded).toUtf8();
  const QByteArray encodedProxy = proxyUrl.toUtf8();
  curl_easy_setopt(curl, CURLOPT_URL, encoded.constData());
  const char *protocols = scheme == QStringLiteral("https") ? "https" : "http,https";
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protocols);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_IGNORED);
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, curl_off_t(4 * 1024 * 1024));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveSignedAvatar);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
#ifdef Q_OS_WIN
  curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_NATIVE_CA));
#elif defined(Q_OS_MACOS)
  QFile certificates(QStringLiteral(":/maichat/auth/cacert.pem"));
  if (!certificates.open(QIODevice::ReadOnly)) {
    curl_easy_cleanup(curl);
    return {};
  }
  const QByteArray caBundle = certificates.readAll();
  if (caBundle.isEmpty()) {
    curl_easy_cleanup(curl);
    return {};
  }
  curl_blob caBlob = {const_cast<char *>(caBundle.constData()),
                      size_t(caBundle.size()), CURL_BLOB_COPY};
  curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &caBlob);
#endif
  if (!encodedProxy.isEmpty()) curl_easy_setopt(curl, CURLOPT_PROXY, encodedProxy.constData());
  if (url.host() == QStringLiteral("127.0.0.1") || url.host() == QStringLiteral("localhost"))
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
  const CURLcode transferred = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  if (transferred != CURLE_OK || status != 200)
    qWarning().noquote() << QStringLiteral("[im] avatar fetch failed: host=%1 status=%2 curl=%3")
                              .arg(url.host()).arg(status).arg(int(transferred));
  return transferred == CURLE_OK && status == 200 ? data : QByteArray{};
}

// 作用：异步读取头像并在目标控件仍有效时请求重绘。
void requestAvatarPixmap(const QString &avatarUrl, QWidget *repaintTarget) {
  const QString url = avatarUrl.trimmed();
  if (url.isEmpty() || avatarPixmapCache().contains(url)) return;
  if (restoreAvatarPixmap(url)) {
    if (repaintTarget) repaintTarget->update();
    return;
  }
  if (failedAvatarUrls().contains(url)) return;
  if (!repaintTarget ||
      !repaintTarget->property("avatarRequestsEnabled").toBool())
    return;

  if (repaintTarget) {
    QList<QPointer<QWidget>> &waiters = avatarRepaintWaiters()[url];
    const auto alreadyWaiting =
        std::any_of(waiters.cbegin(), waiters.cend(),
                    [repaintTarget](const QPointer<QWidget> &item) {
                      return item.data() == repaintTarget;
                    });
    if (!alreadyWaiting)
      waiters.append(QPointer<QWidget>(repaintTarget));
  }
  if (pendingAvatarUrls().contains(url))
    return;
  pendingAvatarUrls().insert(url);

  const QUrl avatarLocation(url);
  QString proxyUrl;
  const auto proxies = QNetworkProxyFactory::systemProxyForQuery(
      QNetworkProxyQuery(avatarLocation));
  if (!proxies.isEmpty() && proxies.first().type() == QNetworkProxy::HttpProxy) {
    QUrl proxy;
    proxy.setScheme(QStringLiteral("http"));
    proxy.setHost(proxies.first().hostName());
    proxy.setPort(proxies.first().port());
    proxyUrl = proxy.toString(QUrl::FullyEncoded);
  }
  QPointer<QApplication> application(qApp);
  QtConcurrent::run([avatarLocation, application, url, proxyUrl] {
    const QByteArray data = downloadAvatar(avatarLocation, proxyUrl);
    if (!application) return;
    QMetaObject::invokeMethod(application.data(), [url, data] {
      finishAvatarPixmap(url, data);
    }, Qt::QueuedConnection);
  });
}

void drawAvatarPixmap(QPainter *painter, const QRectF &target,
                      const QPixmap &source, qreal radius) {
  if (!painter || source.isNull())
    return;
  // 高分屏（DPR>1）下必须按物理分辨率缩放并声明 devicePixelRatio：
  // 按逻辑尺寸缩放的位图会被绘制层再放大一次，头像整体发虚。
  // 文字与 QSS 是矢量渲染不受影响，位图需要自己处理。
  const qreal dpr =
      painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
  QPixmap scaled =
      source.scaled((target.size() * dpr).toSize(),
                    Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
  scaled.setDevicePixelRatio(dpr);
  const QSizeF logicalSize = QSizeF(scaled.size()) / dpr;
  const QPointF topLeft(target.center().x() - logicalSize.width() / 2.0,
                        target.center().y() - logicalSize.height() / 2.0);
  QPainterPath clip;
  clip.addRoundedRect(target, radius, radius);
  painter->save();
  painter->setClipPath(clip);
  painter->drawPixmap(topLeft, scaled);
  painter->restore();
}

bool drawRemoteAvatar(QPainter *painter, const QRectF &target,
                      const QString &avatarUrl, qreal radius,
                      QWidget *repaintTarget) {
  const QString url = avatarUrl.trimmed();
  const auto cached = avatarPixmapCache().constFind(url);
  if (cached != avatarPixmapCache().cend()) {
    drawAvatarPixmap(painter, target, cached.value(), radius);
    return true;
  }
  requestAvatarPixmap(url, repaintTarget);
  return false;
}

class AccountAvatarLabel final : public QLabel {
public:
  using QLabel::QLabel;

  void setProfile(const RemoteIMContact &profile) {
    profile_ = profile;
    setPixmap(
        defaultContactAvatar(profile.userId, width(), devicePixelRatioF()));
    setAccessibleName(profile.displayName);
    update();
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    if (!profile_.avatarUrl.isEmpty()) {
      QPainter painter(this);
      if (drawRemoteAvatar(&painter, rect(), profile_.avatarUrl, UiZoom::s(10),
                           this))
        return;
    }
    QLabel::paintEvent(event);
  }

private:
  RemoteIMContact profile_;
};

class ConversationListDelegate final : public QStyledItemDelegate {
public:
  explicit ConversationListDelegate(QObject *parent = nullptr)
      : QStyledItemDelegate(parent) {}

  QSize sizeHint(const QStyleOptionViewItem &,
                 const QModelIndex &) const override {
    return QSize(0, UiZoom::s(72));
  }

  // 会话行按实际视口宽度绘制选中背景、头像与预览；行内边距不能叠加 Qt 的滚动条留白。
  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    // 上下各留 2px 是行间距；右侧不再内缩——那 6px 会在选中行右边留出一条
    // 空白，看起来像滚动条的位置一直空着。滚动条真的出现时 Qt 已经把它从视口
    // 里扣掉了，这里再缩一次只会让留白更宽。行内文字有自己的右边距，不靠它。
    // 另外这个 6 是裸值，不跟 UiZoom 缩放，放大后留白比例还会失真。
    const QRect rowRect = option.rect.adjusted(0, 2, 0, -2);
    if (option.state & QStyle::State_Selected) {
      painter->setPen(Qt::NoPen);
      painter->setBrush(QColor(QStringLiteral("#dff3ff")));
      painter->drawRoundedRect(rowRect, 0, 0);
    }

    const QString userId = index.data(UserIdRole).toString();
    const QString name = index.data(DisplayNameRole).toString();
    const QString avatarUrl = index.data(AvatarUrlRole).toString();
    const QString preview = index.data(PreviewRole).toString();
    const QString time = index.data(TimeRole).toString();

    const QRect avatarRect(rowRect.left() + UiZoom::s(12),
                           rowRect.top() + UiZoom::s(10), UiZoom::s(40),
                           UiZoom::s(40));
    if (!drawRemoteAvatar(painter, avatarRect, avatarUrl, UiZoom::s(8),
                          const_cast<QWidget *>(option.widget))) {
      const qreal dpr =
          option.widget ? option.widget->devicePixelRatioF() : 1.0;
      painter->drawPixmap(
          avatarRect.topLeft(),
          defaultContactAvatar(userId, avatarRect.width(), dpr));
    }

    const int textLeft = avatarRect.right() + UiZoom::s(14);
  // 时间列按真实文字宽度测量；短的 HH:mm 为名称让位，长日期也不会被裁切。
    QFont timeFont = option.font;
    timeFont.setPixelSize(UiZoom::s(12));
    const int timeWidth =
        time.isEmpty() ? 0 : QFontMetrics(timeFont).horizontalAdvance(time) + 2;
    const QRect timeRect(rowRect.right() - timeWidth - UiZoom::s(10),
                         rowRect.top() + UiZoom::s(12), timeWidth,
                         UiZoom::s(18));
    const int nameRight = time.isEmpty() ? rowRect.right() - UiZoom::s(12)
                                         : timeRect.left() - UiZoom::s(10);
    const QRect nameRect(textLeft, rowRect.top() + UiZoom::s(12),
                         qMax(0, nameRight - textLeft), UiZoom::s(20));
    const QRect previewRect(textLeft, rowRect.top() + UiZoom::s(41),
                            rowRect.right() - textLeft - UiZoom::s(12),
                            UiZoom::s(18));

    QFont nameFont = option.font;
    nameFont.setPixelSize(UiZoom::s(14));
    nameFont.setWeight(QFont::Medium);
    painter->setFont(nameFont);
    painter->setPen(QColor(QStringLiteral("#1f2329")));
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(nameFont).elidedText(name, Qt::ElideRight,
                                                        nameRect.width()));

    painter->setFont(timeFont);
    painter->setPen(QColor(QStringLiteral("#98a2b3")));
    painter->drawText(timeRect, Qt::AlignRight | Qt::AlignVCenter, time);

    // 未读红点（钉钉/飞书风格）：预览行右侧画红色圆角计数徽标，99+ 封顶；
    // 打开会话即清零（ChatState::selectPeer），徽标随之消失。
    QRect clippedPreviewRect = previewRect;
    const int unread = index.data(UnreadRole).toInt();
    if (unread > 0) {
      const QString badgeText =
          unread > 99 ? QStringLiteral("99+") : QString::number(unread);
      QFont badgeFont = option.font;
      badgeFont.setPixelSize(UiZoom::s(11));
      badgeFont.setBold(true);
      const int badgeHeight = UiZoom::s(18);
      const int badgeWidth = qMax(
          badgeHeight,
          QFontMetrics(badgeFont).horizontalAdvance(badgeText) + UiZoom::s(10));
      const QRect badgeRect(rowRect.right() - badgeWidth - UiZoom::s(10),
                            previewRect.top(), badgeWidth, badgeHeight);
      painter->setPen(Qt::NoPen);
      painter->setBrush(QColor(QStringLiteral("#f53f3f")));
      painter->drawRoundedRect(badgeRect, badgeHeight / 2.0, badgeHeight / 2.0);
      painter->setFont(badgeFont);
      painter->setPen(Qt::white);
      painter->drawText(badgeRect, Qt::AlignCenter, badgeText);
      clippedPreviewRect.setRight(badgeRect.left() - UiZoom::s(8));
    }

    QFont previewFont = option.font;
    previewFont.setPixelSize(UiZoom::s(13));
    painter->setFont(previewFont);
    painter->setPen(QColor(QStringLiteral("#667085")));
    painter->drawText(
        clippedPreviewRect, Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetrics(previewFont)
            .elidedText(preview, Qt::ElideRight, clippedPreviewRect.width()));

    painter->restore();
  }
};

// 通讯录分组表头的行高。比联系人行矮一截：它是分隔物，不是内容，
// 做成同高会让一列看起来全是等价的条目，反而看不出层次。
constexpr int kContactGroupHeaderHeight = 30;

class ContactListDelegate final : public QStyledItemDelegate {
public:
  explicit ContactListDelegate(QObject *parent = nullptr)
      : QStyledItemDelegate(parent) {}

  QSize sizeHint(const QStyleOptionViewItem &,
                 const QModelIndex &index) const override {
    if (index.data(IsGroupHeaderRole).toBool()) {
      return QSize(0, UiZoom::s(kContactGroupHeaderHeight));
    }
    return QSize(0, UiZoom::s(54));
  }

  // 联系人行与分组标题共用委托，先按角色分流，避免把不可选表头绘成普通联系人。
  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    if (index.data(IsGroupHeaderRole).toBool()) {
      paintGroupHeader(painter, option, index);
      return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    // 上下各留 2px 是行间距；右侧不再内缩——那 6px 会在选中行右边留出一条
    // 空白，看起来像滚动条的位置一直空着。滚动条真的出现时 Qt 已经把它从视口
    // 里扣掉了，这里再缩一次只会让留白更宽。行内文字有自己的右边距，不靠它。
    // 另外这个 6 是裸值，不跟 UiZoom 缩放，放大后留白比例还会失真。
    const QRect rowRect = option.rect.adjusted(0, 2, 0, -2);
    if (option.state & QStyle::State_Selected) {
      painter->setPen(Qt::NoPen);
      painter->setBrush(QColor(QStringLiteral("#dff3ff")));
      painter->drawRoundedRect(rowRect, 0, 0);
    }

    const QString userId = index.data(UserIdRole).toString();
    const QString name = index.data(DisplayNameRole).toString();
    const QString avatarUrl = index.data(AvatarUrlRole).toString();
    // 属于某个分组的人往里缩一格；没有分组的人留在分组标题那一层。
    // 不这么做的话，排在最后一个分组后面的散人看起来就像那个分组的成员——
    // 唯一能纠正这个误会的只有标题右边那个人数，代价太大了。
    const int indent =
        index.data(GroupNameRole).toString().isEmpty() ? 0 : UiZoom::s(14);
    const QRect avatarRect(rowRect.left() + UiZoom::s(12) + indent,
                           rowRect.top() + UiZoom::s(7), UiZoom::s(36),
                           UiZoom::s(36));
    if (!drawRemoteAvatar(painter, avatarRect, avatarUrl, UiZoom::s(8),
                          const_cast<QWidget *>(option.widget))) {
      const qreal dpr =
          option.widget ? option.widget->devicePixelRatioF() : 1.0;
      painter->drawPixmap(
          avatarRect.topLeft(),
          defaultContactAvatar(userId, avatarRect.width(), dpr));
    }

    const int textLeft = avatarRect.right() + UiZoom::s(14);
    const QRect nameRect(textLeft, rowRect.top(),
                         rowRect.right() - textLeft - UiZoom::s(12),
                         rowRect.height());

    QFont nameFont = option.font;
    nameFont.setPixelSize(UiZoom::s(14));
    nameFont.setWeight(QFont::Medium);
    painter->setFont(nameFont);
    painter->setPen(QColor(QStringLiteral("#1f2329")));
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(nameFont).elidedText(name, Qt::ElideRight,
                                                        nameRect.width()));

    painter->restore();
  }

private:
  // 分组标题只承担导航和分隔；悬停可以提示位置，但不显示选中状态。
  void paintGroupHeader(QPainter *painter, const QStyleOptionViewItem &option,
                        const QModelIndex &index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRect rowRect = option.rect.adjusted(0, 0, -6, 0);
    // 表头不画选中底色：它不是可选中的行，画了会让人以为自己"选中"了一个分组。
    if (option.state & QStyle::State_MouseOver) {
      painter->setPen(Qt::NoPen);
      painter->setBrush(QColor(QStringLiteral("#f2f4f7")));
      painter->drawRoundedRect(rowRect, UiZoom::s(4), UiZoom::s(4));
    }

    const bool collapsed = index.data(GroupCollapsedRole).toBool();
    // 三角形箭头：展开朝下、折叠朝右，和绝大多数树形控件的习惯一致。
    const int arrowSize = UiZoom::s(8);
    const QPointF center(rowRect.left() + UiZoom::s(14),
                         rowRect.center().y() + 0.5);
    QPolygonF arrow;
    if (collapsed) {
      arrow << QPointF(center.x() - arrowSize / 3.0,
                       center.y() - arrowSize / 2.0)
            << QPointF(center.x() - arrowSize / 3.0,
                       center.y() + arrowSize / 2.0)
            << QPointF(center.x() + arrowSize * 2 / 3.0, center.y());
    } else {
      arrow << QPointF(center.x() - arrowSize / 2.0,
                       center.y() - arrowSize / 3.0)
            << QPointF(center.x() + arrowSize / 2.0,
                       center.y() - arrowSize / 3.0)
            << QPointF(center.x(), center.y() + arrowSize * 2 / 3.0);
    }
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(QStringLiteral("#98a2b3")));
    painter->drawPolygon(arrow);

    QFont titleFont = option.font;
    titleFont.setPixelSize(UiZoom::s(12));
    titleFont.setWeight(QFont::Medium);
    painter->setFont(titleFont);
    painter->setPen(QColor(QStringLiteral("#667085")));

    const QString title = index.data(DisplayNameRole).toString();
    const QString count = QString::number(index.data(GroupCountRole).toInt());
    const QFontMetrics metrics(titleFont);
    const int countWidth = metrics.horizontalAdvance(count);
    const int textLeft = rowRect.left() + UiZoom::s(26);
    const int textRight =
        rowRect.right() - UiZoom::s(12) - countWidth - UiZoom::s(8);
    const QRect titleRect(textLeft, rowRect.top(),
                          qMax(0, textRight - textLeft), rowRect.height());
    painter->drawText(
        titleRect, Qt::AlignLeft | Qt::AlignVCenter,
        metrics.elidedText(title, Qt::ElideRight, titleRect.width()));
    // 人数右对齐。折叠之后这是唯一还能看出组里有没有人的线索。
    const QRect countRect(textRight, rowRect.top(), countWidth + UiZoom::s(8),
                          rowRect.height());
    painter->setPen(QColor(QStringLiteral("#98a2b3")));
    painter->drawText(countRect, Qt::AlignRight | Qt::AlignVCenter, count);

    painter->restore();
  }
};

class ClickableImageLabel final : public QLabel {
public:
  explicit ClickableImageLabel(QString imagePath,
                               std::function<void(const QString &, QWidget *)> onClick,
                               QWidget *parent = nullptr)
      : QLabel(parent), imagePath_(std::move(imagePath)),
        onClick_(std::move(onClick)) {
    setCursor(Qt::PointingHandCursor);
  }

protected:
  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && onClick_) {
      onClick_(imagePath_, this);
      event->accept();
      return;
    }
    QLabel::mouseReleaseEvent(event);
  }

private:
  QString imagePath_;
  std::function<void(const QString &, QWidget *)> onClick_;
};

enum class LineIconKind {
  Messages = 1,
  Contacts,
  Settings,
  Search,
  Add,
  More,
  Copy,
  Preview,
  Download,
  Link,
  SelectAll,
  Trash,
  Forward,
  Screen,           // 远程桌面：显示器轮廓 + 底座
  ScreenConnecting, // 连接中：显示器内三点
  ScreenDisconnect, // 已连接，点击断开：显示器内叉
  Send,
  Assistant, // 创作中心：带播放符号的场记板，强调图片和视频创作。
};

int lineIconKindValue(LineIconKind kind) { return static_cast<int>(kind); }

LineIconKind lineIconKindFromValue(int value) {
  switch (static_cast<LineIconKind>(value)) {
  case LineIconKind::Messages:
  case LineIconKind::Contacts:
  case LineIconKind::Settings:
  case LineIconKind::Search:
  case LineIconKind::Add:
  case LineIconKind::More:
  case LineIconKind::Copy:
  case LineIconKind::Preview:
  case LineIconKind::Download:
  case LineIconKind::Link:
  case LineIconKind::SelectAll:
  case LineIconKind::Trash:
  case LineIconKind::Forward:
  case LineIconKind::Screen:
  case LineIconKind::ScreenConnecting:
  case LineIconKind::ScreenDisconnect:
  case LineIconKind::Send:
  case LineIconKind::Assistant:
    return static_cast<LineIconKind>(value);
  }
  return LineIconKind::Messages;
}

QIcon makeLineIcon(LineIconKind kind, const QColor &color) {
  constexpr int kRender = 48;
  QPixmap pixmap(kRender, kRender);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(color, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);

  switch (kind) {
  case LineIconKind::Messages:
    painter.drawRoundedRect(QRectF(10, 12, 28, 22), 6, 6);
    painter.drawLine(QPointF(17, 20), QPointF(31, 20));
    painter.drawLine(QPointF(17, 27), QPointF(27, 27));
    painter.drawLine(QPointF(18, 34), QPointF(14, 40));
    break;
  case LineIconKind::Contacts:
    painter.drawEllipse(QRectF(18, 10, 12, 12));
    painter.drawArc(QRectF(13, 21, 22, 18), 28 * 16, 124 * 16);
    painter.drawEllipse(QRectF(30, 15, 8, 8));
    painter.drawArc(QRectF(27, 24, 16, 13), 20 * 16, 105 * 16);
    break;
  case LineIconKind::Settings:
    painter.drawEllipse(QRectF(17, 17, 14, 14));
    painter.drawLine(QPointF(24, 7), QPointF(24, 12));
    painter.drawLine(QPointF(24, 36), QPointF(24, 41));
    painter.drawLine(QPointF(7, 24), QPointF(12, 24));
    painter.drawLine(QPointF(36, 24), QPointF(41, 24));
    painter.drawLine(QPointF(12, 12), QPointF(16, 16));
    painter.drawLine(QPointF(32, 32), QPointF(36, 36));
    painter.drawLine(QPointF(36, 12), QPointF(32, 16));
    painter.drawLine(QPointF(16, 32), QPointF(12, 36));
    break;
  case LineIconKind::Search:
    painter.drawEllipse(QRectF(11, 11, 20, 20));
    painter.drawLine(QPointF(29, 29), QPointF(38, 38));
    break;
  case LineIconKind::Screen:
    // 显示器：圆角屏幕 + 底座横杠，16px 下轮廓依然清晰。
    painter.drawRoundedRect(QRectF(9, 12, 30, 21), 3, 3);
    painter.drawLine(QPointF(24, 33), QPointF(24, 37));
    painter.drawLine(QPointF(17, 38), QPointF(31, 38));
    break;
  case LineIconKind::ScreenConnecting:
    // 同一台显示器 + 屏内三点：与就绪态形状一致，只有内部符号变化，
    // 避免图标整体跳动造成"换了个按钮"的错觉。
    painter.drawRoundedRect(QRectF(9, 12, 30, 21), 3, 3);
    painter.drawLine(QPointF(24, 33), QPointF(24, 37));
    painter.drawLine(QPointF(17, 38), QPointF(31, 38));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(16, 20, 5, 5));
    painter.drawEllipse(QRectF(21.5, 20, 5, 5));
    painter.drawEllipse(QRectF(27, 20, 5, 5));
    break;
  case LineIconKind::ScreenDisconnect:
    // 屏内打叉 = 点击断开。
    painter.drawRoundedRect(QRectF(9, 12, 30, 21), 3, 3);
    painter.drawLine(QPointF(24, 33), QPointF(24, 37));
    painter.drawLine(QPointF(17, 38), QPointF(31, 38));
    painter.drawLine(QPointF(18, 17), QPointF(30, 28));
    painter.drawLine(QPointF(30, 17), QPointF(18, 28));
    break;
  case LineIconKind::Add:
    painter.drawLine(QPointF(24, 13), QPointF(24, 35));
    painter.drawLine(QPointF(13, 24), QPointF(35, 24));
    break;
  case LineIconKind::More:
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(13, 21, 6, 6));
    painter.drawEllipse(QRectF(21, 21, 6, 6));
    painter.drawEllipse(QRectF(29, 21, 6, 6));
    break;
  case LineIconKind::Copy:
    // 两张叠纸：先画后层，再用白底前层盖住重叠区（16px 下观感干净）。
    painter.drawRoundedRect(QRectF(16, 9, 22, 22), 5, 5);
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(QRectF(10, 16, 22, 22), 5, 5);
    break;
  case LineIconKind::Preview: {
    // 眼睛：上下两条弧线 + 瞳孔。
    QPainterPath eye;
    eye.moveTo(8, 24);
    eye.quadTo(24, 8, 40, 24);
    eye.moveTo(8, 24);
    eye.quadTo(24, 40, 40, 24);
    painter.drawPath(eye);
    painter.drawEllipse(QRectF(19, 19, 10, 10));
    break;
  }
  case LineIconKind::Download:
    // 下载：箭头落进托盘。
    painter.drawLine(QPointF(24, 9), QPointF(24, 27));
    painter.drawLine(QPointF(16, 20), QPointF(24, 28));
    painter.drawLine(QPointF(32, 20), QPointF(24, 28));
    painter.drawPolyline(QPolygonF(
        {QPointF(9, 30), QPointF(9, 37), QPointF(39, 37), QPointF(39, 30)}));
    break;
  case LineIconKind::Link:
    // 链接：斜向两节链环。
    painter.save();
    painter.translate(24, 24);
    painter.rotate(-45);
    painter.drawRoundedRect(QRectF(-17, -6, 19, 12), 6, 6);
    painter.drawRoundedRect(QRectF(-2, -6, 19, 12), 6, 6);
    painter.restore();
    break;
  case LineIconKind::SelectAll: {
    // 全选：虚线选择框。
    QPen dashPen(color, 4, Qt::CustomDashLine, Qt::RoundCap, Qt::RoundJoin);
    dashPen.setDashPattern({2.4, 2.4});
    painter.setPen(dashPen);
    painter.drawRoundedRect(QRectF(11, 11, 26, 26), 6, 6);
    break;
  }
  case LineIconKind::Trash:
    // 垃圾桶：提手 + 顶沿 + 桶身 + 两道内槽。
    painter.drawPolyline(QPolygonF(
        {QPointF(18, 12), QPointF(18, 8), QPointF(30, 8), QPointF(30, 12)}));
    painter.drawLine(QPointF(9, 13), QPointF(39, 13));
    painter.drawRoundedRect(QRectF(14, 13, 20, 26), 3, 3);
    painter.drawLine(QPointF(21, 20), QPointF(21, 33));
    painter.drawLine(QPointF(27, 20), QPointF(27, 33));
    break;
  case LineIconKind::Forward:
    painter.drawPolyline(QPolygonF({
        QPointF(27, 10),
        QPointF(40, 22),
        QPointF(27, 34),
    }));
    painter.drawLine(QPointF(39, 22), QPointF(20, 22));
    {
      QPainterPath bend;
      bend.moveTo(QPointF(20, 22));
      bend.cubicTo(QPointF(12, 22), QPointF(9, 28), QPointF(8, 38));
      painter.drawPath(bend);
    }
    break;
  case LineIconKind::Assistant: {
    // 图标资源缺失时仍绘制同一场记板轮廓，避免退回旧的星光符号。
    painter.drawRoundedRect(QRectF(8, 17, 32, 24), 4, 4);
    QPainterPath clapper;
    clapper.moveTo(8, 17);
    clapper.lineTo(7, 13);
    clapper.quadTo(6, 9, 10, 8);
    clapper.lineTo(35, 3);
    clapper.quadTo(39, 2, 40, 6);
    clapper.lineTo(41, 10);
    clapper.closeSubpath();
    painter.drawPath(clapper);
    painter.drawLine(QPointF(16, 7), QPointF(22, 15));
    painter.drawLine(QPointF(28, 5), QPointF(34, 13));
    painter.drawPolygon(QPolygonF({QPointF(20, 24), QPointF(29, 30),
                                   QPointF(20, 36)}));
    break;
  }
  case LineIconKind::Send:
    // 纸飞机：常见的消息发送语义，缩小到按钮尺寸后仍保持清晰轮廓。
    painter.drawPolygon(QPolygonF({
        QPointF(8, 22),
        QPointF(40, 8),
        QPointF(30, 40),
        QPointF(23, 28),
    }));
    painter.drawLine(QPointF(8, 22), QPointF(23, 28));
    painter.drawLine(QPointF(23, 28), QPointF(40, 8));
    break;
  }
  painter.end();
  return QIcon(pixmap);
}

QString navIconResource(LineIconKind kind) {
  switch (kind) {
  case LineIconKind::Messages:
    return QStringLiteral(":/maichat/icons/message-circle.svg");
  case LineIconKind::Assistant:
    return QStringLiteral(":/maichat/icons/creative-studio.svg");
  case LineIconKind::Contacts:
    return QStringLiteral(":/maichat/icons/users.svg");
  case LineIconKind::Screen:
    return QStringLiteral(":/maichat/icons/device-desktop.svg");
  case LineIconKind::Settings:
    return QStringLiteral(":/maichat/icons/settings.svg");
  case LineIconKind::Send:
    return QStringLiteral(":/maichat/icons/arrow-up.svg");
  default:
    return {};
  }
}

QColor navIconColor(LineIconKind kind, bool selected) {
  switch (kind) {
  case LineIconKind::Messages:
    return QColor(selected ? QStringLiteral("#0879c9")
                           : QStringLiteral("#1487d4"));
  case LineIconKind::Assistant:
    return QColor(selected ? QStringLiteral("#5e52b6")
                           : QStringLiteral("#6f62c7"));
  case LineIconKind::Contacts:
    return QColor(selected ? QStringLiteral("#117960")
                           : QStringLiteral("#198c73"));
  case LineIconKind::Screen:
  case LineIconKind::ScreenConnecting:
  case LineIconKind::ScreenDisconnect:
    return QColor(selected ? QStringLiteral("#169fbd")
                           : QStringLiteral("#32a9c1"));
  case LineIconKind::Settings:
    return QColor(selected ? QStringLiteral("#b76b35")
                           : QStringLiteral("#b98054"));
  default:
    return QColor(selected ? QStringLiteral("#0879c9")
                           : QStringLiteral("#5f6f82"));
  }
}

QIcon makeNavIcon(LineIconKind kind, const QColor &color, bool filled = false) {
  QString resource = navIconResource(kind);
  if (filled && !resource.isEmpty())
    resource.replace(QStringLiteral(".svg"), QStringLiteral("-filled.svg"));
  if (resource.isEmpty())
    return makeLineIcon(kind, color);

  QFile file(resource);
  if (!file.open(QIODevice::ReadOnly))
    return makeLineIcon(kind, color);
  QByteArray svg = file.readAll();
  svg.replace("currentColor", color.name(QColor::HexRgb).toUtf8());
  QSvgRenderer renderer(svg);
  if (!renderer.isValid())
    return makeLineIcon(kind, color);

  constexpr int kRender = 48;
  QPixmap pixmap(kRender, kRender);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  renderer.render(&painter, QRectF(0, 0, kRender, kRender));
  return QIcon(pixmap);
}

QPushButton *makeNavButton(const QString &title, const QString &objectName,
                           QWidget *parent) {
  // 只留图标，不显示文字。文字改挂 tooltip 与 accessibleName：纯图标对
  // 「远程」「设置」这类不常点的入口本来就不好认，去掉文字后必须留个说明，
  // accessibleName 同时让读屏软件和测试仍能按名字找到它。
  auto *button = new QPushButton(parent);
  button->setObjectName(objectName);
  button->setToolTip(title);
  button->setAccessibleName(title);
  button->setCursor(Qt::PointingHandCursor);
  return button;
}

void applyNavButtonIcon(QPushButton *button, bool selected) {
  const QVariant rawKind = button->property("navIconKind");
  if (!rawKind.isValid())
    return;
  const LineIconKind kind = lineIconKindFromValue(rawKind.toInt());
  button->setIcon(makeNavIcon(kind, navIconColor(kind, selected), selected));
}

// 聊天标题栏使用飞书式无边框图标按钮。
QPushButton *makeHeaderIconButton(LineIconKind kind, const QString &tooltip,
                                  QWidget *parent) {
  auto *button = new QPushButton(parent);
  button->setObjectName(QStringLiteral("headerIconButton"));
  button->setProperty("chatHeaderIcon", true);
  button->setIcon(makeLineIcon(kind, QColor(QStringLiteral("#4c5866"))));
  button->setIconSize(QSize(17, 17));
  button->setToolTip(tooltip);
  button->setCursor(Qt::PointingHandCursor);
  return button;
}

// 附件右键菜单统一图标色（与聊天头部图标一致）。
const QColor kMenuIconColor(QStringLiteral("#4c5866"));

// 飞书式消息右键菜单：白底圆角卡片、条目悬浮淡蓝高亮、分组分隔线。
// QMenu 是原生弹窗，QSS 圆角需要无边框 + 透明底配合，否则圆角外露出直角底色。
void applyMessageContextMenuStyle(QMenu &menu) {
  menu.setWindowFlags(menu.windowFlags() | Qt::FramelessWindowHint |
                      Qt::NoDropShadowWindowHint);
  menu.setAttribute(Qt::WA_TranslucentBackground);
  menu.setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QMenu {
            background: #ffffff;
            border: 1px solid #e2e8f0;
            border-radius: 10px;
            padding: 6px;
        }
        QMenu::item {
            background: transparent;
            color: #1f2329;
            font-size: 13px;
            padding: 8px 36px 8px 10px;
            border-radius: 6px;
        }
        QMenu::item:selected {
            background: #f2f6fb;
            color: #1f2329;
        }
        QMenu::icon {
            padding-left: 8px;
        }
        QMenu::separator {
            height: 1px;
            background: #eef2f6;
            margin: 5px 6px;
        }
    )")));
}

// 文本气泡右键菜单：复制（有选区复制选区，否则复制渲染后的整条消息）/
// 复制原始数据（保留 Markdown 源语法）/ 复制链接（点在链接上时出现）/ 全选。
// 样式与图片/文件气泡的菜单一致。
void MarkdownMessageView::contextMenuEvent(QContextMenuEvent *event) {
  QMenu menu(this);
  applyMessageContextMenuStyle(menu);
  const QString anchor = linkAt(event->pos());
  QAction *replyAction =
      replyHandler_ ? menu.addAction(QStringLiteral("回复")) : nullptr;
  QAction *forwardAction =
      forwardHandler_
          ? menu.addAction(makeLineIcon(LineIconKind::Forward, kMenuIconColor),
                           QStringLiteral("转发"))
          : nullptr;
  if (replyAction != nullptr || forwardAction != nullptr)
    menu.addSeparator();
  QAction *copyAction = menu.addAction(
      makeLineIcon(LineIconKind::Copy, kMenuIconColor), QStringLiteral("复制"));
  copyOriginalDataAction_->setIcon(
      makeLineIcon(LineIconKind::Copy, kMenuIconColor));
  menu.addAction(copyOriginalDataAction_);
  QAction *copyLinkAction =
      anchor.isEmpty()
          ? nullptr
          : menu.addAction(makeLineIcon(LineIconKind::Link, kMenuIconColor),
                           QStringLiteral("复制链接"));
  menu.addSeparator();
  QAction *selectAllAction =
      menu.addAction(makeLineIcon(LineIconKind::SelectAll, kMenuIconColor),
                     QStringLiteral("全选"));
  QAction *chosen = menu.exec(event->globalPos());
  if (replyAction != nullptr && chosen == replyAction) {
    replyHandler_();
  } else if (forwardAction != nullptr && chosen == forwardAction) {
    forwardHandler_();
  } else if (chosen == copyAction) {
    // 没选中就整条复制，和原来 QTextBrowser 版本一致。
    if (hasSelection()) {
      copySelection();
    } else {
      QApplication::clipboard()->setText(plainText());
    }
  } else if (copyLinkAction != nullptr && chosen == copyLinkAction) {
    QApplication::clipboard()->setText(anchor);
  } else if (chosen == selectAllAction) {
    selectAll();
  }
}

struct SlashCommandDefinition {
  QString command;
  QString label;
  QString objectName;
};

QList<SlashCommandDefinition> slashCommandDefinitions() {
  return {
      {QStringLiteral("/status"), QStringLiteral("查看状态"),
       QStringLiteral("slashCommandButton_status")},
      {QStringLiteral("/plan"), QStringLiteral("切换 Plan"),
       QStringLiteral("slashCommandButton_plan")},
      {QStringLiteral("/build"), QStringLiteral("切换 Build"),
       QStringLiteral("slashCommandButton_build")},
      {QStringLiteral("/models"), QStringLiteral("模型列表"),
       QStringLiteral("slashCommandButton_models")},
      {QStringLiteral("/model "), QStringLiteral("模型/推理"),
       QStringLiteral("slashCommandButton_model")},
      {QStringLiteral("/goal "), QStringLiteral("管理 Goal"),
       QStringLiteral("slashCommandButton_goal")},
      {QStringLiteral("/btw "), QStringLiteral("子任务"),
       QStringLiteral("slashCommandButton_btw")},
      {QStringLiteral("/diff "), QStringLiteral("仓库 Diff"),
       QStringLiteral("slashCommandButton_diff")},
      {QStringLiteral("/interrupt"), QStringLiteral("中断任务"),
       QStringLiteral("slashCommandButton_interrupt")},
      {QStringLiteral("/compact"), QStringLiteral("压缩上下文"),
       QStringLiteral("slashCommandButton_compact")},
      {QStringLiteral("/clear"), QStringLiteral("清空上下文"),
       QStringLiteral("slashCommandButton_clear")},
      {QStringLiteral("/help"), QStringLiteral("命令帮助"),
       QStringLiteral("slashCommandButton_help")},
  };
}

QString deliveryStatusIndicator(RemoteIMMessageStatus status) {
  switch (status) {
  case RemoteIMMessageStatus::Pending:
    return QString();
  case RemoteIMMessageStatus::Sent:
    return QStringLiteral("✓");
  case RemoteIMMessageStatus::Failed:
    return QStringLiteral("!");
  case RemoteIMMessageStatus::Received:
    return QString();
  }
  return QString();
}

QString conversationMarkdownPreview(const QString &source) {
// 仅在 UI 线程、条目委托的绘制路径之外解析；限制输入和缓存开销，
// 避免很长的 Agent 回复拖慢每次列表刷新。
  static QCache<QString, QString> cache(2 * 1024 * 1024);
  QString input = source.left(8192);
  if (!input.isEmpty() && QChar(input.back()).isHighSurrogate())
    input.chop(1);
  if (const auto *cached = cache.object(input))
    return *cached;
  // 原来是「渲染成 HTML → 塞进 QTextDocument → 再把文字抠出来」，绕了一大圈，
  // 只为了把 **粗体** 的星号去掉。块树自己就能摊平成纯文本，少一条渲染路径。
  QString result = MarkdownDocument::parse(input).plainText().simplified();
  result = PreviewText::truncate(result);
  cache.insert(input, new QString(result),
               (input.size() + result.size()) * 2 + 1);
  return result;
}

QString latestMessageText(const RemoteIMMessage *message) {
  if (!message)
    return QStringLiteral("暂无消息");
  if (message->hasImage || message->hasFile || message->hasVideo ||
      message->hasVoice) {
    QString text = message->text;
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text;
  }
  const QString preview = conversationMarkdownPreview(message->text);
  return preview.isEmpty() ? QStringLiteral("新消息") : preview;
}

QString relativeMessageTimeText(qint64 createdAtMillis) {
  const QDateTime messageTime = QDateTime::fromMSecsSinceEpoch(createdAtMillis);
  const QDate messageDate = messageTime.date();
  const QDate today = QDate::currentDate();
  if (messageDate == today)
    return messageTime.toString(QStringLiteral("HH:mm"));
  if (messageDate == today.addDays(-1))
    return QStringLiteral("昨天 ") +
           messageTime.toString(QStringLiteral("HH:mm"));
  return messageTime.toString(QStringLiteral("M 月 d 日 HH:mm"));
}

// 会话列表使用紧凑时间戳：今天显示时分，昨天及更早显示日期，
// 让窄时间列仍有空间容纳联系人名称。
// 时间文本不能被裁成残字，例如“昨天 14:30”不能只剩“天 14:30”。
QString conversationListTimeText(qint64 createdAtMillis) {
  if (createdAtMillis <= 0)
    return QString();
  const QDateTime messageTime = QDateTime::fromMSecsSinceEpoch(createdAtMillis);
  const QDate messageDate = messageTime.date();
  const QDate today = QDate::currentDate();
  if (messageDate == today)
    return messageTime.toString(QStringLiteral("HH:mm"));
  if (messageDate == today.addDays(-1))
    return QStringLiteral("昨天");
  if (messageDate.year() == today.year())
    return messageTime.toString(QStringLiteral("M月d日"));
  return messageTime.toString(QStringLiteral("yyyy/M/d"));
}

QString latestMessageTime(const RemoteIMMessage *message) {
  return message ? conversationListTimeText(message->createdAtMillis)
                 : QString();
}

QString messageTimeText(const RemoteIMMessage &message) {
  return relativeMessageTimeText(message.createdAtMillis);
}

// 输入框里内联附件的资源名前缀：QTextEdit
// 只认得住一个「资源名」，把类型编码进去 是这里区分图片 / 文件 /
// 视频的唯一手段（图片没有前缀，资源名就是原路径）。
const QString kComposerFilePrefix = QStringLiteral("pending-file://");
const QString kComposerVideoPrefix = QStringLiteral("pending-video://");

bool isHtmlFile(const RemoteIMFileAttachment &attachment) {
  const QString mimeType = attachment.mimeType.toLower();
  const QString fileName = attachment.fileName.toLower();
  return mimeType.contains(QStringLiteral("html")) ||
         fileName.endsWith(QStringLiteral(".html")) ||
         fileName.endsWith(QStringLiteral(".htm"));
}

bool isGitDiffFile(const RemoteIMFileAttachment &attachment) {
  const QString fileName = attachment.fileName.toLower();
  const QRegularExpression pattern(QStringLiteral("-([0-9a-f]{64})\\.html$"));
  return isHtmlFile(attachment) &&
         fileName.startsWith(QStringLiteral("remote-im-diff-")) &&
         pattern.match(fileName).hasMatch();
}

bool hasValidGitDiffIntegrity(const RemoteIMFileAttachment &attachment) {
  const QRegularExpression pattern(QStringLiteral("-([0-9a-f]{64})\\.html$"));
  const QRegularExpressionMatch match =
      pattern.match(attachment.fileName.toLower());
  if (!match.hasMatch())
    return false;
  QFile file(attachment.localPath);
  if (!file.open(QIODevice::ReadOnly))
    return false;
  const QByteArray actual =
      QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
          .toHex();
  return actual == match.captured(1).toLatin1();
}

bool isMarkdownFile(const RemoteIMFileAttachment &attachment) {
  const QString mimeType = attachment.mimeType.toLower();
  const QString fileName = attachment.fileName.toLower();
  return mimeType.contains(QStringLiteral("markdown")) ||
         fileName.endsWith(QStringLiteral(".md")) ||
         fileName.endsWith(QStringLiteral(".markdown"));
}

bool isPdfFile(const RemoteIMFileAttachment &attachment) {
  return attachment.mimeType.compare(QStringLiteral("application/pdf"),
                                     Qt::CaseInsensitive) == 0 ||
         attachment.fileName.endsWith(QStringLiteral(".pdf"),
                                      Qt::CaseInsensitive);
}

// 视频走 IM 的视频消息发出去，但本地回显仍存成文件附件（本地库还没有视频列）。
// 气泡靠 MIME/扩展名认出来，显示成视频卡片而不是一张「文档」卡。
bool isVideoFile(const RemoteIMFileAttachment &attachment) {
  const QString mimeType = attachment.mimeType.toLower();
  const QString fileName = attachment.fileName.toLower();
  return mimeType.startsWith(QStringLiteral("video/")) ||
         fileName.endsWith(QStringLiteral(".mp4")) ||
         fileName.endsWith(QStringLiteral(".mov"));
}

// 仅 md/html 文档支持内嵌预览；其余是普通文件，点击/菜单走「另存为」。
bool isPreviewableDocument(const RemoteIMFileAttachment &attachment) {
  return isHtmlFile(attachment) || isMarkdownFile(attachment) ||
         isPdfFile(attachment);
}

QString fileSizeText(qint64 bytes) {
  if (bytes <= 0)
    return QString();
  if (bytes < 1024)
    return QStringLiteral("%1 B").arg(bytes);
  if (bytes < 1024 * 1024)
    return QStringLiteral("%1 KB").arg(QString::number(bytes / 1024.0, 'f', 1));
  if (bytes < qint64(1024) * 1024 * 1024) {
    return QStringLiteral("%1 MB").arg(
        QString::number(bytes / (1024.0 * 1024.0), 'f', 1));
  }
  return QStringLiteral("%1 GB").arg(
      QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2));
}

QString readTextFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QStringLiteral("文件暂不可预览");
  }
  return QString::fromUtf8(file.readAll());
}

} // namespace

namespace {

// 本地只保存已选模型。Responses 地址由供应商确定；
// 模型凭据领取后仅驻留当前进程内存。
AgentController::ModelConfig loadAgentModelConfig(
    const QHash<QString, QString>& modelKeys) {
  AgentController::ModelConfig config;

  QSettings settings;
  bool removedLegacySetting = false;
  for (const char *obsolete :
       {"agent/apiKey", "agent/deepseekApiKey", "agent/baseUrl",
        "agent/deepseekBaseUrl", "agent/wire", "agent/deepseekWire",
        "agent/glmChatBaseUrl", "agent/cloudServiceUrl",
        "agent/cloudServiceToken", "agent/cloudServiceTokenMigrationFailed"}) {
    const QString key = QLatin1String(obsolete);
    if (!settings.contains(key))
      continue;
    settings.remove(key);
    removedLegacySetting = true;
  }
  if (removedLegacySetting)
    settings.sync();
  config.modelName = QStringLiteral("deepseek-flash");
  config.reasoningEffort = settings.value(
      QStringLiteral("agent/reasoningEffort"), QStringLiteral("high")).toString();
  if (config.reasoningEffort != QStringLiteral("low") &&
      config.reasoningEffort != QStringLiteral("max"))
    config.reasoningEffort = QStringLiteral("high");
  const bool deepseek = config.modelName == QStringLiteral("deepseek-flash");
  config.wire = MaiWireApi::Responses;
  config.baseUrl = deepseek ? QStringLiteral("https://api.deepseek.com")
                            : QStringLiteral("https://open.bigmodel.cn/api/v1");
  config.apiKey = modelKeys.value(deepseek ? QStringLiteral("deepseek") :
                                    QStringLiteral("glm"));
  config.glmApiKey = modelKeys.value(QStringLiteral("glm"));
  config.miniMaxApiKey = modelKeys.value(QStringLiteral("minimax"));
  const QString policy = settings
                             .value(QStringLiteral("agent/approvalPolicy"),
                                    QStringLiteral("on_request"))
                             .toString();
  config.approvalPolicy = policy == QStringLiteral("never")
                              ? MaiApprovalPolicy::Never
                          : policy == QStringLiteral("unless_trusted")
                              ? MaiApprovalPolicy::UnlessTrusted
                              : MaiApprovalPolicy::OnRequest;
  return config;
}

// 会话和消息落在哪。放应用数据目录，不放当前目录——
// 当前目录是用户的工作目录，往里扔数据库文件不礼貌。
QString accountAgentDirectory(const QString &ownerUserId) {
  if (ownerUserId.isEmpty()) return {};
  QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (root.isEmpty())
    root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (root.isEmpty()) return {};
  const QString account = QString::fromLatin1(
      QCryptographicHash::hash(ownerUserId.toUtf8(), QCryptographicHash::Sha256).toHex());
  return QDir(root).filePath(QStringLiteral("Agent/Accounts/%1").arg(account));
}

QString agentDatabasePath(const QString &ownerUserId) {
  const QString root = accountAgentDirectory(ownerUserId);
  if (root.isEmpty() || !QDir().mkpath(root)) return {};
  return QDir(root).filePath(QStringLiteral("agent.db"));
}

#ifdef Q_OS_WIN
void hideNativeTitleBarIcon(QWidget *window) {
  if (window->property("nativeTitleBarIconHidden").toBool())
    return;
  const HWND handle = reinterpret_cast<HWND>(window->winId());
  static HICON transparentSmallIcon = [] {
    unsigned char andMask[64];
    unsigned char xorMask[16 * 16 * 4];
    std::fill_n(andMask, sizeof(andMask), 0xff);
    std::fill_n(xorMask, sizeof(xorMask), 0x00);
    return CreateIcon(nullptr, 16, 16, 1, 32, andMask, xorMask);
  }();
  SendMessageW(handle, WM_SETICON, ICON_SMALL,
               reinterpret_cast<LPARAM>(transparentSmallIcon));
  SendMessageW(handle, WM_SETICON, ICON_SMALL2,
               reinterpret_cast<LPARAM>(transparentSmallIcon));
  window->setProperty("nativeTitleBarIconHidden", true);
}
#endif

} // namespace

MainWindow::MainWindow(RemoteIMApplication &app, QWidget *parent)
    : MainWindow(app, 0, {}, {}, {}, parent) {}

MainWindow::MainWindow(RemoteIMApplication &app, int imSdkAppId,
                       std::function<QString(const QString&)> userSigProvider,
                       QWidget *parent)
    : MainWindow(app, imSdkAppId, std::move(userSigProvider), {}, {}, parent) {}

MainWindow::MainWindow(
    RemoteIMApplication &app, int imSdkAppId,
    std::function<QString(const QString&)> userSigProvider,
    std::function<QString()> arkApiKeyProvider,
    std::function<QPair<QString, QString>()> wanCredentialsProvider,
    QWidget *parent)
    : QMainWindow(parent), app_(app), imSdkAppId_(imSdkAppId),
      userSigProvider_(std::move(userSigProvider)),
      arkApiKeyProvider_(std::move(arkApiKeyProvider)),
      wanCredentialsProvider_(std::move(wanCredentialsProvider)),
      notificationTracker_(QDateTime::currentMSecsSinceEpoch()) {
  Q_INIT_RESOURCE(resources);
  // 缩放只作用于主界面：登录窗（先于此构造）保持设计尺寸，
  // 进入主界面时才把全局字体切到基准 13px × 倍率。
  QFont scaledFont = QApplication::font();
  scaledFont.setPixelSize(UiZoom::s(13));
  QApplication::setFont(scaledFont);
  auto updatePerformanceContext = [this] {
    const auto account = app_.client().currentAccount();
    RemoteDiagnostics::PerformanceLog::shared().setContext(
        account.isValid()
            ? QString::number(account.sdkAppId) + ":" + account.ownerUserId
            : QString());
  };
  updatePerformanceContext();
  connect(&app_, &RemoteIMApplication::stateChanged, this,
          updatePerformanceContext);
  connect(&app_, &RemoteIMApplication::connectionChanged, this,
          updatePerformanceContext);
  auto *performanceTimer = new QTimer(this);
  performanceTimer->setInterval(250);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  connect(performanceTimer, &QTimer::timeout, this, [clock] {
    RemoteDiagnostics::PerformanceLog::shared().heartbeat(
        clock->elapsed(), qApp->applicationState() == Qt::ApplicationActive);
  });
  connect(qApp, &QGuiApplication::applicationStateChanged, this,
          [clock](Qt::ApplicationState) {
            RemoteDiagnostics::PerformanceLog::shared().heartbeat(
                clock->elapsed(), false);
          });
  performanceTimer->start();
  buildUi();
  applyStyle();
  const AgentController::ModelConfig replyModel = loadAgentModelConfig(modelApiKeys_);
  ReplySuggestionController::Config replyConfig;
  replyConfig.baseUrl = replyModel.baseUrl;
  replyConfig.apiKey = replyModel.apiKey;
  replyConfig.modelName = replyModel.modelName;
  replySuggestionController_ = new ReplySuggestionController(replyConfig, this);
  replySuggestionController_->setObjectName(
      QStringLiteral("replySuggestionController"));
  bindSignals();
  // 必须在 buildUi 之后：设置页的控件已建好，这里创建控制器并把当前配置
  // 回填到界面上。
  setupRemoteDesktop();
  refreshRemoteDesktopSettings();
  connect(qApp, &QGuiApplication::applicationStateChanged, this,
          [this](Qt::ApplicationState state) {
            // macOS 从“辅助功能”设置切回来后立即刷新授权状态。
            if (state == Qt::ApplicationActive) {
              refreshRemoteDesktopSettings();
              // 回到前台后系统通知已失去意义；清掉「已经提醒过」状态，
              // 下次重新退后台时，真正的新消息仍可正常提醒一次。
              notificationTracker_.clearAll();
              lastNotifiedPeerId_.clear();
            }
          });
  connect(qApp, &QCoreApplication::aboutToQuit, this,
          [this] { stopRemoteDesktopForShutdown(); });
  // 放在 refresh 之前：refresh 会重建列表但不产生入站消息，先后顺序无所谓，
  // 重要的是通知在第一条实时消息到达之前就已经接好线。
  setUpMessageNotifications();
  refresh();
}

bool MainWindow::event(QEvent *event) {
  const bool handled = QMainWindow::event(event);
  if (event->type() == QEvent::Move)
    QTimer::singleShot(0, this, [this] { syncVideoDock(); });
#ifdef Q_OS_WIN
  if (event->type() == QEvent::WindowActivate) {
    updateWindowChromeBackground(true);
  } else if (event->type() == QEvent::WindowDeactivate) {
    updateWindowChromeBackground(false);
  }
#endif
  return handled;
}

void MainWindow::updateWindowChromeBackground(bool active) {
#ifdef Q_OS_WIN
  const char *color =
      active ? kActiveWindowChromeBackground : kInactiveWindowChromeBackground;
  if (auto *inset = findChild<QWidget *>(QStringLiteral("windowTopInset"))) {
    setSolidWindowBackground(inset, color);
  }
  if (navRail_ != nullptr)
    setSolidWindowBackground(navRail_, color);
#else
  Q_UNUSED(active);
#endif
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (videoDialog_) videoDialog_->close();
  if (cloudDriveDialog_) cloudDriveDialog_->close();
  if (accountDisconnectPending_) {
    event->ignore();
    return;
  }
  if (remoteDesktopShutdownComplete_) {
    QMainWindow::closeEvent(event);
    return;
  }

  // IM 发送是异步的。先拦住本次关闭，等 stop 已交给 SDK（或短超时）
  // 再真正关闭，否则进程退出会让对端永远停在共享/控制状态。
  event->ignore();
  if (remoteDesktopShutdown_)
    return;

  QPointer<MainWindow> window(this);
  stopRemoteDesktopForShutdown([window] {
    if (!window || window->accountDisconnectPending_)
      return;
    QTimer::singleShot(0, window, [window] {
      if (window && !window->accountDisconnectPending_)
        window->close();
    });
  });
}

void MainWindow::prepareForAccountDisconnect(std::function<void()> completion) {
  accountDisconnectPending_ = true;
  ++videoCircleProbeGeneration_;
  videoCircleAvailable_ = false;
  ++cloudDriveProbeGeneration_;
  cloudDriveAvailable_ = false;
  if (cloudSettingsRow_) cloudSettingsRow_->hide();
  if (agentPanel_) agentPanel_->setCloudDriveAvailable(false);
  if (videoCircleDialog_) videoCircleDialog_->close();
  if (cloudDriveDialog_) cloudDriveDialog_->close();
  if (videoDialog_) videoDialog_->close();
  stopRemoteDesktopForShutdown(std::move(completion));
}

void MainWindow::finishAccountDisconnect() {
  accountDisconnectPending_ = false;
  close();
}

void MainWindow::setLogoutHandler(std::function<void()> handler) {
  logoutHandler_ = std::move(handler);
  if (settingsLogoutButton_)
    settingsLogoutButton_->setEnabled(static_cast<bool>(logoutHandler_));
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
  if (watched == videoDockSlot_ &&
      (event->type() == QEvent::Resize || event->type() == QEvent::Move))
    QTimer::singleShot(0, this, [this] { syncVideoDock(); });
  if (watched == videoDockPane_ && event->type() == QEvent::Resize)
    QTimer::singleShot(0, this, [this] { syncVideoDock(); });
  if (watched == videoDialog_ &&
      (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Show ||
       event->type() == QEvent::Hide))
    QTimer::singleShot(0, this, [this] { syncVideoDock(); });
  if (watched == messageComposerHandle_ && messageScroll_) {
    if (event->type() == QEvent::MouseButtonPress &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
      QObject::disconnect(composerResizeRangeConn_);
      QScrollBar *bar = messageScroll_->verticalScrollBar();
      composerResizePeerId_ = app_.chatState().selectedPeerId();
      composerResizeStartedAtBottom_ =
          bar->value() >= bar->maximum() - UiZoom::s(40);
      if (composerResizeStartedAtBottom_) {
        const QString peerId = composerResizePeerId_;
        composerResizeRangeConn_ =
            connect(bar, &QAbstractSlider::rangeChanged, this,
                    [this, peerId](int, int) {
                      QTimer::singleShot(0, this, [this, peerId] {
                        if (messageScroll_ &&
                            app_.chatState().selectedPeerId() == peerId)
                          messageScroll_->verticalScrollBar()->setValue(
                              messageScroll_->verticalScrollBar()->maximum());
                      });
                    });
      }
    } else if ((event->type() == QEvent::MouseButtonRelease &&
                static_cast<QMouseEvent *>(event)->button() ==
                    Qt::LeftButton) ||
               event->type() == QEvent::Hide ||
               event->type() == QEvent::UngrabMouse) {
      QObject::disconnect(composerResizeRangeConn_);
      if (composerResizeStartedAtBottom_) {
        const QString peerId = composerResizePeerId_;
        QTimer::singleShot(0, this, [this, peerId] {
          if (messageScroll_ && app_.chatState().selectedPeerId() == peerId)
            messageScroll_->verticalScrollBar()->setValue(
                messageScroll_->verticalScrollBar()->maximum());
        });
      }
      composerResizeStartedAtBottom_ = false;
      composerResizePeerId_.clear();
    }
  }
  if (watched == commandButton_ && event->type() == QEvent::Enter) {
    showSlashCommandMenu();
  }
  if (watched == commandButton_ && event->type() == QEvent::KeyPress) {
    const int key = static_cast<QKeyEvent *>(event)->key();
    if (key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter) {
      showSlashCommandMenu();
      return true;
    }
  }
  if (watched == aiReplyButton_ && event->type() == QEvent::Enter) {
    aiReplyHint_->show();
    aiReplyHint_->raise();
    static_cast<ComposerTextEdit *>(messageEditor_)->positionCornerAction();
  } else if (watched == aiReplyButton_ && event->type() == QEvent::Leave) {
    aiReplyHint_->hide();
  }
  if (watched == messageEditor_ && event->type() == QEvent::FocusOut) {
    app_.setHumanTypingActive(false);
  }
  if (watched == messageEditor_ && (event->type() == QEvent::KeyPress ||
                                    event->type() == QEvent::InputMethod)) {
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    const auto account = app_.client().currentAccount();
    QTimer::singleShot(0, this, [this, clock, account] {
      if (app_.client().currentAccount() == account)
        RemoteDiagnostics::PerformanceLog::shared().record(
            "composer-event-queue", clock->nsecsElapsed() / 1000000.0);
    });
  }
  if ((watched == conversationList_ || watched == contactsList_) &&
      event->type() == QEvent::KeyPress) {
    auto *keyEvent = static_cast<QKeyEvent *>(event);
    if (keyEvent->key() == Qt::Key_Delete ||
        keyEvent->key() == Qt::Key_Backspace) {
      deleteSelectedContactFromList(qobject_cast<QListWidget *>(watched));
      return true;
    }
  }
  if (watched == messageEditor_ && event->type() == QEvent::KeyPress) {
    auto *keyEvent = static_cast<QKeyEvent *>(event);
    const bool isReturn =
        keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter;
    if (isReturn &&
        (keyEvent->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
      messageEditor_->insertPlainText(QStringLiteral("\n"));
      return true;
    }
    if (isReturn && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
      sendCurrentText();
      return true;
    }
    // Ctrl+V：剪贴板里有图片/文件则直接发送（消费按键）；否则交给默认粘贴文本。
    if (keyEvent->key() == Qt::Key_V &&
        (keyEvent->modifiers() & Qt::ControlModifier) &&
        !(keyEvent->modifiers() & (Qt::ShiftModifier | Qt::AltModifier))) {
      if (handleComposerPaste())
        return true;
    }
  }
  if (messageScroll_ && watched == messageScroll_->viewport() &&
      event->type() == QEvent::Resize) {
    QTimer::singleShot(0, this, [this] { updateMessageBubbleWidths(); });
  }
  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::resizeEvent(QResizeEvent *event) {
  QMainWindow::resizeEvent(event);
  QTimer::singleShot(0, this, [this] {
    updateMessageBubbleWidths();
    // 结果面板是按搜索框位置摆的浮层，不在布局里，窗口一变它就得重新对位。
    layoutGlobalSearchResults();
    syncVideoDock();
  });
}

void MainWindow::showEvent(QShowEvent *event) {
  QMainWindow::showEvent(event);
#ifdef Q_OS_WIN
  hideNativeTitleBarIcon(this);
#endif
  QTimer::singleShot(0, this, [this] { updateMessageBubbleWidths(); });
}

void MainWindow::buildUi() {
  // 单个空格而不是空串：空标题时 Qt 会回退显示 applicationDisplayName
  // （"MaiChat"），飞书风格的标题栏不显示文字。
  setWindowTitle(QStringLiteral(" "));
  resize(UiZoom::s(1280), UiZoom::s(820));
  setMinimumSize(UiZoom::s(980), UiZoom::s(640));

  auto *root = new QWidget(this);
  root->setObjectName(QStringLiteral("root"));
  // 纵向根布局：共享指示条常驻最顶部并横贯整宽，下方才是原有的横向主体。
  // 指示条必须压在所有内容之上，不能被侧栏或会话区挤掉。
  auto *rootColumn = new QVBoxLayout(root);
  rootColumn->setContentsMargins(0, 0, 0, 0);
  rootColumn->setSpacing(0);
  setCentralWidget(root);

#ifdef Q_OS_WIN
// 扩大系统原生标题栏的视觉高度，不替换为自绘标题栏。
// 自绘会破坏窗口缩放、贴边吸附和 Windows 11 贴靠布局菜单。
  auto *windowTopInset = new QWidget(root);
  windowTopInset->setObjectName(QStringLiteral("windowTopInset"));
  windowTopInset->setFixedHeight(UiZoom::s(16));
  setSolidWindowBackground(windowTopInset, kInactiveWindowChromeBackground);
  rootColumn->addWidget(windowTopInset);
#endif

  sharingIndicator_ = new SharingIndicatorBar(root);
  rootColumn->addWidget(sharingIndicator_);

  // 结果面板浮在窗口上，不进布局：它要盖住下方内容，进布局会把主体挤下去。
  globalSearchResults_ = new QListWidget(root);
  globalSearchResults_->setObjectName(QStringLiteral("globalSearchResults"));
  globalSearchResults_->setWindowFlags(Qt::Widget);
  globalSearchResults_->setFrameShape(QFrame::NoFrame);
  globalSearchResults_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  globalSearchResults_->hide();

  auto *rootContent = new QWidget(root);
  rootContent->setObjectName(QStringLiteral("rootContent"));
  auto *rootLayout = new QHBoxLayout(rootContent);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(0);
  rootColumn->addWidget(rootContent, 1);

  auto *rootNavigationSplitter = new QSplitter(Qt::Horizontal, rootContent);
  rootNavigationSplitter->setObjectName(
      QStringLiteral("rootNavigationSplitter"));
  rootNavigationSplitter->setChildrenCollapsible(false);
  rootNavigationSplitter->setHandleWidth(1);

  auto *contentArea = new QWidget(rootNavigationSplitter);
  auto *contentLayout = new QHBoxLayout(contentArea);
  contentLayout->setContentsMargins(0, 0, 0, 0);
  contentLayout->setSpacing(0);
  videoContentSplitter_ = new QSplitter(Qt::Horizontal, contentArea);
  videoContentSplitter_->setObjectName(QStringLiteral("videoContentSplitter"));
  videoContentSplitter_->setChildrenCollapsible(false);
  videoContentSplitter_->setHandleWidth(UiZoom::s(7));
  videoContentSplitter_->setStyleSheet(QStringLiteral(
      "QSplitter#videoContentSplitter::handle { background: #dbe4ef; }"
      "QSplitter#videoContentSplitter::handle:hover { background: #9dc7e8; }"));
  contentLayout->addWidget(videoContentSplitter_);
  videoDockPane_ = new QWidget(videoContentSplitter_);
  videoDockPane_->setObjectName(QStringLiteral("videoDockPane"));
  videoDockPane_->setStyleSheet(QStringLiteral("background:#f8fafc;"));
  videoDockPane_->installEventFilter(this);
  auto *videoPaneLayout = new QVBoxLayout(videoDockPane_);
  videoPaneLayout->setContentsMargins(0, 0, 0, 0);
  videoPaneLayout->setSpacing(0);
  videoDockSlot_ = new QWidget(videoDockPane_);
  videoDockSlot_->setObjectName(QStringLiteral("videoDockSlot"));
  videoDockSlot_->setStyleSheet(QStringLiteral("background:#080c14;"));
  videoDockSlot_->installEventFilter(this);
  videoDockSlot_->hide();
  videoPaneLayout->addWidget(videoDockSlot_, 0, Qt::AlignTop);
  videoPaneLayout->addStretch(1);
  videoContentSplitter_->addWidget(videoDockPane_);
  videoDockPane_->hide();
  contentStack_ = new QStackedWidget(videoContentSplitter_);
  contentStack_->setObjectName(QStringLiteral("contentStack"));
  videoContentSplitter_->addWidget(contentStack_);
  videoContentSplitter_->setStretchFactor(0, 0);
  videoContentSplitter_->setStretchFactor(1, 1);
  videoContentSplitter_->handle(1)->setToolTip(QStringLiteral("拖动调整播放器大小"));
  connect(videoContentSplitter_, &QSplitter::splitterMoved, this,
          [this] {
            const int extent = videoContentSplitter_->sizes().value(0);
            if (extent > 0) {
              if (videoContentSplitter_->orientation() == Qt::Horizontal)
                videoDockHorizontalExtent_ = extent;
              else
                videoDockVerticalExtent_ = extent;
            }
            syncVideoDock();
          });

  messagesPage_ = new QWidget(contentStack_);
  messagesPage_->setObjectName(QStringLiteral("messagesPage"));
  auto *messagesPageLayout = new QHBoxLayout(messagesPage_);
  messagesPageLayout->setContentsMargins(0, 0, 0, 0);
  messagesPageLayout->setSpacing(0);

  auto *contentSplitter = new QSplitter(Qt::Horizontal, messagesPage_);
  contentSplitter->setObjectName(QStringLiteral("contentSplitter"));
  contentSplitter->setChildrenCollapsible(false);
// Qt 会把 1px 分隔手柄的拖动命中区域扩展到相邻控件；
// 因此无需画出很宽的间隔，也能保留可用的拖动目标。
  contentSplitter->setHandleWidth(1);

  navRail_ = new QWidget(rootNavigationSplitter);
  navRail_->setObjectName(QStringLiteral("navRail"));
  setSolidWindowBackground(navRail_, kInactiveWindowChromeBackground);
  // 纯图标之后不需要那么宽：按「图标 + 两侧留白」定宽，不再让它可拉伸。
  navRail_->setFixedWidth(UiZoom::s(64));
  auto *navLayout = new QVBoxLayout(navRail_);
  navLayout->setContentsMargins(8, 0, 8, 12);
  navLayout->setSpacing(8);

  selfProfileButton_ = new QPushButton(navRail_);
  auto *logoContainer = selfProfileButton_;
  logoContainer->setObjectName(QStringLiteral("navLogoContainer"));
  logoContainer->setFixedSize(UiZoom::s(44), UiZoom::s(44));
  logoContainer->setCursor(Qt::PointingHandCursor);
  logoContainer->setToolTip(QStringLiteral("查看我的资料"));
  logoContainer->setAccessibleName(QStringLiteral("查看我的资料"));

  auto *logo = new AccountAvatarLabel(logoContainer);
  logo->setObjectName(QStringLiteral("navLogo"));
  logo->setAlignment(Qt::AlignCenter);
  logo->setFixedSize(UiZoom::s(34), UiZoom::s(34));
  logo->move(UiZoom::s(5), UiZoom::s(5));
  logo->setProfile(app_.selfProfile());
  logo->setAttribute(Qt::WA_TransparentForMouseEvents);

  // 头像独占一行居中；IM 连接状态放在点击头像后打开的个人资料中。
  navLayout->addWidget(logoContainer, 0, Qt::AlignHCenter);
  navLayout->addSpacing(UiZoom::s(6));

  messageNavButton_ = makeNavButton(
      QStringLiteral("消息"), QStringLiteral("messagesNavButton"), navRail_);
  contactsNavButton_ = makeNavButton(
      QStringLiteral("通讯录"), QStringLiteral("contactsNavButton"), navRail_);
  // 远程桌面画面在应用内成页展示，不再弹独立窗口。
  remoteNavButton_ = makeNavButton(QStringLiteral("远程"),
                                   QStringLiteral("remoteNavButton"), navRail_);
  settingsNavButton_ = makeNavButton(
      QStringLiteral("设置"), QStringLiteral("settingsNavButton"), navRail_);
  agentNavButton_ = makeNavButton(QStringLiteral("AI"),
                                  QStringLiteral("agentNavButton"), navRail_);
  messageNavButton_->setProperty("navIconKind",
                                 lineIconKindValue(LineIconKind::Messages));
  contactsNavButton_->setProperty("navIconKind",
                                  lineIconKindValue(LineIconKind::Contacts));
  remoteNavButton_->setProperty("navIconKind",
                                lineIconKindValue(LineIconKind::Screen));
  settingsNavButton_->setProperty("navIconKind",
                                  lineIconKindValue(LineIconKind::Settings));
  // 没有这一句那一格就是空白——makeNavButton 只画文字，图标是靠这个属性来的。
  agentNavButton_->setProperty("navIconKind",
                               lineIconKindValue(LineIconKind::Assistant));
  messageNavButton_->setProperty("selected", true);
  for (QPushButton *navButton :
       {messageNavButton_, contactsNavButton_, remoteNavButton_,
        settingsNavButton_, agentNavButton_}) {
    navButton->setIconSize(QSize(kNavRailIconPixels, kNavRailIconPixels));
  }
  applyNavButtonIcon(messageNavButton_, true);
  applyNavButtonIcon(contactsNavButton_, false);
  applyNavButtonIcon(remoteNavButton_, false);
  applyNavButtonIcon(settingsNavButton_, false);
  // 漏了这一句按钮就是个空白方块：图标不是 makeNavButton 画的，
  // 是这里按 navIconKind 属性单独画上去的。
  applyNavButtonIcon(agentNavButton_, false);
  navLayout->addWidget(messageNavButton_);
  navLayout->addWidget(contactsNavButton_);
  navLayout->addWidget(remoteNavButton_);
  // AI 放在「消息」下面、其它之上：它是要常用的东西，沉到最底下就等于藏起来。
  // makeNavButton 只设 parent，不入布局——漏了这一句按钮会存在但永远不显示。
  navLayout->insertWidget(navLayout->indexOf(contactsNavButton_),
                          agentNavButton_);
  navLayout->addWidget(settingsNavButton_);
  navLayout->addStretch(1);

  auto *conversationPane = new QWidget(messagesPage_);
  conversationPane->setObjectName(QStringLiteral("conversationPane"));
  conversationPane->setMinimumWidth(UiZoom::s(220));
  auto *conversationLayout = new QVBoxLayout(conversationPane);
  conversationLayout->setContentsMargins(20, 18, 0, 16);
  conversationLayout->setSpacing(14);

  // 会话栏不再展示手动搜索，联系人和消息检索统一由 AI 助手完成。
  // 暂保留内部搜索对象供迁移期兼容旧回调，但不展示也不绑定快捷键。
  auto *conversationHeader = new QHBoxLayout();
  conversationHeader->setContentsMargins(0, 0, 16, 0);
  conversationHeader->setSpacing(8);
  auto *conversationTitle = new QLabel(QStringLiteral("消息"), conversationPane);
  conversationTitle->setObjectName(QStringLiteral("conversationSectionTitle"));
  navSearchInput_ = new QLineEdit(conversationPane);
  navSearchInput_->setObjectName(QStringLiteral("globalSearchBox"));
  navSearchInput_->setPlaceholderText(QStringLiteral("搜索消息 (Ctrl+F)"));
  navSearchInput_->setClearButtonEnabled(true);
  navSearchInput_->addAction(
      makeLineIcon(LineIconKind::Search, QColor(QStringLiteral("#98a2b3"))),
      QLineEdit::LeadingPosition);
  navSearchInput_->hide();
  addContactButton_ = new QPushButton(conversationPane);
  addContactButton_->setObjectName(QStringLiteral("addConversationButton"));
  addContactButton_->setIcon(
      makeLineIcon(LineIconKind::Add, QColor(QStringLiteral("#4c5866"))));
  addContactButton_->setIconSize(QSize(kNavIconPixels, kNavIconPixels));
  addContactButton_->setToolTip(QStringLiteral("添加联系人"));
  addContactButton_->setCursor(Qt::PointingHandCursor);
  friendRequestsButton_ = new QPushButton(QStringLiteral("好友申请"), conversationPane);
  friendRequestsButton_->setObjectName(QStringLiteral("friendRequestsButton"));
  friendRequestsButton_->setToolTip(QStringLiteral("查看好友申请"));
  friendRequestsButton_->setCursor(Qt::PointingHandCursor);
  conversationHeader->addWidget(conversationTitle);
  conversationHeader->addStretch(1);
  conversationHeader->addWidget(friendRequestsButton_, 0);
  conversationHeader->addWidget(addContactButton_, 0);

  conversationList_ = new QListWidget(conversationPane);
  conversationList_->setObjectName(QStringLiteral("conversationList"));
  conversationList_->setFrameShape(QFrame::NoFrame);
  conversationList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  conversationList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  conversationList_->setUniformItemSizes(true);
  conversationList_->setItemDelegate(
      new ConversationListDelegate(conversationList_));

  conversationLayout->addLayout(conversationHeader);
  conversationLayout->addWidget(conversationList_, 1);

  auto *chatContentPane = new QWidget(messagesPage_);
  chatContentPane->setObjectName(QStringLiteral("chatContentPane"));
  chatContentPane->setMinimumWidth(UiZoom::s(520));
  auto *chatLayout = new QVBoxLayout(chatContentPane);
  chatLayout->setContentsMargins(0, 0, 0, 0);
  chatLayout->setSpacing(0);

  auto *header = new QWidget(chatContentPane);
  header->setObjectName(QStringLiteral("chatHeader"));
  header->setFixedHeight(UiZoom::s(44));
  auto *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(28, 8, 28, 8);
  headerLayout->setSpacing(12);
  titleLabel_ = new QLabel(header);
  titleLabel_->setObjectName(QStringLiteral("chatTitle"));
  headerLayout->addWidget(titleLabel_, 1);
  // 远程桌面入口：跟着当前会话走——正在跟谁聊天就远程谁，无需另选设备。
  remoteDesktopButton_ = makeHeaderIconButton(
      LineIconKind::Screen, QStringLiteral("远程桌面"), header);
  remoteDesktopButton_->setObjectName(QStringLiteral("remoteDesktopButton"));
  connect(remoteDesktopButton_, &QPushButton::clicked, this,
          &MainWindow::requestRemoteDesktop);
  headerLayout->addWidget(remoteDesktopButton_);
  moreButton_ =
      makeHeaderIconButton(LineIconKind::More, QStringLiteral("更多"), header);
  moreButton_->setObjectName(QStringLiteral("moreButton"));
  connect(moreButton_, &QPushButton::clicked, this, &MainWindow::showMoreMenu);
  headerLayout->addWidget(moreButton_);

  messageScroll_ = new QScrollArea(chatContentPane);
  messageScroll_->setObjectName(QStringLiteral("messageScroll"));
  messageScroll_->setWidgetResizable(true);
  messageScroll_->setFrameShape(QFrame::NoFrame);
  messageScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  // 滚动条常显：按需出现的话，内容一超高视口就变窄、全部气泡按新宽度重排一轮
  // （首访时缓存全未命中），切换成本直接翻倍。常显让排版宽度恒定。样式在 QSS
  // 里收成细条，不会占地方。
  messageScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  messageScroll_->viewport()->installEventFilter(this);
  messageContainer_ = new QWidget(messageScroll_);
  messageContainer_->setObjectName(QStringLiteral("messageContainer"));
  messageLayout_ = new QVBoxLayout(messageContainer_);
  messageLayout_->setObjectName(QStringLiteral("messageLayout"));
  messageLayout_->setContentsMargins(28, 22, 28, 22);
  messageLayout_->setSpacing(14);
  messageScroll_->setWidget(messageContainer_);

  auto *composer = new QWidget(chatContentPane);
  composer->setObjectName(QStringLiteral("composerPanel"));
  composer->setMinimumHeight(UiZoom::s(96));
  auto *composerLayout = new QVBoxLayout(composer);
  composerLayout->setContentsMargins(12, 2, 12, 8);
  composerLayout->setSpacing(6);

  messageEditor_ = new ComposerTextEdit(composer);
  messageEditor_->setObjectName(QStringLiteral("messageEditor"));
  messageEditor_->setPlaceholderText(
      QStringLiteral("输入消息（可拖入文件，或 Ctrl+V 粘贴图片/文件）"));
  messageEditor_->setAcceptRichText(false);
  messageEditor_->setMinimumHeight(UiZoom::s(64));
  messageEditor_->installEventFilter(this);
  // 拖进来的文件走和 Ctrl+V 完全相同的路由，不再被当成 file:/// 文本插入。
  static_cast<ComposerTextEdit *>(messageEditor_)
      ->setMimeHandler([this](const QMimeData *mime) {
        return insertComposerMimeData(mime);
      });

  sendButton_ = new QPushButton(messageEditor_);
  sendButton_->setObjectName(QStringLiteral("sendButton"));
  sendButton_->setIcon(
      makeNavIcon(LineIconKind::Send, QColor(QStringLiteral("#ffffff"))));
  sendButton_->setIconSize(QSize(UiZoom::s(18), UiZoom::s(18)));
  sendButton_->setFixedSize(UiZoom::s(36), UiZoom::s(36));
  sendButton_->setToolTip(QStringLiteral("发送消息"));
  sendButton_->setAccessibleName(QStringLiteral("发送消息"));
  sendButton_->setCursor(Qt::PointingHandCursor);
  static_cast<ComposerTextEdit *>(messageEditor_)->setCornerAction(sendButton_);

  aiReplyButton_ = new QPushButton(messageEditor_);
  aiReplyButton_->setObjectName(QStringLiteral("aiReplyButton"));
  aiReplyButton_->setIcon(
      makeNavIcon(LineIconKind::Assistant, QColor(QStringLiteral("#6254c7"))));
  aiReplyButton_->setIconSize(QSize(UiZoom::s(17), UiZoom::s(17)));
  aiReplyButton_->setFixedSize(UiZoom::s(30), UiZoom::s(30));
  aiReplyButton_->setCursor(Qt::PointingHandCursor);
  aiReplyButton_->setAccessibleName(QStringLiteral("AI 回复"));
  aiReplyButton_->installEventFilter(this);
  aiReplyButton_->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QPushButton#aiReplyButton {
            background: transparent;
            border: none;
            border-radius: 8px;
            padding: 0;
            color: #574ba7;
            font-size: 12px;
            font-weight: 600;
        }
        QPushButton#aiReplyButton:hover { background: #f0edff; }
        QPushButton#aiReplyButton:pressed { background: #e2dcff; }
        QPushButton#aiReplyButton:disabled { background: transparent; }
    )")));
  static_cast<ComposerTextEdit *>(messageEditor_)
      ->setLeadingAction(aiReplyButton_);

  aiReplyHint_ = new QLabel(QStringLiteral("AI 回复"), messageEditor_);
  aiReplyHint_->setObjectName(QStringLiteral("aiReplyHint"));
  aiReplyHint_->setAlignment(Qt::AlignCenter);
  aiReplyHint_->setFixedSize(UiZoom::s(54), UiZoom::s(22));
  aiReplyHint_->setAttribute(Qt::WA_TransparentForMouseEvents);
  aiReplyHint_->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QLabel#aiReplyHint {
            background: #ffffff;
            border: 1px solid #d7dde6;
            border-radius: 4px;
            color: #344054;
            font-size: 11px;
            font-weight: 500;
            padding: 0;
        }
    )")));
  aiReplyHint_->hide();
  static_cast<ComposerTextEdit *>(messageEditor_)->setLeadingHint(aiReplyHint_);

  commandButton_ = new QPushButton(messageEditor_);
  commandButton_->setIcon(
      QIcon(QStringLiteral(":/maichat/icons/terminal-2.svg")));
  commandButton_->setIconSize(QSize(UiZoom::s(18), UiZoom::s(18)));
  commandButton_->setObjectName(QStringLiteral("composerCommandButton"));
  commandButton_->setAccessibleName(QStringLiteral("命令"));
  commandButton_->setCursor(Qt::PointingHandCursor);
  commandButton_->installEventFilter(this);
  commandButton_->setFixedSize(UiZoom::s(30), UiZoom::s(30));
  commandButton_->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
      "QPushButton{background:transparent;border:0;border-radius:8px;color:#"
      "64748b;"
      "font-size:19px;padding:0;}QPushButton:hover{background:#edf2f7;}"
      "QPushButton:pressed{background:#e2e8f0;}")));
  static_cast<ComposerTextEdit *>(messageEditor_)
      ->setSecondaryLeadingAction(commandButton_);
  commandMenu_ = new QMenu(commandButton_);
  commandMenu_->setObjectName(QStringLiteral("composerCommandMenu"));
  applyMessageContextMenuStyle(*commandMenu_);
  for (const SlashCommandDefinition &definition : slashCommandDefinitions()) {
    QAction *action =
        commandMenu_->addAction(definition.command.trimmed() +
                                QStringLiteral("    ") + definition.label);
    action->setObjectName(definition.objectName);
    connect(
        action, &QAction::triggered, this,
        [this, command = definition.command] { sendSlashCommand(command); });
  }

  replySuggestionBar_ = new QWidget(composer);
  replySuggestionBar_->setObjectName(QStringLiteral("replySuggestionBar"));
  auto *suggestionLayout = new QVBoxLayout(replySuggestionBar_);
  suggestionLayout->setContentsMargins(UiZoom::s(10), UiZoom::s(8),
                                       UiZoom::s(8), UiZoom::s(8));
  suggestionLayout->setSpacing(UiZoom::s(6));
  auto *suggestionHeader = new QHBoxLayout();
  suggestionHeader->setContentsMargins(0, 0, 0, 0);
  replySuggestionStatus_ =
      new QLabel(QStringLiteral("正在生成回复建议…"), replySuggestionBar_);
  replySuggestionStatus_->setObjectName(
      QStringLiteral("replySuggestionStatus"));
  auto *refreshSuggestionButton =
      new QPushButton(QStringLiteral("换一批"), replySuggestionBar_);
  refreshSuggestionButton->setObjectName(
      QStringLiteral("refreshReplySuggestions"));
  auto *closeSuggestionButton =
      new QPushButton(QStringLiteral("×"), replySuggestionBar_);
  closeSuggestionButton->setObjectName(QStringLiteral("closeReplySuggestions"));
  closeSuggestionButton->setFixedSize(UiZoom::s(24), UiZoom::s(24));
  suggestionHeader->addWidget(replySuggestionStatus_, 1);
  suggestionHeader->addWidget(refreshSuggestionButton);
  suggestionHeader->addWidget(closeSuggestionButton);
  suggestionLayout->addLayout(suggestionHeader);

  auto *choices = new QHBoxLayout();
  choices->setContentsMargins(0, 0, 0, 0);
  choices->setSpacing(UiZoom::s(8));
  auto makeSuggestionButton = [this, choices](const QString &name,
                                              const QString &label) {
    auto *button = new QPushButton(replySuggestionBar_);
    button->setObjectName(name);
    button->setProperty("styleLabel", label);
    button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    button->setCursor(Qt::PointingHandCursor);
    button->setEnabled(false);
    choices->addWidget(button, 1);
    connect(button, &QPushButton::clicked, this, [this, button] {
      applyReplySuggestion(button->property("replyText").toString());
    });
    return button;
  };
  naturalReplyButton_ = makeSuggestionButton(
      QStringLiteral("naturalReplySuggestion"), QStringLiteral("自然"));
  casualReplyButton_ = makeSuggestionButton(
      QStringLiteral("casualReplySuggestion"), QStringLiteral("轻松"));
  professionalReplyButton_ = makeSuggestionButton(
      QStringLiteral("professionalReplySuggestion"), QStringLiteral("专业"));
  suggestionLayout->addLayout(choices);
  connect(refreshSuggestionButton, &QPushButton::clicked, this,
          &MainWindow::requestReplySuggestions);
  connect(closeSuggestionButton, &QPushButton::clicked, this,
          &MainWindow::clearReplySuggestions);
  replySuggestionBar_->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #replySuggestionBar {
            background: #f8fbff;
            border: 1px solid #cfe5fb;
            border-radius: 10px;
        }
        #replySuggestionStatus {
            color: #52677d;
            font-size: 12px;
            background: transparent;
        }
        #replySuggestionBar QPushButton {
            background: #ffffff;
            border: 1px solid #d8e7f6;
            border-radius: 8px;
            color: #24364b;
            padding: 6px 10px;
            text-align: left;
        }
        #replySuggestionBar QPushButton:hover { border-color: #1597e5; background: #f2f9ff; }
        #replySuggestionBar QPushButton:disabled { color: #8da0b5; background: #f5f8fb; }
        #replySuggestionBar QPushButton#refreshReplySuggestions,
        #replySuggestionBar QPushButton#closeReplySuggestions {
            border: none;
            background: transparent;
            color: #168ad1;
            text-align: center;
            padding: 2px 6px;
        }
    )")));
  replySuggestionBar_->hide();
  composerLayout->addWidget(replySuggestionBar_);

  // 待回复提示条：显示「正在回复谁的哪句话」，右侧 ✕ 取消。
  // 放在输入框**上方**而不是下方——用户视线从提示条落到输入框，顺序才对。
  pendingReplyBar_ = new QWidget(composer);
  pendingReplyBar_->setObjectName(QStringLiteral("pendingReplyBar"));
  auto *replyBarLayout = new QHBoxLayout(pendingReplyBar_);
  replyBarLayout->setContentsMargins(UiZoom::s(10), UiZoom::s(6), UiZoom::s(6),
                                     UiZoom::s(6));
  replyBarLayout->setSpacing(UiZoom::s(8));
  pendingReplyLabel_ = new QLabel(pendingReplyBar_);
  pendingReplyLabel_->setObjectName(QStringLiteral("pendingReplyLabel"));
  pendingReplyLabel_->setWordWrap(false);
  pendingReplyLabel_->setSizePolicy(QSizePolicy::Ignored,
                                    QSizePolicy::Preferred);
  auto *cancelReplyButton =
      new QPushButton(QStringLiteral("×"), pendingReplyBar_);
  cancelReplyButton->setObjectName(QStringLiteral("cancelReplyButton"));
  cancelReplyButton->setCursor(Qt::PointingHandCursor);
  cancelReplyButton->setFixedSize(UiZoom::s(22), UiZoom::s(22));
  cancelReplyButton->setToolTip(QStringLiteral("取消回复"));
  cancelReplyButton->setAccessibleName(QStringLiteral("取消回复"));
  connect(cancelReplyButton, &QPushButton::clicked, this,
          &MainWindow::cancelPendingReply);
  replyBarLayout->addWidget(pendingReplyLabel_, 1);
  replyBarLayout->addWidget(cancelReplyButton);
  pendingReplyBar_->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #pendingReplyBar {
            background: #f1f5f9;
            border: 1px solid #e2e8f0;
            border-radius: 10px;
        }
        #pendingReplyLabel {
            color: #475569;
            font-size: 12px;
            background: transparent;
        }
        QPushButton#cancelReplyButton {
            background: transparent;
            border: none;
            color: #64748b;
            font-size: 15px;
        }
        QPushButton#cancelReplyButton:hover {
            background: #e2e8f0;
            border-radius: 11px;
        }
    )")));
  pendingReplyBar_->hide();
  composerLayout->addWidget(pendingReplyBar_);

  composerLayout->addWidget(messageEditor_, 1);

  auto *messageComposerSplitter = new ComposerResizeSplitter(chatContentPane);
  messageComposerSplitter->setObjectName(
      QStringLiteral("messageComposerSplitter"));
  messageComposerSplitter->setChildrenCollapsible(false);
  messageComposerSplitter->addWidget(messageScroll_);
  messageComposerSplitter->addWidget(composer);
  messageComposerHandle_ = messageComposerSplitter->handle(1);
  messageComposerHandle_->installEventFilter(this);
  messageComposerSplitter->setStretchFactor(0, 1);
  messageComposerSplitter->setStretchFactor(1, 0);
  messageComposerSplitter->setSizes(QList<int>() << 620 << 166);

  chatLayout->addWidget(header);
  diagnosticsStatusLabel_ = new QLabel(chatContentPane);
  diagnosticsStatusLabel_->setObjectName(
      QStringLiteral("remoteDiagnosticsStatus"));
  diagnosticsStatusLabel_->setTextFormat(Qt::PlainText);
  diagnosticsStatusLabel_->setWordWrap(true);
  diagnosticsStatusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  diagnosticsStatusLabel_->setContentsMargins(UiZoom::s(12), UiZoom::s(8),
                                              UiZoom::s(12), UiZoom::s(8));
  diagnosticsStatusLabel_->hide();
  chatLayout->addWidget(diagnosticsStatusLabel_);
  chatLayout->addWidget(messageComposerSplitter, 1);

  contentSplitter->addWidget(conversationPane);
  contentSplitter->addWidget(chatContentPane);
  contentSplitter->setStretchFactor(0, 0);
  contentSplitter->setStretchFactor(1, 1);
  contentSplitter->setSizes(QList<int>() << 320 << 960);
  messagesPageLayout->addWidget(contentSplitter, 1);

  contactsPage_ = new QWidget(contentStack_);
  contactsPage_->setObjectName(QStringLiteral("contactsPage"));
  auto *contactsPageLayout = new QHBoxLayout(contactsPage_);
  contactsPageLayout->setContentsMargins(0, 0, 0, 0);
  contactsPageLayout->setSpacing(0);

  auto *contactsDirectoryPane = new QWidget(contactsPage_);
  contactsDirectoryPane->setObjectName(QStringLiteral("contactsDirectoryPane"));
  contactsDirectoryPane->setMinimumWidth(UiZoom::s(300));
  contactsDirectoryPane->setMaximumWidth(UiZoom::s(420));
  auto *contactsDirectoryLayout = new QVBoxLayout(contactsDirectoryPane);
  contactsDirectoryLayout->setContentsMargins(24, 24, 0, 18);
  contactsDirectoryLayout->setSpacing(16);

  // 通讯录列表不再展示搜索框，联系人检索由 AI 助手完成。
  contactsSearchInput_ = new QLineEdit(contactsDirectoryPane);
  contactsSearchInput_->setObjectName(QStringLiteral("contactsSearchBox"));
  contactsSearchInput_->setPlaceholderText(
      QStringLiteral("搜索联系人 (Ctrl+F)"));
  contactsSearchInput_->setClearButtonEnabled(true);
  contactsSearchInput_->addAction(
      makeLineIcon(LineIconKind::Search, QColor(QStringLiteral("#98a2b3"))),
      QLineEdit::LeadingPosition);
  contactsSearchInput_->hide();

  contactsList_ = new QListWidget(contactsDirectoryPane);
  contactsList_->setObjectName(QStringLiteral("contactsList"));
  contactsList_->setFrameShape(QFrame::NoFrame);
  contactsList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  contactsList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  // 分组表头比联系人行矮，行高不再统一——开着这个优化会让所有行都按第一行的
  // 高度画，表头和联系人会叠在一起。
  contactsList_->setUniformItemSizes(false);
  contactsList_->setItemDelegate(new ContactListDelegate(contactsList_));
  contactsDirectoryLayout->addWidget(contactsList_, 1);

  contactProfileStack_ = new QStackedWidget(contactsPage_);
  contactProfileStack_->setObjectName(QStringLiteral("contactProfileStack"));
  auto *contactHintPane = new QWidget(contactProfileStack_);
  contactHintPane->setObjectName(QStringLiteral("contactHintPane"));
  auto *contactHintLayout = new QVBoxLayout(contactHintPane);
  contactHintLayout->setContentsMargins(36, 36, 36, 36);
  contactHintLayout->setSpacing(10);
  auto *contactHintTitle =
      new QLabel(QStringLiteral("选择联系人查看资料"), contactHintPane);
  contactHintTitle->setObjectName(QStringLiteral("contactHintTitle"));
  auto *contactHintSubtitle = new QLabel(
      QStringLiteral("点击左侧联系人查看资料，再从资料页发消息。"),
      contactHintPane);
  contactHintSubtitle->setObjectName(QStringLiteral("contactHintSubtitle"));
  contactHintLayout->addStretch(1);
  contactHintLayout->addWidget(contactHintTitle, 0, Qt::AlignHCenter);
  contactHintLayout->addWidget(contactHintSubtitle, 0, Qt::AlignHCenter);
  contactHintLayout->addStretch(1);

  auto *contactProfilePane = new QWidget(contactProfileStack_);
  contactProfilePane->setObjectName(QStringLiteral("contactProfilePane"));
  auto *profileLayout = new QVBoxLayout(contactProfilePane);
  profileLayout->setContentsMargins(UiZoom::s(40), UiZoom::s(38),
                                    UiZoom::s(40), UiZoom::s(38));
  profileLayout->setSpacing(UiZoom::s(18));
  contactProfileAvatar_ = new AccountAvatarLabel(contactProfilePane);
  contactProfileAvatar_->setObjectName(QStringLiteral("contactProfileAvatar"));
  contactProfileAvatar_->setFixedSize(UiZoom::s(88), UiZoom::s(88));
  contactProfileAvatar_->setProperty("avatarRequestsEnabled", true);
  profileLayout->addWidget(contactProfileAvatar_, 0, Qt::AlignHCenter);
  contactProfileName_ = new QLabel(contactProfilePane);
  contactProfileName_->setObjectName(QStringLiteral("contactProfileName"));
  contactProfileName_->setAlignment(Qt::AlignCenter);
  contactProfileName_->setWordWrap(true);
  profileLayout->addWidget(contactProfileName_);

  auto *accountCard = new QWidget(contactProfilePane);
  accountCard->setObjectName(QStringLiteral("contactProfileAccountCard"));
  auto *accountLayout = new QVBoxLayout(accountCard);
  accountLayout->setContentsMargins(UiZoom::s(18), UiZoom::s(14),
                                    UiZoom::s(18), UiZoom::s(14));
  auto *accountTitle = new QLabel(QStringLiteral("MaiChat ID"), accountCard);
  accountTitle->setObjectName(QStringLiteral("contactProfileAccountTitle"));
  accountLayout->addWidget(accountTitle);
  auto *accountLine = new QHBoxLayout;
  contactProfileId_ = new QLabel(accountCard);
  contactProfileId_->setObjectName(QStringLiteral("contactProfileAccountId"));
  contactProfileId_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  accountLine->addWidget(contactProfileId_, 1);
  auto *copyAccount = new QPushButton(QStringLiteral("复制"), accountCard);
  copyAccount->setObjectName(QStringLiteral("contactProfileCopyButton"));
  connect(copyAccount, &QPushButton::clicked, this, [this] {
    if (contactProfileId_ && contactProfileId_->isEnabled())
      QApplication::clipboard()->setText(contactProfileId_->text());
  });
  accountLine->addWidget(copyAccount);
  accountLayout->addLayout(accountLine);
  profileLayout->addWidget(accountCard);

  contactProfileMessageButton_ = new QPushButton(QStringLiteral("发消息"), contactProfilePane);
  contactProfileMessageButton_->setObjectName(QStringLiteral("contactProfileMessageButton"));
  connect(contactProfileMessageButton_, &QPushButton::clicked, this, [this] {
    if (selectedContactProfileUserId_.isEmpty()) return;
    app_.selectPeer(selectedContactProfileUserId_);
    showMessagesPage();
  });
  profileLayout->addWidget(contactProfileMessageButton_);
  contactProfileDeleteButton_ = new QPushButton(QStringLiteral("删除好友"), contactProfilePane);
  contactProfileDeleteButton_->setObjectName(QStringLiteral("contactProfileDeleteButton"));
  connect(contactProfileDeleteButton_, &QPushButton::clicked, this, [this] {
    if (selectedContactProfileUserId_.isEmpty()) return;
    const QString name = contactProfileName_->text();
    if (AppMessageDialog::confirm(this, QStringLiteral("删除好友"),
          QStringLiteral("确定将“%1”移出好友列表吗？聊天历史会保留。").arg(name),
          QStringLiteral("删除"), /*destructive=*/true))
      app_.deleteContact(selectedContactProfileUserId_);
  });
  profileLayout->addWidget(contactProfileDeleteButton_);
  profileLayout->addStretch(1);
  contactProfileStack_->addWidget(contactHintPane);
  contactProfileStack_->addWidget(contactProfilePane);
  contactProfileStack_->setCurrentWidget(contactHintPane);

  contactsPageLayout->addWidget(contactsDirectoryPane);
  contactsPageLayout->addWidget(contactProfileStack_, 1);

  settingsPage_ = new QWidget(contentStack_);
  settingsPage_->setObjectName(QStringLiteral("settingsPage"));
  // 设置项只会越加越多。不套滚动区的话，内容一超过页面高度 Qt 就按比例
  // 压扁各行——「被控模式」那三个单选会叠在一起糊成一团（加「允许远程控制」
  // 那行时就压过线了）。
  auto *settingsPageLayout = new QVBoxLayout(settingsPage_);
  settingsPageLayout->setContentsMargins(0, 0, 0, 0);
  settingsPageLayout->setSpacing(0);
  auto *settingsScroll = new QScrollArea(settingsPage_);
  settingsScroll->setObjectName(QStringLiteral("settingsScroll"));
  settingsScroll->setWidgetResizable(true);
  settingsScroll->setFrameShape(QFrame::NoFrame);
  settingsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  settingsPageLayout->addWidget(settingsScroll);

  auto *settingsContent = new QWidget(settingsScroll);
  settingsContent->setObjectName(QStringLiteral("settingsContent"));
  settingsScroll->setWidget(settingsContent);
  auto *settingsLayout = new QVBoxLayout(settingsContent);
  settingsLayout->setContentsMargins(36, 30, 36, 30);
  settingsLayout->setSpacing(18);

  auto *settingsTitle = new QLabel(QStringLiteral("设置"), settingsContent);
  settingsTitle->setObjectName(QStringLiteral("pageTitle"));
  auto *settingsSubtitle =
      new QLabel(QStringLiteral("管理 MaiChat 账号和这台电脑的登录。"),
                 settingsContent);
  settingsSubtitle->setObjectName(QStringLiteral("settingsSubtitle"));

  auto *profileCard = new QWidget(settingsContent);
  profileCard->setObjectName(QStringLiteral("selfProfileCard"));
  auto *selfProfileLayout = new QHBoxLayout(profileCard);
  selfProfileLayout->setContentsMargins(UiZoom::s(18), UiZoom::s(16),
                                       UiZoom::s(18), UiZoom::s(16));
  selfProfileLayout->setSpacing(UiZoom::s(16));
  settingsProfileAvatar_ = new AccountAvatarLabel(profileCard);
  settingsProfileAvatar_->setObjectName(QStringLiteral("selfProfileAvatar"));
  settingsProfileAvatar_->setProperty("avatarRequestsEnabled", true);
  settingsProfileAvatar_->setFixedSize(UiZoom::s(60), UiZoom::s(60));
  selfProfileLayout->addWidget(settingsProfileAvatar_);
  auto *profileText = new QWidget(profileCard);
  auto *profileTextLayout = new QVBoxLayout(profileText);
  profileTextLayout->setContentsMargins(0, 0, 0, 0);
  profileTextLayout->setSpacing(UiZoom::s(5));
  settingsProfileName_ = new QLabel(profileText);
  settingsProfileName_->setObjectName(QStringLiteral("selfProfileName"));
  settingsProfileId_ = new QLabel(profileText);
  settingsProfileId_->setObjectName(QStringLiteral("selfProfileId"));
  profileTextLayout->addWidget(settingsProfileName_);
  profileTextLayout->addWidget(settingsProfileId_);
  auto *connectionRow = new QWidget(profileText);
  auto *connectionLayout = new QHBoxLayout(connectionRow);
  connectionLayout->setContentsMargins(0, 0, 0, 0);
  connectionLayout->setSpacing(UiZoom::s(6));
  statusLabel_ = new QLabel(connectionRow);
  statusLabel_->setObjectName(QStringLiteral("connectionStatusDot"));
  statusLabel_->setFixedSize(UiZoom::s(9), UiZoom::s(9));
  statusLabel_->setScaledContents(false);
  statusLabel_->setAccessibleName(QStringLiteral("IM 连接状态"));
  connectionLayout->addWidget(statusLabel_);
  settingsConnectionText_ = new QLabel(connectionRow);
  settingsConnectionText_->setObjectName(QStringLiteral("selfConnectionStatus"));
  connectionLayout->addWidget(settingsConnectionText_);
  connectionLayout->addStretch(1);
  profileTextLayout->addWidget(connectionRow);
  selfProfileLayout->addWidget(profileText, 1);

  auto *settingsPanel = new QWidget(settingsContent);
  settingsPanel->setObjectName(QStringLiteral("settingsPanel"));
  auto *settingsPanelLayout = new QVBoxLayout(settingsPanel);
  // 上下留白：边距为 0 时首行标题会紧贴面板上边框，末行也贴着下边框。
  // 左右仍为 0，让行的分隔线横贯整个面板宽度。
  settingsPanelLayout->setContentsMargins(0, UiZoom::s(8), 0, UiZoom::s(8));
  settingsPanelLayout->setSpacing(0);

  settingsAccountValue_ = new QLabel(settingsPanel);
  settingsAccountValue_->setObjectName(QStringLiteral("settingsAccountValue"));
  settingsNicknameValue_ = new QLabel(settingsPanel);
  settingsNicknameValue_->setObjectName(QStringLiteral("settingsNicknameValue"));
  settingsPanelLayout->addWidget(
      createSettingsRow(QStringLiteral("当前账号"), settingsAccountValue_,
                        QStringLiteral("MaiChat 账号 ID；与昵称分别显示。")));
  auto* nicknameRow = createSettingsRow(
      QStringLiteral("昵称"), settingsNicknameValue_,
      QStringLiteral("MaiChat 账号的昵称；留空时显示 MaiChat ID。"));
  settingsNicknameButton_ = new QPushButton(QStringLiteral("修改"), nicknameRow);
  settingsNicknameButton_->setObjectName(QStringLiteral("settingsNicknameButton"));
  settingsNicknameButton_->setProperty("settingsRowButton", true);
  settingsNicknameButton_->setEnabled(false);
  qobject_cast<QHBoxLayout*>(nicknameRow->layout())->addWidget(settingsNicknameButton_);
  connect(settingsNicknameButton_, &QPushButton::clicked, this, [this] {
    if (!maiChatNicknameUpdater_) return;
    AppTextInputDialog::Options options;
    options.title = QStringLiteral("修改昵称");
    options.description = QStringLiteral("最多 32 个字符；留空后显示 MaiChat ID。");
    options.placeholder = QStringLiteral("昵称");
    options.initialText = nicknameDraftPending_ ? pendingNicknameDraft_ :
        (maiChatNicknameProvider_ ? maiChatNicknameProvider_() : QString());
    options.confirmText = QStringLiteral("保存");
    bool accepted = false;
    const QString nickname = AppTextInputDialog::getText(this, options, &accepted).trimmed();
    if (!accepted) return;
    if (nickname.toUcs4().size() > 32 || nickname.toUtf8().size() > 120) {
      pendingNicknameDraft_ = nickname;
      nicknameDraftPending_ = true;
      AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                             QStringLiteral("昵称格式不符合要求"),
                             QStringLiteral("昵称最多 32 个字符，且不能超过 120 字节。"));
      return;
    }
    pendingNicknameDraft_ = nickname;
    nicknameDraftPending_ = true;
    settingsNicknameButton_->setEnabled(false);
    QPointer<MainWindow> self(this);
    maiChatNicknameUpdater_(nickname, [self](bool ok, const QString& error) {
      if (!self) return;
      self->settingsNicknameButton_->setEnabled(true);
      if (!ok) {
        AppMessageDialog::show(self, AppMessageDialog::Kind::Warning,
                               QStringLiteral("昵称保存失败"), error);
        return;
      }
      self->nicknameDraftPending_ = false;
      self->pendingNicknameDraft_.clear();
      self->refreshSettings();
    });
  });
  settingsPanelLayout->addWidget(nicknameRow);
  auto *logoutValue = new QLabel(QStringLiteral("当前电脑"), settingsPanel);
  auto *logoutRow = createSettingsRow(
      QStringLiteral("桌面登录"), logoutValue,
      QStringLiteral("退出后可用手机扫码或 MaiChat 账号密码重新登录。"));
  settingsLogoutButton_ = new QPushButton(QStringLiteral("退出登录 / 切换账号"), logoutRow);
  settingsLogoutButton_->setObjectName(QStringLiteral("settingsLogoutButton"));
  settingsLogoutButton_->setProperty("settingsRowButton", true);
  settingsLogoutButton_->setEnabled(false);
  qobject_cast<QHBoxLayout *>(logoutRow->layout())
      ->addWidget(settingsLogoutButton_);
  connect(settingsLogoutButton_, &QPushButton::clicked, this, [this] {
    if (!logoutHandler_) return;
    if (!AppMessageDialog::confirm(
            this, QStringLiteral("退出登录"),
            QStringLiteral("将撤销此电脑的登录并关闭 MaiChat。下次打开可重新登录。"),
            QStringLiteral("退出登录"), true))
      return;
    settingsLogoutButton_->setEnabled(false);
    logoutHandler_();
  });
  settingsPanelLayout->addWidget(logoutRow);

  settingsLayout->addWidget(settingsTitle);
  settingsLayout->addWidget(settingsSubtitle);
  settingsLayout->addWidget(profileCard);
  cloudSettingsValue_ = new QLabel(QStringLiteral("私人空间"), settingsContent);
  cloudSettingsRow_ = createSettingsRow(
      QStringLiteral("视频云盘"), cloudSettingsValue_,
      QStringLiteral("跨设备保存和播放视频；单个最多 2 GB，账号空间 10 GB。"));
  cloudSettingsRow_->hide();
  cloudSettingsButton_ = new QPushButton(QStringLiteral("打开"), cloudSettingsRow_);
  cloudSettingsButton_->setObjectName(QStringLiteral("settingsCloudDriveOpen"));
  cloudSettingsButton_->setProperty("settingsRowButton", true);
  qobject_cast<QHBoxLayout*>(cloudSettingsRow_->layout())->addWidget(cloudSettingsButton_);
  connect(cloudSettingsButton_, &QPushButton::clicked, this, [this] {
    if (cloudDriveAvailable_) openCloudDrive();
    else probeCloudDriveAvailability();
  });
  settingsLayout->addWidget(cloudSettingsRow_);
  settingsLayout->addWidget(settingsPanel);
  settingsLayout->addSpacing(UiZoom::s(24));
  settingsLayout->addWidget(buildRemoteDesktopSettingsPanel(settingsContent));
  settingsLayout->addStretch(1);

  // 远程桌面页：画面在应用内展示，不弹独立窗口。
  remotePage_ = new QWidget(contentStack_);
  remotePage_->setObjectName(QStringLiteral("remotePage"));
  auto *remoteLayout = new QVBoxLayout(remotePage_);
  remoteLayout->setContentsMargins(0, 0, 0, 0);
  remoteLayout->setSpacing(0);
  remoteDesktopView_ = new RemoteDesktopViewPanel(remotePage_);
  remoteLayout->addWidget(remoteDesktopView_);
  connect(remoteDesktopView_, &RemoteDesktopViewPanel::fullScreenChanged, this,
          &MainWindow::applyRemoteDesktopFullScreen);
  connect(remoteDesktopView_, &RemoteDesktopViewPanel::controlToggleRequested,
          this, &MainWindow::toggleRemoteDesktopControl);

  // AI 助手页。整页就是一个 AgentChatPanel，MainWindow 只负责把它放进来，
  // 不参与它内部的任何事——这样将来要摘掉它，删这几行就够。
  agentPage_ = new QWidget(contentStack_);
  {
    // 和消息页一样的两栏：左边会话列表，右边内容。这里不复用消息页那套——
    // 那边一项是"一个人"（头像、未读数、在线状态），这边一项是"一段工作"
    // （标题、工作目录），硬凑到一起两边都别扭。
    auto *agentLayout = new QHBoxLayout(agentPage_);
    agentLayout->setContentsMargins(0, 0, 0, 0);
    agentLayout->setSpacing(0);

    auto *agentSplitter = new QSplitter(Qt::Horizontal, agentPage_);
    agentSplitter->setObjectName(QStringLiteral("agentContentSplitter"));
    agentSplitter->setChildrenCollapsible(false);
    agentSplitter->setHandleWidth(1);
    agentLayout->addWidget(agentSplitter);

    rebuildAgentPage();
  }

  contentStack_->addWidget(messagesPage_);
  contentStack_->addWidget(contactsPage_);
  contentStack_->addWidget(remotePage_);
  contentStack_->addWidget(settingsPage_);
  // **排在最后**。QStackedWidget 默认显示第 0 个，插在 messagesPage_ 前面会让
  // 应用一启动就停在 AI 页——原来的默认页就被我改掉了。
  contentStack_->addWidget(agentPage_);

  rootNavigationSplitter->addWidget(navRail_);
  rootNavigationSplitter->addWidget(contentArea);
  rootNavigationSplitter->setStretchFactor(0, 0);
  rootNavigationSplitter->setStretchFactor(1, 1);
  // 第一格给导航栏的实际定宽。原先写死 180，是导航栏还带文字时的宽度：
  // 收窄成纯图标条之后，多出来的那截就成了导航栏和内容之间一条空白列。
  rootNavigationSplitter->setSizes(QList<int>() << UiZoom::s(64) << 1100);
  rootLayout->addWidget(rootNavigationSplitter, 1);
}

void MainWindow::applyStyle() {
  setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QMainWindow, #root {
            background: #f6f9fc;
            color: #172033;
            font-size: 14px;
        }
        #navLogoContainer {
            background: transparent;
        }
        #navLogo {
            min-width: 34px;
            max-width: 34px;
            min-height: 34px;
            max-height: 34px;
            background: transparent;
        }
        /* 形状和颜色都由 statusDotPixmap 画。 */
        #connectionStatusDot {
            background: transparent;
            border: 0;
        }
        #addConversationButton {
            min-width: 30px;
            max-width: 30px;
            min-height: 30px;
            border: 1px solid #dbe4ef;
            border-radius: 8px;
            background: #ffffff;
            padding: 0;
        }
        #addConversationButton:hover {
            border-color: #8ed0ff;
            background: #f2f9ff;
        }
        #globalSearchBox, #contactsSearchBox {
            min-height: 34px;
            border: 1px solid #dbe6f3;
            border-radius: 9px;
            background: #ffffff;
            color: #172033;
            padding: 0 8px;
            font-size: 13px;
        }
        #globalSearchBox:focus, #contactsSearchBox:focus {
            border-color: #8ed0ff;
        }
        #globalSearchResults {
            background: #ffffff;
            border: 1px solid #dbe4ef;
            border-radius: 10px;
            padding: 4px;
            font-size: 13px;
        }
        #globalSearchResults::item {
            border-radius: 8px;
            padding: 6px 10px;
            color: #33415a;
        }
        #globalSearchResults::item:selected, #globalSearchResults::item:hover {
            background: #e6eef8;
            color: #0b67b7;
        }
        /* 纯图标：不再留文字的左内边距，图标居中，宽高一致，
           否则一列图标会因为各自的留白不同而看起来大小不一。 */
        #messagesNavButton, #contactsNavButton, #remoteNavButton, #settingsNavButton,
        #agentNavButton {
            min-width: 44px;
            max-width: 44px;
            min-height: 44px;
            max-height: 44px;
            border: 0;
            border-radius: 10px;
            padding: 0;
            background: transparent;
        }
        #messagesNavButton[selected="false"]:hover { background: #d9edfc; }
        #agentNavButton[selected="false"]:hover { background: #e7e1ff; }
        #contactsNavButton[selected="false"]:hover { background: #daf1e8; }
        #remoteNavButton[selected="false"]:hover { background: #dfe7f3; }
        #settingsNavButton[selected="false"]:hover { background: #dfe6ef; }
        #messagesNavButton[selected="true"], #contactsNavButton[selected="true"],
        #remoteNavButton[selected="true"], #settingsNavButton[selected="true"],
        #agentNavButton[selected="true"] {
            border: 0;
            background: transparent;
        }
        #navLogoContainer {
            border: 0;
            border-radius: 10px;
            padding: 0;
            background: transparent;
        }
        #navLogoContainer:hover { background: #e9eef5; }
        #conversationPane {
            background: #f8fafc;
            border: 0;
        }
        #conversationSectionTitle {
            color: #172033;
            font-size: 17px;
            font-weight: 600;
        }
        #friendRequestsButton {
            min-height: 30px;
            border: 1px solid #d8e5f1;
            border-radius: 8px;
            background: #ffffff;
            color: #45627f;
            font-size: 12px;
            padding: 0 10px;
        }
        #friendRequestsButton:hover { background: #e9f2fb; }
        #pageTitle {
            color: #101828;
            font-size: 16px;
            font-weight: 600;
        }
        QPushButton[chatHeaderIcon="true"] {
            min-width: 28px;
            max-width: 28px;
            min-height: 28px;
            max-height: 28px;
            border: 0;
            border-radius: 8px;
            background: transparent;
        }
        #addConversationButton {
            min-width: 34px;
            max-width: 34px;
            min-height: 34px;
            max-height: 34px;
            border: 0;
            border-radius: 17px;
            background: transparent;
        }
        QPushButton[chatHeaderIcon="true"]:hover, #addConversationButton:hover {
            background: #e9eef5;
        }
        #conversationList {
            background: transparent;
            outline: 0;
        }
        #conversationList::item {
            min-height: 72px;
            padding: 0;
            color: #344054;
        }
        #conversationList::item:selected {
            background: #e1f5ff;
            border: 0;
        }
        #contactsPage, #settingsPage, #settingsContent, #settingsScroll {
            background: #ffffff;
        }
        #contactsDirectoryPane {
            background: #f8fafc;
            border-right: 1px solid #dae4f0;
        }
        #contactHintPane, #contactProfilePane, #contactProfileStack {
            background: #ffffff;
        }
        #contactProfileName {
            color: #172033;
            font-size: 22px;
            font-weight: 600;
        }
        #contactProfileAccountCard {
            background: #f8fafc;
            border: 1px solid #dae4f0;
            border-radius: 12px;
        }
        #contactProfileAccountTitle { color: #66758d; font-size: 12px; }
        #contactProfileAccountId { color: #172033; font-size: 14px; }
        #contactProfileCopyButton { color: #0985d7; border: 0; background: transparent; }
        #contactProfileMessageButton {
            background: #078ce3; color: #ffffff; border: 0;
            border-radius: 12px; min-height: 44px; font-weight: 600;
        }
        #contactProfileDeleteButton {
            background: #ffffff; color: #dc3535; border: 1px solid #dae4f0;
            border-radius: 12px; min-height: 44px;
        }
        #contactsList {
            background: transparent;
            outline: 0;
        }
        #contactsList::item {
            min-height: 52px;
            padding: 0;
            color: #344054;
        }
        #contactsList::item:selected {
            background: #e1f5ff;
            border: 0;
        }
        /* 左侧两个列表的纵向滚动条收窄到 8px。Qt 默认那条约 17px，
           再乘上界面缩放就更宽——用户看到的就是行右边一整条灰带。
           这里只改这两个列表：消息区和其它滚动条不动。
           仍然是 Qt 原生滚动条（没有换成覆盖式控件），所以拖动滑块、
           滚轮、点轨道翻页这些行为都保持原样。
           px 值会被 UiZoom::scaleQss 按当前缩放换算，不写死物理像素。 */
        #conversationList QScrollBar:vertical, #contactsList QScrollBar:vertical {
            background: transparent;
            width: 8px;
            margin: 0;
        }
        #conversationList QScrollBar::handle:vertical,
        #contactsList QScrollBar::handle:vertical {
            background: #d3dae4;
            border-radius: 4px;
            min-height: 24px;
        }
        #conversationList QScrollBar::handle:vertical:hover,
        #contactsList QScrollBar::handle:vertical:hover {
            background: #b9c3d1;
        }
        /* 上下箭头按钮置零，否则 8px 宽度下它们会挤成两个色块。 */
        #conversationList QScrollBar::add-line:vertical,
        #conversationList QScrollBar::sub-line:vertical,
        #contactsList QScrollBar::add-line:vertical,
        #contactsList QScrollBar::sub-line:vertical {
            height: 0;
            border: 0;
            background: transparent;
        }
        #conversationList QScrollBar::add-page:vertical,
        #conversationList QScrollBar::sub-page:vertical,
        #contactsList QScrollBar::add-page:vertical,
        #contactsList QScrollBar::sub-page:vertical {
            background: transparent;
        }
        #contactHintTitle {
            color: #101828;
            font-size: 16px;
            font-weight: 600;
        }
        #contactHintSubtitle, #settingsSubtitle {
            color: #667085;
            font-size: 13px;
        }
        #selfProfileCard {
            background: #ffffff;
            border: 1px solid #dae4f0;
            border-radius: 12px;
        }
        #selfProfileName { color: #172033; font-size: 20px; font-weight: 600; }
        #selfProfileId { color: #66758d; font-size: 13px; }
        #selfConnectionStatus { color: #667085; font-size: 12px; }
        #selfConnectionStatus[connected="true"] { color: #14805b; }
        #settingsPanel {
            background: #ffffff;
            border: 1px solid #dae4f0;
            border-radius: 8px;
        }
        /* 左边距跟各行标题（行内边距 18px）对齐；上边距不能是 0，
           否则标题直接贴在面板上边框上，看着像挤在线上。 */
        #settingsSectionTitle {
            color: #0f172a;
            font-size: 15px;
            font-weight: 700;
            padding: 6px 0 12px 18px;
        }
        #settingsRadio {
            color: #334155;
            font-size: 13px;
            spacing: 8px;
        }
        #settingsRowButton {
            background: #f1f5f9;
            border: 1px solid #d9e1ec;
            border-radius: 8px;
            color: #1e40af;
            font-size: 13px;
            font-weight: 700;
            padding: 6px 16px;
        }
        #settingsRowButton:hover {
            background: #e2e8f0;
        }
        /* 背景必须透明：面板布局边距为 0，行控件正好铺在面板那 1px 边框上，
           行一旦有不透明背景就会把左右竖边整段盖掉（只剩标题区那一小截还在）。
           白底由 #settingsPanel 提供，行只负责底部分隔线。 */
        #settingsRow {
            background: transparent;
            border-bottom: 1px solid #edf2f7;
        }
        #settingsRowTitle {
            color: #172033;
            font-size: 14px;
            font-weight: 600;
        }
        #settingsRowHelper {
            color: #667085;
            font-size: 12px;
        }
        #settingsAccountValue {
            color: #172033;
            font-size: 14px;
            font-weight: 500;
        }
        #chatContentPane {
            background: #ffffff;
        }
        #chatHeader {
            background: #ffffff;
            border-bottom: 1px solid #f1f2f4;
        }
        #chatTitle {
            color: #101828;
            font-size: 16px;
            font-weight: 600;
        }
        #messageScroll, #messageContainer {
            background: #ffffff;
        }
        /* 消息区滚动条常显（见构造处的说明），样式与左侧列表一致：细条、透明轨道、
           无箭头按钮，常显也不碍眼。 */
        #messageScroll QScrollBar:vertical {
            background: transparent;
            width: 8px;
            margin: 0;
        }
        #messageScroll QScrollBar::handle:vertical {
            background: #d3dae4;
            border-radius: 4px;
            min-height: 24px;
        }
        #messageScroll QScrollBar::handle:vertical:hover {
            background: #b9c3d1;
        }
        #messageScroll QScrollBar::add-line:vertical,
        #messageScroll QScrollBar::sub-line:vertical {
            height: 0;
            border: 0;
            background: transparent;
        }
        #messageScroll QScrollBar::add-page:vertical,
        #messageScroll QScrollBar::sub-page:vertical {
            background: transparent;
        }
        #composerPanel {
            background: #ffffff;
            border: 0;
        }
        #messageEditor {
            border: 1px solid #d1d5db;
            border-radius: 14px;
            background: #ffffff;
            color: #172033;
            padding: 10px 52px 46px 13px;
            font-size: 14px;
        }
        #messageEditor:focus {
            border-color: #b5bbc4;
        }
        #sendButton {
            border-radius: 18px;
            border: 0;
            background: #168eea;
            padding: 0;
        }
        #sendButton:hover {
            background: #087ed2;
        }
        #sendButton:pressed {
            background: #066db7;
        }
        #sendButton:disabled {
            background: #c4def0;
        }
        QSplitter::handle {
            background: transparent;
        }
        QSplitter::handle:horizontal {
            width: 1px;
        }
        QSplitter::handle:vertical {
            height: 1px;
        }
        QSplitter::handle:hover {
            background: transparent;
        }
    )")));
}

void MainWindow::bindSignals() {
  connect(&app_, &RemoteIMApplication::selfProfileChanged, this, [this] {
    if (auto *logo = findChild<QLabel *>(QStringLiteral("navLogo"))) {
      static_cast<AccountAvatarLabel *>(logo)->setProfile(app_.selfProfile());
      requestAvatarPixmap(app_.selfProfile().avatarUrl, logo);
    }
    refreshSettings();
  });
  // 整体缩放（飞书式）：Ctrl+= / Ctrl++（小键盘）放大，Ctrl+- 缩小，Ctrl+0
  // 复位。
  connect(new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_Equal), this),
          &QShortcut::activated, this,
          [this] { changeUiZoom(UiZoom::step()); });
  connect(new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_Plus), this),
          &QShortcut::activated, this,
          [this] { changeUiZoom(UiZoom::step()); });
  connect(new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_Minus), this),
          &QShortcut::activated, this,
          [this] { changeUiZoom(-UiZoom::step()); });
  connect(new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_0), this),
          &QShortcut::activated, this, [this] { resetUiZoom(); });

  connect(&app_, &RemoteIMApplication::stateChanged, this,
          [this] { refresh(); });
  connect(&app_, &RemoteIMApplication::selectionChanged, this,
          [this](const QString &) { refreshSelectedConversation(); });
  connect(
      &app_, &RemoteIMApplication::connectionChanged, this,
      [this](bool connected) {
        auto *logo = findChild<QLabel *>(QStringLiteral("navLogo"));
        if (logo)
          logo->setProperty("avatarRequestsEnabled", connected);
        if (conversationList_) {
          conversationList_->setProperty("avatarRequestsEnabled", connected);
          conversationList_->viewport()->setProperty("avatarRequestsEnabled",
                                                     connected);
        }
        if (contactsList_) {
          contactsList_->setProperty("avatarRequestsEnabled", connected);
          contactsList_->viewport()->setProperty("avatarRequestsEnabled",
                                                 connected);
        }
        if (connected) {
          failedAvatarUrls().clear();
          requestAvatarPixmap(app_.selfProfile().avatarUrl, logo);
          for (const RemoteIMContact &contact : app_.chatState().contacts()) {
            requestAvatarPixmap(contact.avatarUrl, conversationList_);
            requestAvatarPixmap(contact.avatarUrl, contactsList_);
          }
        }
        updateConnectionIndicator();
        refreshSettings();
      });
  connect(
      &app_, &RemoteIMApplication::errorMessage, this,
      [this](const QString &message) {
        if (QCoreApplication::arguments().contains(QStringLiteral("--smoke")))
          return;
        AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                               QStringLiteral("IM"), message);
      });
  connect(&app_, &RemoteIMApplication::friendRequestSent, this, [this] {
    showToast(QStringLiteral("好友申请已发送，等待对方同意"), 14, 3000);
  });
  connect(addContactButton_, &QPushButton::clicked, this,
          [this] { openAddContactDialog(); });
  connect(friendRequestsButton_, &QPushButton::clicked, this,
          [this] { openFriendRequestsDialog(); });
  // 搜索一次要扫所有会话的消息，绝不能挂在每次按键上跑：会话上千条时输入会发涩。
  // 与命令提示条同一套做法——停顿 150ms 才真正执行，输入法组词期间一律不跑。
  globalSearchUpdateTimer_ = new QTimer(this);
  globalSearchUpdateTimer_->setSingleShot(true);
  globalSearchUpdateTimer_->setInterval(150);
  connect(globalSearchUpdateTimer_, &QTimer::timeout, this, [this] {
    // 必须先搜索再过滤：过滤要用搜索算出的 peersWithSearchHits_，
    // 反过来的话会话列表用的是上一次的结果，看着像「少了一个会话」。
    refreshGlobalSearchResults();
    applyConversationFilter();
  });
  connect(navSearchInput_, &QLineEdit::textChanged, this, [this] {
    // 清空是个例外：立刻收起结果、恢复会话列表，不该让人等 150ms。
    if (navSearchInput_->text().trimmed().isEmpty()) {
      globalSearchUpdateTimer_->stop();
      refreshGlobalSearchResults();
      applyConversationFilter();
      return;
    }
    globalSearchUpdateTimer_->start();
  });
  connect(globalSearchResults_, &QListWidget::itemClicked, this,
          &MainWindow::openGlobalSearchResult);
  connect(globalSearchResults_, &QListWidget::itemActivated, this,
          &MainWindow::openGlobalSearchResult);
  // 回车直接打开第一条命中，不必先用方向键选中。
  connect(navSearchInput_, &QLineEdit::returnPressed, this, [this] {
    if (!globalSearchResults_ || globalSearchResults_->count() == 0)
      return;
    QListWidgetItem *first = globalSearchResults_->currentItem();
    if (!first)
      first = globalSearchResults_->item(0);
    openGlobalSearchResult(first);
  });
  // 联系人搜索不用防抖：它只扫好友表（几十条量级）并且是纯字符串比对，
  // 不像消息搜索那样要扫遍每个会话的全部消息，挂在按键上也不会发涩。
  connect(contactsSearchInput_, &QLineEdit::textChanged, this,
          [this] { applyContactFilter(); });
  auto *globalSearchEscape =
      new QShortcut(QKeySequence(Qt::Key_Escape), navSearchInput_);
  globalSearchEscape->setContext(Qt::WidgetWithChildrenShortcut);
  connect(globalSearchEscape, &QShortcut::activated, this,
          &MainWindow::closeGlobalSearchResults);
  auto *contactsSearchEscape =
      new QShortcut(QKeySequence(Qt::Key_Escape), contactsSearchInput_);
  contactsSearchEscape->setContext(Qt::WidgetWithChildrenShortcut);
  connect(contactsSearchEscape, &QShortcut::activated, this,
          [this] { contactsSearchInput_->clear(); });
  connect(messageNavButton_, &QPushButton::clicked, this,
          [this] { showMessagesPage(); });
  connect(contactsNavButton_, &QPushButton::clicked, this,
          [this] { showContactsPage(); });
  connect(remoteNavButton_, &QPushButton::clicked, this,
          [this] { showRemotePage(); });
  connect(settingsNavButton_, &QPushButton::clicked, this,
          [this] { showSettingsPage(); });
  connect(selfProfileButton_, &QPushButton::clicked, this,
          [this] { showSettingsPage(); });
  connect(agentNavButton_, &QPushButton::clicked, this,
          [this] { showAgentPage(); });
  connect(contentStack_, &QStackedWidget::currentChanged, this,
          [this] { syncNavigationSelection(); });
  connect(sendButton_, &QPushButton::clicked, this,
          [this] { sendCurrentText(); });
  connect(aiReplyButton_, &QPushButton::clicked, this,
          &MainWindow::requestReplySuggestions);
  connect(replySuggestionController_,
          &ReplySuggestionController::suggestionsReady, this,
          [this](quint64 requestId, const QString &accountId,
                 const QString &peerId, const QString &latestMessageId,
                 const QString &natural, const QString &casual,
                 const QString &professional) {
            if (requestId != activeReplySuggestionRequest_ ||
                accountId != app_.chatState().ownerUserId() ||
                peerId != app_.chatState().selectedPeerId() ||
                latestMessageId != latestSelectedMessageId()) {
              return;
            }
            replySuggestionStatus_->setText(
                QStringLiteral("选择一条填入输入框，仍需手动发送"));
            const auto update = [](QPushButton *button, const QString &text) {
              const QString label = button->property("styleLabel").toString();
              button->setProperty("replyText", text);
              button->setText(QStringLiteral("%1 · %2").arg(label, text));
              button->setToolTip(text);
              button->setEnabled(true);
            };
            update(naturalReplyButton_, natural);
            update(casualReplyButton_, casual);
            update(professionalReplyButton_, professional);
          });
  connect(replySuggestionController_,
          &ReplySuggestionController::suggestionsFailed, this,
          [this](quint64 requestId, const QString &accountId,
                 const QString &peerId, const QString &latestMessageId,
                 const QString &message) {
            if (requestId != activeReplySuggestionRequest_ ||
                accountId != app_.chatState().ownerUserId() ||
                peerId != app_.chatState().selectedPeerId() ||
                latestMessageId != latestSelectedMessageId()) {
              return;
            }
            replySuggestionStatus_->setText(message);
          });
  connect(messageEditor_, &QTextEdit::textChanged, this, [this] {
    RemoteDiagnostics::PerformanceSpan performance("composer-change");
    updateComposerState();
    app_.setHumanTypingActive(messageEditor_->hasFocus() &&
                              !messageEditor_->toPlainText().isEmpty());
  });
  connect(&app_, &RemoteIMApplication::activityChanged, this,
          [this](const QString &peerId) {
            if (peerId != app_.chatState().selectedPeerId())
              return;
            updateActivityBubble();
          });
  conversationList_->installEventFilter(this);
  contactsList_->installEventFilter(this);
  conversationList_->setContextMenuPolicy(Qt::CustomContextMenu);
  contactsList_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(conversationList_, &QListWidget::customContextMenuRequested, this,
          [this](const QPoint &pos) { showConversationContextMenu(pos); });
  connect(contactsList_, &QListWidget::customContextMenuRequested, this,
          [this](const QPoint &pos) {
            showContactContextMenu(contactsList_, pos);
          });
  connect(conversationList_, &QListWidget::currentItemChanged, this,
          [this](QListWidgetItem *current) {
            if (!current)
              return;
            const QString userId = current->data(Qt::UserRole).toString();
            if (userId.isEmpty())
              return;
            if (userId != app_.chatState().selectedPeerId()) {
              app_.selectPeer(userId);
            }
            // 搜索状态下，左列被过滤成「有命中的会话」。点进去只打开会话而不定位，
            // 等于筛出来了还得自己找——和右侧结果面板点进去的行为也不一致。
            const QString needle =
                navSearchInput_ ? navSearchInput_->text().trimmed() : QString();
            if (needle.isEmpty())
              return;
            const QString hitId = bestSearchHitId(userId, needle);
            if (hitId.isEmpty())
              return;
            // 切会话会整屏重建气泡，高度要等一次布局才定下来；和结果面板走同一套延后定位。
            QTimer::singleShot(0, this, [this, userId, hitId] {
              if (app_.chatState().selectedPeerId() == userId)
                highlightMessage(hitId);
            });
          });
  auto openContactProfile = [this](QListWidgetItem *item) {
    if (!item)
      return;
    if (item->data(IsGroupHeaderRole).toBool()) {
      // 点表头就是折叠/展开。整行都可点，不必去瞄那个小三角。
      toggleContactGroupCollapsed(item->data(GroupNameRole).toString());
      return;
    }
    const QString userId = item->data(Qt::UserRole).toString();
    if (userId.isEmpty())
      return;
    selectedContactProfileUserId_ = userId;
    refreshContactProfile();
  };
  connect(contactsList_, &QListWidget::itemClicked, this,
          openContactProfile);
  connect(contactsList_, &QListWidget::itemActivated, this,
          openContactProfile);
}

void MainWindow::refresh() {
  refreshContacts();
  refreshContactDirectory();
  refreshSettings();
  refreshMessages();
  updateRemoteDesktopButton();
}

void MainWindow::refreshSelectedConversation() {
  const QString selectedPeer = app_.chatState().selectedPeerId();
  {
    QSignalBlocker blocker(conversationList_);
    for (int row = 0; row < conversationList_->count(); ++row) {
      QListWidgetItem *item = conversationList_->item(row);
      if (item->data(UserIdRole).toString() != selectedPeer)
        continue;
      if (conversationList_->currentItem() != item)
        conversationList_->setCurrentItem(item);
      if (item->data(UnreadRole).toInt() != 0)
        item->setData(UnreadRole, 0);
      break;
    }
  }
  conversationList_->viewport()->update();
  refreshMessages();
  updateRemoteDesktopButton();
  if (appIsForegroundVisible())
    notificationTracker_.clear(selectedPeer);
}

void MainWindow::refreshContacts() {
  const QString selectedPeer = app_.chatState().selectedPeerId();
  conversationList_->blockSignals(true);
  conversationList_->clear();
  int selectedRow = -1;
  QList<RemoteIMContact> contacts = app_.chatState().contacts();

  // 会话列表按「最近一条消息」倒序：有新消息的人排到最前面。
  //
  // 此前直接沿用 contacts() 的顺序，而那是按 userId 排的字母序——
  // 结果是刚回你消息的人可能排在第 11 位，而列表顶上是几周没说过话的。
  // 从来没有过消息的联系人排在最后（按名字，保证顺序稳定，不会每次刷新乱跳）。
  QHash<QString, qint64> latestAtByUserId;
  QHash<QString, RemoteIMMessage> latestByUserId;
  for (const RemoteIMContact &contact : contacts) {
    RemoteIMMessage latest;
    if (!app_.chatState().latestMessageWith(contact.userId, &latest))
      continue;
    latestAtByUserId.insert(contact.userId, latest.createdAtMillis);
    latestByUserId.insert(contact.userId, latest);
  }
  std::stable_sort(
      contacts.begin(), contacts.end(),
      [&latestAtByUserId](const RemoteIMContact &a, const RemoteIMContact &b) {
        const bool aHas = latestAtByUserId.contains(a.userId);
        const bool bHas = latestAtByUserId.contains(b.userId);
        if (aHas != bHas)
          return aHas; // 有消息的排在没消息的前面
        if (!aHas)
          return a.userId.localeAwareCompare(b.userId) < 0;
        const qint64 aAt = latestAtByUserId.value(a.userId);
        const qint64 bAt = latestAtByUserId.value(b.userId);
        if (aAt != bAt)
          return aAt > bAt;
        // 时间完全相同时按名字排。产品口径：时间戳是秒级精度，同秒并列按首字母
        // 即可，不引入「最后活动顺序」这类额外状态。名字排序同时保证了顺序稳定——
        // 否则列表每次刷新都可能变一个样。
        return a.userId.localeAwareCompare(b.userId) < 0;
      });

  for (int index = 0; index < contacts.size(); ++index) {
    const RemoteIMContact &contact = contacts[index];
    auto *item = new QListWidgetItem();
    item->setSizeHint(QSize(0, UiZoom::s(76)));
    const bool hasLatestMessage = latestByUserId.contains(contact.userId);
    const RemoteIMMessage latestMessage = latestByUserId.value(contact.userId);
    item->setData(UserIdRole, contact.userId);
    item->setData(DisplayNameRole, contact.displayName.isEmpty()
                                       ? contact.userId
                                       : contact.displayName);
    item->setData(
        PreviewRole,
        latestMessageText(hasLatestMessage ? &latestMessage : nullptr));
    item->setData(TimeRole, latestMessageTime(hasLatestMessage ? &latestMessage
                                                               : nullptr));
    item->setData(UnreadRole, app_.chatState().unreadCount(contact.userId));
    item->setData(AvatarUrlRole, contact.avatarUrl);
    conversationList_->addItem(item);
    if (contact.userId == selectedPeer)
      selectedRow = index;
  }
  if (selectedRow >= 0)
    conversationList_->setCurrentRow(selectedRow);
  conversationList_->blockSignals(false);
  applyConversationFilter();
}

void MainWindow::applyConversationFilter() {
  if (!navSearchInput_)
    return;
  const QString needle = navSearchInput_->text().trimmed();
  for (int row = 0; row < conversationList_->count(); ++row) {
    QListWidgetItem *item = conversationList_->item(row);
    const QString name = item->data(DisplayNameRole).toString();
    const QString preview = item->data(PreviewRole).toString();
    // 会话里有命中的消息也要留下。否则搜一个只出现在聊天记录深处的词，
    // 左边整列会空掉——而右边的结果面板明明列着这些会话，自相矛盾。
    // 「会话里有没有命中」由上一趟搜索算好放在 peersWithSearchHits_ 里，
    // 这里直接查，不再把所有会话重新扫一遍——否则一次输入要扫两遍。
    // 名字和摘要与消息正文走同一套模糊规则。原先是严格 contains：
    // 备注名记岔一个字就整列空掉，而右边结果面板还列着这个会话，自相矛盾。
    const bool matched =
        needle.isEmpty() || MessageSearch::matches(name, needle) ||
        MessageSearch::matches(preview, needle) ||
        peersWithSearchHits_.contains(item->data(UserIdRole).toString());
    item->setHidden(!matched);
  }
}

void MainWindow::applyContactFilter() {
  if (!contactsSearchInput_ || !contactsList_)
    return;
  const QString needle = contactsSearchInput_->text().trimmed();
  const bool searching = !needle.isEmpty();

  // 表头是在联系人之前出现的，可见人数要等这一节走完才知道。
  // 因此先记住表头位置，最后回填。
  int pendingHeaderRow = -1;
  int visibleInSection = 0;

  auto closePendingSection = [&] {
    if (pendingHeaderRow < 0)
      return;
    // 搜索时把整节都没命中的分组表头一并藏掉，否则结果里全是空标题。
    // 不搜索时表头一律显示——空分组必须看得见。
    contactsList_->item(pendingHeaderRow)
        ->setHidden(searching && visibleInSection == 0);
    pendingHeaderRow = -1;
    visibleInSection = 0;
  };

  for (int row = 0; row < contactsList_->count(); ++row) {
    QListWidgetItem *item = contactsList_->item(row);
    if (item->data(IsGroupHeaderRole).toBool()) {
      closePendingSection();
      pendingHeaderRow = row;
      visibleInSection = 0;
      continue;
    }

    const QString group = item->data(GroupNameRole).toString();
    // 没有分组的联系人不属于任何一节，它们的命中不能算到上一个分组的表头上——
    // 否则一个自己没有命中的分组，会因为后面跟着的散人而留在结果里。
    if (group.isEmpty())
      closePendingSection();

    // 备注名和 userId 都参与匹配，且和消息搜索共用一套模糊规则：
    // 名字记岔一个字、只记得 ID 中间一段，都还能搜出来。
    const bool matched =
        !searching ||
        MessageSearch::matches(item->data(DisplayNameRole).toString(),
                               needle) ||
        MessageSearch::matches(item->data(UserIdRole).toString(), needle);
    // 搜索时无视折叠状态：命中的人藏在收起的分组里不显示，看上去就是"搜不到"。
    const bool collapsed =
        !searching && collapsedContactGroups_.contains(group);
    item->setHidden(!matched || collapsed);
    if (matched)
      ++visibleInSection;
  }
  closePendingSection();
}

// Ctrl+F 聚焦「当前这一页的」搜索框。两个框各搜各的，按页分流之后
// 就不存在「按了 Ctrl+F 却聚焦到看不见的那个框」这种情况。
void MainWindow::focusPageSearch() {
  if (contentStack_ && contentStack_->currentWidget() == contactsPage_) {
    if (!contactsSearchInput_)
      return;
    contactsSearchInput_->setFocus();
    contactsSearchInput_->selectAll();
    return;
  }
  focusGlobalSearch();
}

void MainWindow::focusGlobalSearch() {
  if (!navSearchInput_)
    return;
  navSearchInput_->setFocus();
  navSearchInput_->selectAll();
  refreshGlobalSearchResults();
}

void MainWindow::closeGlobalSearchResults() {
  if (globalSearchResults_)
    globalSearchResults_->hide();
}

void MainWindow::layoutGlobalSearchResults() {
  if (!globalSearchResults_ || !navSearchInput_ ||
      !globalSearchResults_->isVisible())
    return;
  // 面板挂在窗口上而不是搜索框里：它要盖住下方内容，塞进布局会把主体挤下去。
  const QPoint topLeft = navSearchInput_->mapTo(
      this, QPoint(0, navSearchInput_->height() + UiZoom::s(6)));
  const int rows = qMin(globalSearchResults_->count(), 8);
  const int height = qMax(UiZoom::s(56), rows * UiZoom::s(52) + UiZoom::s(8));
  globalSearchResults_->setGeometry(topLeft.x(), topLeft.y(),
                                    navSearchInput_->width(), height);
  globalSearchResults_->raise();
}

void MainWindow::refreshGlobalSearchResults() {
  if (!navSearchInput_ || !globalSearchResults_)
    return;
  const QString needle = navSearchInput_->text().trimmed();
  globalSearchResults_->clear();
  peersWithSearchHits_.clear();
  if (needle.isEmpty()) {
    closeGlobalSearchResults();
    return;
  }

  struct Hit {
    QString peerId;
    QString peerName;
    QString messageId;
    QString preview;
    qint64 createdAt = 0;
    int score = 0;
  };

  const ChatState &state = app_.chatState();
  QVector<Hit> hits;
  for (const RemoteIMContact &contact : state.contacts()) {
    const QString peerName =
        contact.displayName.isEmpty() ? contact.userId : contact.displayName;
    // 逐条流式判断，不调 messagesWith——那会把整个会话深拷贝一份出来，
    // 放在搜索里等于每次输入都复制上千条消息。
    state.forEachMessageWith(
        contact.userId, [&](const RemoteIMMessage &message) {
          const int score = MessageSearch::score(message.text, needle);
          if (score == MessageSearch::NoMatch)
            return;
          // 同一趟顺便记下「这个会话有命中」，会话列表过滤直接复用，不再扫第二遍。
          peersWithSearchHits_.insert(contact.userId);
          hits.append({contact.userId, peerName, message.id,
                       message.text.simplified(), message.createdAtMillis,
                       score});
        });
  }

  // 先按贴切度，再按时间新→旧。模糊命中排在原样命中后面，
  // 这样「记岔一点也能搜到」不会把真正想找的那条挤下去。
  std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
    if (a.score != b.score)
      return a.score > b.score;
    return a.createdAt > b.createdAt;
  });

  const bool truncated = hits.size() > MaxGlobalSearchResults;
  const int shown = truncated ? MaxGlobalSearchResults : hits.size();
  for (int i = 0; i < shown; ++i) {
    const Hit &hit = hits.at(i);
    auto *item = new QListWidgetItem(
        QStringLiteral("%1 · %2\n%3")
            .arg(hit.peerName,
                 QDateTime::fromMSecsSinceEpoch(hit.createdAt)
                     .toString(QStringLiteral("MM-dd HH:mm")),
                 hit.preview));
    item->setData(SearchPeerRole, hit.peerId);
    item->setData(SearchMessageRole, hit.messageId);
    item->setSizeHint(QSize(0, UiZoom::s(52)));
    globalSearchResults_->addItem(item);
  }

  if (hits.isEmpty()) {
    // 说清范围：没点过「加载更早」的历史不在内存里，也就搜不到。
    auto *empty = new QListWidgetItem(
        QStringLiteral("无结果\n只搜索各会话中已加载的消息"));
    empty->setFlags(Qt::NoItemFlags);
    empty->setSizeHint(QSize(0, UiZoom::s(52)));
    globalSearchResults_->addItem(empty);
  } else if (truncated) {
    auto *more = new QListWidgetItem(
        QStringLiteral("结果过多，仅显示最贴切的 %1 条\n再输入几个字缩小范围")
            .arg(MaxGlobalSearchResults));
    more->setFlags(Qt::NoItemFlags);
    more->setSizeHint(QSize(0, UiZoom::s(52)));
    globalSearchResults_->addItem(more);
  }

  globalSearchResults_->show();
  layoutGlobalSearchResults();
}

void MainWindow::openGlobalSearchResult(QListWidgetItem *item) {
  if (!item)
    return;
  const QString peerId = item->data(SearchPeerRole).toString();
  const QString messageId = item->data(SearchMessageRole).toString();
  if (peerId.isEmpty() || messageId.isEmpty())
    return;

  clearMessageSearchHighlight();
  if (peerId != app_.chatState().selectedPeerId()) {
    app_.selectPeer(peerId);
  }
  showMessagesPage();
  closeGlobalSearchResults();
  // 切会话会整屏重建气泡，气泡高度还要等一次布局才定下来，
  // 所以定位放到事件循环下一轮，否则滚到的位置是旧布局算出来的。
  QTimer::singleShot(0, this, [this, peerId, messageId] {
    if (app_.chatState().selectedPeerId() == peerId)
      highlightMessage(messageId);
  });
}

QString MainWindow::bestSearchHitId(const QString &peerId,
                                    const QString &needle) const {
  QString bestId;
  int bestScore = MessageSearch::NoMatch;
  qint64 bestAt = 0;
  // 逐条流式判断，不调 messagesWith——那会把整个会话深拷贝一份，
  // 而这个函数挂在「点会话」这个交互上，长会话会卡一下。
  app_.chatState().forEachMessageWith(
      peerId, [&](const RemoteIMMessage &message) {
        const int s = MessageSearch::score(message.text, needle);
        if (s == MessageSearch::NoMatch)
          return;
        if (s > bestScore ||
            (s == bestScore && message.createdAtMillis > bestAt)) {
          bestScore = s;
          bestAt = message.createdAtMillis;
          bestId = message.id;
        }
      });
  return bestId;
}

void MainWindow::highlightMessage(const QString &messageId) {
  clearMessageSearchHighlight();
  // 命中可能在渲染窗口之外（更早的消息）：先把窗口扩到覆盖它，再谈定位。
  ensureMessageRendered(messageId);
  QWidget *row = messageRowById_.value(messageId);
  if (!row)
    return;
  messageSearchHighlightedId_ = messageId;
  row->setProperty("searchHit", true);
  row->style()->unpolish(row);
  row->style()->polish(row);
  if (messageScroll_)
    messageScroll_->ensureWidgetVisible(row, 0, UiZoom::s(60));
}

void MainWindow::clearMessageSearchHighlight() {
  if (messageSearchHighlightedId_.isEmpty())
    return;
  if (QWidget *row = messageRowById_.value(messageSearchHighlightedId_)) {
    row->setProperty("searchHit", false);
    row->style()->unpolish(row);
    row->style()->polish(row);
  }
  messageSearchHighlightedId_.clear();
}

void MainWindow::refreshContactDirectory() {
  contactsList_->blockSignals(true);
  contactsList_->clear();
  const QList<RemoteIMContact> contacts = acceptedContacts();
  const QStringList groups = app_.chatState().contactGroups();

  // 先按分组归拢。联系人本身已按 userId 排好，桶内顺序自然继承。
  QHash<QString, QList<RemoteIMContact>> byGroup;
  for (const RemoteIMContact &contact : contacts) {
    byGroup[contact.groupName].append(contact);
  }
  for (const QString &group : groups) {
    appendContactGroupSection(group, byGroup.value(group));
  }
  // 没有分组的联系人不套标题，直接列在分组的同一层、排在分组之后。
  // 给它们加一个「未分组」标题只是在给"没有分组"这件事起名字，界面上凭空多一层，
  // 而那一层不对应任何用户能操作的东西。
  //
  // 一个分组都没建过时，上面的循环什么都不产生，这里就退化成原来的平铺列表——
  // 不需要为"没有分组"单独写一条分支。
  for (const RemoteIMContact &contact : byGroup.value(QString())) {
    contactsList_->addItem(makeContactItem(contact));
  }

  contactsList_->blockSignals(false);
  // 列表被重建过，隐藏状态跟着没了；搜索词还在框里，这里补回过滤，
  // 否则来一条消息触发刷新，搜索结果就会突然变回全部联系人。
  applyContactFilter();
  refreshContactProfile();
}

void MainWindow::refreshContactProfile() {
  if (!contactProfileStack_) return;
  const auto contacts = acceptedContacts();
  const auto match = std::find_if(contacts.cbegin(), contacts.cend(),
      [this](const RemoteIMContact &contact) {
        return contact.userId == selectedContactProfileUserId_;
      });
  if (selectedContactProfileUserId_.isEmpty() || match == contacts.cend()) {
    selectedContactProfileUserId_.clear();
    contactProfileStack_->setCurrentIndex(0);
    return;
  }
  const QString name = match->displayName.isEmpty() ? match->userId : match->displayName;
  contactProfileName_->setText(name);
  static_cast<AccountAvatarLabel *>(contactProfileAvatar_)->setProfile(*match);
  const QString accountId = app_.maiChatAccountForImId(match->userId);
  contactProfileId_->setText(accountId.isEmpty() ? QStringLiteral("正在读取账号") : accountId);
  contactProfileId_->setEnabled(!accountId.isEmpty());
  if (auto *copy = contactProfileStack_->findChild<QPushButton *>(
          QStringLiteral("contactProfileCopyButton")))
    copy->setEnabled(!accountId.isEmpty());
  contactProfileStack_->setCurrentIndex(1);
}

QList<RemoteIMContact> MainWindow::acceptedContacts() const {
  QList<RemoteIMContact> contacts;
  for (const RemoteIMContact &contact : app_.chatState().contacts()) {
    if (app_.isAcceptedFriend(contact.userId)) contacts.append(contact);
  }
  return contacts;
}

QListWidgetItem *MainWindow::makeContactItem(const RemoteIMContact &contact) {
  auto *item = new QListWidgetItem();
  item->setSizeHint(QSize(0, UiZoom::s(54)));
  item->setData(UserIdRole, contact.userId);
  item->setData(DisplayNameRole, contact.displayName.isEmpty()
                                     ? contact.userId
                                     : contact.displayName);
  item->setData(AvatarUrlRole, contact.avatarUrl);
  item->setData(GroupNameRole, contact.groupName);
  item->setData(IsGroupHeaderRole, false);
  return item;
}

void MainWindow::appendContactGroupSection(
    const QString &groupName, const QList<RemoteIMContact> &members) {
  const bool collapsed = collapsedContactGroups_.contains(groupName);
  auto *header = new QListWidgetItem();
  header->setSizeHint(QSize(0, UiZoom::s(kContactGroupHeaderHeight)));
  header->setData(IsGroupHeaderRole, true);
  header->setData(GroupNameRole, groupName);
  header->setData(DisplayNameRole, groupName);
  header->setData(GroupCollapsedRole, collapsed);
  header->setData(GroupCountRole, members.size());
  // 表头不可选中：选中态在这个列表里代表「当前联系人」，让表头也能被选中
  // 会把方向键操作变成在人和标题之间跳。
  header->setFlags(Qt::ItemIsEnabled);
  contactsList_->addItem(header);

  for (const RemoteIMContact &contact : members) {
    contactsList_->addItem(makeContactItem(contact));
  }
}

void MainWindow::toggleContactGroupCollapsed(const QString &groupName) {
  if (collapsedContactGroups_.contains(groupName)) {
    collapsedContactGroups_.remove(groupName);
  } else {
    collapsedContactGroups_.insert(groupName);
  }
  refreshContactDirectory();
}

void MainWindow::refreshSettings() {
  const QString accountId = maiChatAccountIdProvider_ ?
      maiChatAccountIdProvider_() : app_.chatState().ownerUserId();
  settingsAccountValue_->setText(accountId);
  const QString nickname = maiChatNicknameProvider_ ? maiChatNicknameProvider_().trimmed() :
      QString();
  settingsNicknameValue_->setText(nickname.isEmpty() ? QStringLiteral("未设置") : nickname);
  const QString profileName = nickname.isEmpty() ? app_.selfProfile().displayName : nickname;
  settingsProfileName_->setText(profileName.isEmpty() ? accountId : profileName);
  settingsProfileId_->setText(QStringLiteral("MaiChat ID  ") + accountId);
  static_cast<AccountAvatarLabel *>(settingsProfileAvatar_)->setProfile(app_.selfProfile());
}

void MainWindow::setAccountModelKeys(QHash<QString, QString> keys,
                                  const QString& error) {
  modelApiKeys_ = std::move(keys);
  modelKeyError_ = error;
  {
    QMutexLocker lock(&modelKeyState_->mutex);
    modelKeyState_->keys = modelApiKeys_;
  }
  const QString requested = std::exchange(pendingModelSelection_, QString());
  if (!requested.isEmpty()) {
    const QString provider = requested == QStringLiteral("deepseek-flash") ?
        QStringLiteral("deepseek") : QStringLiteral("glm");
    if (modelApiKeys_.value(provider).isEmpty()) {
      showToast(modelKeyError_.isEmpty() ?
          QStringLiteral("当前账号暂不可用所选模型") : modelKeyError_, 13, 2500);
    } else if (agentPanel_) {
      QSettings settings;
      const QVariant oldModel = settings.value(QStringLiteral("agent/model"));
      settings.setValue(QStringLiteral("agent/model"), requested);
      settings.sync();
      if (settings.status() != QSettings::NoError ||
          !agentPanel_->applySelectedModel(requested)) {
        if (oldModel.isValid()) settings.setValue(QStringLiteral("agent/model"), oldModel);
        else settings.remove(QStringLiteral("agent/model"));
        settings.sync();
        showToast(QStringLiteral("模型切换失败，已保留原模型"), 13, 2500);
      }
    }
  }
  if (agentPanel_) {
    const AgentController::ModelConfig selected = loadAgentModelConfig(modelApiKeys_);
    const bool ready = !selected.apiKey.isEmpty();
    const QString label = ready ? selected.modelName :
        !modelKeyError_.isEmpty() ?
            (modelKeyError_.contains(QStringLiteral("未开通")) ?
                QStringLiteral("模型未开通") : QStringLiteral("模型连接失败")) :
        !modelApiKeys_.isEmpty() ? QStringLiteral("模型未开通") :
                                   QStringLiteral("正在连接模型");
    agentPanel_->setModelLabel(label, ready);
  }
  if (replySuggestionController_) {
    const AgentController::ModelConfig selected = loadAgentModelConfig(modelApiKeys_);
    ReplySuggestionController::Config reply;
    reply.baseUrl = selected.baseUrl;
    reply.apiKey = selected.apiKey;
    reply.modelName = selected.modelName;
    replySuggestionController_->setConfig(reply);
  }
  refreshSettings();
}

void MainWindow::setModelKeyRefreshHandler(std::function<void()> handler) {
  modelKeyRefreshHandler_ = std::move(handler);
}

void MainWindow::setMediaServiceAccessProvider(
    std::function<QPair<QString, QString>()> provider) {
  mediaServiceAccessProvider_ = std::move(provider);
  rebuildAgentPage();
}

void MainWindow::setVideoCircleAccessProvider(
    std::function<DesktopVideoCircleAccess()> provider) {
  videoCircleAccessProvider_ = std::move(provider);
  probeVideoCircleAvailability();
  probeCloudDriveAvailability();
  // The Agent registrar captures the access provider. Login wiring installs it
  // after the initial Agent page, so refresh that registrar once here.
  rebuildAgentPage();
}

void MainWindow::probeCloudDriveAvailability() {
  const unsigned generation = ++cloudDriveProbeGeneration_;
  cloudDriveAvailable_ = false;
  if (cloudSettingsRow_) cloudSettingsRow_->hide();
  if (agentPanel_) agentPanel_->setCloudDriveAvailable(false);
  if (!videoCircleAccessProvider_ || !maiChatAccountIdProvider_) return;
  const DesktopVideoCircleAccess access = videoCircleAccessProvider_();
  const QString account = maiChatAccountIdProvider_();
  if (!access.isValid() || account.isEmpty() || access.accountId != account) return;
  auto* watcher = new QFutureWatcher<int>(this);
  connect(watcher, &QFutureWatcher<int>::finished, this,
          [this, watcher, generation, account] {
    const int available = watcher->result();
    watcher->deleteLater();
    if (generation != cloudDriveProbeGeneration_ || accountDisconnectPending_ ||
        !maiChatAccountIdProvider_ || maiChatAccountIdProvider_() != account ||
        !videoCircleAccessProvider_ || videoCircleAccessProvider_().accountId != account)
      return;
    cloudDriveAvailable_ = available == 1;
    if (agentPanel_) agentPanel_->setCloudDriveAvailable(cloudDriveAvailable_);
    if (cloudSettingsRow_) cloudSettingsRow_->setVisible(available != 0);
    if (cloudSettingsValue_) cloudSettingsValue_->setText(
        available == 1 ? QStringLiteral("私人空间") : QStringLiteral("连接失败"));
    if (cloudSettingsButton_) cloudSettingsButton_->setText(
        available == 1 ? QStringLiteral("打开") : QStringLiteral("重试"));
  });
  watcher->setFuture(QtConcurrent::run([access] {
    return CloudDriveDialog::availability(access);
  }));
}

void MainWindow::probeVideoCircleAvailability() {
  const unsigned generation = ++videoCircleProbeGeneration_;
  videoCircleAvailable_ = false;
  if (agentPanel_) agentPanel_->setVideoCircleAvailable(false);
  if (!videoCircleAccessProvider_ || !maiChatAccountIdProvider_) return;
  const DesktopVideoCircleAccess access = videoCircleAccessProvider_();
  const QString account = maiChatAccountIdProvider_();
  if (!access.isValid() || account.isEmpty() || access.accountId != account) return;
  auto* watcher = new QFutureWatcher<bool>(this);
  connect(watcher, &QFutureWatcher<bool>::finished, this,
          [this, watcher, generation, account] {
    const bool available = watcher->result();
    watcher->deleteLater();
    if (generation != videoCircleProbeGeneration_ || accountDisconnectPending_ ||
        !maiChatAccountIdProvider_ || maiChatAccountIdProvider_() != account ||
        !videoCircleAccessProvider_ || !videoCircleAccessProvider_().isValid() ||
        videoCircleAccessProvider_().accountId != account) return;
    videoCircleAvailable_ = available;
    if (agentPanel_) agentPanel_->setVideoCircleAvailable(available);
  });
  watcher->setFuture(QtConcurrent::run([access] { return VideoCircleDialog::isAvailable(access); }));
}

void MainWindow::openVideoCircle(const QString& completedCardJson, const QString& sourcePath) {
  if (!videoCircleAvailable_ || !videoCircleAccessProvider_ || !maiChatAccountIdProvider_) return;
  if (!videoCircleDialog_) {
    videoCircleDialog_ = new VideoCircleDialog(videoCircleAccessProvider_, maiChatAccountIdProvider_,
        [this](const QString& path) {
          if (!openDockedVideo(path)) {
            QFile::remove(path);
            showToast(QStringLiteral("视频无法播放"), 13, 3000);
            return;
          }
          // The shared downloader owns the temporary file until the hosted
          // ffplay dialog is fully destroyed; deleting it earlier can break
          // playback, especially on Windows.
          if (videoDialog_)
            connect(videoDialog_, &QObject::destroyed, this,
                    [path] { QFile::remove(path); });
        }, this);
  }
  if (completedCardJson.isEmpty()) videoCircleDialog_->openBrowse();
  else videoCircleDialog_->openPublish(completedCardJson, sourcePath);
}

void MainWindow::openCloudDrive(const QString& suggestedVideo) {
  if (!cloudDriveAvailable_ || !videoCircleAccessProvider_ || !maiChatAccountIdProvider_) return;
  if (!cloudDriveDialog_) {
    cloudDriveDialog_ = new CloudDriveDialog(videoCircleAccessProvider_,
        maiChatAccountIdProvider_, [this](std::shared_ptr<MaiCloudVod> source,
                                         const QString& name) {
          const QString display = QFileInfo(name).fileName().isEmpty() ?
              QStringLiteral("云盘视频.mp4") : QFileInfo(name).fileName();
          if (!openDockedVideo(display, {}, 0, std::move(source))) {
            showToast(QStringLiteral("视频无法播放"), 13, 3000);
          }
        }, this);
  }
  cloudDriveDialog_->openDrive(suggestedVideo);
}

void MainWindow::setFriendQrProvider(std::function<void(FriendQrCompletion)> provider) {
  friendQrProvider_ = std::move(provider);
}

void MainWindow::setMaiChatAccountIdProvider(std::function<QString()> provider) {
  maiChatAccountIdProvider_ = std::move(provider);
  refreshSettings();
}

void MainWindow::setMaiChatNicknameProvider(std::function<QString()> provider) {
  maiChatNicknameProvider_ = std::move(provider);
  refreshSettings();
}

void MainWindow::setMaiChatNicknameUpdater(
    std::function<void(const QString&, RemoteIMCompletion)> updater) {
  maiChatNicknameUpdater_ = std::move(updater);
  if (settingsNicknameButton_) settingsNicknameButton_->setEnabled(bool(maiChatNicknameUpdater_));
}

void MainWindow::setSessionSyncCoordinator(DesktopSessionSyncCoordinator* coordinator) {
  sessionSync_ = coordinator;
  if (!sessionSync_) return;
  const QString accountRoot = accountAgentDirectory(app_.chatState().ownerUserId());
  sessionSync_->setDefaultWorkspaceDirectory(
      accountRoot.isEmpty() ? QString() :
          QDir(accountRoot).filePath(QStringLiteral("Workspace")));
  QPointer<MainWindow> guard(this);
  sessionSync_->setCatalogHandler([guard](const QJsonArray& sessions) {
    if (!guard) return;
    guard->remoteAgentSessions_ = sessions;
    if (guard->agentSessions_) guard->agentSessions_->setRemoteSessions(sessions);
    if (!guard->agentRemotePanel_ || !guard->agentPanelStack_ ||
        guard->agentPanelStack_->currentWidget() != guard->agentRemotePanel_) return;
    QJsonObject current = guard->agentRemotePanel_->session();
    for (const QJsonValue& value : sessions) {
      const QJsonObject session = value.toObject();
      if (session.value(QStringLiteral("device_slot")) == current.value(QStringLiteral("device_slot")) &&
          session.value(QStringLiteral("login_session_id")) == current.value(QStringLiteral("login_session_id")) &&
          session.value(QStringLiteral("id")) == current.value(QStringLiteral("id"))) {
        guard->agentRemotePanel_->updateSession(session);
        return;
      }
    }
    current.insert(QStringLiteral("online"), false);
    guard->agentRemotePanel_->updateSession(current);
  });
  sessionSync_->setDeviceHandler([guard](const QJsonArray& devices) {
    if (!guard) return;
    guard->remoteAgentDevices_ = devices;
    if (guard->agentSessions_) guard->agentSessions_->setRemoteDevices(devices);
  });
  sessionSync_->setPayloadHandler([guard](const QJsonObject& session, const QString& kind,
                                          const QJsonObject& payload) {
    if (guard && guard->agentRemotePanel_) guard->agentRemotePanel_->receivePayload(session, kind, payload);
  });
  wireRemoteAgentPanel();
}

void MainWindow::wireRemoteAgentPanel() {
  if (!agentRemotePanel_ || !sessionSync_) return;
  QPointer<RemoteAgentPanel> panel(agentRemotePanel_);
  agentRemotePanel_->setHistoryHandler([this, panel](const QJsonObject& session, const QString& before, int limit,
                                                      RemoteAgentPanel::Completion completion) {
    sessionSync_->requestRemoteHistory(session, before, limit,
                                       [panel, completion = std::move(completion)](const QJsonObject&,
                                                                                    const QString& error) {
      if (panel) completion(error);
    });
  });
  agentRemotePanel_->setCommandHandler([this, panel](const QJsonObject& session, const QJsonObject& payload,
                                                      RemoteAgentPanel::Completion completion) {
    sessionSync_->sendRemoteCommand(session, payload,
                                    [panel, completion = std::move(completion)](const QJsonObject&,
                                                                                 const QString& error) {
      if (panel) completion(error);
    });
  });
}

void MainWindow::rebuildAgentPage() {
  if (agentPage_ == nullptr)
    return;
  auto *agentSplitter = agentPage_->findChild<QSplitter *>(
      QStringLiteral("agentContentSplitter"), Qt::FindDirectChildrenOnly);
  if (agentSplitter == nullptr)
    return;

  const QString sessionToRestore =
      agentPanel_ == nullptr ? QString() : agentPanel_->sessionId();
  const QJsonObject remoteToRestore =
      agentPanelStack_ && agentRemotePanel_ && agentPanelStack_->currentWidget() == agentRemotePanel_
          ? agentRemotePanel_->session() : QJsonObject();
  const QList<int> sizesToRestore = agentSplitter->sizes();

  if (agentPanelStack_) delete agentPanelStack_;
  else delete agentPanel_;
  agentPanelStack_ = nullptr;
  agentPanel_ = nullptr;
  agentRemotePanel_ = nullptr;
  delete agentSessions_;
  agentSessions_ = nullptr;
  delete agentController_;
  agentController_ = nullptr;

  AgentController::ModelConfig modelConfig = loadAgentModelConfig(modelApiKeys_);
  const auto keyState = modelKeyState_;
  modelConfig.accountModelKeyProvider = [keyState](const QString& provider) {
    QMutexLocker lock(&keyState->mutex);
    return keyState->keys.value(provider);
  };
  modelConfig.glmApiKeyProvider = [keyState] {
    QMutexLocker lock(&keyState->mutex);
    return keyState->keys.value(QStringLiteral("glm"));
  };
  modelConfig.miniMaxApiKeyProvider = [keyState] {
    QMutexLocker lock(&keyState->mutex);
    return keyState->keys.value(QStringLiteral("minimax"));
  };
  modelConfig.glmBaseUrl = QStringLiteral("https://open.bigmodel.cn/api/v1");
  modelConfig.deepseekBaseUrl = QStringLiteral("https://api.deepseek.com");
  modelConfig.reasoningEffortProvider = [] {
    QSettings settings;
    const QString value = settings.value(
        QStringLiteral("agent/reasoningEffort"), QStringLiteral("high")).toString();
    return value == QStringLiteral("low") || value == QStringLiteral("max") ?
        value : QStringLiteral("high");
  };
  modelConfig.arkApiKeyProvider = arkApiKeyProvider_;
  modelConfig.wanCredentialsProvider = wanCredentialsProvider_;
  modelConfig.mediaServiceAccessProvider = mediaServiceAccessProvider_;
  if (replySuggestionController_) {
    ReplySuggestionController::Config replyConfig;
    replyConfig.baseUrl = modelConfig.baseUrl;
    replyConfig.apiKey = modelConfig.apiKey;
    replyConfig.modelName = modelConfig.modelName;
    clearReplySuggestions();
    replySuggestionController_->setConfig(replyConfig);
  }
  agentController_ = new AgentController(
      modelConfig, agentDatabasePath(app_.chatState().ownerUserId()),
      [this](MaiToolRegistry &registry) {
        registerMaiChatHostTools(registry, app_);
        QPointer<MainWindow> cloudPresenter(this);
        registerDesktopCloudDriveTools(
            registry, videoCircleAccessProvider_, this,
            [cloudPresenter](const QString& path, bool isImage,
                             std::shared_ptr<MaiCloudVod> cloudSource) {
              if (cloudPresenter.isNull()) return false;
              if (isImage) {
                if (cloudPresenter->imagePreviewDialog_) return false;
                QImage decoded;
#if defined(MAICHAT_FFMPEG_IMAGE_DECODE)
                const QByteArray utf8 = path.toUtf8();
                const MaiImageDecodeResult result = maiImageDecodeFile(utf8.constData(), 4096, 4096);
                if (result.rgba) {
                  decoded = QImage(result.rgba, result.width, result.height, result.stride,
                                   QImage::Format_RGBA8888).copy();
                  maiImageDecodeFree(result.rgba);
                }
#else
                QImageReader reader(path);
                reader.setAutoTransform(true);
                decoded = reader.read();
#endif
                if (decoded.isNull()) return false;
                const QPixmap image = QPixmap::fromImage(decoded);
                cloudPresenter->openImagePreview(path, nullptr, &image);
                if (!cloudPresenter->imagePreviewDialog_) return false;
                QObject::connect(cloudPresenter->imagePreviewDialog_, &QObject::destroyed,
                                 cloudPresenter.data(), [path] { QFile::remove(path); });
                return true;
              }
              if (!cloudPresenter->openDockedVideo(path, {}, 0,
                                                    std::move(cloudSource))) return false;
              return true;
            });
        registerDesktopRemoteSessionTools(
            registry, this, [this] { return sessionSync_; });
        const auto sshPassword =
            [this](const std::string &host, int port,
                   const std::string &username) -> MaiResult<std::string> {
          QString password;
          bool accepted = false;
          const auto prompt = [&] {
            password = QInputDialog::getText(
                this, QStringLiteral("SSH 登录"),
                QStringLiteral("%1@%2:%3 的密码：")
                    .arg(QString::fromUtf8(username.c_str()),
                         QString::fromUtf8(host.c_str()))
                    .arg(port),
                QLineEdit::Password, QString(), &accepted);
          };
          if (QThread::currentThread() == thread())
            prompt();
          else
            QMetaObject::invokeMethod(this, prompt,
                                      Qt::BlockingQueuedConnection);
          if (!accepted)
            return {MaiErrorCode::Canceled, "SSH password entry was canceled"};
          const QByteArray utf8 = password.toUtf8();
          return std::string(utf8.constData(),
                             static_cast<std::size_t>(utf8.size()));
        };
        const auto sshTrust =
            [this](const std::string &host, int port,
                   const std::string &fingerprint) -> MaiResult<bool> {
          bool trusted = false;
          const auto verify = [&] {
            const QString key = QStringLiteral("ssh/known-hosts/%1:%2")
                                    .arg(QString::fromUtf8(host.c_str()))
                                    .arg(port);
            QSettings settings;
            if (settings.contains(key)) {
              trusted = settings.value(key).toString() ==
                        QString::fromUtf8(fingerprint.c_str());
              if (!trusted)
                QMessageBox::warning(
                    this, QStringLiteral("SSH 服务器身份变化"),
                    QStringLiteral("%1:%2 的主机指纹与以前不同，连接已拒绝。")
                        .arg(QString::fromUtf8(host.c_str()))
                        .arg(port));
              return;
            }
            trusted =
                QMessageBox::question(
                    this, QStringLiteral("确认 SSH 服务器身份"),
                    QStringLiteral("%1:%2\n%3\n请先与云主机控制台核对指纹。")
                        .arg(QString::fromUtf8(host.c_str()))
                        .arg(port)
                        .arg(QString::fromUtf8(fingerprint.c_str())),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) == QMessageBox::Yes;
            if (trusted)
              settings.setValue(key, QString::fromUtf8(fingerprint.c_str()));
          };
          if (QThread::currentThread() == thread())
            verify();
          else
            QMetaObject::invokeMethod(this, verify,
                                      Qt::BlockingQueuedConnection);
          return trusted;
        };
        registry.add(makeMaiSshTool(sshPassword, sshTrust));
#if defined(MAICHAT_HAS_FFPLAY)
        registerDesktopMediaTools(registry, this);
#endif
      },
      this);
  agentSessions_ = new AgentSessionList(*agentController_, agentSplitter);
  agentPanelStack_ = new QStackedWidget(agentSplitter);
  agentPanel_ = new AgentChatPanel(*agentController_, agentPanelStack_);
  agentRemotePanel_ = new RemoteAgentPanel(agentPanelStack_);
  agentPanelStack_->addWidget(agentPanel_);
  agentPanelStack_->addWidget(agentRemotePanel_);
  agentSessions_->setRemoteSessions(remoteAgentSessions_);
  agentSessions_->setRemoteDevices(remoteAgentDevices_);
  wireRemoteAgentPanel();
  const QString accountRoot = accountAgentDirectory(app_.chatState().ownerUserId());
  const QString defaultWorkspace = accountRoot.isEmpty() ? QString() :
      QDir(accountRoot).filePath(QStringLiteral("Workspace"));
  agentPanel_->setDefaultWorkspaceDirectory(defaultWorkspace);
  if (sessionSync_) sessionSync_->setDefaultWorkspaceDirectory(defaultWorkspace);
#if defined(MAICHAT_HAS_FFPLAY)
  agentPanel_->setOpenVideoCallback([this](const QString &path, const QStringList &paths) {
    openDockedVideo(path, paths);
  });
#endif
  agentPanel_->setForwardMediaCallback(
      [this](const QString &path, const QString &type) {
        const QList<RemoteIMContact> contacts = acceptedContacts();
        if (contacts.isEmpty())
          return;
        BroadcastDialog dialog(contacts, {}, QString(),
                               BroadcastDialog::Mode::Forward, this);
        if (dialog.exec() != QDialog::Accepted ||
            dialog.selectedPeerIds().size() != 1)
          return;
        app_.selectPeer(dialog.selectedPeerIds().first());
        if (type == QStringLiteral("image"))
          app_.sendImage(path);
        else if (type == QStringLiteral("video"))
          app_.sendVideo(path);
        else
          app_.sendFile(path);
      });
  connect(agentPanel_, &AgentChatPanel::videoCircleRequested, this,
          [this] { openVideoCircle(); });
  connect(agentPanel_, &AgentChatPanel::videoCirclePublishRequested, this,
          [this](const QString& card, const QString& source) { openVideoCircle(card, source); });
  connect(agentPanel_, &AgentChatPanel::cloudDriveSaveRequested, this,
          [this](const QString& source) { openCloudDrive(source); });
  agentPanel_->setCloudDriveAvailable(cloudDriveAvailable_);
  probeVideoCircleAvailability();
  const bool modelReady = !modelConfig.baseUrl.isEmpty() &&
                          !modelConfig.apiKey.isEmpty();
  const QString modelLabel = modelReady ? modelConfig.modelName :
      !modelKeyError_.isEmpty() ?
          (modelKeyError_.contains(QStringLiteral("未开通")) ?
              QStringLiteral("模型未开通") : QStringLiteral("模型连接失败")) :
      !modelApiKeys_.isEmpty() ? QStringLiteral("模型未开通") :
                                 QStringLiteral("正在连接模型");
  agentPanel_->setModelLabel(modelLabel, modelReady);
  agentPanel_->setReasoningEffortLabel(modelConfig.reasoningEffort);
  agentSplitter->addWidget(agentSessions_);
  agentSplitter->addWidget(agentPanelStack_);
  agentSplitter->setStretchFactor(0, 0);
  agentSplitter->setStretchFactor(1, 1);
  if (sizesToRestore.size() == 2 && sizesToRestore.at(0) > 0 &&
      sizesToRestore.at(1) > 0) {
    agentSplitter->setSizes(sizesToRestore);
  } else {
    agentSplitter->setSizes(QList<int>() << UiZoom::s(300) << UiZoom::s(980));
  }

  connect(agentSessions_, &AgentSessionList::selected, agentPanel_,
          [this](const QString &sessionId) {
            agentPanelStack_->setCurrentWidget(agentPanel_);
            agentPanel_->openSession(sessionId);
          });
  connect(agentSessions_, &AgentSessionList::remoteSelected, agentRemotePanel_,
          [this](const QJsonObject& session) {
            if (!sessionSync_) {
              showToast(QStringLiteral("设备同步服务尚未连接"), 13, 2500);
              return;
            }
            agentPanelStack_->setCurrentWidget(agentRemotePanel_);
            agentRemotePanel_->openSession(session);
          });
  connect(agentSessions_, &AgentSessionList::newSessionRequested, agentPanel_,
          [this] {
            if (agentPanel_->chooseDirectoryAndCreateSession())
              agentPanelStack_->setCurrentWidget(agentPanel_);
          });
  connect(agentSessions_, &AgentSessionList::deleteSessionRequested, this,
          [this](const QString& sessionId) {
            MaiSession session;
            if (!agentController_->agent().getSession(sessionId.toStdString(), session)) {
              agentSessions_->refresh();
              return;
            }
            const QString title = session.isUntitled()
                                      ? QStringLiteral("新对话")
                                      : QString::fromUtf8(session.title.c_str());
            if (!AppMessageDialog::confirm(
                    this, QStringLiteral("删除对话"),
                    QStringLiteral("删除“%1”及其中的聊天记录？\n删除后无法恢复。")
                        .arg(title),
                    QStringLiteral("删除"), /*destructive=*/true))
              return;
            const auto result = agentController_->agent().submit(
                MaiDeleteSession{sessionId.toStdString()});
            if (!result) {
              showToast(result.error().code() == MaiErrorCode::Busy
                            ? QStringLiteral("对话仍在执行，请先停止后再删除")
                            : QString::fromUtf8(result.error().message().c_str()),
                        13, 3500);
            }
          });
  connect(agentSessions_, &AgentSessionList::remoteCreateRequested, this,
          [this](const QJsonObject& device) {
            if (!sessionSync_) {
              showToast(QStringLiteral("设备同步服务尚未连接"), 13, 3000);
              return;
            }
            QPointer<MainWindow> guard(this);
            sessionSync_->sendRemoteLifecycle(
                device, {{QStringLiteral("action"), QStringLiteral("session_create")}},
                [guard](const QJsonObject& result, const QString& error) {
                  if (!guard) return;
                  guard->showToast(error.isEmpty() && result.value(QStringLiteral("ok")).toBool()
                                       ? QStringLiteral("对话已在所属设备创建")
                                       : error.isEmpty() ? QStringLiteral("新建对话失败") : error,
                                   13, 3500);
                });
          });
  connect(agentSessions_, &AgentSessionList::remoteDeleteRequested, this,
          [this](const QJsonObject& session) {
            if (!sessionSync_) {
              showToast(QStringLiteral("设备同步服务尚未连接"), 13, 3000);
              return;
            }
            QString title = session.value(QStringLiteral("title")).toString();
            if (title.isEmpty() || title == QStringLiteral("New session"))
              title = QStringLiteral("新对话");
            if (!AppMessageDialog::confirm(
                    this, QStringLiteral("删除其他设备的对话"),
                    QStringLiteral("删除“%1”及其中的聊天记录？\n此操作会在所属设备上执行，删除后无法恢复。")
                        .arg(title),
                    QStringLiteral("删除"), /*destructive=*/true))
              return;
            const QJsonObject device{{QStringLiteral("device_slot"),
                                       session.value(QStringLiteral("device_slot"))},
                                      {QStringLiteral("device_id"),
                                       session.value(QStringLiteral("device_id"))},
                                      {QStringLiteral("login_session_id"),
                                       session.value(QStringLiteral("login_session_id"))}};
            QPointer<MainWindow> guard(this);
            sessionSync_->sendRemoteLifecycle(
                device,
                {{QStringLiteral("action"), QStringLiteral("session_delete")},
                 {QStringLiteral("target_session_id"), session.value(QStringLiteral("id"))}},
                [guard, session](const QJsonObject& result, const QString& error) {
                  if (!guard) return;
                  if (!error.isEmpty() || !result.value(QStringLiteral("ok")).toBool()) {
                    guard->showToast(error.isEmpty() ? QStringLiteral("删除对话失败") : error, 13, 3500);
                    return;
                  }
                  guard->showToast(QStringLiteral("对话已删除"), 13, 2500);
                  if (guard->agentRemotePanel_ && guard->agentPanelStack_ &&
                      guard->agentPanelStack_->currentWidget() == guard->agentRemotePanel_ &&
                      guard->agentRemotePanel_->session().value(QStringLiteral("id")) ==
                          session.value(QStringLiteral("id"))) {
                    guard->agentPanelStack_->setCurrentWidget(guard->agentPanel_);
                    guard->agentSessions_->setCurrent(guard->agentPanel_->sessionId());
                  }
                });
          });
  connect(agentPanel_, &AgentChatPanel::sessionListChanged, agentSessions_,
          [this] {
            agentSessions_->refresh();
            if (agentPanelStack_->currentWidget() == agentRemotePanel_)
              agentSessions_->setCurrentRemote(agentRemotePanel_->session());
            else
              agentSessions_->setCurrent(agentPanel_->sessionId());
          });
  connect(agentPanel_, &AgentChatPanel::modelConfigurationRequested, this,
          [this] {
            if (modelKeyRefreshHandler_) modelKeyRefreshHandler_();
            showToast(QStringLiteral("正在从账号服务更新模型凭据"), 13, 2500);
          });
  connect(agentPanel_, &AgentChatPanel::modelChangeRequested, this,
          &MainWindow::requestAgentModelChange);
  connect(agentPanel_, &AgentChatPanel::reasoningEffortChangeRequested, this,
          [this](const QString& effort) {
            if (effort != QStringLiteral("low") && effort != QStringLiteral("high") &&
                effort != QStringLiteral("max")) return;
            QSettings settings;
            settings.setValue(QStringLiteral("agent/reasoningEffort"), effort);
            settings.sync();
            if (settings.status() == QSettings::NoError) {
              agentPanel_->setReasoningEffortLabel(effort);
            } else {
              showToast(QStringLiteral("思考强度保存失败"), 13, 2500);
            }
          });
  if (!sessionToRestore.isEmpty())
    agentPanel_->openSession(sessionToRestore);
  if (!remoteToRestore.isEmpty()) {
    agentPanelStack_->setCurrentWidget(agentRemotePanel_);
    agentRemotePanel_->openSession(remoteToRestore);
    agentSessions_->setCurrentRemote(remoteToRestore);
  }
}

void MainWindow::requestAgentModelChange(const QString& model) {
  const QString selected = model.trimmed();
  if (selected.isEmpty() ||
      selected == loadAgentModelConfig(modelApiKeys_).modelName) return;
  pendingModelSelection_ = selected;
  if (modelKeyRefreshHandler_) modelKeyRefreshHandler_();
  else setAccountModelKeys(modelApiKeys_, modelKeyError_);
}

void MainWindow::setUpMessageNotifications() {
  // 系统托盘不可用时（某些精简桌面环境）直接不做通知，而不是崩掉或静默假装成功。
  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    qInfo("[notify] system tray unavailable; message notifications disabled");
    return;
  }
  // 必须给一个真图标：QSystemTrayIcon 拿到空图标时不会真正显示出来，
  // 而 showMessage 对一个没显示的托盘图标是静默失效的——代码照跑、日志照打，
  // 屏幕上什么都不出现。这个坑我踩过一次，是靠真机截图才发现的。
  QIcon trayImage = windowIcon();
  if (trayImage.isNull())
    trayImage = QIcon(QStringLiteral(":/maichat/app-icon.png"));
  // 图标仍然为空就别装通知能用了——托盘不显示时 showMessage 是静默失效的。
  if (trayImage.isNull()) {
    qWarning("[notify] app icon missing; message notifications disabled");
    return;
  }
  trayIcon_ = new QSystemTrayIcon(trayImage, this);
  trayIcon_->setToolTip(QStringLiteral("MaiChat"));
  trayIcon_->show();

  connect(trayIcon_, &QSystemTrayIcon::messageClicked, this,
          [this] { openConversationFromNotification(lastNotifiedPeerId_); });
  connect(&app_, &RemoteIMApplication::incomingMessageArrived, this,
          [this](const QString &peerId, const RemoteIMMessage &message) {
            handleIncomingMessageForNotification(peerId, message);
          });
}

bool MainWindow::appIsForegroundVisible() const {
  // 用整个应用的激活态，而不是只看主窗口：自绘对话框获得焦点时主窗口的
  // isActiveWindow() 会是 false，但 MaiChat
  // 显然仍在用户眼前，也不该弹系统通知。
  const bool applicationActive =
      QGuiApplication::applicationState() == Qt::ApplicationActive;
  return MessageNotification::shouldSuppressForForegroundWindow(
      applicationActive, isMinimized());
}

void MainWindow::handleIncomingMessageForNotification(
    const QString &peerId, const RemoteIMMessage &message) {
  if (!trayIcon_ || peerId.isEmpty())
    return;

  if (appIsForegroundVisible()) {
    // 应用就在用户眼前时，消息页红点和列表更新已经足够；无论当前打开哪一页，
    // 再弹一层 Windows 通知都是纯打扰。
    qInfo("[notify] suppressed peer=%s reason=app-foreground",
          qUtf8Printable(peerId));
    notificationTracker_.clear(peerId);
    return;
  }

  const MessageNotification::DeliveryDecision decision =
      notificationTracker_.record(peerId, message.createdAtMillis);
  if (decision.disposition ==
      MessageNotification::DeliveryDisposition::StartupBacklog) {
    qInfo("[notify] suppressed peer=%s reason=startup-backlog",
          qUtf8Printable(peerId));
    return;
  }
  if (decision.disposition ==
      MessageNotification::DeliveryDisposition::AlreadyPending) {
    qInfo("[notify] suppressed peer=%s reason=already-pending pending=%d",
          qUtf8Printable(peerId), decision.pendingCount);
    return;
  }

  lastNotifiedPeerId_ = peerId;

  const QString title = MessageNotification::title(contactName(peerId), peerId);
  // Windows 的 QSystemTrayIcon 没有可复用的通知 ID，不能原位更新成“69 条”；
  // 再 show 一次只会新增气泡。未查看前只展示首条，数量交给会话未读红点。
  const QString body = MessageNotification::preview(message);
  // 正文不入日志：通知内容会进系统通知中心已经够了，日志里再存一份没有必要。
  qInfo("[notify] requested peer=%s pending=%d", qUtf8Printable(peerId),
        decision.pendingCount);
  trayIcon_->showMessage(title, body, QSystemTrayIcon::Information, 5000);
}

void MainWindow::openConversationFromNotification(const QString &peerId) {
  if (peerId.isEmpty())
    return;
  qInfo("[notify] clicked peer=%s", qUtf8Printable(peerId));

  // 只解除最小化；showNormal 会同时清掉最大化/全屏状态。
  if (isMinimized())
    setWindowState(windowState() & ~Qt::WindowMinimized);
  show();
  raise();
  activateWindow();
  app_.selectPeer(peerId);
  showMessagesPage();
  notificationTracker_.clear(peerId);
}

void MainWindow::showMessagesPage() {
  // 用户看到消息了，堆积计数归零；不清的话下一条通知会显示成「第 5 条」，
  // 而他其实前 4 条都读过了。
  notificationTracker_.clear(app_.chatState().selectedPeerId());
  contentStack_->setCurrentWidget(messagesPage_);
  syncNavigationSelection();
  messageEditor_->setFocus();
}

void MainWindow::showContactsPage() {
  app_.refreshFriendList();
  refreshContactDirectory();
  contentStack_->setCurrentWidget(contactsPage_);
  syncNavigationSelection();
}

void MainWindow::showSettingsPage() {
  refreshSettings();
  probeCloudDriveAvailability();
  contentStack_->setCurrentWidget(settingsPage_);
  syncNavigationSelection();
}

void MainWindow::showRemotePage() {
  contentStack_->setCurrentWidget(remotePage_);
  syncNavigationSelection();
}

void MainWindow::showAgentPage() {
  // 第一次进来才建会话：不进这一页的用户不该在库里留下一个空会话。
  // 已经有历史的话挑最近动过的那个接着用（listSessions 按 updated 倒序）。
  if (agentPanel_->sessionId().isEmpty()) {
    const std::vector<MaiSession> existing =
        agentController_->agent().listSessions();
    if (existing.empty()) {
      agentPanel_->openSession();
    } else {
      agentPanel_->openSession(
          QString::fromUtf8(existing.front().id.data(),
                            static_cast<int>(existing.front().id.size())));
    }
  }
  contentStack_->setCurrentWidget(agentPage_);
  syncNavigationSelection();
}

void MainWindow::syncNavigationSelection() {
  if (contentStack_->currentWidget() == agentPage_) {
    updateNavigationSelection(agentNavButton_);
    return;
  }
  if (contentStack_->currentWidget() == contactsPage_) {
    updateNavigationSelection(contactsNavButton_);
    return;
  }
  if (contentStack_->currentWidget() == remotePage_) {
    updateNavigationSelection(remoteNavButton_);
    return;
  }
  if (contentStack_->currentWidget() == settingsPage_) {
    updateNavigationSelection(settingsNavButton_);
    return;
  }
  updateNavigationSelection(messageNavButton_);
}

void MainWindow::updateNavigationSelection(QPushButton *selectedButton) {
  const QList<QPushButton *> buttons = {messageNavButton_, contactsNavButton_,
                                        remoteNavButton_, settingsNavButton_,
                                        agentNavButton_};
  for (QPushButton *button : buttons) {
    if (!button)
      continue;
    const bool isSelected = button == selectedButton;
    button->setProperty("selected", isSelected);
    applyNavButtonIcon(button, isSelected);
    button->style()->unpolish(button);
    button->style()->polish(button);
    button->update();
  }
}

void MainWindow::updateConnectionIndicator() {
  if (!statusLabel_ || !settingsConnectionText_)
    return;
  const bool connected = app_.isConnected();
  statusLabel_->setText(QString());
  statusLabel_->setPixmap(statusDotPixmap(connected, UiZoom::s(9), UiZoom::s(2),
                                          statusLabel_->devicePixelRatioF()));
  statusLabel_->setProperty("connected", connected);
  statusLabel_->setToolTip(connected ? QStringLiteral("IM 已连接")
                                     : QStringLiteral("IM 未连接"));
  statusLabel_->setAccessibleDescription(connected ? QStringLiteral("已连接")
                                                   : QStringLiteral("未连接"));
  statusLabel_->update();
  settingsConnectionText_->setText(connected ? QStringLiteral("消息已连接")
                                             : QStringLiteral("消息未连接"));
  settingsConnectionText_->setProperty("connected", connected);
  settingsConnectionText_->setToolTip(statusLabel_->toolTip());
  settingsConnectionText_->setAccessibleDescription(statusLabel_->accessibleDescription());
  settingsConnectionText_->style()->unpolish(settingsConnectionText_);
  settingsConnectionText_->style()->polish(settingsConnectionText_);
}

void MainWindow::syncComposerPeer(const QString &peerId) {
  if (!messageEditor_ || composerPeerId_ == peerId)
    return;
  if (!composerPeerId_.isEmpty()) {
    ComposerDraft draft;
    if (!messageEditor_->document()->isEmpty())
      draft.content = QTextDocumentFragment(messageEditor_->document());
    draft.quote = pendingQuote_;
    draft.hasQuote = hasPendingQuote_;
    if (!draft.content.isEmpty() || draft.hasQuote)
      composerDrafts_.insert(composerPeerId_, std::move(draft));
    else
      composerDrafts_.remove(composerPeerId_);
  }

  composerPeerId_ = peerId;
  clearReplySuggestions();
  messageEditor_->clear();
  cancelPendingReply();
  if (composerDrafts_.contains(peerId)) {
    const ComposerDraft draft = composerDrafts_.take(peerId);
    if (!draft.content.isEmpty()) {
      QTextCursor cursor = messageEditor_->textCursor();
      cursor.insertFragment(draft.content);
      cursor.movePosition(QTextCursor::End);
      messageEditor_->setTextCursor(cursor);
    }
    pendingQuote_ = draft.quote;
    hasPendingQuote_ = draft.hasQuote;
    refreshPendingReplyBar();
  }
  updateComposerState();
}

void MainWindow::refreshMessages() {
  RemoteDiagnostics::PerformanceSpan performance("message-refresh");
  const QString selectedPeer = app_.chatState().selectedPeerId();
  syncComposerPeer(selectedPeer);
  titleLabel_->setText(selectedPeer.isEmpty() ? QStringLiteral("请选择会话")
                                              : contactName(selectedPeer));
  updateConnectionIndicator();
  updateComposerState();

  const QList<RemoteIMMessage> messages =
      app_.chatState().messagesWith(selectedPeer);
  if (replySuggestionBar_ && replySuggestionBar_->isVisible() &&
      (selectedPeer != replySuggestionPeerId_ ||
       latestSelectedMessageId() != replySuggestionLatestMessageId_)) {
    clearReplySuggestions();
  }
  sentApprovalTokens_.clear();
  resolvedApprovalTokens_.clear();
  autoDeclinedApprovalTokens_.clear();
  for (const RemoteIMMessage &message : messages) {
    if (!message.hasApprovalDecision || !message.approvalDecision.isValid())
      continue;
    const QString &token = message.approvalDecision.token;
    if (message.approvalDecision.action ==
        RemoteIMApprovalAction::AutoDeclined) {
      autoDeclinedApprovalTokens_.insert(token);
    } else if (message.approvalDecision.action ==
               RemoteIMApprovalAction::Resolved) {
      resolvedApprovalTokens_.insert(token);
    } else if (message.direction == RemoteIMMessageDirection::Outgoing &&
               message.status == RemoteIMMessageStatus::Sent) {
      sentApprovalTokens_.insert(token);
    }
  }
  bool needFullRebuild = selectedPeer != renderedPeerId_ ||
                         renderedEmptyView_ != messages.isEmpty() ||
                         messageLayout_->count() == 0;
  if (!needFullRebuild) {
    // 渲染窗口只覆盖尾部一批，重排检测同样只对着窗口做：窗口内的尾部块与
    // 已渲染的集合相同但顺序变化时才完整重建；增删交给增量路径。
    const int renderedCount = renderedMessageIds_.size();
    if (renderedCount > 0 && messages.size() >= renderedCount) {
      QStringList tailIds;
      tailIds.reserve(renderedCount);
      for (int i = messages.size() - renderedCount; i < messages.size(); ++i) {
        tailIds.append(messages.at(i).id);
      }
      if (tailIds != renderedMessageIds_) {
        QSet<QString> renderedIds;
        QSet<QString> tailIdSet;
        for (const QString &id : renderedMessageIds_)
          renderedIds.insert(id);
        for (const QString &id : tailIds)
          tailIdSet.insert(id);
        needFullRebuild = renderedIds == tailIdSet;
      }
    }
  }
// 好友资料回调可能晚于历史消息。仅在已绘制发送者的名称或头像 URL 变化时重建，
// 让旧气泡采用真实头像，同时避免常规消息更新触发整页重绘。
  if (!needFullRebuild) {
    const QList<QLabel *> avatars = messageContainer_->findChildren<QLabel *>();
    for (const QLabel *avatar : avatars) {
      const QString userId = avatar->property("avatarUserId").toString();
      if (userId.isEmpty())
        continue;
      QString avatarUrl;
      for (const RemoteIMContact &contact : app_.chatState().contacts()) {
        if (contact.userId == userId) {
          avatarUrl = contact.avatarUrl.trimmed();
          break;
        }
      }
      if (avatar->property("avatarDisplayName").toString() !=
              contactName(userId) ||
          avatar->property("avatarUrl").toString() != avatarUrl) {
        needFullRebuild = true;
        break;
      }
    }
  }
  if (needFullRebuild) {
    rebuildMessageList(selectedPeer, messages);
    return;
  }
  applyIncrementalMessageUpdate(messages);
}

MainWindow::ApprovalDisplayState
MainWindow::approvalDisplayState(const RemoteIMMessage &message) const {
  if (!message.hasApprovalRequest || !message.approvalRequest.isValid()) {
    return ApprovalDisplayState::Available;
  }
  const QString &token = message.approvalRequest.token;
  if (autoDeclinedApprovalTokens_.contains(token))
    return ApprovalDisplayState::AutoDeclined;
  if (resolvedApprovalTokens_.contains(token))
    return ApprovalDisplayState::Resolved;
  if (sentApprovalTokens_.contains(token))
    return ApprovalDisplayState::Sent;
  if (submittingApprovalTokens_.contains(token))
    return ApprovalDisplayState::Sending;
  return ApprovalDisplayState::Available;
}

// 针对切换会话等全量场景重建当前消息行和索引。
// 此路径会移除旧控件再创建新行；不能用于普通单条消息更新或流式内容刷新。
void MainWindow::rebuildMessageList(const QString &peerId,
                                    const QList<RemoteIMMessage> &messages) {
  QObject::disconnect(messageScrollToBottomConn_);
  while (QLayoutItem *item = messageLayout_->takeAt(0)) {
    if (QWidget *widget = item->widget())
      delete widget;
    delete item;
  }
  renderedPeerId_ = peerId;
  renderedMessageIds_.clear();
  messageRowById_.clear();
  renderedStatusById_.clear();
  renderedApprovalStateById_.clear();
  loadEarlierButton_ = nullptr;
  activityBubble_ = nullptr;
  const RemoteIMActivitySignal activity = app_.activityForPeer(peerId);
  renderedEmptyView_ = messages.isEmpty() && activity.activityId.isEmpty();

  if (renderedEmptyView_) {
    auto *emptyView = new QWidget(messageContainer_);
    emptyView->setObjectName(QStringLiteral("emptyMessagesView"));
    auto *emptyLayout = new QVBoxLayout(emptyView);
    emptyLayout->setContentsMargins(0, 52, 0, 0);
    emptyLayout->setSpacing(10);

    auto *iconLabel = new QLabel(QStringLiteral("◇"), emptyView);
    iconLabel->setObjectName(QStringLiteral("emptyMessageIcon"));
    iconLabel->setAlignment(Qt::AlignCenter);
    auto *title = new QLabel(QStringLiteral("暂无消息"), emptyView);
    title->setObjectName(QStringLiteral("emptyMessageTitle"));
    title->setAlignment(Qt::AlignCenter);
    auto *subtitle =
        new QLabel(QStringLiteral("发送一条消息开始远程任务。"), emptyView);
    subtitle->setObjectName(QStringLiteral("emptyMessageSubtitle"));
    subtitle->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(iconLabel);
    emptyLayout->addWidget(title);
    emptyLayout->addWidget(subtitle);
    emptyLayout->addStretch(1);
    emptyView->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
            #emptyMessageIcon {
                color: #98a2b3;
                font-size: 28px;
                background: transparent;
            }
            #emptyMessageTitle {
                color: #101828;
                font-size: 16px;
                font-weight: 700;
                background: transparent;
            }
            #emptyMessageSubtitle {
                color: #667085;
                font-size: 13px;
                background: transparent;
            }
        )")));
    messageLayout_->addWidget(emptyView);
    return;
  }

  // 布局固定结构：[0]=加载更早按钮（无更早时隐藏），随后消息行，末尾弹簧。
  loadEarlierButton_ =
      new QPushButton(QStringLiteral("加载更早的消息"), messageContainer_);
  loadEarlierButton_->setObjectName(QStringLiteral("loadEarlierButton"));
  loadEarlierButton_->setCursor(Qt::PointingHandCursor);
  loadEarlierButton_->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QPushButton#loadEarlierButton {
            background: #f1f5f9;
            border: 1px solid #e2e8f0;
            border-radius: 14px;
            color: #475569;
            font-size: 12px;
            font-weight: 700;
            padding: 5px 14px;
        }
        QPushButton#loadEarlierButton:hover {
            background: #e2e8f0;
        }
    )")));
  connect(loadEarlierButton_, &QPushButton::clicked, this, [this] {
    const QString peer = app_.chatState().selectedPeerId();
    // 内存里已有但没渲染的先补一批（便宜，布局有缓存）；内存用尽才向 DB
    // 取下一页。 两条路最后都落在 prependRenderWindow——DB
    // 页进来时落在渲染窗口之外， 增量路径不会给它们建行，必须由这里显式扩窗口。
    if (app_.chatState().messageCountWith(peer) <= renderedMessageIds_.size() &&
        app_.loadEarlierMessages(peer) <= 0) {
      return;
    }
    const QList<RemoteIMMessage> messages = app_.chatState().messagesWith(peer);
    int head = messages.size();
    for (int i = 0; i < messages.size(); ++i) {
      if (messageRowById_.contains(messages.at(i).id)) {
        head = i;
        break;
      }
    }
    prependRenderWindow(messages, qMax(0, head - kMessageRenderWindow), true);
  });
  auto *buttonRow = new QWidget(messageContainer_);
  auto *buttonRowLayout = new QHBoxLayout(buttonRow);
  buttonRowLayout->setContentsMargins(0, 0, 0, 0);
  buttonRowLayout->addStretch(1);
  buttonRowLayout->addWidget(loadEarlierButton_);
  buttonRowLayout->addStretch(1);
  messageLayout_->addWidget(buttonRow);

  // 渲染窗口：只画尾部这批。深度取「默认窗口」和「本会话此前翻过的深度」的较大者，
  // 用户往下翻过的历史在切走再切回时不丢。全量排版的首切成本见
  // kMessageRenderWindow。
  const int total = messages.size();
  const int remembered = renderedWindowByPeer_.value(peerId, 0);
  const int window = qBound(0, qMax(kMessageRenderWindow, remembered), total);
  // 首屏同步渲染的行数：AI 回答类的行一条就几百像素高，6 条足以铺满视口；
  // 同步多排一行就多一拍点击延迟（富格式段落实测 7ms/块、列表 32ms/块，
  // 长回答一条几十块）。窗口剩余部分在下方按小块分批补上。
  constexpr int kMessageRenderSyncRows = 6;
  const int syncRows = qMin(kMessageRenderSyncRows, window);
  const int firstRendered = total - syncRows;
  renderedWindowByPeer_.insert(peerId, window);
  QStringList renderedApprovalIds;
  for (int i = firstRendered; i < total; ++i) {
    const RemoteIMMessage &message = messages.at(i);
    QWidget *row = createMessageBubble(message);
    setMessageRowDivider(row, i > 0);
    messageLayout_->addWidget(row);
    renderedMessageIds_.append(message.id);
    messageRowById_.insert(message.id, row);
    renderedStatusById_.insert(message.id, message.status);
    renderedApprovalStateById_.insert(message.id,
                                      approvalDisplayState(message));
    if (message.hasApprovalRequest && message.approvalRequest.isValid()) {
      renderedApprovalIds.append(message.id);
    }
  }
  messageLayout_->addStretch(1);
  updateActivityBubble();
  updateLoadEarlierVisibility();

  QTimer::singleShot(0, this, [this, peerId, renderedApprovalIds] {
    if (renderedPeerId_ != peerId ||
        app_.chatState().selectedPeerId() != peerId)
      return;
    updateMessageBubbleWidths();
    scrollMessagesToBottom();
    QTimer::singleShot(0, this, [this, peerId, renderedApprovalIds] {
      if (renderedPeerId_ != peerId ||
          app_.chatState().selectedPeerId() != peerId)
        return;
      QWidget *viewport = messageScroll_ ? messageScroll_->viewport() : nullptr;
      for (const QString &messageId : renderedApprovalIds) {
        QWidget *row = messageRowById_.value(messageId);
        if (!row || !viewport)
          continue;
        const QRect rowRect(row->mapTo(viewport, QPoint(0, 0)), row->size());
        const bool intersectsViewport = viewport->rect().intersects(rowRect);
        qInfo().noquote()
            << QStringLiteral("[approval-ui] card-rendered id=%1 peer=%2 "
                              "path=full-rebuild visible=%3 scroll=%4/%5")
                   .arg(messageId, peerId,
                        intersectsViewport ? QStringLiteral("true")
                                           : QStringLiteral("false"))
                   .arg(messageScroll_->verticalScrollBar()->value())
                   .arg(messageScroll_->verticalScrollBar()->maximum());
      }
    });
  });

  // 窗口剩余部分分块补渲染：每块走一轮事件循环，点击先出画面、补排版在后台
  // 落地。块不能太大，否则单块又是一次可感知的停顿。
  if (syncRows < window) {
    constexpr int kFillChunkRows = 6;
    const int targetRows = window;
    auto fillStep = std::make_shared<std::function<void()>>();
    *fillStep = [this, peerId, targetRows, fillStep, kFillChunkRows]() {
      // 切走了/整屏重建了就停；窗口补满也停。
      if (renderedPeerId_ != peerId ||
          app_.chatState().selectedPeerId() != peerId)
        return;
      if (renderedMessageIds_.size() >= targetRows)
        return;
      const QList<RemoteIMMessage> messages =
          app_.chatState().messagesWith(peerId);
      int head = messages.size();
      for (int i = 0; i < messages.size(); ++i) {
        if (messageRowById_.contains(messages.at(i).id)) {
          head = i;
          break;
        }
      }
      prependRenderWindow(messages, qMax(0, head - kFillChunkRows), true);
      QTimer::singleShot(0, this, *fillStep);
    };
    QTimer::singleShot(30, this, *fillStep);
  }
}

// 把渲染窗口向上扩：给 [newHeadIndex, 当前窗口头) 的消息补建气泡行。
// keepViewport=true 时锚定当前可视位置（头顶补内容视口不跳），跳转定位时传
// false。
void MainWindow::prependRenderWindow(const QList<RemoteIMMessage> &messages,
                                     int newHeadIndex, bool keepViewport) {
  int currentHead = messages.size();
  for (int i = 0; i < messages.size(); ++i) {
    if (messageRowById_.contains(messages.at(i).id)) {
      currentHead = i;
      break;
    }
  }
  if (newHeadIndex >= currentHead || newHeadIndex < 0)
    return;

  QScrollBar *bar = messageScroll_->verticalScrollBar();
  const int oldMax = bar->maximum();
  const int oldValue = bar->value();
  constexpr int kLayoutBase = 1; // [0] 是加载更早按钮行
  QStringList newIds;
  for (int i = newHeadIndex; i < currentHead; ++i) {
    const RemoteIMMessage &message = messages.at(i);
    QWidget *row = createMessageBubble(message);
    setMessageRowDivider(row, i > 0);
    messageLayout_->insertWidget(kLayoutBase + (i - newHeadIndex), row);
    newIds.append(message.id);
    messageRowById_.insert(message.id, row);
    renderedStatusById_.insert(message.id, message.status);
    renderedApprovalStateById_.insert(message.id,
                                      approvalDisplayState(message));
  }
  renderedMessageIds_ = newIds + renderedMessageIds_;
  renderedWindowByPeer_.insert(app_.chatState().selectedPeerId(),
                               renderedMessageIds_.size());
  updateLoadEarlierVisibility();

  if (!keepViewport)
    return;
  // 高度要等下一轮布局才进滚动条范围，等 rangeChanged 再补偿，一次性触发。
  const QString peerId = app_.chatState().selectedPeerId();
  QTimer::singleShot(0, this, [this, peerId, bar, oldMax, oldValue] {
    if (renderedPeerId_ != peerId ||
        app_.chatState().selectedPeerId() != peerId)
      return;
    QObject::disconnect(messageScrollToBottomConn_);
    messageScrollToBottomConn_ =
        connect(bar, &QAbstractSlider::rangeChanged, this,
                [this, peerId, bar, oldMax, oldValue](int, int max) {
                  if (renderedPeerId_ != peerId ||
                      app_.chatState().selectedPeerId() != peerId) {
                    QObject::disconnect(messageScrollToBottomConn_);
                    return;
                  }
                  bar->setValue(oldValue + (max - oldMax));
                  QObject::disconnect(messageScrollToBottomConn_);
                });
    bar->setValue(oldValue + (bar->maximum() - oldMax));
  });
}

// 目标消息在渲染窗口之外（更早的）时，把窗口扩到覆盖它。引用跳转/搜索定位用：
// 定位的前提是那一行真的存在。
bool MainWindow::ensureMessageRendered(const QString &messageId) {
  if (messageRowById_.contains(messageId))
    return true;
  const QString peer = app_.chatState().selectedPeerId();
  if (peer.isEmpty())
    return false;
  const QList<RemoteIMMessage> messages = app_.chatState().messagesWith(peer);
  for (int i = 0; i < messages.size(); ++i) {
    if (messages.at(i).id == messageId) {
      prependRenderWindow(messages, i, false);
      return messageRowById_.contains(messageId);
    }
  }
  return false;
}

// 以消息稳定标识比较新旧集合，仅替换发生变化的消息行。
// 正在阅读的滚动位置由原容器保留，不走全量重建路径。
void MainWindow::applyIncrementalMessageUpdate(
    const QList<RemoteIMMessage> &messages) {
  RemoteDiagnostics::PerformanceSpan performance("message-layout");
  const QString peerId = app_.chatState().selectedPeerId();
  QSet<QString> newIds;
  newIds.reserve(messages.size());
  for (const RemoteIMMessage &message : messages)
    newIds.insert(message.id);

  // 移除已消失的消息（如临时 UUID 被 SDK 稳定 id 采纳后旧行退场）。
  for (const QString &id : renderedMessageIds_) {
    if (newIds.contains(id))
      continue;
    if (QWidget *row = messageRowById_.take(id)) {
      messageLayout_->removeWidget(row);
      row->deleteLater();
    }
    renderedStatusById_.remove(id);
    renderedApprovalStateById_.remove(id);
  }

  // 首个仍在的旧消息在新列表中的位置：其前方的新增视为「向上翻页」，
  // 其后方的新增视为实时追加。
  int firstKeptIndex = messages.size();
  for (int i = 0; i < messages.size(); ++i) {
    if (messageRowById_.contains(messages.at(i).id)) {
      firstKeptIndex = i;
      break;
    }
  }

  QScrollBar *bar = messageScroll_->verticalScrollBar();
  const int oldMax = bar->maximum();
  const int oldValue = bar->value();
  const bool wasNearBottom = oldValue >= oldMax - 60;

  bool prepended = false;
  bool appended = false;
  QStringList appendedApprovalIds;
  constexpr int kLayoutBase = 1; // [0] 是加载更早按钮行
  QStringList resultIds;
  resultIds.reserve(messages.size());
  for (int i = 0; i < messages.size(); ++i) {
    const RemoteIMMessage &message = messages.at(i);
    if (QWidget *existing = messageRowById_.value(message.id)) {
      resultIds.append(message.id);
      setMessageRowDivider(existing, i > 0);
      const ApprovalDisplayState approvalState = approvalDisplayState(message);
      if (renderedStatusById_.value(message.id) != message.status ||
          renderedApprovalStateById_.value(message.id) != approvalState) {
        // 状态徽标在气泡内部：原位替换单个气泡，代价 O(1)。
        const int layoutIndex = messageLayout_->indexOf(existing);
        QWidget *fresh = createMessageBubble(message);
        setMessageRowDivider(fresh, i > 0);
        messageLayout_->removeWidget(existing);
        existing->deleteLater();
        messageLayout_->insertWidget(layoutIndex, fresh);
        messageRowById_.insert(message.id, fresh);
        renderedStatusById_.insert(message.id, message.status);
        renderedApprovalStateById_.insert(message.id, approvalState);
      }
      continue;
    }
    // 渲染窗口之外的更早消息（DB 翻页进来的整页都落在窗口上方）：不由增量
    // 路径建行——那是「加载更早」按钮和 ensureMessageRendered 的职责。
    // 注意只在「确有已渲染行」时才跳过：一条行都没有（比如临时 id 被 SDK id
    // 采纳、旧行刚退场）时 firstKeptIndex == size，这时必须走下面的建行，
    // 否则消息列表会被清空。
    if (firstKeptIndex < messages.size() && i < firstKeptIndex)
      continue;
    // i 是完整历史中的下标，不能直接当布局下标。长会话只渲染尾部窗口，
    // 此时 i 可能远大于当前布局数量，QBoxLayout 会把控件追加到末尾弹簧后面。
    // 使用已经确认保留/新增的渲染行数，始终把新消息插在活动状态与弹簧之前。
    const int layoutIndex = kLayoutBase + resultIds.size();
    resultIds.append(message.id);
    QWidget *row = createMessageBubble(message);
    setMessageRowDivider(row, i > 0);
    messageLayout_->insertWidget(layoutIndex, row);
    messageRowById_.insert(message.id, row);
    renderedStatusById_.insert(message.id, message.status);
    renderedApprovalStateById_.insert(message.id,
                                      approvalDisplayState(message));
    if (message.hasApprovalRequest && message.approvalRequest.isValid()) {
      appendedApprovalIds.append(message.id);
      qInfo().noquote() << QStringLiteral(
                               "[approval-ui] card-inserted id=%1 peer=%2 "
                               "path=incremental near_bottom=%3 scroll=%4/%5")
                               .arg(message.id,
                                    app_.chatState().selectedPeerId(),
                                    wasNearBottom ? QStringLiteral("true")
                                                  : QStringLiteral("false"))
                               .arg(oldValue)
                               .arg(oldMax);
    }
    if (i < firstKeptIndex)
      prepended = true;
    else
      appended = true;
  }
  renderedMessageIds_ = resultIds;
  // 活动状态不是历史消息，它永远属于当前这一轮。实时追加消息时按消息索引插入，
  // 会把既有状态行挤到新消息前面；增量更新结束后把它重新锚定到末尾弹簧之前。
  if (activityBubble_) {
    messageLayout_->removeWidget(activityBubble_);
    messageLayout_->insertWidget(qMax(0, messageLayout_->count() - 1),
                                 activityBubble_);
  }
  updateLoadEarlierVisibility();

  QTimer::singleShot(
      0, this,
      [this, peerId, prepended, appended, wasNearBottom, oldMax, oldValue,
       appendedApprovalIds] {
        if (renderedPeerId_ != peerId ||
            app_.chatState().selectedPeerId() != peerId)
          return;
        updateMessageBubbleWidths();
        QScrollBar *bar = messageScroll_->verticalScrollBar();
        QObject::disconnect(messageScrollToBottomConn_);
        if (prepended) {
          // 向上翻页：锚定原可视位置（新内容顶入的高度差补偿到滚动值）。
          messageScrollToBottomConn_ =
              connect(bar, &QAbstractSlider::rangeChanged, this,
                      [this, peerId, bar, oldMax, oldValue](int, int max) {
                        if (renderedPeerId_ != peerId ||
                            app_.chatState().selectedPeerId() != peerId) {
                          QObject::disconnect(messageScrollToBottomConn_);
                          return;
                        }
                        bar->setValue(oldValue + (max - oldMax));
                        QObject::disconnect(messageScrollToBottomConn_);
                      });
          bar->setValue(oldValue + (bar->maximum() - oldMax));
          return;
        }
        if (appended && wasNearBottom) {
          scrollMessagesToBottom();
        }
        QTimer::singleShot(0, this, [this, peerId, appendedApprovalIds] {
          if (renderedPeerId_ != peerId ||
              app_.chatState().selectedPeerId() != peerId)
            return;
          QWidget *viewport =
              messageScroll_ ? messageScroll_->viewport() : nullptr;
          for (const QString &messageId : appendedApprovalIds) {
            QWidget *row = messageRowById_.value(messageId);
            if (!row || !viewport)
              continue;
            const QRect rowRect(row->mapTo(viewport, QPoint(0, 0)),
                                row->size());
            const bool intersectsViewport =
                viewport->rect().intersects(rowRect);
            qInfo().noquote()
                << QStringLiteral("[approval-ui] card-visibility id=%1 peer=%2 "
                                  "visible=%3 scroll=%4/%5")
                       .arg(messageId, app_.chatState().selectedPeerId(),
                            intersectsViewport ? QStringLiteral("true")
                                               : QStringLiteral("false"))
                       .arg(messageScroll_->verticalScrollBar()->value())
                       .arg(messageScroll_->verticalScrollBar()->maximum());
          }
        });
      });
}

void MainWindow::updateLoadEarlierVisibility() {
  if (!loadEarlierButton_)
    return;
  const QString peer = app_.chatState().selectedPeerId();
  // 按钮承担两件事：补渲染内存里没画的窗口外消息，以及内存用尽后向 DB 翻页。
  const bool unrenderedInMemory =
      app_.chatState().messageCountWith(peer) > renderedMessageIds_.size();
  loadEarlierButton_->setVisible(unrenderedInMemory ||
                                 app_.hasEarlierMessages(peer));
}

void MainWindow::updateActivityBubble() {
  const QString peerId = app_.chatState().selectedPeerId();
  const RemoteIMActivitySignal activity = app_.activityForPeer(peerId);
  const bool hasActivity = !activity.activityId.isEmpty();
  if (renderedEmptyView_) {
    if (hasActivity)
      rebuildMessageList(peerId, app_.chatState().messagesWith(peerId));
    return;
  }
  if (activityBubble_) {
    messageLayout_->removeWidget(activityBubble_);
    activityBubble_->deleteLater();
    activityBubble_ = nullptr;
  }
  if (!hasActivity) {
    if (app_.chatState().messagesWith(peerId).isEmpty()) {
      rebuildMessageList(peerId, {});
    }
    return;
  }

  const bool wasNearBottom = messageScroll_->verticalScrollBar()->maximum() -
                                 messageScroll_->verticalScrollBar()->value() <=
                             UiZoom::s(40);
  auto *row = new QWidget(messageContainer_);
  row->setObjectName(QStringLiteral("remoteImActivityRow"));
  auto *layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(new RemoteIMActivityBubble(activity, row));
  layout->addStretch(1);
  activityBubble_ = row;
  const int insertion = qMax(0, messageLayout_->count() - 1);
  messageLayout_->insertWidget(insertion, row);
  if (wasNearBottom)
    scrollMessagesToBottom();
}

void MainWindow::scrollMessagesToBottom() {
  // 气泡高度依赖刚设好的宽度（自动换行），滚动条范围要到下一轮布局才正确；
  // 此刻直接读 maximum() 常拿到旧值，改为等 rangeChanged 再跳到底，一次性触发；
  // 先断开上一次挂起的连接，避免快速切换会话时处理器堆叠。
  QScrollBar *bar = messageScroll_->verticalScrollBar();
  QObject::disconnect(messageScrollToBottomConn_);
  messageScrollToBottomConn_ = connect(
      bar, &QAbstractSlider::rangeChanged, this, [this, bar](int, int max) {
        bar->setValue(max);
        QObject::disconnect(messageScrollToBottomConn_);
      });
  // 内容本就放得下、不会触发 rangeChanged 时的兜底：此时 maximum() 已正确。
  bar->setValue(bar->maximum());
}

void MainWindow::openAddContactDialog() {
  AddContactDialog dialog(this);
  QPointer<AddContactDialog> target(&dialog);
  if (friendQrProvider_) {
    friendQrProvider_([target](const QString& userId, const QByteArray& png,
                               const QString& error) {
      if (target) target->setOwnQr(userId, png, error);
    });
  }
  if (dialog.exec() != QDialog::Accepted)
    return;
  const QString userId = dialog.userId();
  if (userId.isEmpty())
    return;
  app_.requestFriend(userId, userId);
}

void MainWindow::openFriendRequestsDialog() {
  auto* dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(QStringLiteral("好友申请"));
  dialog->resize(UiZoom::s(500), UiZoom::s(420));
  auto* layout = new QVBoxLayout(dialog);
  auto* status = new QLabel(QStringLiteral("正在获取好友申请…"), dialog);
  layout->addWidget(status);
  auto* scroll = new QScrollArea(dialog);
  scroll->setWidgetResizable(true);
  auto* content = new QWidget(scroll);
  auto* rows = new QVBoxLayout(content);
  rows->setAlignment(Qt::AlignTop);
  scroll->setWidget(content);
  layout->addWidget(scroll);
  QPointer<QDialog> target(dialog);
  QPointer<MainWindow> self(this);
  app_.friendRequests([self, target, rows, status](bool ok, const QString& error,
                                                   const QList<RemoteIMFriendRequest>& requests) {
    if (!self || !target) return;
    if (!ok || requests.isEmpty()) {
      status->setText(ok ? QStringLiteral("暂无待处理的好友申请") :
                           (error.isEmpty() ? QStringLiteral("获取好友申请失败") : error));
      return;
    }
    status->setText(QStringLiteral("等待你处理的好友申请"));
    for (const RemoteIMFriendRequest& request : requests) {
      auto* row = new QFrame(target);
      auto* rowLayout = new QHBoxLayout(row);
      auto* description = new QLabel(
          QStringLiteral("%1\n%2").arg(request.displayName,
                                       request.message.isEmpty() ?
                                           QStringLiteral("请求添加你为好友") : request.message), row);
      description->setWordWrap(true);
      rowLayout->addWidget(description, 1);
      auto* refuse = new QPushButton(QStringLiteral("拒绝"), row);
      auto* accept = new QPushButton(QStringLiteral("同意"), row);
      rowLayout->addWidget(refuse);
      rowLayout->addWidget(accept);
      auto decide = [self, target, row, request](bool allow) {
        if (!self || !target) return;
        self->app_.decideFriendRequest(request.userId, allow,
                                       [self, target, row](bool success, const QString& error) {
          if (!self || !target) return;
          if (success) row->hide();
          else AppMessageDialog::show(self.data(), AppMessageDialog::Kind::Warning,
                                      QStringLiteral("好友申请"), error);
        });
      };
      connect(refuse, &QPushButton::clicked, row, [decide] { decide(false); });
      connect(accept, &QPushButton::clicked, row, [decide] { decide(true); });
      rows->addWidget(row);
    }
  });
  dialog->show();
}

bool MainWindow::handleComposerPaste() {
  return insertComposerMimeData(QApplication::clipboard()->mimeData());
}

bool MainWindow::insertComposerMimeData(const QMimeData *mime) {
  if (app_.chatState().selectedPeerId().isEmpty())
    return false;
  if (!mime)
    return false;

  // 1) 本地文件（资源管理器复制或直接拖进来的）：内联插入到输入框。
  if (mime->hasUrls()) {
    QStringList files;
    for (const QUrl &url : mime->urls()) {
      if (!url.isLocalFile())
        continue;
      const QString path = url.toLocalFile();
      if (QFileInfo(path).isFile())
        files << path;
    }
    if (!files.isEmpty()) {
      for (const QString &path : files) {
        // mp4/mov 走 IM 的视频消息（对端能直接播）；能被 Qt 认出的图片按图片发
        // （对端气泡里直接出图、可预览）；其余一律按文件卡发。图片判断走内容而非
        // 扩展名，HEIC 之类 Qt
        // 读不了的会自然落到文件卡，不会变成一张打不开的破图。
        if (isSupportedVideoFile(path)) {
          insertComposerVideo(path);
        } else if (!QImageReader::imageFormat(path).isEmpty()) {
          insertComposerImageFile(path);
        } else {
          insertComposerFile(path);
        }
      }
      return true;
    }
  }

  // 2) 图像数据（截图工具、复制的图片，没有对应磁盘文件）：内联插入到输入框。
  if (mime->hasImage()) {
    const QImage image = qvariant_cast<QImage>(mime->imageData());
    if (!image.isNull()) {
      insertComposerImage(image);
      return true;
    }
  }
  return false; // 交给 QTextEdit 默认处理（插入文本）
}

void MainWindow::insertComposerImage(const QImage &image) {
  // 原图存临时 PNG（发送用原图）；内联显示时按最大宽度缩放，资源名即文件路径，
  // QTextEdit 会从磁盘加载渲染，发送时也从这个路径取原图。
  const QString dir =
      QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
          .filePath(QStringLiteral("maichat-paste"));
  QDir().mkpath(dir);
  const QString path = QDir(dir).filePath(
      QStringLiteral("paste-%1.png").arg(QDateTime::currentMSecsSinceEpoch()));
  if (!image.save(path, "PNG"))
    return;

  QTextImageFormat fmt;
  fmt.setName(path);
  int w = image.width();
  int h = image.height();
  constexpr int kMaxWidth = 240;
  if (w > kMaxWidth && w > 0) {
    h = h * kMaxWidth / w;
    w = kMaxWidth;
  }
  fmt.setWidth(w);
  fmt.setHeight(h);
  QTextCursor cursor = messageEditor_->textCursor();
  cursor.insertImage(fmt);
  messageEditor_->setTextCursor(cursor);
  messageEditor_->setFocus();
  updateComposerState();
}

void MainWindow::insertComposerImageFile(const QString &localPath) {
  // 资源名直接用原文件路径：QTextEdit
  // 从磁盘加载渲染，collectComposerAttachments 也据此判定为图片（没有
  // pending-file:// 前缀），发送时发的就是这个原文件——
  // 不像剪贴板图像那样先落一份 PNG，3MB 的 JPG 不会被重编码成十几 MB 的 PNG。
  QImageReader reader(localPath);
  const QSize size = reader.size();
  if (!size.isValid() || size.isEmpty()) {
    insertComposerFile(localPath); // 读不出尺寸就别硬塞，退回文件卡
    return;
  }

  QTextImageFormat fmt;
  fmt.setName(localPath);
  int w = size.width();
  int h = size.height();
  constexpr int kMaxWidth = 240;
  if (w > kMaxWidth && w > 0) {
    h = h * kMaxWidth / w;
    w = kMaxWidth;
  }
  fmt.setWidth(w);
  fmt.setHeight(h);
  QTextCursor cursor = messageEditor_->textCursor();
  cursor.insertImage(fmt);
  messageEditor_->setTextCursor(cursor);
  messageEditor_->setFocus();
  updateComposerState();
}

void MainWindow::insertComposerFile(const QString &localPath) {
  // 文件在输入框里用一枚「文件卡」缩略图表示（📄 文件名），资源名带
  // pending-file:// 前缀， 发送时据此识别为文件（区别于图片路径）。
  insertComposerChip(localPath, QStringLiteral("📄"), kComposerFilePrefix);
}

void MainWindow::insertComposerVideo(const QString &localPath) {
  // 视频同理，只是前缀换成 pending-video://，发送时走 IM
  // 的视频消息而不是文件消息。
  insertComposerChip(localPath, QStringLiteral("🎬"), kComposerVideoPrefix);
}

void MainWindow::insertComposerChip(const QString &localPath,
                                    const QString &icon,
                                    const QString &resourcePrefix) {
  QString shown = QFileInfo(localPath).fileName();
  if (shown.size() > 22)
    shown = shown.left(19) + QStringLiteral("…");
  const QString label = icon + QStringLiteral(" ") + shown;

  const QFontMetrics fm(messageEditor_->font());
  const int chipW = fm.horizontalAdvance(label) + 22;
  const int chipH = 28;
  const qreal chipDpr = messageEditor_->devicePixelRatioF();
  QPixmap chip((QSizeF(chipW, chipH) * chipDpr).toSize());
  chip.setDevicePixelRatio(chipDpr);
  chip.fill(Qt::transparent);
  {
    QPainter p(&chip);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(messageEditor_->font());
    p.setPen(QPen(QColor(QStringLiteral("#d9e4ef"))));
    p.setBrush(QColor(QStringLiteral("#f1f6fb")));
    p.drawRoundedRect(QRectF(0.5, 0.5, chipW - 1.0, chipH - 1.0), 6, 6);
    p.setPen(QColor(QStringLiteral("#33475b")));
    p.drawText(QRectF(11, 0, chipW - 14.0, chipH),
               Qt::AlignVCenter | Qt::AlignLeft, label);
  }

  const QString resourceName = resourcePrefix + localPath;
  messageEditor_->document()->addResource(QTextDocument::ImageResource,
                                          QUrl(resourceName), chip);
  QTextImageFormat fmt;
  fmt.setName(resourceName);
  fmt.setWidth(chipW);
  fmt.setHeight(chipH);
  QTextCursor cursor = messageEditor_->textCursor();
  cursor.insertImage(fmt);
  messageEditor_->setTextCursor(cursor);
  messageEditor_->setFocus();
  updateComposerState();
}

bool MainWindow::composerHasAttachments() const {
  // 内联的图片/文件在纯文本里表现为对象替换符（U+FFFC）。
  return messageEditor_ &&
         messageEditor_->toPlainText().contains(QChar(0xFFFC));
}

QList<MainWindow::ComposerAttachment>
MainWindow::collectComposerAttachments() const {
  QList<ComposerAttachment> attachments;
  if (!messageEditor_)
    return attachments;
  const QTextDocument *doc = messageEditor_->document();
  // 按文档顺序取出所有内联对象（图片/文件）。
  for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
      const QTextFragment frag = it.fragment();
      if (!frag.isValid() || !frag.charFormat().isImageFormat())
        continue;
      const QString name = frag.charFormat().toImageFormat().name();
      const int position = frag.position();
      if (name.startsWith(kComposerFilePrefix)) {
        attachments.append(
            ComposerAttachment{ComposerAttachment::Kind::File,
                               name.mid(kComposerFilePrefix.size()), position});
      } else if (name.startsWith(kComposerVideoPrefix)) {
        attachments.append(ComposerAttachment{
            ComposerAttachment::Kind::Video,
            name.mid(kComposerVideoPrefix.size()), position});
      } else {
        attachments.append(ComposerAttachment{ComposerAttachment::Kind::Image,
                                              name, position});
      }
    }
  }
  return attachments;
}

bool MainWindow::composerTextPrecedesFirstAttachment(
    int firstAttachmentPosition) const {
  if (!messageEditor_ || firstAttachmentPosition <= 0)
    return false;
  QString before =
      messageEditor_->document()->toPlainText().left(firstAttachmentPosition);
  // 内联附件本身在纯文本里是一个对象替换符，不能算作「文字」。
  before.remove(QChar(0xFFFC));
  return !before.trimmed().isEmpty();
}

bool MainWindow::openDockedVideo(const QString &path, const QStringList &playlist,
                                 int64_t startPositionUs,
                                 std::shared_ptr<MaiCloudVod> cloudSource) {
#if defined(MAICHAT_HAS_FFPLAY)
  if (!videoDockSlot_ || (!cloudSource && !QFileInfo(path).isFile())) return false;
  const bool cloud = static_cast<bool>(cloudSource);
  if (auto active = MaiFfplayVideoDialog::activeDialog()) active->close();
  auto *dialog = cloudSource ?
      new MaiFfplayVideoDialog(path, std::move(cloudSource), this) :
      new MaiFfplayVideoDialog(path, this, startPositionUs);
  dialog->setObjectName(QStringLiteral("dockedVideoPlayer"));
  dialog->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
  dialog->setMinimumSize(0, 0);
  dialog->setDocked(true);
  if (!cloud) {
    dialog->setPlaylist(playlist);
    dialog->setForwardAction([this](const QString &currentPath) {
      forwardLocalVideo(currentPath);
    });
  }
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->installEventFilter(this);
  if (cloud && maiChatAccountIdProvider_) {
    const QString ownerAccount = maiChatAccountIdProvider_();
    auto* accountGuard = new QTimer(dialog);
    accountGuard->setInterval(500);
    connect(accountGuard, &QTimer::timeout, dialog,
            [this, dialog, ownerAccount] {
      if (!maiChatAccountIdProvider_ ||
          maiChatAccountIdProvider_() != ownerAccount) dialog->close();
    });
    accountGuard->start();
  }
  videoDialog_ = dialog;
  connect(dialog, &QObject::destroyed, this, [this] {
    if (!videoDialog_ && videoDockSlot_) videoDockSlot_->hide();
    if (!videoDialog_ && videoDockPane_) videoDockPane_->hide();
  });
  dialog->show();
  dialog->raise();
  syncVideoDock();
  QTimer::singleShot(0, this, [this] { syncVideoDock(); });
  if (dialog->isStarted()) return true;
  dialog->close();
  return false;
#else
  Q_UNUSED(path);
  Q_UNUSED(playlist);
  Q_UNUSED(startPositionUs);
  Q_UNUSED(cloudSource);
  return false;
#endif
}

void MainWindow::syncVideoDock() {
  if (!videoDockSlot_ || !videoDockPane_ || !videoContentSplitter_) return;
  if (!videoDialog_ || !videoDialog_->isVisible() || videoDialog_->isFullScreen()) {
    videoDockSlot_->hide();
    videoDockPane_->hide();
    return;
  }
  QWidget *area = videoContentSplitter_->parentWidget();
  if (!area) return;
  const bool vertical = area->width() < UiZoom::s(1100);
  const Qt::Orientation orientation = vertical ? Qt::Vertical : Qt::Horizontal;
  const bool orientationChanged = videoContentSplitter_->orientation() != orientation;
  const bool wasHidden = videoDockPane_->isHidden();
  if (orientationChanged) videoContentSplitter_->setOrientation(orientation);
  if (wasHidden) videoDockPane_->show();
  if (orientationChanged || wasHidden) {
    const int extent = vertical ? area->height() : area->width();
    const int maximum = vertical ? qMax(0, extent - UiZoom::s(300))
                                 : qMax(0, extent - UiZoom::s(640));
    const int initial = vertical
        ? qMin(extent / 2, qMax(0, extent - UiZoom::s(300)))
        : qMin(extent * 52 / 100, qMax(0, extent - UiZoom::s(640)));
    const int previous = vertical ? videoDockVerticalExtent_ : videoDockHorizontalExtent_;
    const int requested = previous > 0 ? qMin(previous, maximum) : initial;
    videoContentSplitter_->setSizes({requested, qMax(0, extent - requested)});
  }
  const int width = videoDockPane_->width();
#if defined(Q_OS_MAC)
  const int chromeHeight = 0;
#else
  const int chromeHeight = UiZoom::s(105);
#endif
  const int height = qMin(width * 9 / 16 + chromeHeight,
                          qMax(0, videoDockPane_->height() - UiZoom::s(8)));
  videoDockSlot_->setMinimumWidth(0);
  videoDockSlot_->setMaximumWidth(QWIDGETSIZE_MAX);
  videoDockSlot_->setFixedHeight(height);
  videoDockSlot_->show();
  if (videoDockSlot_->width() > 0 && videoDockSlot_->height() > 0)
    videoDialog_->setGeometry(QRect(videoDockSlot_->mapToGlobal(QPoint(0, 0)),
                                    videoDockSlot_->size()));
}

void MainWindow::openVideoPreview(const RemoteIMVideoAttachment &attachment) {
  const QString path = attachment.localPath.trimmed();
  if (path.isEmpty() || !QFile::exists(path)) {
    AppMessageDialog::show(
        this, AppMessageDialog::Kind::Warning, QStringLiteral("无法播放"),
        QStringLiteral("视频尚未下载完成或本地缓存已被清理。"));
    return;
  }
#if defined(MAICHAT_HAS_FFPLAY)
  if (!openDockedVideo(path))
    AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                           QStringLiteral("无法播放"),
                           QStringLiteral("FFplay 渲染后端或视频文件不可用。"));
#else
  const QString title = attachment.fileName.trimmed().isEmpty()
                            ? QFileInfo(path).fileName()
                            : attachment.fileName.trimmed();
  auto *dialog = new VideoPreviewDialog(path, title, this);
  dialog->setForwardAction([this, path] { forwardLocalVideo(path); });
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->show();
#endif
}

void MainWindow::forwardLocalVideo(const QString &path) {
  if (!QFileInfo(path).isFile()) {
    AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                           QStringLiteral("无法转发"),
                           QStringLiteral("视频文件已丢失。"));
    return;
  }
  const QList<RemoteIMContact> contacts = acceptedContacts();
  if (contacts.isEmpty())
    return;
  BroadcastDialog dialog(contacts, {}, QString(),
                         BroadcastDialog::Mode::Forward, this);
  if (dialog.exec() != QDialog::Accepted ||
      dialog.selectedPeerIds().size() != 1)
    return;
  app_.selectPeer(dialog.selectedPeerIds().first());
  app_.sendVideo(path);
}

void MainWindow::openImagePreview(const QString &imagePath, QWidget *source,
                                  const QPixmap *decodedImage) {
  if (imagePreviewDialog_) {
    if (imagePreviewDialog_->isVisible()) {
      imagePreviewDialog_->raise();
      imagePreviewDialog_->activateWindow();
    }
    return;
  }

  auto *dialog = decodedImage ? new ImagePreviewDialog(*decodedImage, this)
                              : new ImagePreviewDialog(imagePath, this);
  imagePreviewDialog_ = dialog;
  dialog->setObjectName(QStringLiteral("imagePreviewDialog"));
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowModality(Qt::ApplicationModal);
  connect(dialog, &QObject::destroyed, this, [this, dialog] {
    if (imagePreviewDialog_ == dialog)
      imagePreviewDialog_ = nullptr;
  });
  auto sourceOpacity = std::make_shared<QPointer<QGraphicsOpacityEffect>>();
  QPointer<QWidget> sourceForRestore(source);
  connect(dialog, &QObject::destroyed, this, [sourceOpacity, sourceForRestore] {
    if (sourceForRestore && *sourceOpacity &&
        sourceForRestore->graphicsEffect() == sourceOpacity->data())
      sourceForRestore->setGraphicsEffect(nullptr);
  });

  if (source) {
    QPointer<QWidget> sourceGuard(source);
    auto closing = std::make_shared<bool>(false);
    dialog->setCloseHandler([this, dialog, sourceGuard, sourceOpacity, closing] {
      if (*closing) return true;
      if (!sourceGuard || !sourceGuard->isVisible() || dialog->sourceImage().isNull())
        return false;
      QRect destination(mapFromGlobal(sourceGuard->mapToGlobal(QPoint(0, 0))),
                        sourceGuard->size());
      if (auto *label = qobject_cast<QLabel *>(sourceGuard.data()); label &&
          label->pixmap() && !label->pixmap()->isNull()) {
        QSize fitted = (QSizeF(label->pixmap()->size()) /
                        label->pixmap()->devicePixelRatioF()).toSize();
        fitted.scale(sourceGuard->size(), Qt::KeepAspectRatio);
        const QPoint inset((sourceGuard->width() - fitted.width()) / 2,
                           (sourceGuard->height() - fitted.height()) / 2);
        destination = QRect(mapFromGlobal(sourceGuard->mapToGlobal(inset)), fitted);
      }
      if (!rect().intersects(destination)) return false;
      *closing = true;
      QSize visibleImage = (QSizeF(dialog->sourceImage().size()) /
                            dialog->sourceImage().devicePixelRatioF()).toSize();
      visibleImage.scale(dialog->size(), Qt::KeepAspectRatio);
      const QPoint imageInset((dialog->width() - visibleImage.width()) / 2,
                              (dialog->height() - visibleImage.height()) / 2);
      const QRect start(mapFromGlobal(dialog->mapToGlobal(imageInset)), visibleImage);
      auto *scrim = new QWidget(this);
      scrim->setObjectName(QStringLiteral("imagePreviewTransitionScrim"));
      scrim->setGeometry(rect());
      scrim->setStyleSheet(QStringLiteral("background:#08090b;"));
      auto *opacity = new QGraphicsOpacityEffect(scrim);
      opacity->setOpacity(0.62);
      scrim->setGraphicsEffect(opacity);
      scrim->show();
      scrim->raise();
      auto *snapshot = new QLabel(this);
      snapshot->setObjectName(QStringLiteral("imagePreviewTransitionSnapshot"));
      snapshot->setPixmap(dialog->sourceImage());
      snapshot->setScaledContents(true);
      snapshot->setGeometry(start);
      snapshot->show();
      snapshot->raise();
      dialog->setWindowOpacity(0.0);
      dialog->hide();
      auto *fade = new QPropertyAnimation(opacity, "opacity", scrim);
      fade->setDuration(220);
      fade->setStartValue(0.62);
      fade->setEndValue(0.0);
      auto *shrink = new QPropertyAnimation(snapshot, "geometry", snapshot);
      shrink->setDuration(220);
      shrink->setEasingCurve(QEasingCurve::OutCubic);
      shrink->setStartValue(start);
      shrink->setEndValue(destination);
      QPointer<ImagePreviewDialog> previewGuard(dialog);
      connect(shrink, &QPropertyAnimation::finished, this,
              [previewGuard, sourceOpacity, scrim, snapshot] {
                scrim->deleteLater();
                snapshot->deleteLater();
                if (*sourceOpacity) (*sourceOpacity)->setOpacity(1.0);
                if (previewGuard) {
                  previewGuard->setCloseHandler({});
                  previewGuard->accept();
                }
              });
      QTimer::singleShot(50, this, [fade, shrink] {
        fade->start(QAbstractAnimation::DeleteWhenStopped);
        shrink->start(QAbstractAnimation::DeleteWhenStopped);
      });
      return true;
    });
  }

  // 保留普通可缩放窗口；气泡点击时在主窗口里用原缩略图过渡到预览位置。
  if (source && source->isVisible() && !source->size().isEmpty()) {
    QPixmap thumbnail;
    QRect start;
    if (auto *label = qobject_cast<QLabel *>(source); label &&
        label->pixmap() && !label->pixmap()->isNull()) {
      thumbnail = *label->pixmap();
      QSize imageSize = (QSizeF(thumbnail.size()) / thumbnail.devicePixelRatioF()).toSize();
      imageSize.scale(source->size(), Qt::KeepAspectRatio);
      const QPoint inset((source->width() - imageSize.width()) / 2,
                         (source->height() - imageSize.height()) / 2);
      start = QRect(mapFromGlobal(source->mapToGlobal(inset)), imageSize);
    } else {
      thumbnail = source->grab();
      start = QRect(mapFromGlobal(source->mapToGlobal(QPoint(0, 0))), source->size());
    }
    if (!thumbnail.isNull()) {
      const QRect target(rect().center().x() - dialog->width() / 2,
                         rect().center().y() - dialog->height() / 2,
                         dialog->width(), dialog->height());
      dialog->move(mapToGlobal(target.topLeft()));
      auto *scrim = new QWidget(this);
      scrim->setObjectName(QStringLiteral("imagePreviewTransitionScrim"));
      scrim->setGeometry(rect());
      scrim->setStyleSheet(QStringLiteral("background:#08090b;"));
      auto *opacity = new QGraphicsOpacityEffect(scrim);
      opacity->setOpacity(0);
      scrim->setGraphicsEffect(opacity);
      scrim->show();
      scrim->raise();
      auto *snapshot = new QLabel(this);
      snapshot->setObjectName(QStringLiteral("imagePreviewTransitionSnapshot"));
      snapshot->setPixmap(thumbnail);
      snapshot->setScaledContents(true);
      snapshot->setGeometry(start);
      snapshot->show();
      snapshot->raise();
      auto *effect = new QGraphicsOpacityEffect(source);
      effect->setOpacity(0.0);
      source->setGraphicsEffect(effect);
      *sourceOpacity = effect;
      auto *fade = new QPropertyAnimation(opacity, "opacity", scrim);
      fade->setDuration(220);
      fade->setStartValue(0.0);
      fade->setEndValue(0.62);
      fade->start(QAbstractAnimation::DeleteWhenStopped);
      auto *expand = new QPropertyAnimation(snapshot, "geometry", snapshot);
      expand->setDuration(220);
      expand->setEasingCurve(QEasingCurve::OutCubic);
      expand->setStartValue(start);
      expand->setEndValue(target);
      connect(expand, &QPropertyAnimation::finished, this,
              [this, dialog, scrim, snapshot] {
                if (imagePreviewDialog_ == dialog) {
                  dialog->show();
                  dialog->raise();
                  dialog->activateWindow();
                }
                QTimer::singleShot(100, this, [scrim, snapshot] {
                  scrim->deleteLater();
                  snapshot->deleteLater();
                });
              });
      expand->start(QAbstractAnimation::DeleteWhenStopped);
      return;
    }
  }
  // 无可见源气泡时直接显示，避免对缓存失效或右键打开制造空过渡。
  dialog->show();
  dialog->raise();
  dialog->activateWindow();
}

void MainWindow::openFilePreview(const RemoteIMFileAttachment &attachment) {
  // 非文档类型没有内嵌预览，转「另存为」。
  if (!isPreviewableDocument(attachment)) {
    saveFileAttachmentToLocal(attachment);
    return;
  }
  if (isPdfFile(attachment)) {
    showPdfPreview(this, attachment.localPath, attachment.fileName);
    return;
  }
  if (isGitDiffFile(attachment) && !hasValidGitDiffIntegrity(attachment)) {
    AppMessageDialog::show(
        this, AppMessageDialog::Kind::Warning, QStringLiteral("Diff 校验失败"),
        QStringLiteral("文件内容与发送方提供的 SHA256 不一致，已停止渲染。"));
    return;
  }
  const QString displayName = attachment.fileName.isEmpty()
                                  ? QFileInfo(attachment.localPath).fileName()
                                  : attachment.fileName;
  const bool htmlFile = isHtmlFile(attachment);
  QString content = readTextFile(attachment.localPath);
  if (isGitDiffFile(attachment))
    content = FilePreviewDialog::normalizeGitDiffHtmlForQt(content);
  FilePreviewDialog dialog(displayName, content, this,
                           htmlFile
                               ? FilePreviewDialog::ContentFormat::Html
                               : FilePreviewDialog::ContentFormat::Markdown);
  dialog.exec();
}

bool MainWindow::copyAttachmentToPath(const RemoteIMFileAttachment &attachment,
                                      const QString &targetPath,
                                      QString *errorMessage) {
  const auto fail = [errorMessage](const QString &reason) {
    if (errorMessage)
      *errorMessage = reason;
    return false;
  };
  const QString sourcePath = attachment.localPath.trimmed();
  if (sourcePath.isEmpty() || !QFile::exists(sourcePath)) {
    return fail(QStringLiteral("文件尚未下载完成或本地缓存已被清理。"));
  }
  if (targetPath.trimmed().isEmpty()) {
    return fail(QStringLiteral("保存路径为空。"));
  }
  if (QFileInfo(sourcePath).canonicalFilePath() ==
      QFileInfo(targetPath).canonicalFilePath()) {
    return fail(QStringLiteral("目标位置与源文件相同。"));
  }
  // QFile::copy
  // 遇到已存在的目标会失败；覆盖语义由调用方的「另存为」对话框确认过。
  if (QFile::exists(targetPath) && !QFile::remove(targetPath)) {
    return fail(QStringLiteral("无法覆盖已存在的文件：%1").arg(targetPath));
  }
  if (!QFile::copy(sourcePath, targetPath)) {
    return fail(QStringLiteral("写入失败：%1").arg(targetPath));
  }
  return true;
}

void MainWindow::saveFileAttachmentToLocal(
    const RemoteIMFileAttachment &attachment) {
  const QString suggestedName = attachment.fileName.isEmpty()
                                    ? QFileInfo(attachment.localPath).fileName()
                                    : attachment.fileName;
  QString startDir = lastAttachmentSaveDir_;
  if (startDir.isEmpty() || !QDir(startDir).exists()) {
    startDir =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  }
  if (startDir.isEmpty())
    startDir = QDir::homePath();
  const QString targetPath = QFileDialog::getSaveFileName(
      this, QStringLiteral("保存到本地"),
      QDir(startDir).filePath(suggestedName.isEmpty() ? QStringLiteral("file")
                                                      : suggestedName));
  if (targetPath.isEmpty())
    return; // 用户取消

  QString error;
  if (!copyAttachmentToPath(attachment, targetPath, &error)) {
    AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                           QStringLiteral("保存失败"), error);
    return;
  }
  lastAttachmentSaveDir_ = QFileInfo(targetPath).absolutePath();
  AppMessageDialog::show(this, AppMessageDialog::Kind::Info,
                         QStringLiteral("保存成功"),
                         QStringLiteral("已保存到：\n%1")
                             .arg(QDir::toNativeSeparators(targetPath)));
}

// 气泡顶部的引用块：一条竖线 + 「发送者：摘要」。
//
// 内容来自**发送时的快照**，不去本地查原消息——被引用的消息可能根本不在本地
// （对端设备发的、本地已清理、或落在还没加载的分页里）。查不到就渲染成空白，
// 而空白引用块比没有引用更难看。
void MainWindow::beginReplyTo(const RemoteIMMessage &message) {
  const QString peerId = message.direction == RemoteIMMessageDirection::Incoming
                             ? message.fromUserId
                             : message.toUserId;
  if (!peerId.isEmpty() && peerId != app_.chatState().selectedPeerId())
    return;
  pendingQuote_ = MessageQuote::quoteFor(message);
  // 摘要为空说明这条消息压不出可展示的内容，引用它只会得到一个空白块。
  hasPendingQuote_ = !pendingQuote_.digest.isEmpty();
  refreshPendingReplyBar();
  if (hasPendingQuote_ && messageEditor_)
    messageEditor_->setFocus();
}

void MainWindow::cancelPendingReply() {
  hasPendingQuote_ = false;
  pendingQuote_ = RemoteIMQuote{};
  refreshPendingReplyBar();
}

void MainWindow::refreshPendingReplyBar() {
  if (!pendingReplyBar_ || !pendingReplyLabel_)
    return;
  if (!hasPendingQuote_) {
    pendingReplyBar_->hide();
    return;
  }
  const QString sender = pendingQuote_.senderId.trimmed();
  pendingReplyLabel_->setText(
      sender.isEmpty() ? QStringLiteral("回复：") + pendingQuote_.digest
                       : QStringLiteral("回复 ") + sender +
                             QStringLiteral("：") + pendingQuote_.digest);
  pendingReplyBar_->show();
}

// 跨端只承诺定位到同一条**逻辑 SDK 消息**。desktop 把一条 SDK 消息按 elem
// 拆成多行， 所以命中可能不止一条：锚定下标最小的那条，不承诺定位到 desktop
// 私有拆分后的某个 elem。
void MainWindow::jumpToQuotedMessage(const QString &sdkMsgId) {
  if (sdkMsgId.isEmpty())
    return;

  // 本地主键是 <sdkMsgId>#<元素序号>：一条 SDK 消息可能拆成多行（图片 +
  // 配文）， 定位到序号最小的那一行，才是「原消息的开头」。在 ChatState
  // 的会话列表里找，
  // 不在已渲染的行里找——渲染窗口只覆盖尾部，更早的命中要等扩窗后才有行。
  const QString prefix = sdkMsgId + QLatin1Char('#');
  const auto findLoadedMessage = [this, &sdkMsgId, &prefix]() -> QString {
    QString best;
    int bestIndex = std::numeric_limits<int>::max();
    app_.chatState().forEachMessageWith(
        app_.chatState().selectedPeerId(),
        [&sdkMsgId, &prefix, &best,
         &bestIndex](const RemoteIMMessage &message) {
          if (message.id == sdkMsgId) {
            best = message.id;
            return;
          }
          if (!message.id.startsWith(prefix))
            return;
          bool ok = false;
          const int index =
              QStringView(message.id).mid(prefix.size()).toInt(&ok);
          if (ok && index < bestIndex) {
            bestIndex = index;
            best = message.id;
          }
        });
    return best;
  };

  // 被引用的消息常常比当前已加载的范围更早——重开会话只加载最近一页。
  // 用户点的是「带我去那条」，所以先按页往回加载再找，而不是直接放弃。
  constexpr int kMaxPagesToLoad = 20;
  const QString peerId = app_.chatState().selectedPeerId();
  for (int page = 0;; ++page) {
    const QString hit = findLoadedMessage();
    if (!hit.isEmpty()) {
      // highlightMessage 内部会把渲染窗口扩到覆盖命中并滚动过去。
      highlightMessage(hit);
      return;
    }
    if (page >= kMaxPagesToLoad)
      break;
    if (!app_.hasEarlierMessages(peerId))
      break;
    if (app_.loadEarlierMessages(peerId) <= 0)
      break;
  }

  // 翻到头也没有：原消息不在本地（别的设备发的、或本地已清理）。引用块里的
  // 快照已经把内容显示出来了，但必须说一声——点了完全没反应最让人困惑，
  // 而这个块还带着手型光标，等于承诺了会有反应。
  showToast(QStringLiteral("原消息不在本地记录中"), 15, 1600);
}

namespace {

// 会随宽度变化重新计算省略号的单行标签。
//
// QLabel 不会自动省略：setWordWrap(false) 只是把超出的部分**硬裁掉**，
// 裁在半个字上，看起来像渲染坏了。摘要虽然已经限到 120 字，
// 但气泡宽度装不下 120 个汉字，所以一定会触发。
class ElidedLabel : public QLabel {
public:
  using QLabel::QLabel;

  void setFullText(const QString &text) {
    fullText_ = text;
    applyElide();
  }

  QSize minimumSizeHint() const override {
    // 不让完整文本决定最小宽度，否则气泡会被一条长引用撑爆。
    return QSize(0, QLabel::minimumSizeHint().height());
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QLabel::resizeEvent(event);
    applyElide();
  }

private:
  QString fullText_;

  void applyElide() {
    if (fullText_.isEmpty())
      return;
    const int available = qMax(0, width());
    QLabel::setText(available <= 0 ? fullText_
                                   : fontMetrics().elidedText(
                                         fullText_, Qt::ElideRight, available));
  }
};

} // namespace

namespace {

// 引用块要整块可点。QWidget 自己不发点击信号，套一个只管 mouseReleaseEvent 的
// 小子类，比在 MainWindow::eventFilter 里按 objectName 分发要干净——那种分发
// 以后每多一个可点部件都得回去改同一个函数。
class ClickableWidget final : public QWidget {
public:
  ClickableWidget(QWidget *parent, std::function<void()> onClick)
      : QWidget(parent), onClick_(std::move(onClick)) {}

protected:
  void mouseReleaseEvent(QMouseEvent *event) override {
    // 只认「按下和松开都在块内」的左键，和按钮的判定一致：
    // 从块里按下拖出去再松手不该触发跳转。
    if (event->button() == Qt::LeftButton && rect().contains(event->pos()) &&
        onClick_) {
      onClick_();
    }
    QWidget::mouseReleaseEvent(event);
  }

private:
  std::function<void()> onClick_;
};

} // namespace

QWidget *MainWindow::createQuoteBlock(const RemoteIMMessage &message,
                                      QWidget *parent) {
  if (!message.hasQuote || message.quote.digest.isEmpty())
    return nullptr;

  const QString quoteMsgId = message.quote.msgId;
  // 只有拿得到原始 SDK 消息 ID 时才做成可点的。没有 ID 的引用块照常显示，
  // 但既不给手型光标也不装点击处理——点了没反应比不可点更让人困惑。
  QWidget *block =
      quoteMsgId.isEmpty()
          ? new QWidget(parent)
          : new ClickableWidget(parent, [this, quoteMsgId] {
              // 定位可能触发「加载更早」，那会整屏刷新、重建气泡——在被点部件
              // 自己的事件处理里同步做这件事，等于在自己脚下拆房子。
              const QString peerId = app_.chatState().selectedPeerId();
              QTimer::singleShot(0, this, [this, peerId, quoteMsgId] {
                if (app_.chatState().selectedPeerId() == peerId)
                  jumpToQuotedMessage(quoteMsgId);
              });
            });
  block->setObjectName(QStringLiteral("messageQuoteBlock"));
  auto *layout = new QHBoxLayout(block);
  layout->setContentsMargins(UiZoom::s(9), UiZoom::s(6), UiZoom::s(9),
                             UiZoom::s(6));
  layout->setSpacing(UiZoom::s(8));

  auto *bar = new QLabel(block);
  bar->setObjectName(QStringLiteral("messageQuoteBar"));
  bar->setFixedWidth(UiZoom::s(3));
  layout->addWidget(bar);

  const QString sender = message.quote.senderId.trimmed();
  auto *text = new ElidedLabel(block);
  text->setObjectName(QStringLiteral("messageQuoteText"));
  text->setFullText(sender.isEmpty()
                        ? message.quote.digest
                        : sender + QStringLiteral("：") + message.quote.digest);
  // 引用块是单行摘要，超出用省略号，绝不让它把气泡撑高。
  text->setWordWrap(false);
  text->setTextInteractionFlags(Qt::NoTextInteraction);
  text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  layout->addWidget(text, 1);

  if (!quoteMsgId.isEmpty()) {
    block->setCursor(Qt::PointingHandCursor);
    block->setProperty("quoteMsgId", quoteMsgId);
  }

  block->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #messageQuoteBlock {
            background: rgba(15, 23, 42, 0.04);
            border-radius: 8px;
        }
        #messageQuoteBar {
            background: #94a3b8;
            border-radius: 2px;
        }
        #messageQuoteText {
            color: #475569;
            font-size: 12px;
            background: transparent;
        }
    )")));
  return block;
}

// 作用：按消息类型组装文本、媒体和操作气泡，并为每个组件建立稳定的父子关系。
// 气泡几何尺寸由真实内容和当前宽度确定；附件加载只更新该行，不重建整个消息页。
QWidget *MainWindow::createMessageBubble(const RemoteIMMessage &message) {
  const bool outgoing = message.direction == RemoteIMMessageDirection::Outgoing;
  const bool expandedTextBubble = message.hasApprovalRequest ||
                                  (!message.hasImage && !message.hasFile &&
                                   !message.hasVideo && !message.hasVoice &&
                                   (!outgoing || message.text.size() >= 50 ||
                                    message.text.contains(QLatin1Char('\n'))));
  const bool fullWidthBody = !outgoing;
  auto *row = new QWidget(messageContainer_);
  row->setObjectName(outgoing ? QStringLiteral("messageRowOutgoing")
                              : QStringLiteral("messageRowIncoming"));
  row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto *rowLayout = new QBoxLayout(
      fullWidthBody ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight, row);
  rowLayout->setContentsMargins(0, 0, 0, 0);
  rowLayout->setSpacing(UiZoom::s(fullWidthBody ? 4 : MessageRowGap));

  auto *bubble = new QWidget(row);
  bubble->setObjectName(outgoing ? QStringLiteral("messageBubbleOutgoing")
                                 : QStringLiteral("messageBubbleIncoming"));
  bubble->setProperty("fullWidthBody", fullWidthBody);
  bubble->setProperty("expandedTextBubble", expandedTextBubble);
  applyMessageBubbleWidth(bubble, expandedTextBubble);
  bubble->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  // 取消消息外框；发送仍用浅蓝底区分，接收正文融入聊天背景。
  bubble->setStyleSheet(UiZoom::scaleQss(
      outgoing
          ? QStringLiteral("#messageBubbleOutgoing{background:#eaf4ff;border:0;"
                           "border-radius:12px;}")
          : QStringLiteral(
                "#messageBubbleIncoming{background:transparent;border:0;}")));

  auto *bubbleLayout = new QVBoxLayout(bubble);
  bubbleLayout->setContentsMargins(fullWidthBody ? 0 : UiZoom::s(14),
                                   UiZoom::s(8), UiZoom::s(14), UiZoom::s(8));
  bubbleLayout->setSpacing(7);

  // 引用块在正文之上，和聊天软件的惯例一致：先看到「在回复什么」，再看到回复内容。
  if (QWidget *quoteBlock = createQuoteBlock(message, bubble)) {
    bubbleLayout->addWidget(quoteBlock);
  }

  const bool inlineDate = !message.hasImage && !message.hasFile &&
                          !message.hasVideo && !message.hasVoice &&
                          !message.hasApprovalRequest;

  // 以下五条互斥分支只负责气泡主体；时间、投递状态和审批控件在主体之后统一组装。
  // 附件路径可能暂时没有实体文件，视图要保留可恢复的占位与打开反馈。
  auto *contentRow = new QHBoxLayout();
  contentRow->setContentsMargins(0, 0, 0, 0);
  contentRow->setSpacing(10);
  QLabel *deliveryStatusLabel = nullptr;

  if (message.hasImage) {
    {
      auto *imageLabel = new ClickableImageLabel(
          message.image.localPath,
          [this](const QString &path, QWidget *source) { openImagePreview(path, source); }, bubble);
      imageLabel->setObjectName(QStringLiteral("messageImageLabel"));
      // 高分屏（DPR>1）按物理分辨率解码并声明
      // DPR，否则缩略图被绘制层二次放大而发虚； 控件尺寸用逻辑值（物理尺寸 /
      // DPR）。
      const qreal thumbDpr = imageLabel->devicePixelRatioF();
      const QSize targetPixels =
          (QSizeF(UiZoom::s(280), UiZoom::s(200)) * thumbDpr).toSize();
      imageLabel->setAlignment(Qt::AlignCenter);
      // 先按目标尺寸占位，避免解码回来时气泡高度突变把列表顶得乱跳。
      imageLabel->setMinimumSize((QSizeF(targetPixels) / thumbDpr).toSize());
      // 消息图片交给 MaiAgent Graphics 的原生子视图；解码和绘制均不在 UI 线程。
      MessageImageLoader::instance().loadInto(
          message.image.localPath, targetPixels, imageLabel,
          [imageLabel, thumbDpr] {
            imageLabel->setText(QStringLiteral("图片暂不可预览"));
            imageLabel->setMinimumSize(QSize());
            Q_UNUSED(thumbDpr);
          });
      // 右键菜单（飞书式）：复制图片 / 预览 / 保存到本地。
      imageLabel->setContextMenuPolicy(Qt::CustomContextMenu);
      connect(imageLabel, &QLabel::customContextMenuRequested, this,
              [this, imageLabel, image = message.image,
               quoted = message](const QPoint &pos) {
                QMenu menu(imageLabel);
                applyMessageContextMenuStyle(menu);
                QAction *replyAction = menu.addAction(QStringLiteral("回复"));
                QAction *forwardAction = menu.addAction(
                    makeLineIcon(LineIconKind::Forward, kMenuIconColor),
                    QStringLiteral("转发"));
                menu.addSeparator();
                QAction *copyAction = menu.addAction(
                    makeLineIcon(LineIconKind::Copy, kMenuIconColor),
                    QStringLiteral("复制"));
                QAction *previewAction = menu.addAction(
                    makeLineIcon(LineIconKind::Preview, kMenuIconColor),
                    QStringLiteral("预览"));
                menu.addSeparator();
                QAction *saveAction = menu.addAction(
                    makeLineIcon(LineIconKind::Download, kMenuIconColor),
                    QStringLiteral("保存到本地…"));
                QAction *chosen = menu.exec(imageLabel->mapToGlobal(pos));
                if (chosen == replyAction) {
                  beginReplyTo(quoted);
                } else if (chosen == forwardAction) {
                  openForwardDialog(quoted);
                } else if (chosen == copyAction) {
                  // 位图 + 文件 URL
                  // 一起放剪贴板：贴到聊天/文档得到图片，贴到资源管理器得到文件。
                  const QImage imageData(image.localPath);
                  if (imageData.isNull()) {
                    AppMessageDialog::show(
                        this, AppMessageDialog::Kind::Warning,
                        QStringLiteral("复制失败"),
                        QStringLiteral("图片缓存已不存在，无法复制。"));
                    return;
                  }
                  auto *mimeData = new QMimeData();
                  mimeData->setImageData(imageData);
                  mimeData->setUrls({QUrl::fromLocalFile(image.localPath)});
                  QApplication::clipboard()->setMimeData(mimeData);
                } else if (chosen == previewAction) {
                  openImagePreview(image.localPath, imageLabel);
                } else if (chosen == saveAction) {
                  saveFileAttachmentToLocal(RemoteIMFileAttachment{
                      image.localPath, QFileInfo(image.localPath).fileName(),
                      QString(), image.sizeBytes});
                }
              });
      contentRow->addWidget(imageLabel);
    }
  } else if (message.hasVoice) {
    // 语音气泡：时长 + 点击用系统播放器打开。腾讯 IM 的语音多是 AMR/SILK，
    // Qt Multimedia 在 Windows 上不一定解得了，交给系统关联程序更可靠。
    auto *voiceButton = new QPushButton(bubble);
    voiceButton->setObjectName(QStringLiteral("messageVoiceButton"));
    voiceButton->setCursor(Qt::PointingHandCursor);
    const int seconds = qMax(1, message.voice.durationSeconds);
    voiceButton->setText(QStringLiteral("🔊 语音 %1\"").arg(seconds));
    voiceButton->setMinimumWidth(UiZoom::s(120 + qMin(seconds, 30) * 4));
    voiceButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    voiceButton->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
            QPushButton#messageVoiceButton {
                background: #f8fafc;
                border: 1px solid #d9e4ef;
                border-radius: 8px;
                color: #172033;
                font-size: 13px;
                padding: 10px 14px;
                text-align: left;
            }
            QPushButton#messageVoiceButton:hover {
                border-color: #1aa7ec;
                background: #edf8ff;
            }
        )")));
    connect(voiceButton, &QPushButton::clicked, this,
            [this, path = message.voice.localPath]() {
              if (path.trimmed().isEmpty() || !QFile::exists(path)) {
                AppMessageDialog::show(
                    this, AppMessageDialog::Kind::Warning,
                    QStringLiteral("无法播放"),
                    QStringLiteral("语音尚未下载完成或本地缓存已被清理。"));
                return;
              }
              QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            });
    voiceButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(voiceButton, &QPushButton::customContextMenuRequested, this,
            [this, voiceButton, quoted = message](const QPoint &pos) {
              QMenu menu(voiceButton);
              applyMessageContextMenuStyle(menu);
              QAction *replyAction = menu.addAction(QStringLiteral("回复"));
              QAction *forwardAction = menu.addAction(
                  makeLineIcon(LineIconKind::Forward, kMenuIconColor),
                  QStringLiteral("转发"));
              QAction *chosen = menu.exec(voiceButton->mapToGlobal(pos));
              if (chosen == replyAction)
                beginReplyTo(quoted);
              else if (chosen == forwardAction)
                openForwardDialog(quoted);
            });
    contentRow->addWidget(voiceButton);
  } else if (message.hasVideo) {
    // 视频气泡：封面 + 中心播放角标，点击开播。封面拿不到（老消息、或生成失败）
    // 时退化成深色底 + 角标，仍然可点——不能因为没有封面就让视频打不开。
    auto *videoButton = new QPushButton(bubble);
    videoButton->setObjectName(QStringLiteral("messageVideoButton"));
    videoButton->setCursor(Qt::PointingHandCursor);
    videoButton->setFlat(true);

    constexpr int kCoverMaxWidth = 240;
    // 封面同样不在这里同步解码：先摆一张纯色占位，解码回来再把带角标的封面换上。
    const int coverWidth = UiZoom::s(kCoverMaxWidth);
    const int coverHeight = UiZoom::s(135);
    const qreal coverDpr = videoButton->devicePixelRatioF();
    QPixmap cover((QSizeF(coverWidth, coverHeight) * coverDpr).toSize());
    cover.setDevicePixelRatio(coverDpr);
    cover.fill(QColor(0x11, 0x18, 0x27));
    // 播放角标画进封面本身：QPushButton 的 icon 只有一层，叠控件在这里
    // 会被气泡的布局挤走。
    // 抽成 lambda：占位封面和随后解码回来的真封面都要画同一个角标。
    auto paintPlayBadge = [](QPixmap &target) {
      QPainter painter(&target);
      painter.setRenderHint(QPainter::Antialiasing, true);
      const QSizeF logicalSize =
          QSizeF(target.size()) / target.devicePixelRatioF();
      const QPointF center(logicalSize.width() / 2.0,
                           logicalSize.height() / 2.0);
      const double radius =
          qMin(logicalSize.width(), logicalSize.height()) * 0.16;
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(0, 0, 0, 110));
      painter.drawEllipse(center, radius, radius);
      QPainterPath triangle;
      const double half = radius * 0.62;
      const double shift = half * 0.18;
      triangle.moveTo(center.x() - half * 0.55 + shift, center.y() - half);
      triangle.lineTo(center.x() - half * 0.55 + shift, center.y() + half);
      triangle.lineTo(center.x() + half * 0.85 + shift, center.y());
      triangle.closeSubpath();
      painter.setBrush(QColor(255, 255, 255, 230));
      painter.drawPath(triangle);
    };
    paintPlayBadge(cover);
    videoButton->setIcon(QIcon(cover));
    videoButton->setIconSize(QSize(coverWidth, coverHeight));
    // 真封面后台解码，回来再换上；解不出来就一直是占位图，不影响播放。
    if (!message.video.coverPath.trimmed().isEmpty()) {
      MessageImageLoader::instance().load(
          message.video.coverPath,
          (QSizeF(coverWidth, coverHeight) * coverDpr).toSize(), videoButton,
          [videoButton, coverWidth, coverHeight, coverDpr,
           paintPlayBadge](const QPixmap &loaded) {
            QPixmap composed = loaded.scaled(
                (QSizeF(coverWidth, coverHeight) * coverDpr).toSize(),
                Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            composed.setDevicePixelRatio(coverDpr);
            paintPlayBadge(composed);
            videoButton->setIcon(QIcon(composed));
          });
    }
    videoButton->setFixedSize(coverWidth, coverHeight);
    videoButton->setStyleSheet(
        QStringLiteral("QPushButton#messageVideoButton { border: none; "
                       "padding: 0; background: transparent; }"));
    const QString durationText =
        message.video.durationSeconds > 0
            ? QStringLiteral("%1:%2")
                  .arg(message.video.durationSeconds / 60, 2, 10,
                       QLatin1Char('0'))
                  .arg(message.video.durationSeconds % 60, 2, 10,
                       QLatin1Char('0'))
            : QString();
    const QString sizeText = fileSizeText(message.video.sizeBytes);
    QStringList parts;
    if (!durationText.isEmpty())
      parts << durationText;
    if (!sizeText.isEmpty())
      parts << sizeText;
    videoButton->setToolTip(parts.isEmpty()
                                ? QStringLiteral("点击播放")
                                : QStringLiteral("点击播放 · %1")
                                      .arg(parts.join(QStringLiteral(" · "))));
    connect(
        videoButton, &QPushButton::clicked, this,
        [this, attachment = message.video]() { openVideoPreview(attachment); });
    videoButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(videoButton, &QPushButton::customContextMenuRequested, this,
            [this, videoButton, quoted = message](const QPoint &pos) {
              QMenu menu(videoButton);
              applyMessageContextMenuStyle(menu);
              QAction *replyAction = menu.addAction(QStringLiteral("回复"));
              QAction *forwardAction = menu.addAction(
                  makeLineIcon(LineIconKind::Forward, kMenuIconColor),
                  QStringLiteral("转发"));
              QAction *previewAction = menu.addAction(
                  makeLineIcon(LineIconKind::Preview, kMenuIconColor),
                  QStringLiteral("预览"));
              QAction *chosen = menu.exec(videoButton->mapToGlobal(pos));
              if (chosen == replyAction)
                beginReplyTo(quoted);
              else if (chosen == forwardAction)
                openForwardDialog(quoted);
              else if (chosen == previewAction)
                openVideoPreview(quoted.video);
            });
    contentRow->addWidget(videoButton);
    if (!parts.isEmpty()) {
      auto *metaLabel = new QLabel(parts.join(QStringLiteral(" · ")), bubble);
      metaLabel->setStyleSheet(
          QStringLiteral("color: #6b7a8c; font-size: 12px;"));
      contentRow->addWidget(metaLabel);
    }
  } else if (message.hasFile) {
    // 文件气泡优先使用用户可读名称，并按文档、差异、视频或普通文件选择打开行为。
    // 预览不可用时保留另存为入口，不把尚未下载的缓存路径当成可复制文件。
    auto *fileButton = new QPushButton(bubble);
    fileButton->setObjectName(QStringLiteral("messageFileButton"));
    fileButton->setCursor(Qt::PointingHandCursor);
    const QString displayName =
        message.file.fileName.isEmpty()
            ? QFileInfo(message.file.localPath).fileName()
            : message.file.fileName;
    QString subtitle;
    QString icon = QStringLiteral("📄");
    if (isGitDiffFile(message.file)) {
      icon = QStringLiteral("Δ");
      subtitle = QStringLiteral("代码 Diff · 点击查看");
    } else if (isHtmlFile(message.file)) {
      subtitle = QStringLiteral("HTML 文件，点击预览");
    } else if (isMarkdownFile(message.file)) {
      subtitle = QStringLiteral("Markdown 文件，点击预览");
    } else if (isVideoFile(message.file)) {
      icon = QStringLiteral("🎬");
      const QString size = fileSizeText(message.file.sizeBytes);
      subtitle = size.isEmpty()
                     ? QStringLiteral("视频，点击另存为")
                     : QStringLiteral("视频 · %1，点击另存为").arg(size);
    } else {
      // 普通文件：无内嵌预览，点击直接另存为；有大小时一并展示。
      const QString size = fileSizeText(message.file.sizeBytes);
      subtitle = size.isEmpty()
                     ? QStringLiteral("文件，点击另存为")
                     : QStringLiteral("文件 · %1，点击另存为").arg(size);
    }
    fileButton->setText(
        QStringLiteral("%1 %2\n%3")
            .arg(icon)
            .arg(displayName.isEmpty() ? QStringLiteral("file") : displayName)
            .arg(subtitle));
    fileButton->setMinimumWidth(UiZoom::s(220));
    fileButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    fileButton->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
            QPushButton#messageFileButton {
                background: #f8fafc;
                border: 1px solid #d9e4ef;
                border-radius: 8px;
                color: #172033;
                font-size: 13px;
                font-weight: 700;
                padding: 10px 12px;
                text-align: left;
            }
            QPushButton#messageFileButton:hover {
                border-color: #1aa7ec;
                background: #edf8ff;
            }
        )")));
    connect(fileButton, &QPushButton::clicked, this,
            [this, attachment = message.file]() {
              if (isPreviewableDocument(attachment)) {
                openFilePreview(attachment);
              } else {
                saveFileAttachmentToLocal(attachment);
              }
            });
    // 右键菜单（飞书式）：复制文件 / 预览（仅文档） / 保存到本地。
    fileButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(fileButton, &QPushButton::customContextMenuRequested, this,
            [this, fileButton, attachment = message.file,
             quoted = message](const QPoint &pos) {
              QMenu menu(fileButton);
              applyMessageContextMenuStyle(menu);
              QAction *replyAction = menu.addAction(QStringLiteral("回复"));
              QAction *forwardAction = menu.addAction(
                  makeLineIcon(LineIconKind::Forward, kMenuIconColor),
                  QStringLiteral("转发"));
              menu.addSeparator();
              QAction *copyAction = menu.addAction(
                  makeLineIcon(LineIconKind::Copy, kMenuIconColor),
                  QStringLiteral("复制"));
              QAction *previewAction =
                  isPreviewableDocument(attachment)
                      ? menu.addAction(
                            makeLineIcon(LineIconKind::Preview, kMenuIconColor),
                            QStringLiteral("预览"))
                      : nullptr;
              menu.addSeparator();
              QAction *saveAction = menu.addAction(
                  makeLineIcon(LineIconKind::Download, kMenuIconColor),
                  QStringLiteral("保存到本地…"));
              QAction *chosen = menu.exec(fileButton->mapToGlobal(pos));
              if (chosen == replyAction) {
                beginReplyTo(quoted);
              } else if (chosen == forwardAction) {
                openForwardDialog(quoted);
              } else if (chosen == copyAction) {
                // 以文件形式放剪贴板：可直接粘贴到资源管理器/聊天窗口。
                if (attachment.localPath.trimmed().isEmpty() ||
                    !QFile::exists(attachment.localPath)) {
                  AppMessageDialog::show(
                      this, AppMessageDialog::Kind::Warning,
                      QStringLiteral("复制失败"),
                      QStringLiteral("文件尚未下载完成或本地缓存已被清理。"));
                  return;
                }
                auto *mimeData = new QMimeData();
                mimeData->setUrls({QUrl::fromLocalFile(attachment.localPath)});
                QApplication::clipboard()->setMimeData(mimeData);
              } else if (previewAction && chosen == previewAction) {
                openFilePreview(attachment);
              } else if (chosen == saveAction) {
                saveFileAttachmentToLocal(attachment);
              }
            });
    contentRow->addWidget(fileButton);
  } else {
    // 纯文本由 Markdown 视图统一排版；回复和转发仍引用这条消息的稳定标识。
    auto *markdownView = new MarkdownMessageView(bubble);
    markdownView->setReplyHandler(
        [this, quoted = message] { beginReplyTo(quoted); });
    markdownView->setForwardHandler(
        [this, forwarded = message] { openForwardDialog(forwarded); });
    markdownView->setMessageMarkdown(
        message.text, inlineDate ? messageTimeText(message) : QString());
    contentRow->addWidget(markdownView, 1);
  }

  const QString status =
      outgoing ? deliveryStatusIndicator(message.status) : QString();
  // 投递指示只属于本机发出的消息；收到的消息不凭本地状态补一个假回执。
  if (!status.isEmpty()) {
    deliveryStatusLabel = new QLabel(status, row);
    deliveryStatusLabel->setObjectName(QStringLiteral("messageStatusLabel"));
    deliveryStatusLabel->setAlignment(Qt::AlignCenter);
    deliveryStatusLabel->setFixedSize(UiZoom::s(16), UiZoom::s(16));
    deliveryStatusLabel->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
            QLabel#messageStatusLabel {
                border: 1px solid #12a150;
                border-radius: 8px;
                background: transparent;
                color: #12a150;
                font-size: 11px;
                font-weight: 700;
                padding: 0;
            }
        )")));
  }
  bubbleLayout->addLayout(contentRow);

  if (!outgoing && message.hasApprovalRequest &&
      message.approvalRequest.isValid()) {
    // 审批按钮读取消息模型的最新决定状态；网络回调后重新按模型刷新本行。
    // 已处理、正在发送和可点击必须互斥，避免重复提交审批。
    const QString token = message.approvalRequest.token;
    const ApprovalDisplayState approvalState = approvalDisplayState(message);
    if (approvalState != ApprovalDisplayState::Available) {
      const bool sent = approvalState == ApprovalDisplayState::Sent;
      const bool autoDeclined =
          approvalState == ApprovalDisplayState::AutoDeclined;
      const bool resolved = approvalState == ApprovalDisplayState::Resolved;
      auto *sentLabel =
          new QLabel(autoDeclined ? QStringLiteral("✕ 审批已因新消息自动拒绝")
                     : resolved   ? QStringLiteral("✓ 审批已处理")
                     : sent       ? QStringLiteral("✓ 审批选择已发送")
                                  : QStringLiteral("审批选择正在发送…"),
                     bubble);
      sentLabel->setObjectName(QStringLiteral("approvalSentLabel"));
      sentLabel->setStyleSheet(UiZoom::scaleQss(
          QStringLiteral("QLabel#approvalSentLabel{color:%1;font-size:13px;"
                         "font-weight:700;"
                         "padding:7px 2px;background:transparent;}")
              .arg(autoDeclined ? QStringLiteral("#d97706")
                   : resolved   ? QStringLiteral("#64748b")
                                : QStringLiteral("#059669"))));
      bubbleLayout->addWidget(sentLabel);
    } else {
      auto *divider = new QFrame(bubble);
      divider->setFrameShape(QFrame::HLine);
      divider->setStyleSheet(
          QStringLiteral("color:#ead99d;background:#ead99d;max-height:1px;"));
      bubbleLayout->addWidget(divider);

      auto *actions = new QHBoxLayout();
      actions->setContentsMargins(0, 0, 0, 0);
      actions->setSpacing(UiZoom::s(8));
      for (RemoteIMApprovalAction action : message.approvalRequest.actions) {
        auto *button =
            new QPushButton(remoteIMApprovalActionTitle(action), bubble);
        const bool reject = action == RemoteIMApprovalAction::Reject;
        const bool persistent = action == RemoteIMApprovalAction::ApprovePrefix;
        button->setObjectName(
            action == RemoteIMApprovalAction::ApproveOnce
                ? QStringLiteral("approvalApproveOnceButton")
            : persistent ? QStringLiteral("approvalApprovePrefixButton")
                         : QStringLiteral("approvalRejectButton"));
        button->setCursor(Qt::PointingHandCursor);
        button->setMinimumHeight(UiZoom::s(38));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setStyleSheet(UiZoom::scaleQss(
            reject ? QStringLiteral("QPushButton{background:transparent;border:"
                                    "1px solid #ef4444;"
                                    "border-radius:9px;color:#dc2626;font-size:"
                                    "13px;font-weight:700;padding:7px 10px;}"
                                    "QPushButton:hover{background:#fff1f2;}")
                   : QStringLiteral(
                         "QPushButton{background:%1;border:1px solid "
                         "%1;border-radius:9px;"
                         "color:white;font-size:13px;font-weight:700;padding:"
                         "7px 10px;}"
                         "QPushButton:hover{background:%2;border-color:%2;}")
                         .arg(persistent ? QStringLiteral("#059669")
                                         : QStringLiteral("#0f8dde"),
                              persistent ? QStringLiteral("#047857")
                                         : QStringLiteral("#087abe"))));
        connect(
            button, &QPushButton::clicked, this,
            [this, token, action, messageId = message.id] {
              if (submittingApprovalTokens_.contains(token) ||
                  sentApprovalTokens_.contains(token) ||
                  resolvedApprovalTokens_.contains(token) ||
                  autoDeclinedApprovalTokens_.contains(token))
                return;
              qInfo().noquote()
                  << QStringLiteral(
                         "[approval-ui] button-clicked id=%1 peer=%2 action=%3")
                         .arg(messageId, app_.chatState().selectedPeerId(),
                              remoteIMApprovalActionWireName(action));
              submittingApprovalTokens_.insert(token);
              app_.sendApprovalDecision(
                  token, action, [this, token, messageId, action](bool ok) {
                    qInfo().noquote()
                        << QStringLiteral("[approval-ui] decision-finished "
                                          "id=%1 peer=%2 action=%3 ok=%4")
                               .arg(messageId,
                                    app_.chatState().selectedPeerId(),
                                    remoteIMApprovalActionWireName(action),
                                    ok ? QStringLiteral("true")
                                       : QStringLiteral("false"));
                    submittingApprovalTokens_.remove(token);
                    // 不能只改点击时的旧控件：网络回调前切换会话/重建列表后，
                    // 旧控件已销毁。按消息模型重新计算，失败恢复按钮，成功显示已发送。
                    refreshMessages();
                  });
            });
        actions->addWidget(button);
      }
      bubbleLayout->addLayout(actions);
    }
  }

  // 图片/文件带配文时：配文与附件同属一条气泡，上下位置按发送时的排版走。
  // 占位文字（[图片消息]/[文件消息] …）不是真正配文，不再重复展示。
  if ((message.hasImage || message.hasFile) &&
      !message.text.trimmed().isEmpty() &&
      !message.text.startsWith(QStringLiteral("[图片消息] ")) &&
      !message.text.startsWith(QStringLiteral("[文件消息] "))) {
    auto *captionView = new MarkdownMessageView(bubble);
    captionView->setReplyHandler(
        [this, quoted = message] { beginReplyTo(quoted); });
    captionView->setForwardHandler(
        [this, forwarded = message] { openForwardDialog(forwarded); });
    captionView->setMessageMarkdown(message.text);
    if (message.captionAbove) {
      // 附件是这条气泡里先加进来的部件，插到 0 就排到它上面。
      bubbleLayout->insertWidget(0, captionView);
    } else {
      bubbleLayout->addWidget(captionView);
    }
  }
  row->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #messageRowIncoming[showMessageDivider="true"], #messageRowOutgoing[showMessageDivider="true"] {
            border-top: 1px solid #e8edf3;
        }
        #messageTimeLabel {
            color: #0f8ddd;
            font-size: 11px;
            font-weight: 500;
            background: transparent;
        }
        #messageRowOutgoing[searchHit="true"], #messageRowIncoming[searchHit="true"] {
            background: #fff4c2;
            border-radius: 10px;
        }
    )")));

  auto *bubbleRow = new QHBoxLayout();
  bubbleRow->setContentsMargins(0, 0, 0, 0);
  bubbleRow->setSpacing(UiZoom::s(8));
  bubbleRow->setAlignment(outgoing ? Qt::AlignRight : Qt::AlignLeft);
  bubbleRow->addWidget(bubble);
  int trailingWidth = deliveryStatusLabel ? UiZoom::s(24) : 0;
  if (!inlineDate) {
    // 附件和审批控件已经占据单独部件；日期贴在底边，避免增加整行元数据高度。
    auto *date =
        new QLabel(QStringLiteral("· ") + messageTimeText(message), row);
    date->setObjectName(QStringLiteral("messageTimeLabel"));
    date->setProperty("messageTimestamp", messageTimeText(message));
    bubbleRow->addWidget(date, 0, Qt::AlignBottom);
    trailingWidth += date->sizeHint().width() + UiZoom::s(8);
  }
  if (deliveryStatusLabel)
    bubbleRow->addWidget(deliveryStatusLabel, 0, Qt::AlignVCenter);
  // 尾部时间和投递标识完成实测后再确定气泡宽度；
  // 宽度不能提前凭文件类型估算，否则长文本和附件容易挤占可视区。
  bubble->setProperty("trailingMetadataWidth", trailingWidth);
  applyMessageBubbleWidth(bubble, expandedTextBubble);
  if (outgoing)
    rowLayout->addStretch(1);
  rowLayout->addLayout(bubbleRow);
  return row;
}

int MainWindow::messageBubbleMaximumWidth() const {
  int viewportWidth = messageScroll_ && messageScroll_->viewport()
                          ? messageScroll_->viewport()->width()
                          : 0;
  if (viewportWidth <= 80) {
    // 视口尚未完成布局时只能使用暂时宽度；窗口显示或尺寸变化后，
    // updateMessageBubbleWidths() 会以实际视口宽度重新计算气泡。
    viewportWidth = qMax(360, width() / 2);
  }
  const QMargins margins =
      messageLayout_ ? messageLayout_->contentsMargins() : QMargins();
  const int rowWidth = viewportWidth - margins.left() - margins.right();
  return qMax(0, rowWidth);
}

void MainWindow::applyMessageBubbleWidth(QWidget *bubble, bool expanded) const {
  if (!bubble)
    return;
  int maximumWidth = messageBubbleMaximumWidth();
  maximumWidth =
      qMax(0, maximumWidth - bubble->property("trailingMetadataWidth").toInt());
  if (!bubble->property("fullWidthBody").toBool())
    maximumWidth = qMin(maximumWidth, 1280);
  bubble->setMaximumWidth(maximumWidth);
  bubble->setMinimumWidth(expanded ? maximumWidth : 0);
}

void MainWindow::updateMessageBubbleWidths() {
  RemoteDiagnostics::PerformanceSpan performance("message-layout");
  QList<QWidget *> bubbles = messageContainer_->findChildren<QWidget *>(
      QStringLiteral("messageBubbleIncoming"));
  bubbles.append(messageContainer_->findChildren<QWidget *>(
      QStringLiteral("messageBubbleOutgoing")));
  for (QWidget *bubble : bubbles) {
    applyMessageBubbleWidth(bubble,
                            bubble->property("expandedTextBubble").toBool());
  }
}

QWidget *MainWindow::createSettingsRow(const QString &title, QLabel *valueLabel,
                                       const QString &helperText) {
  auto *row =
      new QWidget(valueLabel ? valueLabel->parentWidget() : settingsPage_);
  row->setObjectName(QStringLiteral("settingsRow"));
  row->setMinimumHeight(UiZoom::s(72));
  auto *layout = new QHBoxLayout(row);
  layout->setContentsMargins(18, 12, 18, 12);
  layout->setSpacing(20);

  auto *textColumn = new QVBoxLayout();
  textColumn->setContentsMargins(0, 0, 0, 0);
  textColumn->setSpacing(4);
  auto *titleLabel = new QLabel(title, row);
  titleLabel->setObjectName(QStringLiteral("settingsRowTitle"));
  auto *helperLabel = new QLabel(helperText, row);
  helperLabel->setObjectName(QStringLiteral("settingsRowHelper"));
  helperLabel->setWordWrap(true);
  textColumn->addWidget(titleLabel);
  textColumn->addWidget(helperLabel);

  if (valueLabel) {
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    valueLabel->setTextFormat(Qt::PlainText);
    valueLabel->setMinimumWidth(UiZoom::s(180));
    // 供 applyScaledFixedGeometry 在倍率变化时重放最小宽度。
    valueLabel->setProperty("settingsRowValue", true);
  }

  layout->addLayout(textColumn, 1);
  if (valueLabel)
    layout->addWidget(valueLabel);
  return row;
}

void MainWindow::sendCurrentText() {
  const QString peerId = app_.chatState().selectedPeerId();
  if (peerId.isEmpty())
    return;
  QString text = messageEditor_->toPlainText();
  text.remove(QChar(0xFFFC)); // 去掉内联图片/文件的对象替换占位符
  text = text.trimmed();
  const QList<ComposerAttachment> attachments = collectComposerAttachments();
  if (text.isEmpty() && attachments.isEmpty())
    return;

  const bool captionAbove = !attachments.isEmpty() &&
      composerTextPrecedesFirstAttachment(attachments.first().position);
  const RemoteIMQuote quote = pendingQuote_;
  const bool hasQuote = hasPendingQuote_;
  // 发送会同步通知界面刷新，刷新过程可能切换联系人并保存当前草稿。
  // 先消费这一条的输入，避免已发出的附件又作为未发送草稿恢复出来。
  messageEditor_->clear();
  cancelPendingReply();
  updateComposerState();

  if (attachments.isEmpty()) {
    app_.sendTextTo(peerId, text, quote, hasQuote);
  } else {
    // 文字并入「第一个」附件，合并成一条消息发送；其余附件各自单独发。
    //
    // 配文排在附件上面还是下面，取决于用户在输入框里怎么排的：第一个附件之前
    // 只要有非空白文字，就是「文字在上」。以前这里写死成图上文下，
    // 用户明明是文字在上打的字，发出去却被翻过来。
    for (int i = 0; i < attachments.size(); ++i) {
      const QString caption = (i == 0) ? text : QString();
      switch (attachments.at(i).kind) {
      case ComposerAttachment::Kind::File:
        app_.sendFileTo(peerId, attachments.at(i).path, caption, captionAbove);
        break;
      case ComposerAttachment::Kind::Video:
        app_.sendVideoTo(peerId, attachments.at(i).path, caption, captionAbove);
        break;
      case ComposerAttachment::Kind::Image:
        app_.sendImageTo(peerId, attachments.at(i).path, caption, captionAbove);
        break;
      }
    }
  }
}

QString MainWindow::latestSelectedMessageId() const {
  const QString peerId = app_.chatState().selectedPeerId();
  if (peerId.isEmpty())
    return QString();
  RemoteIMMessage latest;
  return app_.chatState().latestMessageWith(peerId, &latest) ? latest.id
                                                             : QString();
}

void MainWindow::requestReplySuggestions() {
  const QString peerId = app_.chatState().selectedPeerId();
  const QList<RemoteIMMessage> messages = app_.chatState().messagesWith(peerId);
  if (peerId.isEmpty() || messages.isEmpty()) {
    showToast(QStringLiteral("当前会话还没有可参考的消息"), 13, 1800);
    return;
  }

  QVector<ReplySuggestionTurn> turns;
  const int begin = std::max(0, messages.size() - 24);
  turns.reserve(messages.size() - begin);
  for (int i = begin; i < messages.size(); ++i) {
    const RemoteIMMessage &message = messages.at(i);
    if (message.hasApprovalRequest || message.hasApprovalDecision)
      continue;
    QString text = message.text.trimmed();
    if (text.isEmpty() && (message.hasImage || message.hasFile ||
                           message.hasVideo || message.hasVoice)) {
      text = MessageQuote::digestOf(message);
    }
    if (text.isEmpty())
      continue;
    turns.append(
        {message.direction == RemoteIMMessageDirection::Outgoing, text});
  }
  if (turns.isEmpty()) {
    showToast(QStringLiteral("当前会话没有可用于生成回复的文字"), 13, 1800);
    return;
  }

  replySuggestionPeerId_ = peerId;
  replySuggestionLatestMessageId_ = latestSelectedMessageId();
  replySuggestionStatus_->setText(
      QStringLiteral("正在生成自然、轻松和专业回复…"));
  for (QPushButton *button :
       {naturalReplyButton_, casualReplyButton_, professionalReplyButton_}) {
    button->setText(button->property("styleLabel").toString());
    button->setProperty("replyText", QString());
    button->setToolTip(QString());
    button->setEnabled(false);
  }
  replySuggestionBar_->show();
  activeReplySuggestionRequest_ =
      replySuggestionController_->requestSuggestions(
          app_.chatState().ownerUserId(), peerId,
          replySuggestionLatestMessageId_, turns);
}

void MainWindow::clearReplySuggestions() {
  if (replySuggestionController_)
    replySuggestionController_->cancel();
  activeReplySuggestionRequest_ = 0;
  replySuggestionPeerId_.clear();
  replySuggestionLatestMessageId_.clear();
  if (replySuggestionBar_)
    replySuggestionBar_->hide();
}

void MainWindow::applyReplySuggestion(const QString &text) {
  const QString suggestion = text.trimmed();
  if (suggestion.isEmpty() || !messageEditor_)
    return;
  messageEditor_->setPlainText(suggestion);
  QTextCursor cursor = messageEditor_->textCursor();
  cursor.movePosition(QTextCursor::End);
  messageEditor_->setTextCursor(cursor);
  messageEditor_->setFocus();
  clearReplySuggestions();
}

void MainWindow::updateComposerState() {
  const bool hasPeer = !app_.chatState().selectedPeerId().isEmpty();
  QString plain = messageEditor_ ? messageEditor_->toPlainText() : QString();
  plain.remove(QChar(0xFFFC));
  const bool hasText = !plain.trimmed().isEmpty();
  const bool hasAttachments = composerHasAttachments();
  messageEditor_->setEnabled(hasPeer);
  if (aiReplyButton_) {
    aiReplyButton_->setEnabled(hasPeer &&
                               app_.chatState().messageCountWith(
                                   app_.chatState().selectedPeerId()) > 0);
  }
  if (commandButton_)
    commandButton_->setEnabled(hasPeer);
  sendButton_->setEnabled(hasPeer && (hasText || hasAttachments));
}

void MainWindow::showSlashCommandMenu() {
  const QString peerId = app_.chatState().selectedPeerId();
  if (peerId.isEmpty() || !commandButton_->isEnabled() ||
      commandMenu_->isVisible())
    return;
  commandMenu_->setProperty("peerId", peerId);
  const QPoint anchor = commandButton_->mapToGlobal(QPoint(0, 0));
  commandMenu_->popup(
      anchor - QPoint(0, commandMenu_->sizeHint().height() + UiZoom::s(6)));
}

void MainWindow::sendSlashCommand(const QString &command) {
  const QString peerId = commandMenu_->property("peerId").toString();
  if (peerId.isEmpty() || peerId != app_.chatState().selectedPeerId())
    return;
  QString text = command.trimmed();
  if (command.endsWith(QLatin1Char(' '))) {
    bool accepted = false;
    AppTextInputDialog::Options options;
    options.title = text;
    options.placeholder = QStringLiteral("命令参数");
    const QString arguments =
        AppTextInputDialog::getText(this, options, &accepted).trimmed();
    if (!accepted)
      return;
    if (text == QStringLiteral("/btw") && arguments.isEmpty())
      return;
    if (!arguments.isEmpty())
      text += QLatin1Char(' ') + arguments;
  }
  if (peerId != app_.chatState().selectedPeerId())
    return;
  app_.sendText(text);
}

void MainWindow::changeUiZoom(qreal delta) {
  UiZoom::setFactor(UiZoom::factor() + delta);
  applyUiZoom(true);
}

void MainWindow::resetUiZoom() {
  UiZoom::setFactor(1.0);
  applyUiZoom(true);
}

void MainWindow::applyUiZoom(bool showToastPopup) {
  // 全局默认字体随倍率缩放（基准 13px，与 main.cpp 启动设置一致）。
  QFont font = QApplication::font();
  font.setPixelSize(UiZoom::s(13));
  QApplication::setFont(font);
  // 全局样式表按新倍率重放；列表条目行高/头像与气泡样式都依赖倍率，
  // 清空渲染缓存强制消息全量重建。代码级最小宽高也须重放（否则缩不回去）。
  applyScaledFixedGeometry();
  applyStyle();
  renderedPeerId_.clear();
  refresh();
  if (showToastPopup)
    showZoomToast();
}

// 作用：按当前缩放比例同步固定尺寸控件的几何位置。
void MainWindow::applyScaledFixedGeometry() {
  setMinimumSize(UiZoom::s(980), UiZoom::s(640));
  if (auto *header = findChild<QWidget *>(QStringLiteral("chatHeader"))) {
    header->setFixedHeight(UiZoom::s(44));
    header->layout()->setContentsMargins(UiZoom::s(28), UiZoom::s(8),
                                         UiZoom::s(28), UiZoom::s(8));
  }
  // 必须和构造时一致地定宽。这里原先放的是 160~260（同样是带文字时代的遗留），
  // 会把构造时的 setFixedWidth(64) 顶掉——缩放一次导航栏就胖回去。
  if (navRail_)
    navRail_->setFixedWidth(UiZoom::s(64));
  // navLogo 是生成位图（QPainterPath 灰度抗锯齿），倍率变化时按新尺寸重生成。
  if (auto *container =
          findChild<QWidget *>(QStringLiteral("navLogoContainer"))) {
    container->setFixedSize(UiZoom::s(44), UiZoom::s(44));
  }
  if (auto *logo = findChild<QLabel *>(QStringLiteral("navLogo"))) {
    logo->setFixedSize(UiZoom::s(34), UiZoom::s(34));
    logo->move(UiZoom::s(5), UiZoom::s(5));
    static_cast<AccountAvatarLabel *>(logo)->setProfile(app_.selfProfile());
  }
  if (statusLabel_) {
    statusLabel_->setFixedSize(UiZoom::s(9), UiZoom::s(9));
    updateConnectionIndicator(); // 重画点阵图，否则放大后还是旧尺寸那张
  }
  if (auto *pane = findChild<QWidget *>(QStringLiteral("conversationPane"))) {
    pane->setMinimumWidth(UiZoom::s(220));
  }
  if (auto *pane = findChild<QWidget *>(QStringLiteral("chatContentPane"))) {
    pane->setMinimumWidth(UiZoom::s(520));
  }
  if (auto *pane = findChild<QWidget *>(QStringLiteral("composerPanel"))) {
    pane->setMinimumHeight(UiZoom::s(96));
  }
  if (auto *pane =
          findChild<QWidget *>(QStringLiteral("contactsDirectoryPane"))) {
    pane->setMinimumWidth(UiZoom::s(300));
    pane->setMaximumWidth(UiZoom::s(420));
  }
  if (messageEditor_)
    messageEditor_->setMinimumHeight(UiZoom::s(64));
  if (aiReplyButton_) {
    aiReplyButton_->setFixedSize(UiZoom::s(30), UiZoom::s(30));
    aiReplyButton_->setIconSize(QSize(UiZoom::s(17), UiZoom::s(17)));
  }
  if (commandButton_)
    commandButton_->setFixedSize(UiZoom::s(30), UiZoom::s(30));
  if (aiReplyHint_)
    aiReplyHint_->setFixedSize(UiZoom::s(54), UiZoom::s(22));
  if (sendButton_) {
    sendButton_->setFixedSize(UiZoom::s(36), UiZoom::s(36));
    sendButton_->setIconSize(QSize(UiZoom::s(18), UiZoom::s(18)));
    static_cast<ComposerTextEdit *>(messageEditor_)->positionCornerAction();
  }
  const QList<QWidget *> settingsRows =
      findChildren<QWidget *>(QStringLiteral("settingsRow"));
  for (QWidget *row : settingsRows)
    row->setMinimumHeight(UiZoom::s(72));
  const QList<QLabel *> labels = findChildren<QLabel *>();
  for (QLabel *label : labels) {
    if (label->property("settingsRowValue").toBool())
      label->setMinimumWidth(UiZoom::s(180));
  }
}

void MainWindow::showZoomToast() {
  showToast(QStringLiteral("%1%").arg(qRound(UiZoom::factor() * 100)), 18, 900);
}

void MainWindow::showToast(const QString &text, int fontPx, int durationMs) {
  if (!toast_) {
    toast_ = new QLabel(this);
    toast_->setObjectName(QStringLiteral("toast"));
    toast_->setAlignment(Qt::AlignCenter);
    toast_->setAttribute(Qt::WA_TransparentForMouseEvents);
    toastTimer_ = new QTimer(this);
    toastTimer_->setSingleShot(true);
    connect(toastTimer_, &QTimer::timeout, this, [this] { toast_->hide(); });
  }
  toast_->setText(text);
  toast_->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QLabel#toast {
            background: rgba(17, 24, 39, 0.88);
            color: #ffffff;
            border-radius: 10px;
            font-size: %1px;
            font-weight: 700;
            padding: 12px 26px;
        }
    )")
                                             .arg(fontPx)));
  toast_->adjustSize();
  toast_->move((width() - toast_->width()) / 2,
               (height() - toast_->height()) / 2);
  toast_->raise();
  toast_->show();
  toastTimer_->start(durationMs);
}

void MainWindow::updateRemoteDesktopButton() {
  if (!remoteDesktopButton_)
    return;

  const QString peerId = app_.chatState().selectedPeerId();
  const bool trtcReady = RemoteDesktop::isTrtcAvailable();
  const RemoteDesktop::ViewerState state =
      remoteDesktop_ ? remoteDesktop_->viewerState()
                     : RemoteDesktop::ViewerState::Idle;

  // 同一个按钮承载三态：发起 / 连接中 / 断开。会话进行中时按钮即出口，
  // 观看窗被最小化也能从主界面掐断。
  LineIconKind icon = LineIconKind::Screen;
  QColor color(QStringLiteral("#4c5866"));
  QString tooltip;
  bool enabled = trtcReady && !peerId.isEmpty();

  switch (state) {
  case RemoteDesktop::ViewerState::Inviting:
  case RemoteDesktop::ViewerState::Connecting:
    icon = LineIconKind::ScreenConnecting;
    color = QColor(QStringLiteral("#b45309"));
    tooltip = QStringLiteral("正在连接远程桌面…点击取消");
    enabled = true;
    break;
  case RemoteDesktop::ViewerState::Viewing:
    icon = LineIconKind::ScreenDisconnect;
    color = QColor(QStringLiteral("#b42318"));
    tooltip = QStringLiteral("远程桌面已连接 · 点击断开");
    enabled = true;
    break;
  case RemoteDesktop::ViewerState::Idle:
  case RemoteDesktop::ViewerState::Failed:
    // 不可用时用 tooltip 说清原因，避免用户对着灰按钮猜。
    if (!trtcReady) {
      tooltip = QStringLiteral("当前版本未包含远程桌面组件");
    } else if (peerId.isEmpty()) {
      tooltip = QStringLiteral("请先选择一个会话");
    } else {
      tooltip = QStringLiteral("远程桌面 · 请求查看 %1 的屏幕").arg(peerId);
    }
    break;
  }

  remoteDesktopButton_->setIcon(makeLineIcon(icon, color));
  remoteDesktopButton_->setToolTip(tooltip);
  remoteDesktopButton_->setEnabled(enabled);
}

// 作用：建立远程桌面配置面板及其操作反馈。
// 页面状态变化和用户操作在这里衔接；退出或失败时维持已显示内容。
QWidget *MainWindow::buildRemoteDesktopSettingsPanel(QWidget *parent) {
  auto *panel = new QWidget(parent);
  panel->setObjectName(QStringLiteral("settingsPanel"));
  auto *layout = new QVBoxLayout(panel);
  // 同上：上下留白，左右保持 0 以便分隔线横贯整宽。
  layout->setContentsMargins(0, UiZoom::s(8), 0, UiZoom::s(8));
  layout->setSpacing(0);

  auto *heading = new QLabel(QStringLiteral("远程桌面"), panel);
  heading->setObjectName(QStringLiteral("settingsSectionTitle"));
  layout->addWidget(heading);

  // 被控模式三选一。默认无人值守：主场景是自己在外面连自己的电脑，
  // 每次都要人在电脑前点同意就失去意义了。
  auto *modeRow = new QWidget(panel);
  modeRow->setObjectName(QStringLiteral("settingsRow"));
  auto *modeLayout = new QVBoxLayout(modeRow);
  modeLayout->setContentsMargins(18, 14, 18, 14);
  modeLayout->setSpacing(8);

  auto *modeTitle = new QLabel(QStringLiteral("被控模式"), modeRow);
  modeTitle->setObjectName(QStringLiteral("settingsRowTitle"));
  modeLayout->addWidget(modeTitle);

  remoteDesktopModeGroup_ = new QButtonGroup(this);
  struct ModeOption {
    RemoteDesktop::HostMode mode;
    QString label;
    QString hint;
  };
  const QVector<ModeOption> options{
      {RemoteDesktop::HostMode::Unattended, QStringLiteral("无人值守"),
       QStringLiteral("允许列表内的设备可直接连入，不打扰你")},
      {RemoteDesktop::HostMode::Attended, QStringLiteral("每次确认"),
       QStringLiteral("每次收到请求都弹窗，60 秒无应答自动拒绝")},
      {RemoteDesktop::HostMode::Disabled, QStringLiteral("关闭"),
       QStringLiteral("拒绝一切远程请求")}};
  for (const ModeOption &option : options) {
    auto *radio = new QRadioButton(
        QStringLiteral("%1 · %2").arg(option.label, option.hint), modeRow);
    radio->setObjectName(QStringLiteral("settingsRadio"));
    radio->setCursor(Qt::PointingHandCursor);
    remoteDesktopModeGroup_->addButton(radio, static_cast<int>(option.mode));
    modeLayout->addWidget(radio);
  }
  connect(remoteDesktopModeGroup_,
          QOverload<int>::of(&QButtonGroup::buttonClicked), this,
          [this](int id) {
            RemoteDesktopSettings settings = remoteDesktop_->settings();
            settings.mode = static_cast<RemoteDesktop::HostMode>(id);
            // 换模式即视为用户明确表态，清掉此前的失败计数。
            settings.consecutiveAuthFailures = 0;
            remoteDesktop_->updateSettings(settings);
            remoteDesktopSettingsStore_->save(settings);
            refreshRemoteDesktopSettings();
          });
  layout->addWidget(modeRow);

  // 访问密码：可选加固，不是无人值守的前提。
  remoteDesktopPasswordValue_ = new QLabel(panel);
  remoteDesktopPasswordValue_->setObjectName(
      QStringLiteral("settingsRowValue"));
  remoteDesktopPasswordValue_->setProperty("settingsRowValue", true);
  auto *passwordRow = createSettingsRow(
      QStringLiteral("访问密码"), remoteDesktopPasswordValue_,
      QStringLiteral(
          "可选。设置后，对方连入时还需输入此密码；不设则仅凭允许列表授权。"));
  auto *passwordButton = new QPushButton(QStringLiteral("设置"), passwordRow);
  passwordButton->setObjectName(QStringLiteral("settingsRowButton"));
  passwordButton->setCursor(Qt::PointingHandCursor);
  connect(passwordButton, &QPushButton::clicked, this,
          &MainWindow::editRemoteDesktopPassword);
  if (auto *rowLayout = qobject_cast<QHBoxLayout *>(passwordRow->layout())) {
    rowLayout->addWidget(passwordButton);
  }
  layout->addWidget(passwordRow);

  // 允许列表：谁能连入本机。
  remoteDesktopAllowValue_ = new QLabel(panel);
  remoteDesktopAllowValue_->setObjectName(QStringLiteral("settingsRowValue"));
  remoteDesktopAllowValue_->setProperty("settingsRowValue", true);
  auto *allowRow = createSettingsRow(
      QStringLiteral("允许连入的设备"), remoteDesktopAllowValue_,
      QStringLiteral("只有列表内的账号能远程本机，其余一律拒绝。"));
  auto *allowButton = new QPushButton(QStringLiteral("编辑"), allowRow);
  allowButton->setObjectName(QStringLiteral("settingsRowButton"));
  allowButton->setCursor(Qt::PointingHandCursor);
  connect(allowButton, &QPushButton::clicked, this,
          &MainWindow::editRemoteDesktopAllowList);
  if (auto *rowLayout = qobject_cast<QHBoxLayout *>(allowRow->layout())) {
    rowLayout->addWidget(allowButton);
  }
  layout->addWidget(allowRow);

  // 远程控制：与「允许观看」彼此独立的一道闸，默认关。
  // 不跟着无人值守走——否则那一个开关会在用户不知情的情况下，
  // 把语义从"允许别人看我的屏幕"放大成"允许别人完全操作我的电脑"。
  remoteDesktopControlValue_ = new QLabel(panel);
  remoteDesktopControlValue_->setObjectName(QStringLiteral("settingsRowValue"));
  remoteDesktopControlValue_->setProperty("settingsRowValue", true);
  QString remoteControlHelp = QStringLiteral(
      "关闭时对方只能看画面，动不了你的鼠标键盘。开启后无需每次确认——"
      "人不在电脑前时弹窗没人应答，等于让无人值守失效。");
#ifdef Q_OS_MAC
  remoteControlHelp += QStringLiteral(
      " macOS 首次开启还需在“隐私与安全性 > 辅助功能”中允许 MaiChat。");
#endif
  auto *controlRow =
      createSettingsRow(QStringLiteral("允许远程控制"),
                        remoteDesktopControlValue_, remoteControlHelp);
  remoteDesktopControlToggle_ =
      new QCheckBox(QStringLiteral("允许"), controlRow);
  remoteDesktopControlToggle_->setObjectName(
      QStringLiteral("remoteControlToggle"));
  const bool canInjectInput = RemoteInput::isInputInjectionSupported();
  remoteDesktopControlToggle_->setEnabled(canInjectInput);
  remoteDesktopControlToggle_->setCursor(canInjectInput ? Qt::PointingHandCursor
                                                        : Qt::ArrowCursor);
  connect(remoteDesktopControlToggle_, &QCheckBox::toggled, this,
          [this](bool checked) {
            if (!RemoteInput::isInputInjectionSupported())
              return;
            RemoteDesktopSettings settings = remoteDesktop_->settings();
            if (settings.allowRemoteControl == checked)
              return;
            settings.allowRemoteControl = checked;
            remoteDesktop_->updateSettings(settings);
            remoteDesktopSettingsStore_->save(settings);
            if (checked && !RemoteInput::hasInputInjectionPermission()) {
              RemoteInput::requestInputInjectionPermission();
              AppMessageDialog::show(
                  this, AppMessageDialog::Kind::Info,
                  QStringLiteral("需要辅助功能权限"),
                  QStringLiteral(
                      "请在“系统设置 > 隐私与安全性 > 辅助功能”中允许 MaiChat。"
                      "授权后返回本应用即可接受远程鼠标和键盘操作。"));
            }
            refreshRemoteDesktopSettings();
          });
  if (auto *rowLayout = qobject_cast<QHBoxLayout *>(controlRow->layout())) {
    rowLayout->addWidget(remoteDesktopControlToggle_);
  }
  layout->addWidget(controlRow);

  remoteDesktopProxyValue_ = new QLabel(panel);
  remoteDesktopProxyValue_->setObjectName(QStringLiteral("settingsRowValue"));
  remoteDesktopProxyValue_->setProperty("settingsRowValue", true);
  auto *proxyRow = createSettingsRow(
      QStringLiteral("TRTC 网络代理"), remoteDesktopProxyValue_,
      QStringLiteral(
          "为远程桌面单独设置 SOCKS5，不影响 IM。保存后重启 MaiChat 生效。"));
  auto *proxyButton = new QPushButton(QStringLiteral("配置"), proxyRow);
  proxyButton->setObjectName(QStringLiteral("settingsRowButton"));
  proxyButton->setCursor(Qt::PointingHandCursor);
  connect(proxyButton, &QPushButton::clicked, this,
          &MainWindow::editRemoteDesktopProxy);
  if (auto *rowLayout = qobject_cast<QHBoxLayout *>(proxyRow->layout())) {
    rowLayout->addWidget(proxyButton);
  }
  layout->addWidget(proxyRow);

  return panel;
}

void MainWindow::refreshRemoteDesktopSettings() {
  if (!remoteDesktop_ || !remoteDesktopModeGroup_)
    return;
  const RemoteDesktopSettings &settings = remoteDesktop_->settings();

  if (auto *button =
          remoteDesktopModeGroup_->button(static_cast<int>(settings.mode))) {
    button->setChecked(true);
  }
  remoteDesktopPasswordValue_->setText(settings.hasPassword()
                                           ? QStringLiteral("已设置")
                                           : QStringLiteral("未设置"));
  remoteDesktopAllowValue_->setText(
      settings.allowedUserIds.isEmpty()
          ? QStringLiteral("尚未添加")
          : settings.allowedUserIds.join(QStringLiteral("、")));

  if (remoteDesktopControlToggle_) {
    // 回填时挡掉 toggled：否则会反过来触发一次保存，形成回环。
    QSignalBlocker blocker(remoteDesktopControlToggle_);
    remoteDesktopControlToggle_->setChecked(
        RemoteInput::isInputInjectionSupported() &&
        settings.allowRemoteControl);
  }
  if (remoteDesktopControlValue_) {
    QString status = QStringLiteral("仅可观看");
    if (!RemoteInput::isInputInjectionSupported()) {
      status = QStringLiteral("当前平台暂不支持");
    } else if (settings.allowRemoteControl &&
               !RemoteInput::hasInputInjectionPermission()) {
      status = QStringLiteral("等待辅助功能授权");
    } else if (settings.allowRemoteControl) {
      status = QStringLiteral("已允许");
    }
    remoteDesktopControlValue_->setText(status);
  }
  if (remoteDesktopProxyValue_) {
    remoteDesktopProxyValue_->setText(
        settings.trtcProxyEnabled
            ? QStringLiteral("%1:%2 · %3")
                  .arg(settings.trtcProxyHost)
                  .arg(settings.trtcProxyPort)
                  .arg(settings.trtcProxyUdp ? QStringLiteral("TCP + UDP")
                                             : QStringLiteral("仅 TCP"))
            : QStringLiteral("直连"));
  }
}

void MainWindow::editRemoteDesktopPassword() {
  AppTextInputDialog::Options options;
  options.title = QStringLiteral("访问密码");
  options.description = QStringLiteral(
      "留空表示不设密码，仅凭允许列表授权。设置后对方连入时还需输入此密码。");
  options.placeholder = QStringLiteral("留空即不设密码");
  options.password = true;
  bool ok = false;
  const QString password = AppTextInputDialog::getText(this, options, &ok);
  if (!ok)
    return;

  RemoteDesktopSettings settings = remoteDesktop_->settings();
  settings.secret = password.isEmpty()
                        ? RemoteDesktopAuth::StoredSecret{}
                        : RemoteDesktopAuth::deriveSecret(
                              password, settings.secret.salt.isEmpty()
                                            ? RemoteDesktopAuth::generateSalt()
                                            : settings.secret.salt);
  settings.consecutiveAuthFailures = 0;
  remoteDesktop_->updateSettings(settings);
  remoteDesktopSettingsStore_->save(settings);
  refreshRemoteDesktopSettings();
}

void MainWindow::editRemoteDesktopAllowList() {
  const RemoteDesktopSettings current = remoteDesktop_->settings();
  AppTextInputDialog::Options options;
  options.title = QStringLiteral("允许连入的设备");
  options.description =
      QStringLiteral("只有列表内的账号能远程本机，多个账号用逗号分隔。");
  options.placeholder = QStringLiteral("例如：whq-iphone, mac-air");
  options.initialText = current.allowedUserIds.join(QStringLiteral(","));
  bool ok = false;
  const QString text = AppTextInputDialog::getText(this, options, &ok);
  if (!ok)
    return;

  RemoteDesktopSettings settings = current;
  settings.allowedUserIds.clear();
  const QStringList parts = text.split(
      QRegularExpression(QStringLiteral("[,，]")), Qt::SkipEmptyParts);
  for (const QString &part : parts) {
    const QString clean = part.trimmed();
    if (!clean.isEmpty())
      settings.allowedUserIds.append(clean);
  }
  remoteDesktop_->updateSettings(settings);
  remoteDesktopSettingsStore_->save(settings);
  refreshRemoteDesktopSettings();
}

void MainWindow::editRemoteDesktopProxy() {
  const RemoteDesktopSettings current = remoteDesktop_->settings();
  RemoteDesktopProxyDialog::Config dialogConfig;
  dialogConfig.enabled = current.trtcProxyEnabled;
  dialogConfig.host = current.trtcProxyHost;
  dialogConfig.port = current.trtcProxyPort;
  dialogConfig.supportUdp = current.trtcProxyUdp;
  RemoteDesktopProxyDialog dialog(dialogConfig, this);
  if (dialog.exec() != QDialog::Accepted)
    return;
  const RemoteDesktopProxyDialog::Config proxy = dialog.config();

  RemoteDesktopSettings settings = current;
  settings.trtcProxyEnabled = proxy.enabled;
  settings.trtcProxyHost =
      proxy.host.isEmpty() ? QStringLiteral("127.0.0.1") : proxy.host;
  settings.trtcProxyPort = proxy.port;
  settings.trtcProxyUdp = proxy.supportUdp;
  if (!remoteDesktopSettingsStore_->save(settings)) {
    AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                           QStringLiteral("保存失败"),
                           QStringLiteral("无法保存 TRTC 网络代理设置。"));
    return;
  }
  remoteDesktop_->updateSettings(settings);
  refreshRemoteDesktopSettings();
  AppMessageDialog::show(
      this, AppMessageDialog::Kind::Info, QStringLiteral("代理设置已保存"),
      QStringLiteral("重启 MaiChat 后，TRTC 将使用新的网络代理。"));
}

// 作用：初始化远程桌面控制器、画面和状态回调。
// 页面状态变化和用户操作在这里衔接；退出或失败时维持已显示内容。
void MainWindow::setupRemoteDesktop() {
  // 配置与本地消息库同级：每账号一份。
  QString root =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (root.isEmpty())
    root = QDir::homePath() + QStringLiteral("/.maichat-desktop");
  const QString ownerId = app_.chatState().ownerUserId();
  remoteDesktopSettingsStore_ = std::make_unique<RemoteDesktopSettingsStore>(
      QDir(root).filePath(QStringLiteral("RemoteDesktop/") + ownerId +
                          QStringLiteral("/settings.json")));

  RemoteDesktopController::Config config;
  config.sdkAppId = imSdkAppId_;
  config.localUserId = ownerId;
  config.userSigProvider = [this, ownerId](const QString &userId) {
    return userId == ownerId && userSigProvider_ ? userSigProvider_(userId) : QString();
  };

  RemoteDesktopSettings remoteSettings = remoteDesktopSettingsStore_->load();
  if (!RemoteInput::isInputInjectionSupported())
    remoteSettings.allowRemoteControl = false;
  RemoteDesktop::TrtcNetworkProxyConfig proxyConfig;
  proxyConfig.enabled = remoteSettings.trtcProxyEnabled;
  proxyConfig.host = remoteSettings.trtcProxyHost;
  proxyConfig.port = remoteSettings.trtcProxyPort;
  proxyConfig.supportUdp = remoteSettings.trtcProxyUdp;
  auto trtcEngine = std::unique_ptr<RemoteDesktop::ITrtcEngine>(
      RemoteDesktop::createTrtcEngine(proxyConfig));
  const QString trtcInitializationError = trtcEngine->initializationError();
  remoteDesktop_ = new RemoteDesktopController(
      config, remoteSettings, std::move(trtcEngine),
      [this](const QString &peerId, const QString &text,
             RemoteDesktopController::SignalSendCompletion completion) {
// 远程桌面协议帧由程序生成，不是用户聊天输入；接收方不得通过 IM 自动回复。
        app_.client().sendMachineText(
            peerId, text,
            [completion = std::move(completion)](
                bool, const QString &, const RemoteIMSendReceipt &) mutable {
              if (completion)
                completion();
            });
      },
      this);
  if (!trtcInitializationError.isEmpty()) {
    QTimer::singleShot(0, this, [this, trtcInitializationError] {
      AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                             QStringLiteral("TRTC 初始化失败"),
                             trtcInitializationError);
    });
  }

  connect(&app_, &RemoteIMApplication::remoteDesktopSignalReceived, this,
          [this](const QString &fromUserId, const QString &text) {
            remoteDesktop_->handleIncomingText(fromUserId, text);
          });

  connect(remoteDesktop_, &RemoteDesktopController::consentRequested, this,
          [this](const QString &fromUserId) {
            handleRemoteDesktopConsent(fromUserId);
          });

  connect(remoteDesktop_, &RemoteDesktopController::sharingStarted, this,
          [this](const QString &peerUserId) {
            sharingIndicator_->startSharing(peerUserId);
          });
  connect(remoteDesktop_, &RemoteDesktopController::sharingStopped, this,
          [this] { sharingIndicator_->stopSharing(); });
  connect(sharingIndicator_, &SharingIndicatorBar::stopRequested, this,
          [this] { remoteDesktop_->stopSession(); });

  // 控制器改写设置（失败计数、模式降级）后立即落盘，避免重启后计数丢失。
  connect(remoteDesktop_, &RemoteDesktopController::settingsChanged, this,
          [this](const RemoteDesktopSettings &settings) {
            remoteDesktopSettingsStore_->save(settings);
          });
  connect(
      remoteDesktop_, &RemoteDesktopController::modeDowngraded, this, [this] {
        AppMessageDialog::show(
            this, AppMessageDialog::Kind::Warning, QStringLiteral("远程桌面"),
            QStringLiteral("访问密码连续校验失败次数过多，"
                           "无人值守已自动关闭，现在改为每次弹窗确认。"));
      });

  // 观看状态回调同时驱动按钮、远端画面窗和错误提示；
  // 先更新当前状态再决定是否展示窗口，避免连接中显示旧视频帧。
  connect(
      remoteDesktop_, &RemoteDesktopController::viewerStateChanged, this,
      [this](RemoteDesktop::ViewerState state, const QString &failureReason) {
        // 按钮三态跟着会话状态走，必须先刷新再处理窗口开关。
        updateRemoteDesktopButton();
        switch (state) {
        case RemoteDesktop::ViewerState::Connecting:
          // 对方已同意：先把观看窗开出来，渲染句柄要在窗口显示后才有效。
          {
            const QString peerUserId = remoteDesktop_->viewerPeerId();
            openRemoteDesktopViewer(peerUserId);
            // 已知对端 userId 时无需等待辅流通知。下一轮事件循环
            // 再绑定，确保 startViewing 已发起且 NSView/HWND 已创建。
            QTimer::singleShot(0, this, [this, peerUserId] {
              if (!remoteDesktop_ || !remoteDesktopView_ ||
                  peerUserId.isEmpty() ||
                  !remoteDesktopView_->isSessionVisible()) {
                return;
              }
              const auto state = remoteDesktop_->viewerState();
              if (state != RemoteDesktop::ViewerState::Connecting &&
                  state != RemoteDesktop::ViewerState::Viewing) {
                return;
              }
              remoteDesktop_->bindRemoteView(
                  peerUserId,
                  remoteDesktopView_->renderWindowHandle(peerUserId));
            });
          }
          break;
        case RemoteDesktop::ViewerState::Failed:
          closeRemoteDesktopViewer();
          if (failureReason == RemoteDesktop::reasonBadPassword()) {
            // 只有对方额外设了密码才会走到这里：此时才问，问一次记住。
            promptRemoteDesktopPassword(remoteDesktop_->viewerPeerId());
          } else if (!failureReason.isEmpty()) {
            AppMessageDialog::show(this, AppMessageDialog::Kind::Info,
                                   QStringLiteral("远程桌面"), failureReason);
          }
          break;
        case RemoteDesktop::ViewerState::Idle:
          closeRemoteDesktopViewer();
          break;
        case RemoteDesktop::ViewerState::Inviting:
          // 新会话不能沿用上一场的编码帧尺寸。首帧/尺寸回调前宁可
          // 暂停坐标输入，也不能拿固定 1920x1080 短暂映射错。
          remoteDesktopRemoteVideoSize_ = QSize();
          if (remoteInputCapture_) {
            remoteInputCapture_->setRemoteVideoSize(QSize());
          }
          break;
        case RemoteDesktop::ViewerState::Viewing:
          break;
        }
      });

  // 辅流状态变化时刷新 UI 并重申绑定；首次订阅已在 Connecting 时主动发起，
  // 不依赖这条可能早于原生窗口创建的单次通知。
  remoteDesktop_->setRemoteVideoHandler(
      [this](const QString &userId, bool available) {
        if (!remoteDesktopView_)
          return;
        const QString peerUserId =
            userId.isEmpty() ? remoteDesktop_->viewerPeerId() : userId;
        if (available && !remoteDesktopView_->isSessionVisible()) {
          openRemoteDesktopViewer(remoteDesktop_->viewerPeerId());
        }
        if (!remoteDesktopView_->isSessionVisible())
          return;
        if (!available && remoteDesktopView_->isControlActive(peerUserId)) {
          toggleRemoteDesktopControl(peerUserId);
        }
        if (available) {
          remoteDesktop_->bindRemoteView(
              peerUserId, remoteDesktopView_->renderWindowHandle(peerUserId));
        }
        remoteDesktopView_->setStreamActive(peerUserId, available);
      });
  // TRTC 接收端实际编码帧分辨率，不等于被控屏幕。它与 Accept 里的
  // CaptureGeometry 一起参与两级换算；此前写死 1920x1080 会在首帧前映射错。
  remoteDesktop_->setRemoteVideoSizeHandler(
      [this](const QString &, int width, int height) {
        const QSize size(width, height);
        if (!size.isValid() || size.isEmpty() ||
            remoteDesktopRemoteVideoSize_ == size)
          return;
        remoteDesktopRemoteVideoSize_ = size;
        // 控制进行中也要立刻跟上：尺寸是首帧之后才到的，晚一步就意味着
        // 开控制的头几秒鼠标是偏的。
        if (remoteInputCapture_)
          remoteInputCapture_->setRemoteVideoSize(size);
      });
  // 被控端的状态播报：让用户能区分"对方在等系统授权"和"断网/崩溃"，
  // 而不是对着一块卡住的画面猜。
  connect(
      remoteDesktop_, &RemoteDesktopController::peerNoticeReceived, this,
      [this](const QString &noticeCode) {
        if (!remoteDesktopView_)
          return;
        QString text;
        if (noticeCode ==
            QLatin1String(
                RemoteDesktopSignals::NoticeCodes::kSecureDesktopEntered)) {
          text = QStringLiteral(
              "对方电脑弹出了系统授权框（UAC）或已锁屏。这段时间画面会卡住、"
              "鼠标键盘也点不动，需要有人在那台电脑前操作一下。");
        }
        // 离开安全桌面时 text 为空，正好撤下提示。
        remoteDesktopView_->setNoticeText(QString(), text);
      });

  remoteDesktop_->setErrorHandler([this](int code, const QString &message) {
    if (remoteDesktopView_) {
      for (const QString &peerId : remoteDesktopView_->sessionPeerIds()) {
        if (remoteDesktopView_->isControlActive(peerId)) {
          toggleRemoteDesktopControl(peerId);
        }
        remoteDesktopView_->setStreamActive(peerId, false);
      }
      remoteDesktopView_->setStatusText(
          QStringLiteral("连接异常（%1）：%2").arg(code).arg(message));
    }
  });
}

void MainWindow::promptRemoteDesktopPassword(const QString &peerUserId) {
  if (peerUserId.isEmpty())
    return;
  AppTextInputDialog::Options options;
  options.title = QStringLiteral("需要访问密码");
  options.description =
      QStringLiteral("%1 为远程桌面设置了访问密码，输入后即可连接。")
          .arg(peerUserId);
  options.placeholder = QStringLiteral("对方设置的访问密码");
  options.password = true;
  options.confirmText = QStringLiteral("连接");
  bool ok = false;
  const QString password = AppTextInputDialog::getText(this, options, &ok);
  if (!ok || password.isEmpty())
    return;

  // 记住本次会话内的密码，避免重试时反复询问。
  remoteDesktopPasswords_.insert(peerUserId, password);
  remoteDesktop_->requestView(peerUserId, password);
}

void MainWindow::openRemoteDesktopViewer(const QString &peerUserId) {
  if (!remoteDesktopView_)
    return;
  remoteDesktopView_->beginSession(peerUserId);
  // 自动切到远程页：用户刚点了发起，画面理应立刻可见，不必再手动找。
  showRemotePage();
}

void MainWindow::closeRemoteDesktopViewer() {
  if (!remoteDesktopView_)
    return;
  remoteDesktopView_->showIdle();
}

void MainWindow::stopRemoteDesktopForShutdown(
    std::function<void()> completion) {
  if (completion)
    remoteDesktopShutdownCallbacks_.push_back(std::move(completion));
  if (remoteDesktopShutdown_) {
    if (remoteDesktopShutdownComplete_) {
      auto callbacks = std::move(remoteDesktopShutdownCallbacks_);
      remoteDesktopShutdownCallbacks_.clear();
      for (auto &callback : callbacks)
        callback();
    }
    return;
  }
  remoteDesktopShutdown_ = true;

  if (remoteInputCapture_) {
    remoteInputCapture_->setEnabled(false);
    remoteInputCapture_->attachTo(nullptr);
  }

  auto completed = std::make_shared<bool>(false);
  auto completeOnce = [this, completed]() {
    if (*completed)
      return;
    *completed = true;
    remoteDesktopShutdownComplete_ = true;
    auto callbacks = std::move(remoteDesktopShutdownCallbacks_);
    remoteDesktopShutdownCallbacks_.clear();
    for (auto &callback : callbacks)
      callback();
  };

  // SDK 异常时不能让应用永久关不掉；正常情况下 sendText 的回执会更早到。
  QTimer::singleShot(RemoteDesktopStopSendTimeoutMs, this, completeOnce);
  if (remoteDesktop_) {
    remoteDesktop_->stopSession(completeOnce);
  } else {
    completeOnce();
  }
}

void MainWindow::toggleRemoteDesktopControl(const QString &peerUserId) {
  if (!remoteDesktop_ || !remoteDesktopView_)
    return;
  auto *card = remoteDesktopView_->cardFor(peerUserId);
  if (card == nullptr)
    return;

  const bool turningOn = !card->isControlActive();
  if (turningOn && !card->isStreamActive())
    return;
  if (turningOn) {
    if (!remoteInputCapture_) {
      remoteInputCapture_ = std::make_unique<RemoteInputCapture>(
          remoteDesktop_->inputSender(), this);
      // 急停：鼠标被注入动作带偏时，键盘是唯一还点得中的退路。
      connect(remoteInputCapture_.get(),
              &RemoteInputCapture::releaseControlRequested, this, [this] {
                if (!remoteDesktopView_)
                  return;
                for (const QString &peerId :
                     remoteDesktopView_->sessionPeerIds()) {
                  if (remoteDesktopView_->isControlActive(peerId)) {
                    toggleRemoteDesktopControl(peerId);
                  }
                }
              });
    }
    // 一次只控一台：换目标前先把上一台的采集停掉并让它全部放开。
    for (const QString &peerId : remoteDesktopView_->sessionPeerIds()) {
      if (peerId != peerUserId)
        remoteDesktopView_->setControlActive(peerId, false);
    }
    remoteInputCapture_->attachTo(card->renderSurface());
    // 首帧/尺寸回调前这里是空尺寸，capture 会保持空映射且不发送坐标。
    remoteInputCapture_->setRemoteVideoSize(remoteDesktopRemoteVideoSize_);
    remoteInputCapture_->setEnabled(true);
  } else if (remoteInputCapture_) {
    // 关掉时 capture 会自动发一次"全部抬起"，不留悬空按键。
    remoteInputCapture_->setEnabled(false);
    remoteInputCapture_->attachTo(nullptr);
  }
  remoteDesktopView_->setControlActive(peerUserId, turningOn);
}

void MainWindow::applyRemoteDesktopFullScreen(bool fullScreen) {
  // 只收窗口外壳，不动卡片自身层级：画面渲染在原生子窗口上，
  // 一旦重新 parent 就可能重建 HWND，TRTC 手里的句柄失效直接黑屏。
  if (fullScreen) {
    remoteFullScreenWasMaximized_ = isMaximized();
    showRemotePage();
    if (navRail_)
      navRail_->hide();
    showFullScreen();
  } else {
    if (navRail_)
      navRail_->show();
    if (remoteFullScreenWasMaximized_) {
      showMaximized();
    } else {
      showNormal();
    }
  }
}

void MainWindow::handleRemoteDesktopConsent(const QString &fromUserId) {
  RemoteDesktopConsentDialog dialog(fromUserId,
                                    RemoteDesktop::kConsentTimeoutMs, this);
  const bool accepted = dialog.exec() == QDialog::Accepted;
  remoteDesktop_->resolveConsent(accepted);
}

void MainWindow::showMoreMenu() {
  // 菜单跟着当前会话走，和远程桌面按钮同一套逻辑：正在跟谁聊天就排查谁。
  const QString peerId = app_.chatState().selectedPeerId();
  QMenu menu(this);
  QAction *diagnose = menu.addAction(QStringLiteral("远程排障"));
  // 没有选中会话、或排障已在进行时不可用。让菜单项灰着而不是点了没反应——
  // 后者会让人以为程序卡了。
  diagnose->setEnabled(!peerId.isEmpty() &&
                       (diagnostics_ == nullptr || !diagnostics_->isRunning()));
  connect(diagnose, &QAction::triggered, this,
          &MainWindow::requestRemoteDiagnostics);
  if (diagnostics_ && diagnostics_->isRunning()) {
    QAction *cancel = menu.addAction(QStringLiteral("取消远程排障"));
    cancel->setEnabled(!diagnostics_->isSending());
    connect(cancel, &QAction::triggered, this, [this] {
      if (diagnostics_)
        diagnostics_->cancel();
    });
  }
  menu.exec(moreButton_->mapToGlobal(QPoint(0, moreButton_->height())));
}

// 作用：收集当前远程桌面诊断信息并请求导出。
void MainWindow::requestRemoteDiagnostics() {
  const QString peerId = app_.chatState().selectedPeerId();
  if (peerId.isEmpty())
    return;

  // 被采集好友也可以负责排查并接收 A+B 合并报告。
  QList<RemoteIMContact> candidates;
  for (const RemoteIMContact &contact : app_.chatState().contacts()) {
    if (contact.userId == app_.chatState().ownerUserId())
      continue;
    auto candidate = contact;
    if (candidate.userId == peerId)
      candidate.displayName += QStringLiteral(" · 当前会话");
    candidates.append(candidate);
  }
  if (candidates.isEmpty()) {
    AppMessageDialog::show(
        this, AppMessageDialog::Kind::Info, QStringLiteral("远程排障"),
        QStringLiteral("没有可以接收报告的好友。先添加一位好友再试。"));
    return;
  }

  // ChatState 没有按 id 取显示名的接口，从联系人列表里找；找不到就用 id 本身，
  // 不留空——确认框里必须能看出是在排查谁。
  QString peerName = peerId;
  for (const RemoteIMContact &contact : app_.chatState().contacts()) {
    if (contact.userId != peerId)
      continue;
    if (!contact.displayName.isEmpty())
      peerName = contact.displayName;
    break;
  }
  const auto confirmedAccount = app_.client().currentAccount();
  RemoteDiagnosticsDialog dialog(peerName, candidates, this);
  if (dialog.exec() != QDialog::Accepted)
    return;
  if (confirmedAccount != app_.client().currentAccount()) {
    diagnosticsStatusLabel_->setText(QStringLiteral(
        "远程排障：确认期间账号已变更，未发起采集，请重新确认。"));
    diagnosticsStatusLabel_->show();
    return;
  }
  const QString recipientId = dialog.selectedRecipientId();
  if (recipientId.isEmpty())
    return;

  if (!diagnostics_) {
    diagnostics_ = new RemoteDiagnosticsController(&app_, this);
    connect(diagnostics_, &RemoteDiagnosticsController::changed, this,
            &MainWindow::refreshDiagnosticsStatus);
  }
  // 收件人在这里定死，交给控制器锁住——之后用户切到别的聊天也不改收件人。
  diagnostics_->start(peerId, recipientId);
  refreshDiagnosticsStatus();
}

void MainWindow::refreshDiagnosticsStatus() {
  if (!diagnostics_)
    return;
  const QString status = diagnostics_->status();
  if (status.isEmpty())
    return;
  if (diagnosticsStatusLabel_) {
    diagnosticsStatusLabel_->setText(QStringLiteral("远程排障：") + status);
    diagnosticsStatusLabel_->show();
  }
}

void MainWindow::requestRemoteDesktop() {
  if (!remoteDesktop_)
    return;

  // 会话进行中时同一个按钮就是断开入口（连接中点击则取消）。
  if (remoteDesktop_->viewerState() != RemoteDesktop::ViewerState::Idle &&
      remoteDesktop_->viewerState() != RemoteDesktop::ViewerState::Failed) {
    remoteDesktop_->stopSession();
    return;
  }

  const QString peerId = app_.chatState().selectedPeerId();
  if (peerId.isEmpty())
    return;

  // 直接发起，不打断用户。绝大多数情况对方靠白名单授权即可；
  // 只有对方额外设了访问密码时才会被拒，那时再按需索取密码。
  remoteDesktop_->requestView(peerId, remoteDesktopPasswords_.value(peerId));
}

void MainWindow::showConversationContextMenu(const QPoint &pos) {
  if (!conversationList_)
    return;
  QListWidgetItem *item = conversationList_->itemAt(pos);
  if (!item)
    return;
  conversationList_->setCurrentItem(item);

  QMenu menu(this);
  applyMessageContextMenuStyle(menu);
  QAction *clearAction =
      menu.addAction(makeLineIcon(LineIconKind::Trash, kMenuIconColor),
                     QStringLiteral("删除消息"));
  QAction *selectedAction =
      menu.exec(conversationList_->viewport()->mapToGlobal(pos));
  if (selectedAction == clearAction) {
    clearMessagesFromItem(item);
  }
}

void MainWindow::showContactContextMenu(QListWidget *list, const QPoint &pos) {
  if (!list)
    return;
  QListWidgetItem *item = list->itemAt(pos);
  if (!item)
    return;

  if (item->data(IsGroupHeaderRole).toBool()) {
    showContactGroupContextMenu(list, item, pos);
    return;
  }
  list->setCurrentItem(item);

  QMenu menu(this);
  applyMessageContextMenuStyle(menu);
  // 「移动到分组」只在通讯录页出现：会话列表里的行是会话，不是联系人。
  if (list == contactsList_) {
    appendMoveToGroupMenu(menu, item->data(UserIdRole).toString(),
                          item->data(GroupNameRole).toString());
    menu.addSeparator();
  }
  QAction *deleteAction =
      menu.addAction(makeLineIcon(LineIconKind::Trash, kMenuIconColor),
                     QStringLiteral("删除好友"));
  QAction *selectedAction = menu.exec(list->viewport()->mapToGlobal(pos));
  if (selectedAction == deleteAction) {
    deleteContactFromItem(item);
  }
}

void MainWindow::appendMoveToGroupMenu(QMenu &menu, const QString &userId,
                                       const QString &currentGroup) {
  if (userId.isEmpty())
    return;
  QMenu *submenu = menu.addMenu(QStringLiteral("移动到分组"));
  applyMessageContextMenuStyle(*submenu);

  const QStringList groups = app_.chatState().contactGroups();
  for (const QString &group : groups) {
    QAction *action = submenu->addAction(group);
    action->setCheckable(true);
    action->setChecked(group == currentGroup);
    // 当前分组仍然列出并打勾，只是点不动：只列出别的分组的话，
    // 用户就无从知道这个人现在究竟在哪一组。
    action->setEnabled(group != currentGroup);
    connect(action, &QAction::triggered, this,
            [this, userId, group] { app_.setContactGroup(userId, group); });
  }
  // 「移出分组」是个动作，不是一个叫「未分组」的去处——界面上没有那一节。
  if (!currentGroup.isEmpty()) {
    QAction *ungroup = submenu->addAction(QStringLiteral("移出分组"));
    connect(ungroup, &QAction::triggered, this,
            [this, userId] { app_.setContactGroup(userId, QString()); });
  }
}

void MainWindow::showContactGroupContextMenu(QListWidget *list,
                                             QListWidgetItem *item,
                                             const QPoint &pos) {
  const QString groupName = item->data(GroupNameRole).toString();
  if (groupName.isEmpty())
    return;
  QMenu menu(this);
  applyMessageContextMenuStyle(menu);
  QAction *renameAction = menu.addAction(QStringLiteral("重命名分组"));
  QAction *deleteAction =
      menu.addAction(makeLineIcon(LineIconKind::Trash, kMenuIconColor),
                     QStringLiteral("删除分组"));

  QAction *selected = menu.exec(list->viewport()->mapToGlobal(pos));
  if (!selected)
    return;
  if (selected == renameAction) {
    renameContactGroup(groupName);
  } else if (selected == deleteAction) {
    deleteContactGroup(groupName);
  }
}

void MainWindow::openForwardDialog(const RemoteIMMessage &message) {
  const QList<RemoteIMContact> contacts = acceptedContacts();
  if (contacts.isEmpty()) {
    AppMessageDialog::show(this, AppMessageDialog::Kind::Info,
                           QStringLiteral("还没有联系人"),
                           QStringLiteral("通讯录是空的，暂时无法转发。"));
    return;
  }

  BroadcastDialog dialog(contacts, {}, QString(),
                         BroadcastDialog::Mode::Forward, this);
  if (dialog.exec() != QDialog::Accepted)
    return;
  const QStringList peerIds = dialog.selectedPeerIds();
  if (peerIds.size() != 1)
    return;
  app_.forwardMessage(message, peerIds.first());
}

void MainWindow::renameContactGroup(const QString &groupName) {
  if (groupName.isEmpty())
    return;
  bool accepted = false;
  AppTextInputDialog::Options options;
  options.title = QStringLiteral("重命名分组");
  options.initialText = groupName;
  options.placeholder = QStringLiteral("分组名");
  const QString name = AppTextInputDialog::getText(this, options, &accepted);
  if (!accepted)
    return;

  const QString clean = ContactGroups::normalize(name);
  if (clean == groupName)
    return;
  if (!ContactGroups::isAcceptableName(clean)) {
    AppMessageDialog::show(this, AppMessageDialog::Kind::Warning,
                           QStringLiteral("分组名不可用"),
                           QStringLiteral("分组名不能为空。"));
    return;
  }
  if (!app_.renameContactGroup(groupName, clean)) {
    AppMessageDialog::show(
        this, AppMessageDialog::Kind::Warning, QStringLiteral("改名失败"),
        QStringLiteral("已经有一个叫「%1」的分组了。两个分组不会被合并。")
            .arg(clean));
  }
}

void MainWindow::deleteContactGroup(const QString &groupName) {
  if (groupName.isEmpty())
    return;
  int memberCount = 0;
  for (const RemoteIMContact &contact : app_.chatState().contacts()) {
    if (contact.groupName == groupName)
      ++memberCount;
  }
  // 说清楚人不会丢。不说的话，删一个装着十几个好友的分组是很吓人的操作。
  const QString body =
      memberCount == 0
          ? QStringLiteral("分组「%1」是空的，删除后不影响任何联系人。")
                .arg(groupName)
          : QStringLiteral("删除分组「%1」后，组里的 %2 "
                           "位联系人会直接列在通讯录里，好友本身不会被删除。")
                .arg(groupName)
                .arg(memberCount);
  if (!AppMessageDialog::confirm(this, QStringLiteral("删除分组"), body,
                                 QStringLiteral("删除分组"), true)) {
    return;
  }
  app_.deleteContactGroup(groupName);
}

void MainWindow::clearMessagesFromItem(QListWidgetItem *item) {
  if (!item)
    return;
  const QString userId = item->data(UserIdRole).toString().trimmed();
  if (userId.isEmpty())
    return;
  const QString displayName = item->data(DisplayNameRole).toString().trimmed();
  if (!AppMessageDialog::confirm(
          this, QStringLiteral("删除消息"),
          QStringLiteral(
              "确定删除与“%1”的全部聊天记录吗？好友会保留，此操作不可恢复。")
              .arg(displayName.isEmpty() ? userId : displayName),
          QStringLiteral("删除"), /*destructive=*/true)) {
    return;
  }
  app_.clearMessagesWith(userId);
}

void MainWindow::deleteContactFromItem(QListWidgetItem *item) {
  if (!item)
    return;
  const QString userId = item->data(UserIdRole).toString().trimmed();
  if (userId.isEmpty())
    return;
  const QString displayName = item->data(DisplayNameRole).toString().trimmed();
  if (!AppMessageDialog::confirm(
          this, QStringLiteral("删除好友"),
          QStringLiteral("确定将“%1”移出好友列表吗？聊天历史会保留。")
              .arg(displayName.isEmpty() ? userId : displayName),
          QStringLiteral("删除"), /*destructive=*/true)) {
    return;
  }
  app_.deleteContact(userId);
}

void MainWindow::deleteSelectedContactFromList(QListWidget *list) {
  if (!list)
    return;
  // 与右键菜单语义一致：会话列表 Delete 只清空聊天记录，删除好友走通讯录。
  if (list == conversationList_) {
    clearMessagesFromItem(list->currentItem());
    return;
  }
  deleteContactFromItem(list->currentItem());
}

QString MainWindow::contactName(const QString &userId) const {
  for (const RemoteIMContact &contact : app_.chatState().contacts()) {
    if (contact.userId == userId)
      return contact.displayName.isEmpty() ? contact.userId
                                           : contact.displayName;
  }
  return userId;
}
