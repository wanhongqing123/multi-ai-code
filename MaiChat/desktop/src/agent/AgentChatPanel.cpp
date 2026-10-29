// 文件作用：桌面端创作中心和 Agent 会话；AgentChatPanel 负责面板呈现、输入处理和状态反馈，本文件给出其实现。
// 主要入口：isCurrentSession、sessionId、setModelLabel、setOpenVideoCallback。
// 直接依赖：MaiPathGuard.h、agent/AgentController.h、markdown/MarkdownView.h、ui/ComposerResizeSplitter.h；实现与这些接口协作。
#include "agent/AgentChatPanel.h"

#include "ui/DesktopCameraDialog.h"
#include "ui/DesktopMusicPlayerDialog.h"
#include "MaiVideoCircleClient.h"

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <atomic>
#include <functional>
#include <variant>

#include "MaiPathGuard.h"
#include "agent/AgentController.h"
#include "markdown/MarkdownView.h"
#include "ui/ComposerResizeSplitter.h"
#include "ui/ComposerTextEdit.h"
#include "ui/ImagePreviewDialog.h"
#include "ui/MessageImageLoader.h"
#include "ui/PdfPreviewDialog.h"
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
const QString kComposerVideoPrefix = QStringLiteral("maichat-video:");

QString videoResourceName(const QString& path) {
    return kComposerVideoPrefix + QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath())
        .toString(QUrl::FullyEncoded);
}

QString videoPathFromResource(const QString& name) {
    return name.startsWith(kComposerVideoPrefix)
        ? QUrl(name.mid(kComposerVideoPrefix.size())).toLocalFile() : QString();
}

QString videoMimeType(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("mp4")) return QStringLiteral("video/mp4");
    if (suffix == QStringLiteral("mov")) return QStringLiteral("video/quicktime");
    if (suffix == QStringLiteral("webm")) return QStringLiteral("video/webm");
    const QString detected = QMimeDatabase().mimeTypeForFile(path).name().toLower();
    return detected.startsWith(QStringLiteral("video/")) ? detected : QString();
}

QImage videoDraftImage(const QImage& source) {
    QImage preview(480, 270, QImage::Format_ARGB32_Premultiplied);
    preview.fill(QColor(QStringLiteral("#111827")));
    QPainter painter(&preview);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!source.isNull()) {
        const QImage fitted = source.scaled(preview.size(), Qt::KeepAspectRatioByExpanding,
                                            Qt::SmoothTransformation);
        painter.drawImage(QRect(QPoint(0, 0), preview.size()), fitted,
                          QRect((fitted.width() - preview.width()) / 2,
                                (fitted.height() - preview.height()) / 2,
                                preview.width(), preview.height()));
    }
    painter.setBrush(QColor(0, 0, 0, 160));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPointF(240, 120), 30, 30);
    painter.setBrush(Qt::white);
    QPolygonF triangle;
    triangle << QPointF(231, 104) << QPointF(231, 136) << QPointF(258, 120);
    painter.drawPolygon(triangle);
    return preview;
}

std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString fromUtf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

QString defaultWorkspaceDirectory(const QString& preferred, bool configured) {
    if (configured) {
        if (preferred.isEmpty()) return {};
        if (QDir(preferred).isRoot() || !QDir().mkpath(preferred) ||
            !QFileInfo(preferred).isWritable()) return {};
        return QDir(preferred).canonicalPath();
    }
    const QStringList roots = {
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
        QDir::homePath(),
    };
    for (const QString& root : roots) {
        if (root.isEmpty() || QDir(root).isRoot()) continue;
        const QString workspace = QDir(root).filePath(QStringLiteral("Workspace"));
        if (!QDir().mkpath(workspace) || !QFileInfo(workspace).isWritable()) continue;
        return QDir(workspace).canonicalPath();
    }
    return {};
}

struct LocalMediaMention {
    QString path;
    bool video = false;
};

QVector<LocalMediaMention> localMediaMentions(const QString& text, const QString& workspace) {
    if (text.size() > 100000 || workspace.isEmpty()) return {};
    static const QRegularExpression code(QStringLiteral(R"rx(`([^`\r\n]+)`)rx"),
                                         QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bare(
        QStringLiteral(R"rx([^\s`()<>\[\]{}"'“”‘’,，。;；:：|]+?\.(?:mp4|mov|wav|mp3|m4a|aac)(?![A-Za-z0-9_.]))rx"),
        QRegularExpression::CaseInsensitiveOption);
    QVector<LocalMediaMention> mentions;
    QSet<QString> seen;
    bool fenced = false;
    const auto add = [&](const QString& raw) {
        const QString name = raw.trimmed();
        const QString path = fromUtf8(maiResolvePathWithinRoot(toUtf8(workspace), toUtf8(name)));
        const QFileInfo file(path);
        if (path.isEmpty() || !file.isFile() || file.size() <= 0 || seen.contains(path)) return;
        const QString suffix = file.suffix().toLower();
        const bool video = suffix == QStringLiteral("mp4") || suffix == QStringLiteral("mov");
        const bool audio = suffix == QStringLiteral("wav") || suffix == QStringLiteral("mp3") ||
                           suffix == QStringLiteral("m4a") || suffix == QStringLiteral("aac");
        if (!video && !audio) return;
        seen.insert(path);
        mentions.push_back({path, video});
    };
    for (const QString& line : text.split(QLatin1Char('\n'), Qt::KeepEmptyParts)) {
        if (line.trimmed().startsWith(QStringLiteral("```"))) {
            fenced = !fenced;
            continue;
        }
        if (fenced || line.startsWith(QStringLiteral("    "))) continue;
        QVector<QPair<int, int>> codeRanges;
        auto quoted = code.globalMatch(line);
        while (quoted.hasNext()) {
            const auto match = quoted.next();
            codeRanges.push_back({match.capturedStart(), match.capturedEnd()});
            add(match.captured(1));
            if (mentions.size() > 4) return {};
        }
        auto unquoted = bare.globalMatch(line);
        while (unquoted.hasNext()) {
            const auto match = unquoted.next();
            const bool insideCode = std::any_of(codeRanges.cbegin(), codeRanges.cend(), [&](const auto& range) {
                return match.capturedStart() < range.second && match.capturedEnd() > range.first;
            });
            if (insideCode) continue;
            add(match.captured());
            if (mentions.size() > 4) return {};
        }
    }
    return mentions;
}

QString toolPrimaryArgument(const QString& tool, const QString& arguments) {
    const QJsonDocument document = QJsonDocument::fromJson(arguments.toUtf8());
    if (!document.isObject()) return arguments.simplified();
    const QJsonObject object = document.object();
    if (tool == QStringLiteral("maichat_broadcast_text")) {
        QStringList recipients;
        for (const QJsonValue& value : object.value(QStringLiteral("peer_ids")).toArray())
            recipients.append(value.toString());
        return recipients.join(QStringLiteral("、"));
    }
    const QString key = tool == QStringLiteral("shell")               ? QStringLiteral("command")
                        : tool == QStringLiteral("file_glob")         ? QStringLiteral("pattern")
                        : tool == QStringLiteral("file_grep")         ? QStringLiteral("pattern")
                        : tool == QStringLiteral("curl_fetch")        ? QStringLiteral("url")
                        : tool.startsWith(QStringLiteral("maichat_")) ? QStringLiteral("peer_id")
                                                                      : QStringLiteral("path");
    return object.value(key).toString().simplified();
}

QString toolActionText(const QString& tool, const QString& argument, MaiToolState state, bool waitingForUser) {
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

bool isPaidGenerationTool(const QString& tool) {
    return tool == QStringLiteral("wan_video") || tool == QStringLiteral("wan_image") ||
           tool == QStringLiteral("seedance_video") || tool == QStringLiteral("seedream_image") ||
           tool == QStringLiteral("glm_image") || tool == QStringLiteral("kling_bailian_video") ||
           tool == QStringLiteral("kling_bailian_image") || tool == QStringLiteral("minimax_video") ||
           tool == QStringLiteral("minimax_image");
}

QString videoTaskManagementAction(const QString& tool, const QString& arguments) {
    if (tool != QStringLiteral("wan_video") && tool != QStringLiteral("seedance_video") &&
        tool != QStringLiteral("kling_bailian_video") && tool != QStringLiteral("minimax_video"))
        return {};
    const QJsonObject input = QJsonDocument::fromJson(arguments.toUtf8()).object();
    const QString action = input.value(QStringLiteral("action")).toString();
    const bool providerCanCancel = tool == QStringLiteral("wan_video") || tool == QStringLiteral("seedance_video") ||
                                   tool == QStringLiteral("kling_bailian_video");
    return action == QStringLiteral("delete") || (providerCanCancel && action == QStringLiteral("cancel")) ? action
                                                                                                           : QString{};
}

QString paidGenerationTitle(const QString& tool, const QString& arguments) {
    const QJsonObject input = QJsonDocument::fromJson(arguments.toUtf8()).object();
    const bool video = tool == QStringLiteral("wan_video") || tool == QStringLiteral("seedance_video") ||
                       tool == QStringLiteral("kling_bailian_video") || tool == QStringLiteral("minimax_video");
    const bool revision = input.value(QStringLiteral("action")).toString() == QStringLiteral("revise") ||
                          input.value(QStringLiteral("mode")).toString() == QStringLiteral("edit") ||
                          input.value(QStringLiteral("mode")).toString() == QStringLiteral("extend") ||
                          input.value(QStringLiteral("task_mode")).toString() == QStringLiteral("edit") || false;
    return QStringLiteral("确认%1%2")
        .arg(revision ? QStringLiteral("编辑") : QStringLiteral("生成"),
             video ? QStringLiteral("视频") : QStringLiteral("图片"));
}

QString paidGenerationDetail(const QString& tool, const QString& arguments) {
    const QJsonObject input = QJsonDocument::fromJson(arguments.toUtf8()).object();
    QString provider = QStringLiteral("Seedream");
    if (tool == QStringLiteral("wan_video") || tool == QStringLiteral("wan_image"))
        provider = QStringLiteral("万相");
    else if (tool == QStringLiteral("seedance_video"))
        provider = QStringLiteral("Seedance");
    else if (tool == QStringLiteral("glm_image"))
        provider = QStringLiteral("GLM");
    else if (tool == QStringLiteral("kling_bailian_video") || tool == QStringLiteral("kling_bailian_image"))
        provider = QStringLiteral("可灵 · 百炼");
    else if (tool == QStringLiteral("minimax_video") || tool == QStringLiteral("minimax_image"))
        provider = QStringLiteral("海螺 / MiniMax");
    QString request = input.value(QStringLiteral("message")).toString().trimmed();
    if (request.isEmpty()) request = input.value(QStringLiteral("prompt")).toString().trimmed();
    QStringList specifications;
    const QString model = input.value(QStringLiteral("model")).toString();
    if (tool == QStringLiteral("seedance_video")) {
        if (model == QStringLiteral("doubao-seedance-2-0-fast-260128"))
            specifications.push_back(QStringLiteral("Seedance 2.0 Fast"));
        else if (model == QStringLiteral("doubao-seedance-2-0-mini-260615"))
            specifications.push_back(QStringLiteral("Seedance 2.0 Mini"));
        else if (model == QStringLiteral("doubao-seedance-2-0-260128"))
            specifications.push_back(QStringLiteral("Seedance 2.0"));
        else if (model == QStringLiteral("doubao-seedance-2-5-260628"))
            specifications.push_back(QStringLiteral("Seedance 2.5"));
        else
            specifications.push_back(model.isEmpty() && input.value(QStringLiteral("action")).toString() ==
                                                            QStringLiteral("revise")
                                         ? QStringLiteral("沿用上一任务型号")
                                         : (model.isEmpty() ? QStringLiteral("Seedance 2.0 Mini") : model));
    }
    if (model == QStringLiteral("MiniMax-H3") || model == QStringLiteral("MiniMax-H3-Max"))
        specifications.push_back(model == QStringLiteral("MiniMax-H3-Max") ? QStringLiteral("H3 Max")
                                                                           : QStringLiteral("H3"));
    if (tool == QStringLiteral("minimax_image"))
        specifications.push_back(model == QStringLiteral("image-01-live") ? QStringLiteral("Image-01 Live")
                                                                          : QStringLiteral("Image-01"));
    if (tool == QStringLiteral("kling_bailian_video") || tool == QStringLiteral("kling_bailian_image"))
        specifications.push_back(model.contains(QStringLiteral("omni")) ? QStringLiteral("可灵 V3 Omni")
                                 : model.contains(QStringLiteral("turbo")) ||
                                         (model.isEmpty() && tool == QStringLiteral("kling_bailian_video"))
                                     ? QStringLiteral("可灵 V3 Turbo")
                                     : QStringLiteral("可灵 V3"));
    const QJsonObject production = input.value(QStringLiteral("production")).toObject();
    const int duration =
        input.value(QStringLiteral("duration")).toInt(production.value(QStringLiteral("duration")).toInt());
    if (duration > 0) specifications.push_back(QStringLiteral("%1 秒").arg(duration));
    if (input.value(QStringLiteral("source_duration_seconds")).isDouble())
        specifications.push_back(
            QStringLiteral("源片 %1 秒")
                .arg(input.value(QStringLiteral("source_duration_seconds")).toDouble(), 0, 'f', 1));
    QString resolution = input.value(QStringLiteral("resolution")).toString();
    if (resolution.isEmpty()) resolution = production.value(QStringLiteral("resolution")).toString();
    if (!resolution.isEmpty()) specifications.push_back(resolution);
    QString ratio = input.value(QStringLiteral("ratio")).toString();
    if (ratio.isEmpty()) ratio = production.value(QStringLiteral("ratio")).toString();
    if (!ratio.isEmpty())
        specifications.push_back(ratio == QStringLiteral("adaptive") ? QStringLiteral("按素材比例") : ratio);
    const bool virtualAvatar = !input.value(QStringLiteral("virtual_avatar_asset_id")).toString().isEmpty();
    if (virtualAvatar) specifications.push_back(QStringLiteral("平台虚拟人像"));
    if (!input.value(QStringLiteral("authorized_portrait_asset_id")).toString().isEmpty())
        specifications.push_back(QStringLiteral("已授权真人形象"));
    const QJsonArray registeredAssets = input.value(QStringLiteral("reference_asset_ids")).toArray();
    if (!registeredAssets.isEmpty())
        specifications.push_back(QStringLiteral("已登记参考素材 %1 张").arg(registeredAssets.size()));
    int strictFrameAssets = input.value(QStringLiteral("first_frame_asset_id")).toString().isEmpty() ? 0 : 1;
    if (!input.value(QStringLiteral("last_frame_asset_id")).toString().isEmpty()) ++strictFrameAssets;
    if (strictFrameAssets > 0) specifications.push_back(QStringLiteral("已登记严格帧 %1 张").arg(strictFrameAssets));
    if (!input.value(QStringLiteral("video_asset_id")).toString().isEmpty())
        specifications.push_back(QStringLiteral("已登记视频素材"));
    const QString assetGroupType = input.value(QStringLiteral("asset_group_type")).toString();
    if (!assetGroupType.isEmpty())
        specifications.push_back(assetGroupType == QStringLiteral("LivenessFace") ? QStringLiteral("真人授权素材组")
                                                                                  : QStringLiteral("虚拟素材组"));
    if (!input.value(QStringLiteral("size")).toString().isEmpty())
        specifications.push_back(input.value(QStringLiteral("size")).toString());
    QJsonArray images = input.value(QStringLiteral("local_image_paths")).toArray();
    if (images.isEmpty()) images = input.value(QStringLiteral("image_paths")).toArray();
    int imageCount = images.size();
    if (!input.value(QStringLiteral("image_path")).toString().isEmpty() ||
        !input.value(QStringLiteral("image_url")).toString().isEmpty() ||
        !input.value(QStringLiteral("first_frame")).toString().isEmpty())
        ++imageCount;
    if (!input.value(QStringLiteral("last_frame_path")).toString().isEmpty() ||
        !input.value(QStringLiteral("last_frame")).toString().isEmpty())
        ++imageCount;
    int videoCount = !input.value(QStringLiteral("video_path")).toString().isEmpty() ||
                             !input.value(QStringLiteral("video_asset_id")).toString().isEmpty() ||
                             !input.value(QStringLiteral("video_task_id")).toString().isEmpty() ||
                             !input.value(QStringLiteral("video_url")).toString().isEmpty() ||
                             !input.value(QStringLiteral("reference_video_path")).toString().isEmpty() ||
                             !input.value(QStringLiteral("reference_video")).toString().isEmpty()
                         ? 1
                         : 0;
    int audioCount = !input.value(QStringLiteral("reference_audio_path")).toString().isEmpty() ||
                             !input.value(QStringLiteral("reference_audio")).toString().isEmpty()
                         ? 1
                         : 0;
    const QJsonArray content = input.value(QStringLiteral("content")).toArray();
    for (const QJsonValue& item : content) {
        const QString type = item.toObject().value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("image_url")) ++imageCount;
        if (type == QStringLiteral("video_url")) ++videoCount;
        if (type == QStringLiteral("audio_url")) ++audioCount;
    }
    if (imageCount > 0) specifications.push_back(QStringLiteral("参考图片 %1 张").arg(imageCount));
    if (videoCount > 0) specifications.push_back(QStringLiteral("参考视频 %1 个").arg(videoCount));
    if (audioCount > 0) specifications.push_back(QStringLiteral("参考音频 %1 个").arg(audioCount));
    const bool authorizedPortrait = !input.value(QStringLiteral("authorized_portrait_asset_id")).toString().isEmpty();
    const bool usesRegisteredAsset = !registeredAssets.isEmpty() || strictFrameAssets > 0 ||
                                     !input.value(QStringLiteral("video_asset_id")).toString().isEmpty();
    QString detail = usesRegisteredAsset ? QStringLiteral(
                                               "将使用已登记的参考素材；平台仍会审核。确认后提交给%"
                                               "1，可能消耗模型额度。")
                                               .arg(provider)
                     : virtualAvatar     ? QStringLiteral(
                                               "将使用平台虚拟人像，不保留真实人物长相"
                                               "。确认后提交给%1，可能消耗模型额度。")
                                               .arg(provider)
                     : authorizedPortrait
                         ? QStringLiteral("将使用已授权真人形象。确认后提交给%1，可能消耗模型额度。").arg(provider)
                         : QStringLiteral("确认后将提交给%1，可能消耗模型额度。").arg(provider);
    if (!request.isEmpty()) detail += QStringLiteral("\n\n") + request;
    if (!specifications.isEmpty()) detail += QStringLiteral("\n\n") + specifications.join(QStringLiteral(" · "));
    return detail;
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

class WelcomeScrollArea final : public QScrollArea {
public:
    explicit WelcomeScrollArea(QWidget* parent) : QScrollArea(parent) {}

    void setFeatures(QGridLayout* grid, const QList<QWidget*>& cards) {
        grid_ = grid;
        cards_ = cards;
        arrangeFeatures();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QScrollArea::resizeEvent(event);
        arrangeFeatures();
    }

private:
    void arrangeFeatures() {
        if (!grid_) return;
        const int width = viewport()->width();
        const int columns = width >= UiZoom::s(900) ? 4 : width >= UiZoom::s(650) ? 3 : 2;
        if (columns == columns_) return;
        for (QWidget* card : cards_) grid_->removeWidget(card);
        for (int column = 0; column < 4; ++column) grid_->setColumnStretch(column, column < columns ? 1 : 0);
        for (int index = 0; index < cards_.size(); ++index)
            grid_->addWidget(cards_[index], index / columns, index % columns);
        columns_ = columns;
        setProperty("welcomeColumns", columns);
    }

    QGridLayout* grid_ = nullptr;
    QList<QWidget*> cards_;
    int columns_ = 0;
};

QWidget* makeWelcomeView(QWidget* parent) {
    auto* scroll = new WelcomeScrollArea(parent);
    scroll->setObjectName(QStringLiteral("agentWelcomeView"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea#agentWelcomeView{background:#ffffff;border:0;}"));

    auto* canvas = new QWidget(scroll);
    canvas->setObjectName(QStringLiteral("agentWelcomeCanvas"));
    auto* canvasLayout = new QVBoxLayout(canvas);
    canvasLayout->setSizeConstraint(QLayout::SetMinimumSize);
    canvasLayout->setContentsMargins(UiZoom::s(20), UiZoom::s(24), UiZoom::s(20), UiZoom::s(24));
    canvasLayout->addStretch(1);
    auto* centered = new QHBoxLayout;
    centered->addStretch(0);
    auto* content = new QWidget(canvas);
    content->setMaximumWidth(UiZoom::s(1100));
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setSizeConstraint(QLayout::SetMinimumSize);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    auto* sparkle = makeLabel(QStringLiteral("✦"), 28, kAccent, true);
    sparkle->setObjectName(QStringLiteral("agentWelcomeSparkle"));
    sparkle->setAlignment(Qt::AlignCenter);
    sparkle->setFixedSize(UiZoom::s(52), UiZoom::s(52));
    sparkle->setTextInteractionFlags(Qt::NoTextInteraction);
    sparkle->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QLabel#agentWelcomeSparkle{background:#e4f4ff;color:#168bd4;border-radius:15px;}")));
    contentLayout->addWidget(sparkle, 0, Qt::AlignHCenter);
    auto* heading = makeLabel(QStringLiteral("从这里开始创作"), 25, kInk, true);
    heading->setObjectName(QStringLiteral("agentWelcomeHeading"));
    heading->setAlignment(Qt::AlignCenter);
    heading->setTextInteractionFlags(Qt::NoTextInteraction);
    contentLayout->addSpacing(UiZoom::s(18));
    contentLayout->addWidget(heading);
    contentLayout->addSpacing(UiZoom::s(28));

    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(UiZoom::s(10));
    grid->setVerticalSpacing(UiZoom::s(10));
    QList<QWidget*> cards;
    const QList<QPair<QString, QString>> features = {
        {QStringLiteral("AI 问答"), QStringLiteral("AI")},      {QStringLiteral("图片创作"), QStringLiteral("图")},
        {QStringLiteral("视频创作"), QStringLiteral("视")},     {QStringLiteral("文件处理"), QStringLiteral("文")},
        {QStringLiteral("桌面截屏"), QStringLiteral("屏")},     {QStringLiteral("浏览器操作"), QStringLiteral("网")},
        {QStringLiteral("终端与进程"), QStringLiteral("终")},   {QStringLiteral("电脑操作"), QStringLiteral("控")},
        {QStringLiteral("会话定时任务"), QStringLiteral("时")}, {QStringLiteral("日历导入"), QStringLiteral("历")},
        {QStringLiteral("二维码生成"), QStringLiteral("码")},   {QStringLiteral("本地媒体处理"), QStringLiteral("媒")},
        {QStringLiteral("拍照"), QStringLiteral("拍")},         {QStringLiteral("音乐播放"), QStringLiteral("音")},
        {QStringLiteral("我的云盘"), QStringLiteral("云")},     {QStringLiteral("MaiChat 消息"), QStringLiteral("聊")},
        {QStringLiteral("联系人"), QStringLiteral("人")},     {QStringLiteral("跨设备会话"), QStringLiteral("跨")},
    };
    for (int index = 0; index < features.size(); ++index) {
        auto* card = new QFrame(content);
        card->setObjectName(QStringLiteral("agentWelcomeFeature"));
        card->setAccessibleName(features[index].first);
        card->setAccessibleDescription(QStringLiteral("功能介绍"));
        card->setMinimumHeight(UiZoom::s(58));
        card->setStyleSheet(
            UiZoom::scaleQss(QStringLiteral("QFrame#agentWelcomeFeature{background:#ffffff;border:1px solid #e5edf5;"
                                            "border-radius:13px;}")));
        auto* row = new QHBoxLayout(card);
        row->setContentsMargins(UiZoom::s(14), UiZoom::s(8), UiZoom::s(14), UiZoom::s(8));
        row->setSpacing(UiZoom::s(10));
        auto* symbol = makeLabel(features[index].second, 14, kAccent, true);
        symbol->setAlignment(Qt::AlignCenter);
        symbol->setTextInteractionFlags(Qt::NoTextInteraction);
        symbol->setFixedSize(UiZoom::s(28), UiZoom::s(28));
        symbol->setStyleSheet(
            UiZoom::scaleQss(QStringLiteral("QLabel{background:#eaf5ff;color:#168bd4;border-radius:7px;}")));
        row->addWidget(symbol);
        auto* label = makeLabel(features[index].first, 14, kInk, true);
        label->setWordWrap(false);
        label->setTextInteractionFlags(Qt::NoTextInteraction);
        row->addWidget(label, 1);
        cards.append(card);
    }
    contentLayout->addLayout(grid);
    centered->addWidget(content, 1);
    centered->addStretch(0);
    canvasLayout->addLayout(centered);
    canvasLayout->addStretch(1);
    scroll->setWidget(canvas);
    scroll->setFeatures(grid, cards);
    return scroll;
}

void applyAgentMenuStyle(QMenu* menu) {
    if (menu == nullptr) return;
    menu->setWindowFlags(menu->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
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
        // 与 IM 输入栏和移动端保持一致；18px 简单上箭头比复杂纸飞机更容易识别为发送。
        painter.drawLine(QPointF(24, 38), QPointF(24, 10));
        painter.drawLine(QPointF(13, 21), QPointF(24, 10));
        painter.drawLine(QPointF(35, 21), QPointF(24, 10));
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
    std::function<void(const QString&)> onOpenVideo;

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        const QTextCursor clicked = cursorForPosition(event->pos());
        const QString path = videoPathFromResource(clicked.charFormat().toImageFormat().name());
        if (!path.isEmpty() && onOpenVideo) {
            onOpenVideo(path);
            return;
        }
        ComposerTextEdit::mouseDoubleClickEvent(event);
    }
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
// 原来每个回答是一个 QTextBrowser，走 MarkdownRenderer 出 HTML 交给
// QTextDocument 排。 那条路受 CSS 子集限制（行内
// padding、块级圆角、border-left、复选框都不支持），
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

        setStyleSheet(UiZoom::scaleQss(QStringLiteral("QFrame#agentSubTaskCard{background:#fbfcfe;border:1px solid %1;"
                                                      "border-radius:8px;}")
                                           .arg(kLine)));
    }

    void setTask(const QString& name) {
        name_->setText(QStringLiteral("子任务 · ") + name);
    }

    void setState(const QString& state) {
        state_->setText(state);
    }

    // 输入是流式 delta，不是完整正文；先保留有限尾部再取末行，
    // 否则最后一个标点 chunk 会把整条进度摘要覆盖成一个标点。
    void appendLatestText(const QString& partId, const QString& text) {
        if (partId != latestPartId_) {
            latestPartId_ = partId;
            latestText_.clear();
        }
        latestText_.append(text);
        if (latestText_.size() > 4096) {
            latestText_ = latestText_.right(4096);
            if (latestText_.at(0).isLowSurrogate()) latestText_.remove(0, 1);
        }
        QString line = latestText_.trimmed();
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
    QString latestPartId_;
    QString latestText_;
};

class AgentChatPanel::ThinkingLine final : public QFrame {
public:
    explicit ThinkingLine(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName(QStringLiteral("agentThinkingCard"));
        setStyleSheet(QStringLiteral("QFrame#agentThinkingCard{background:transparent;border:none;}"));
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(0, UiZoom::s(2), 0, UiZoom::s(2));
        caption_ = makeLabel(QStringLiteral("正在思考…"), 12, kInkFaint);
        caption_->setWordWrap(false);
        column->addWidget(caption_);
    }

private:
    QLabel* caption_ = nullptr;
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
        expand_->setStyleSheet(
            QStringLiteral("QPushButton{background:transparent;border:none;color:#"
                           "98a2b3;padding:0;}"));
        expand_->hide();
        head->addWidget(icon_);
        head->addWidget(name_);
        head->addStretch(1);
        head->addWidget(state_);
        head->addWidget(expand_);
        column->addLayout(head);

        detail_ = makeLabel(QString(), 11, kInkSoft);
        detail_->setObjectName(QStringLiteral("agentToolDetail"));
        detail_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        detail_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont detailFont = detail_->font();
        detailFont.setStyleHint(QFont::Monospace);
        detailFont.setFamily(QStringLiteral("Menlo"));
        detail_->setFont(detailFont);
        detail_->setStyleSheet(
            UiZoom::scaleQss(QStringLiteral("QLabel#agentToolDetail{background:#f7f8fa;border:1px solid %1;"
                                            "border-radius:7px;color:%2;padding:9px;}")
                                 .arg(kLine, kInkSoft)));
        detail_->hide();
        column->addWidget(detail_);

        approval_ = new QWidget(this);
        auto* buttons = new QHBoxLayout(approval_);
        buttons->setContentsMargins(0, 0, 0, 0);
        buttons->setSpacing(UiZoom::s(7));
        allow_ = new QPushButton(QStringLiteral("允许"));
        deny_ = new QPushButton(QStringLiteral("拒绝"));
        always_ = new QPushButton(QStringLiteral("本会话都允许"));
        allow_->setStyleSheet(UiZoom::scaleQss(QStringLiteral("QPushButton{background:%1;color:#fff;"
                                                              "border:none;border-radius:6px;"
                                                              "padding:4px 14px;}")
                                                   .arg(kAccent)));
        deny_->setStyleSheet(UiZoom::scaleQss(QStringLiteral("QPushButton{background:#fff;color:%1;border:1px solid %2;"
                                                             "border-radius:6px;padding:4px 14px;}")
                                                  .arg(kInk, kLine)));
        always_->setStyleSheet(
            UiZoom::scaleQss(QStringLiteral("QPushButton{background:transparent;color:%1;border:none;"
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
        const bool wasWaiting = waiting_;
        stateValue_ = toolState;
        waiting_ = waitingForUser;
        if (isPaidGenerationTool(tool_) && wasWaiting && !waitingForUser) {
            expanded_ = false;
            detail_->hide();
            expand_->setText(QStringLiteral("›"));
        }
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
            case MaiToolState::Canceled:
                edge = kInkFaint;
                label = QStringLiteral("已取消");
                break;
        }
        state_->setText(label);
        state_->setStyleSheet(QStringLiteral("color:%1;background:transparent;")
                                  .arg(toolState == MaiToolState::Error ? kDanger : kInkFaint));
        setStyleSheet(QStringLiteral("QFrame#agentToolCard{background:transparent;border:none;}"));
        icon_->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(edge));
        approval_->setVisible(waitingForUser);
        always_->setVisible(waitingForUser && allowForSession_ && !rememberOnApproval_ && !isVideoTaskManagement());
        if (waitingForUser && !allowForSession_) {
            expanded_ = true;
            detail_->setVisible(!detailText_.isEmpty());
            expand_->setText(QStringLiteral("⌄"));
        }
        refreshSummary();
    }

    void setApprovalBehavior(bool allow, bool rememberOnApproval, int fileCount) {
        allowForSession_ = allow;
        rememberOnApproval_ = rememberOnApproval;
        if (isRealValidationLink())
            allow_->setText(QStringLiteral("生成验证链接"));
        else if (isVideoTaskManagement())
            allow_->setText(videoTaskManagementAction(tool_, arguments_) == QStringLiteral("delete")
                                ? QStringLiteral("确认删除")
                                : QStringLiteral("确认取消任务"));
        else if (isPaidGenerationTool(tool_))
            allow_->setText(paidGenerationTitle(tool_, arguments_).contains(QStringLiteral("编辑"))
                                ? QStringLiteral("确认编辑")
                                : QStringLiteral("确认生成"));
        else
            allow_->setText(rememberOnApproval ? QStringLiteral("允许并记住") : QStringLiteral("允许"));
        deny_->setText(isVideoTaskManagement()                                 ? QStringLiteral("返回")
                       : isPaidGenerationTool(tool_) || isRealValidationLink() ? QStringLiteral("取消")
                                                                               : QStringLiteral("拒绝"));
        always_->setText(fileCount == 1  ? QStringLiteral("本会话允许此文件")
                         : fileCount > 1 ? QStringLiteral("本会话允许这些文件")
                                         : QStringLiteral("本会话都允许"));
        always_->setVisible(waiting_ && allowForSession_ && !rememberOnApproval_ && !isRealValidationLink() &&
                            !isVideoTaskManagement());
    }

    void setDetail(const QString& text) {
        QString rendered;
        if (tool_ == QStringLiteral("shell") && !primaryArgument_.isEmpty()) {
            rendered = QStringLiteral("$ ") + primaryArgument_;
            if (!text.trimmed().isEmpty()) rendered += QStringLiteral("\n\n") + text.trimmed();
        } else if (tool_ == QStringLiteral("maichat_send_text") || tool_ == QStringLiteral("maichat_reply_message") ||
                   tool_ == QStringLiteral("maichat_broadcast_text")) {
            const QJsonObject object = QJsonDocument::fromJson(arguments_.toUtf8()).object();
            QString recipients = object.value(QStringLiteral("peer_id")).toString();
            if (tool_ == QStringLiteral("maichat_broadcast_text")) {
                QStringList values;
                for (const QJsonValue& value : object.value(QStringLiteral("peer_ids")).toArray())
                    values.append(value.toString());
                recipients = values.join(QStringLiteral("、"));
            }
            rendered = QStringLiteral("收件人：%1\n\n消息内容：\n%2")
                           .arg(recipients, object.value(QStringLiteral("text")).toString());
            const QString replyTo = object.value(QStringLiteral("message_id")).toString();
            if (!replyTo.isEmpty()) rendered += QStringLiteral("\n\n回复消息：%1").arg(replyTo);
            if (!text.trimmed().isEmpty()) rendered += QStringLiteral("\n\n") + text.trimmed();
        } else if (isRealValidationLink()) {
            rendered = QStringLiteral(
                "将创建一次性 H5 链接，请由照片中的本人打开并完成验证。"
                "认证凭证有效期为 30 分钟。");
        } else if (isVideoTaskManagement()) {
            const QJsonObject input = QJsonDocument::fromJson(arguments_.toUtf8()).object();
            const bool deleting = videoTaskManagementAction(tool_, arguments_) == QStringLiteral("delete");
            rendered = deleting ? (tool_ == QStringLiteral("seedance_video")
                                       ? QStringLiteral("删除方舟中的已结束任务记录和本地索引；已"
                                                        "保存的视频保留。")
                                       : QStringLiteral("只删除本地已交付任务记录；云端任务和视频保留。"))
                                : QStringLiteral("仅排队中的任务可以取消；运行中的任务会被平台拒绝。");
            rendered += QStringLiteral("\n任务：") + input.value(QStringLiteral("conversation_id")).toString();
        } else if (isPaidGenerationTool(tool_)) {
            rendered = paidGenerationDetail(tool_, arguments_);
            if (!text.trimmed().isEmpty()) rendered += QStringLiteral("\n\n") + text.trimmed();
        } else {
            rendered = arguments_.trimmed();
            if (!text.trimmed().isEmpty()) rendered += QStringLiteral("\n\n") + text.trimmed();
        }
        constexpr int kMaxDetailCharacters = 12000;
        if (rendered.size() > kMaxDetailCharacters) {
            rendered = rendered.left(kMaxDetailCharacters) + QStringLiteral("\n\n…（输出过长，已截断显示）");
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
    bool isVideoTaskManagement() const {
        return !videoTaskManagementAction(tool_, arguments_).isEmpty();
    }
    bool isRealValidationLink() const {
        if (tool_ != QStringLiteral("ark_assets")) return false;
        const QJsonObject input = QJsonDocument::fromJson(arguments_.toUtf8()).object();
        return input.value(QStringLiteral("action")).toString() == QStringLiteral("begin_real_validation");
    }

    void refreshSummary() {
        const QJsonObject input = QJsonDocument::fromJson(arguments_.toUtf8()).object();
        const bool preparingSeedance = tool_ == QStringLiteral("seedance_video") &&
                                       input.value(QStringLiteral("action")).toString() == QStringLiteral("prepare");
        if (preparingSeedance) {
            if (stateValue_ == MaiToolState::Error)
                name_->setText(QStringLiteral("人物替换 · 素材准备失败"));
            else if (stateValue_ == MaiToolState::Completed)
                name_->setText(QStringLiteral("人物替换 · 素材状态已更新"));
            else
                name_->setText(QStringLiteral("人物替换 · 正在准备素材"));
        } else if (isRealValidationLink() && waiting_)
            name_->setText(QStringLiteral("创建真人验证链接"));
        else if (isVideoTaskManagement() && waiting_)
            name_->setText(videoTaskManagementAction(tool_, arguments_) == QStringLiteral("delete")
                               ? QStringLiteral("删除视频任务记录")
                               : QStringLiteral("取消视频任务"));
        else if (isPaidGenerationTool(tool_) && waiting_)
            name_->setText(paidGenerationTitle(tool_, arguments_));
        else
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
    bool allowForSession_ = true;
    bool rememberOnApproval_ = false;
};

class AgentChatPanel::ToolGroupCard final : public QFrame {
public:
    explicit ToolGroupCard(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName(QStringLiteral("agentToolGroup"));
        setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(UiZoom::s(4));
        summary_ = new QPushButton(this);
        summary_->setObjectName(QStringLiteral("agentToolGroupDisclosure"));
        summary_->setCursor(Qt::PointingHandCursor);
        summary_->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
            "QPushButton#agentToolGroupDisclosure{background:transparent;border:none;"
            "color:#667085;font-size:12px;text-align:left;padding:3px 0;}"
            "QPushButton#agentToolGroupDisclosure:hover{color:#0b67b7;}")));
        column->addWidget(summary_);
        details_ = new QWidget(this);
        detailsLayout_ = new QVBoxLayout(details_);
        detailsLayout_->setContentsMargins(UiZoom::s(12), 0, 0, 0);
        detailsLayout_->setSpacing(UiZoom::s(2));
        details_->hide();
        column->addWidget(details_);
        QObject::connect(summary_, &QPushButton::clicked, this, [this] {
            autoExpanded_ = false;
            setExpanded(!details_->isVisible());
        });
    }

    void addCard(ToolCard* card) { detailsLayout_->addWidget(card); }

    void removeCard(ToolCard* card) {
        detailsLayout_->removeWidget(card);
        card->hide();
        card->deleteLater();
    }

    int cardCount() const { return detailsLayout_->count(); }

    void setExpanded(bool expanded) {
        details_->setVisible(expanded);
        refreshTitle();
    }

    void showApproval() {
        autoExpanded_ = true;
        setExpanded(true);
    }

    void setSummary(const MaiMessage& message, const QHash<QString, ToolCard*>& visibleCards) {
        int count = 0;
        bool running = false;
        bool pending = false;
        bool failed = false;
        bool canceled = false;
        for (const MaiMessagePart& part : message.parts) {
            const auto* tool = std::get_if<MaiToolPart>(&part.body);
            if (!tool || !visibleCards.contains(fromUtf8(part.id))) continue;
            ++count;
            running |= tool->state == MaiToolState::Running;
            pending |= tool->state == MaiToolState::Pending;
            failed |= tool->state == MaiToolState::Error;
            canceled |= tool->state == MaiToolState::Canceled;
        }
        const QString state = running ? QStringLiteral("运行中")
                              : pending ? QStringLiteral("等待授权")
                              : failed ? QStringLiteral("有失败")
                              : canceled ? QStringLiteral("部分取消")
                                         : QStringLiteral("已完成");
        title_ = QStringLiteral("工具调用 %1 次 · %2").arg(count).arg(state);
        if (!running && !pending && autoExpanded_) {
            autoExpanded_ = false;
            setExpanded(false);
        }
        refreshTitle();
    }

private:
    void refreshTitle() {
        summary_->setText(QStringLiteral("⌘  %1  %2")
                              .arg(title_, details_->isVisible() ? QStringLiteral("⌄") : QStringLiteral("›")));
    }

    QPushButton* summary_ = nullptr;
    QWidget* details_ = nullptr;
    QVBoxLayout* detailsLayout_ = nullptr;
    QString title_;
    bool autoExpanded_ = false;
};

struct AgentChatPanel::Runtime {
    AgentController* controller = nullptr;
    QString sessionId;

    QLabel* title = nullptr;
    QLabel* contextSize = nullptr;
    QPushButton* modelChip = nullptr;
    QPushButton* modelSection = nullptr;
    QPushButton* policySection = nullptr;
    QLabel* modelSectionValue = nullptr;
    QLabel* policySectionValue = nullptr;
    QLabel* workspaceSectionValue = nullptr;
    QPushButton* moreButton = nullptr;
    QPushButton* videoCircleButton = nullptr;
    QList<QPointer<QPushButton>> videoCirclePublishButtons;
    bool videoCircleAvailable = false;
    QList<QPointer<QPushButton>> cloudDriveSaveButtons;
    bool cloudDriveAvailable = false;
    QWidget* actionPanel = nullptr;
    QWidget* modelOptions = nullptr;
    QWidget* policyOptions = nullptr;
    QList<QPushButton*> modelOptionButtons;
    QList<QPushButton*> policyOptionButtons;
    MarkdownView* view = nullptr;
    QWidget* welcomeView = nullptr;
    PromptEdit* editor = nullptr;
    QWidget* composerHost = nullptr;
    ComposerResizeSplitter* composerSplitter = nullptr;
    int composerSizeBeforeMedia = 0;
    bool mediaAutoExpanded = false;
    QScrollArea* mediaTray = nullptr;
    QHBoxLayout* mediaTrayLayout = nullptr;
    QStringList visibleDraftMedia;
    QString visibleDraftSession;
    QSet<QString> unsentImportedPaths;
    QSet<QString> pendingImportDestinations;
    std::shared_ptr<std::atomic<bool>> importCanceled =
        std::make_shared<std::atomic<bool>>(false);
    QHash<QString, int> pendingMediaImports;
    int visiblePendingImports = -1;
    QPushButton* send = nullptr;
    QPushButton* hint = nullptr;
    QPushButton* chooseWorkspace = nullptr;
    QWidget* quoteStrip = nullptr;
    QLabel* quoteLabel = nullptr;
    QString quotedMessageId;
    QString quotedPreview;

    // partId -> 思考条 / 工具卡。这两样要能点，画不出来，所以还是部件，
    // 由 MarkdownView 负责摆位置和跟着滚。
    QHash<QString, QTextDocumentFragment> composerDrafts;
    ThinkingLine* pendingThinking = nullptr;
    QString pendingThinkingItemId;
    QHash<QString, ToolCard*> toolCards;
    QHash<QString, ToolGroupCard*> toolGroups;
    // 子会话 id -> 那张子任务卡。
    QHash<QString, SubAgentCard*> subAgentCards;

    // partId -> 已经攒到的 Markdown 原文。正文没有部件了，源在这儿。
    QHash<QString, QString> answers;
    QSet<QString> renderedUserMessages;
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

    // 顶栏只展示固定页标题和更多；模型与权限在更多面板内原位展开。
    auto* head = new QWidget(this);
    head->setObjectName(QStringLiteral("agentHeader"));
    head->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QWidget#agentHeader{background:#ffffff;border-bottom:1px solid %1;}").arg(kLine)));
    auto* headRow = new QHBoxLayout(head);
    head->setFixedHeight(UiZoom::s(54));
    headRow->setContentsMargins(UiZoom::s(16), UiZoom::s(8), UiZoom::s(16), UiZoom::s(8));
    headRow->setSpacing(UiZoom::s(8));
    auto* circle = new QPushButton(QStringLiteral("视频圈"), head);
    runtime_->videoCircleButton = circle;
    circle->setObjectName(QStringLiteral("agentVideoCircleOpen"));
    circle->setAccessibleName(QStringLiteral("打开 AI 视频圈"));
    circle->setCursor(Qt::PointingHandCursor);
    circle->setFixedSize(UiZoom::s(76), UiZoom::s(34));
    circle->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
        "QPushButton{background:#eef6fd;border:0;border-radius:9px;color:#0b67b7;font-weight:600;}"
        "QPushButton:hover{background:#dceefb;}")));
    circle->hide();
    connect(circle, &QPushButton::clicked, this, &AgentChatPanel::videoCircleRequested);
    headRow->addWidget(circle);
    runtime_->title = makeLabel(QStringLiteral("创作中心"), 18, kInk, true);
    runtime_->title->setObjectName(QStringLiteral("agentHeaderTitle"));
    runtime_->title->setAlignment(Qt::AlignCenter);
    runtime_->title->setWordWrap(false);
    headRow->addWidget(runtime_->title, 1);

    // 历史字数仍留在内部状态，不占用顶栏空间。
    runtime_->contextSize = makeLabel(QString(), 11, kInkFaint);
    runtime_->contextSize->setWordWrap(false);
    runtime_->contextSize->setParent(head);
    runtime_->contextSize->hide();
    auto* moreButton = new QPushButton(QStringLiteral("⋯"));
    runtime_->moreButton = moreButton;
    moreButton->setObjectName(QStringLiteral("agentMoreActions"));
    moreButton->setAccessibleName(QStringLiteral("更多"));
    moreButton->setCursor(Qt::PointingHandCursor);
    moreButton->setFixedSize(UiZoom::s(34), UiZoom::s(34));
    moreButton->setStyleSheet(
        UiZoom::scaleQss(QStringLiteral("QPushButton{background:#f5f7fa;border:none;border-radius:8px;"
                                        "color:%1;padding:5px 9px;}QPushButton:hover{background:#eef2f6;}")
                             .arg(kInkSoft)));
    headRow->addWidget(moreButton);
    auto* rightBalance = new QWidget(head);
    rightBalance->setFixedSize(UiZoom::s(34), UiZoom::s(34));
    headRow->addWidget(rightBalance);
    root->addWidget(head);

    // ---- 对话流：整片就是一个 MarkdownView ----
    // AI 页面使用窗口可用宽度，但正文和表格需要比普通消息列表更宽的呼吸区。
    // 48px 给正文和宽表格留出明确的页面边界，同时仍使用窗口剩余宽度。
    runtime_->view = new MarkdownView(this);
    MarkdownTheme conversationTheme = MarkdownTheme::standard(UiZoom::factor());
    conversationTheme.viewMargin = UiZoom::s(48);
    runtime_->view->setTheme(conversationTheme);
    auto* conversationSurface = new QWidget(this);
    conversationSurface->setObjectName(QStringLiteral("agentConversationSurface"));
    auto* conversationLayers = new QStackedLayout(conversationSurface);
    conversationLayers->setContentsMargins(0, 0, 0, 0);
    conversationLayers->setStackingMode(QStackedLayout::StackAll);
    conversationLayers->addWidget(runtime_->view);
    runtime_->welcomeView = makeWelcomeView(conversationSurface);
    conversationLayers->addWidget(runtime_->welcomeView);

    runtime_->flushTimer = new QTimer(this);
    runtime_->flushTimer->setSingleShot(true);
    runtime_->flushTimer->setInterval(90);
    connect(runtime_->flushTimer, &QTimer::timeout, this, [this] { flushAnswers(); });

    connect(runtime_->view, &MarkdownView::linkActivated, this,
            [](const QString& href) { QDesktopServices::openUrl(QUrl(href)); });

    // ---- 输入区：与普通 IM 共用同一个 ComposerTextEdit 形状 ----
    auto* composerHost = new QWidget(this);
    runtime_->composerHost = composerHost;
    composerHost->setObjectName(QStringLiteral("agentComposerPanel"));
    // 用户把分隔条拖到最低时仍保留一行可见文字；编辑器底部还要给发送按钮留一行。
    composerHost->setMinimumHeight(UiZoom::s(112));
    composerHost->setStyleSheet(QStringLiteral("QWidget#agentComposerPanel{background:#ffffff;}"));
    auto* composerLayout = new QVBoxLayout(composerHost);
    composerLayout->setContentsMargins(UiZoom::s(12), UiZoom::s(2), UiZoom::s(12), UiZoom::s(8));
    composerLayout->setSpacing(0);

    runtime_->quoteStrip = new QWidget(composerHost);
    runtime_->quoteStrip->setObjectName(QStringLiteral("agentQuoteStrip"));
    runtime_->quoteStrip->setStyleSheet(QStringLiteral(
        "QWidget#agentQuoteStrip{background:#e8f4fc;border-radius:8px;}"));
    auto* quoteLayout = new QHBoxLayout(runtime_->quoteStrip);
    quoteLayout->setContentsMargins(UiZoom::s(12), UiZoom::s(6), UiZoom::s(8), UiZoom::s(6));
    runtime_->quoteLabel = makeLabel(QString(), 12, kInkSoft);
    runtime_->quoteLabel->setWordWrap(false);
    quoteLayout->addWidget(runtime_->quoteLabel, 1);
    auto* cancelQuote = new QPushButton(QStringLiteral("×"), runtime_->quoteStrip);
    cancelQuote->setObjectName(QStringLiteral("agentCancelQuoteButton"));
    cancelQuote->setAccessibleName(QStringLiteral("取消引用"));
    cancelQuote->setFixedSize(UiZoom::s(28), UiZoom::s(28));
    cancelQuote->setStyleSheet(QStringLiteral("QPushButton{border:0;background:transparent;color:#667085;}"));
    connect(cancelQuote, &QPushButton::clicked, this, &AgentChatPanel::clearQuote);
    quoteLayout->addWidget(cancelQuote);
    runtime_->quoteStrip->hide();
    composerLayout->addWidget(runtime_->quoteStrip);

    runtime_->mediaTray = new QScrollArea(composerHost);
    runtime_->mediaTray->setObjectName(QStringLiteral("agentMediaDraftTray"));
    runtime_->mediaTray->setFixedHeight(UiZoom::s(106));
    runtime_->mediaTray->setFrameShape(QFrame::NoFrame);
    runtime_->mediaTray->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    runtime_->mediaTray->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    runtime_->mediaTray->setWidgetResizable(true);
    auto* trayContent = new QWidget(runtime_->mediaTray);
    runtime_->mediaTrayLayout = new QHBoxLayout(trayContent);
    runtime_->mediaTrayLayout->setContentsMargins(UiZoom::s(12), UiZoom::s(5),
                                                   UiZoom::s(12), UiZoom::s(5));
    runtime_->mediaTrayLayout->setSpacing(UiZoom::s(8));
    runtime_->mediaTray->setWidget(trayContent);
    runtime_->mediaTray->hide();
    composerLayout->addWidget(runtime_->mediaTray);

    runtime_->editor = new PromptEdit(composerHost);
    runtime_->editor->setObjectName(QStringLiteral("agentPromptEditor"));
    runtime_->editor->setPlaceholderText(QStringLiteral("交给它做点什么…"));
    runtime_->editor->setMinimumHeight(UiZoom::s(96));
    runtime_->editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    runtime_->editor->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QTextEdit#agentPromptEditor {
            border:1px solid %1;
            border-radius:14px;
            background:#ffffff;
            color:%2;
            padding:10px 52px 10px 13px;
        }
        QTextEdit#agentPromptEditor:focus { border-color:#b5bbc4; }
    )")
                                                         .arg(QStringLiteral("#d1d5db"), kInk)));
    QFont editorFont = runtime_->editor->font();
    editorFont.setPixelSize(UiZoom::s(14));
    runtime_->editor->setFont(editorFont);
    runtime_->editor->onSubmit = [this] { onSend(); };
    runtime_->editor->setMimeHandler([this](const QMimeData* mime) { return insertComposerMimeData(mime); });
    connect(runtime_->editor, &QTextEdit::textChanged, this, &AgentChatPanel::refreshComposerMediaTray);
    runtime_->editor->onOpenVideo = [this](const QString& path) {
        if (openVideo_ && QFileInfo(path).isFile()) openVideo_(path, composerVideoPaths());
    };
    auto* attachMedia = new QPushButton(QStringLiteral("+"), runtime_->editor);
    attachMedia->setObjectName(QStringLiteral("agentAttachMediaButton"));
    attachMedia->setAccessibleName(QStringLiteral("添加媒体"));
    attachMedia->setToolTip(QStringLiteral("添加媒体"));
    attachMedia->setCursor(Qt::PointingHandCursor);
    attachMedia->setFixedSize(UiZoom::s(36), UiZoom::s(36));
    attachMedia->setStyleSheet(QStringLiteral(
        "QPushButton{border:0;border-radius:18px;background:#eef6ff;color:#0b67b7;font-size:24px;}"
        "QPushButton:hover{background:#dceeff;}"
        "QPushButton::menu-indicator{image:none;width:0px;}"));
    runtime_->editor->setLeadingAction(attachMedia);
    auto* attachMenu = new QMenu(attachMedia);
    applyAgentMenuStyle(attachMenu);
    auto* fromFile = attachMenu->addAction(QStringLiteral("选择图片或视频"));
    connect(fromFile, &QAction::triggered, this, [this] {
        const QStringList paths = QFileDialog::getOpenFileNames(
            this, QStringLiteral("添加图片或视频"),
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
            QStringLiteral("媒体文件 (*.png *.jpg *.jpeg *.webp *.gif *.mp4 *.mov *.webm)"));
        for (const QString& path : paths) {
            if (!videoMimeType(path).isEmpty())
                insertComposerVideoFile(path);
            else insertComposerImageFile(path);
        }
    });
    auto* takePhoto = attachMenu->addAction(QStringLiteral("打开摄像头拍照"));
    connect(takePhoto, &QAction::triggered, this, [this] {
        MaiSession session;
        if (!runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), session) ||
            session.directory.empty()) {
            QMessageBox::warning(this, QStringLiteral("拍照失败"),
                                 QStringLiteral("当前会话没有可写工作目录"));
            return;
        }
        QString error;
        const QString image = DesktopCameraDialog::capturePhoto(
            this, fromUtf8(session.directory), &error);
        if (!image.isEmpty()) insertComposerImageFile(image);
        else if (!error.isEmpty()) QMessageBox::warning(this, QStringLiteral("拍照失败"), error);
    });
    auto* playMusic = attachMenu->addAction(QStringLiteral("播放本地音乐"));
    connect(playMusic, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择本地音乐"),
            QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
            QStringLiteral("音频文件 (*.mp3 *.m4a *.wav)"));
        if (path.isEmpty()) return;
        QString error;
        if (!DesktopMusicPlayerDialog::openFile(this, path, &error))
            QMessageBox::warning(this, QStringLiteral("播放失败"), error);
    });
    attachMedia->setMenu(attachMenu);
    composerLayout->addWidget(runtime_->editor, 1);

    runtime_->modelChip = new QPushButton(head);
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
    runtime_->modelChip->hide();
    // 这句常驻。它是这套东西最重要的一句承诺，写在文档里没人看。
    runtime_->hint = new QPushButton(head);
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
    auto addPolicy = [policyMenu](const QString& title, const QString& detail, MaiApprovalPolicy policy) {
        QAction* action = policyMenu->addAction(title);
        action->setData(static_cast<int>(policy));
        action->setCheckable(true);
        action->setToolTip(detail);
    };
    addPolicy(QStringLiteral("请求批准"), QStringLiteral("文件变更、访问网络或执行高风险命令时询问；可单独授权文件"),
              MaiApprovalPolicy::OnRequest);
    addPolicy(QStringLiteral("帮我批准"),
              QStringLiteral("每个文件首次变更先询问，此后本会话免问；网络和高风险命令仍询问"),
              MaiApprovalPolicy::UnlessTrusted);
    addPolicy(QStringLiteral("完全访问"), QStringLiteral("一般操作不询问；付费操作仍会确认"),
              MaiApprovalPolicy::Never);
    runtime_->hint->setMenu(policyMenu);
    connect(policyMenu, &QMenu::triggered, this,
            [this](QAction* action) { selectApprovalPolicy(static_cast<MaiApprovalPolicy>(action->data().toInt())); });
    updateApprovalPolicyUi();
    runtime_->hint->hide();

    // 与 iOS 操作面板同层展开：顶栏只保留标题和更多，模型/权限不另开设置弹窗。
    auto* actionPanel = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
    runtime_->actionPanel = actionPanel;
    actionPanel->setObjectName(QStringLiteral("agentActionPanel"));
    actionPanel->setFixedWidth(UiZoom::s(288));
    actionPanel->setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QFrame#agentActionPanel { background:#ffffff; border:1px solid #e2e8f0; border-radius:12px; }
        QFrame#agentActionPanel QPushButton { background:transparent; border:0; color:#24334a;
                                             text-align:left; padding:0 12px; font-size:13px;
                                             border-radius:8px; }
        QFrame#agentActionPanel QPushButton:hover { background:#f1f6fb; }
        QFrame#agentActionPanel QPushButton:focus { border:1px solid #82b8e7; }
        QFrame#agentActionPanel QPushButton#agentEffortSection,
        QFrame#agentActionPanel QPushButton#agentPolicySection,
        QFrame#agentActionPanel QPushButton#agentChooseWorkspaceButton { padding:0; }
        QFrame#agentActionPanel QPushButton#agentChooseWorkspaceButton {
            color:#24334a;
        }
        QFrame#agentActionPanel QPushButton#agentChooseWorkspaceButton:hover {
            background:#f2f7fc;
        }
        QFrame#agentActionPanel QPushButton#agentOpenWorkspaceButton {
            color:#53627a; text-align:center; padding:0; font-size:17px;
        }
        QFrame#agentActionPanel QPushButton:checked {
            background:#e6f2ff; color:#0a65ad; font-weight:600;
        }
        QFrame#agentActionPanel QPushButton#agentClearConversation { color:#c0362c; }
        QFrame#agentActionPanel QPushButton#agentClearConversation:hover { background:#fff1f0; }
        QPushButton:disabled { color:#98a2b3; }
    )")));
    auto* actionLayout = new QVBoxLayout(actionPanel);
    actionLayout->setContentsMargins(UiZoom::s(8), UiZoom::s(8), UiZoom::s(8), UiZoom::s(8));
    actionLayout->setSpacing(UiZoom::s(2));
    auto makeSummary = [actionPanel](const QString& name, const QString& title,
                                     const QString& initial, QLabel** currentValue) {
        auto* row = new QPushButton(actionPanel);
        row->setObjectName(name);
        row->setAccessibleName(title + QStringLiteral("：") + initial);
        row->setCursor(Qt::PointingHandCursor);
        row->setFixedHeight(UiZoom::s(54));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(UiZoom::s(12), 0, UiZoom::s(12), 0);
        layout->setSpacing(UiZoom::s(10));
        auto* textColumn = new QVBoxLayout;
        textColumn->setContentsMargins(0, 0, 0, 0);
        textColumn->setSpacing(UiZoom::s(2));
        auto* label = new QLabel(title, row);
        label->setStyleSheet(UiZoom::scaleQss(
            QStringLiteral("QLabel{background:transparent;border:0;color:#24334a;font-size:13px;font-weight:600;}")));
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        textColumn->addWidget(label);
        auto* value = new QLabel(initial, row);
        value->setStyleSheet(UiZoom::scaleQss(
            QStringLiteral("QLabel{background:transparent;border:0;color:#738198;font-size:11px;}")));
        value->setAttribute(Qt::WA_TransparentForMouseEvents);
        textColumn->addWidget(value);
        layout->addLayout(textColumn, 1);
        auto* arrow = new QLabel(QStringLiteral("›"), row);
        arrow->setStyleSheet(UiZoom::scaleQss(
            QStringLiteral("QLabel{background:transparent;border:0;color:#98a2b3;font-size:18px;}")));
        arrow->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(arrow);
        *currentValue = value;
        return row;
    };
    auto* modelSection = makeSummary(QStringLiteral("agentEffortSection"),
                                     QStringLiteral("思考强度"), QStringLiteral("标准"),
                                     &runtime_->modelSectionValue);
    runtime_->modelSection = modelSection;
    actionLayout->addWidget(modelSection);
    auto* modelOptions = new QWidget(actionPanel);
    runtime_->modelOptions = modelOptions;
    modelOptions->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("background:#f5f8fc;border-radius:8px;")));
    auto* modelOptionsLayout = new QVBoxLayout(modelOptions);
    modelOptionsLayout->setContentsMargins(0, UiZoom::s(4), 0, UiZoom::s(4));
    modelOptionsLayout->setSpacing(0);
    const QList<QPair<QString, QString>> efforts = {
        {QStringLiteral("简单"), QStringLiteral("low")},
        {QStringLiteral("标准"), QStringLiteral("high")},
        {QStringLiteral("深度"), QStringLiteral("max")}};
    for (const auto& effort : efforts) {
        auto* option = new QPushButton(effort.first, modelOptions);
        option->setObjectName(QStringLiteral("agentEffortOption-") + effort.second);
        option->setProperty("reasoningEffort", effort.second);
        option->setCheckable(true);
        option->setAutoExclusive(true);
        option->setFixedHeight(UiZoom::s(38));
        option->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
            "QPushButton{padding-left:20px;text-align:left;background:#f5f8fc;}"
            "QPushButton:hover{background:#e8f2fc;}"
            "QPushButton:checked{background:#e6f2ff;color:#0a65ad;font-weight:600;}")));
        modelOptionsLayout->addWidget(option);
        runtime_->modelOptionButtons.append(option);
        connect(option, &QPushButton::clicked, this, [this, actionPanel, effort] {
            actionPanel->hide();
            emit reasoningEffortChangeRequested(effort.second);
        });
    }
    actionLayout->addWidget(modelOptions);
    modelOptions->hide();
    auto* effortDivider = new QFrame(actionPanel);
    effortDivider->setFrameShape(QFrame::HLine);
    effortDivider->setStyleSheet(QStringLiteral("color:#edf1f5;"));
    actionLayout->addWidget(effortDivider);

    auto* policySection = makeSummary(QStringLiteral("agentPolicySection"),
                                      QStringLiteral("操作权限"), QStringLiteral("请求批准"),
                                      &runtime_->policySectionValue);
    runtime_->policySection = policySection;
    actionLayout->addWidget(policySection);
    auto* policyOptions = new QWidget(actionPanel);
    runtime_->policyOptions = policyOptions;
    policyOptions->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("background:#f5f8fc;border-radius:8px;")));
    auto* policyOptionsLayout = new QVBoxLayout(policyOptions);
    policyOptionsLayout->setContentsMargins(0, UiZoom::s(4), 0, UiZoom::s(4));
    policyOptionsLayout->setSpacing(0);
    const QList<QPair<QString, MaiApprovalPolicy>> policies = {
        {QStringLiteral("请求批准"), MaiApprovalPolicy::OnRequest},
        {QStringLiteral("帮我批准"), MaiApprovalPolicy::UnlessTrusted},
        {QStringLiteral("完全访问"), MaiApprovalPolicy::Never}};
    for (const auto& choice : policies) {
        auto* option = new QPushButton(choice.first, policyOptions);
        option->setObjectName(QStringLiteral("agentPolicyOption-") + choice.first);
        option->setProperty("policyValue", static_cast<int>(choice.second));
        option->setCheckable(true);
        option->setAutoExclusive(true);
        option->setFixedHeight(UiZoom::s(38));
        option->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
            "QPushButton{padding-left:20px;text-align:left;background:#f5f8fc;}"
            "QPushButton:hover{background:#e8f2fc;}"
            "QPushButton:checked{background:#e6f2ff;color:#0a65ad;font-weight:600;}")));
        policyOptionsLayout->addWidget(option);
        runtime_->policyOptionButtons.append(option);
        connect(option, &QPushButton::clicked, this, [this, actionPanel, choice] {
            actionPanel->hide();
            selectApprovalPolicy(choice.second);
        });
    }
    actionLayout->addWidget(policyOptions);
    policyOptions->hide();
    auto* sectionDivider = new QFrame(actionPanel);
    sectionDivider->setFrameShape(QFrame::HLine);
    sectionDivider->setStyleSheet(QStringLiteral("color:#edf1f5;"));
    actionLayout->addWidget(sectionDivider);
    auto* workspaceRow = new QWidget(actionPanel);
    auto* workspaceLayout = new QHBoxLayout(workspaceRow);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(UiZoom::s(2));
    workspaceRow->setFixedHeight(UiZoom::s(54));
    actionLayout->addWidget(workspaceRow);
    auto* divider = new QFrame(actionPanel);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color:#edf1f5;"));
    actionLayout->addWidget(divider);
    auto* clearButton = new QPushButton(QStringLiteral("清空当前对话"), actionPanel);
    clearButton->setObjectName(QStringLiteral("agentClearConversation"));
    clearButton->setFixedHeight(UiZoom::s(42));
    actionLayout->addWidget(clearButton);
    connect(clearButton, &QPushButton::clicked, this, [this, actionPanel] {
        actionPanel->hide();
        onClear();
    });
    const auto positionPopup = [actionPanel, moreButton] {
        actionPanel->adjustSize();
        QPoint position = moreButton->mapToGlobal(
            QPoint(moreButton->width() - actionPanel->width(),
                   moreButton->height() + UiZoom::s(6)));
        QScreen* screen = QGuiApplication::screenAt(position);
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect available = screen->availableGeometry();
            position.setX(qBound(available.left(), position.x(),
                                 available.right() - actionPanel->width() + 1));
            if (position.y() + actionPanel->height() > available.bottom())
                position.setY(moreButton->mapToGlobal(QPoint(0, 0)).y() -
                              actionPanel->height() - UiZoom::s(6));
        }
        actionPanel->move(position);
    };
    connect(modelSection, &QPushButton::clicked, this,
            [modelOptions, policyOptions, positionPopup] {
        const bool expand = !modelOptions->isVisible();
        policyOptions->hide();
        modelOptions->setVisible(expand);
        positionPopup();
    });
    connect(policySection, &QPushButton::clicked, this,
            [modelOptions, policyOptions, positionPopup] {
        const bool expand = !policyOptions->isVisible();
        modelOptions->hide();
        policyOptions->setVisible(expand);
        positionPopup();
    });
    connect(moreButton, &QPushButton::clicked, this,
            [actionPanel, positionPopup] {
        if (actionPanel->isVisible()) actionPanel->hide();
        else {
            positionPopup();
            actionPanel->show();
            actionPanel->raise();
        }
    });
    updateApprovalPolicyUi();
    runtime_->send = new QPushButton(runtime_->editor);
    runtime_->send->setObjectName(QStringLiteral("agentSendButton"));
    runtime_->send->setCursor(Qt::PointingHandCursor);
    runtime_->send->setFixedSize(UiZoom::s(36), UiZoom::s(36));
    runtime_->send->setIconSize(QSize(UiZoom::s(18), UiZoom::s(18)));
    runtime_->editor->setCornerAction(runtime_->send);

    runtime_->chooseWorkspace = new QPushButton(workspaceRow);
    runtime_->chooseWorkspace->setObjectName(QStringLiteral("agentChooseWorkspaceButton"));
    runtime_->chooseWorkspace->setAccessibleName(QStringLiteral("切换当前会话工作目录"));
    runtime_->chooseWorkspace->setCursor(Qt::PointingHandCursor);
    runtime_->chooseWorkspace->setFixedHeight(UiZoom::s(54));
    runtime_->chooseWorkspace->setToolTip(QStringLiteral("切换当前会话工作目录"));
    auto* workspaceButtonLayout = new QHBoxLayout(runtime_->chooseWorkspace);
    workspaceButtonLayout->setContentsMargins(UiZoom::s(12), 0, 0, 0);
    workspaceButtonLayout->setSpacing(UiZoom::s(10));
    auto* folderIcon = new QLabel(runtime_->chooseWorkspace);
    folderIcon->setPixmap(style()->standardIcon(QStyle::SP_DirIcon).pixmap(
        UiZoom::s(18), UiZoom::s(18)));
    folderIcon->setStyleSheet(QStringLiteral("background:transparent;border:0;"));
    folderIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
    workspaceButtonLayout->addWidget(folderIcon);
    auto* workspaceText = new QVBoxLayout;
    workspaceText->setContentsMargins(0, 0, 0, 0);
    workspaceText->setSpacing(UiZoom::s(2));
    auto* workspaceTitle = new QLabel(QStringLiteral("工作目录"), runtime_->chooseWorkspace);
    workspaceTitle->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
        "QLabel{background:transparent;border:0;color:#24334a;font-size:13px;font-weight:600;}")));
    workspaceTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    workspaceText->addWidget(workspaceTitle);
    runtime_->workspaceSectionValue = new QLabel(QStringLiteral("未选择"), runtime_->chooseWorkspace);
    runtime_->workspaceSectionValue->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
        "QLabel{background:transparent;border:0;color:#738198;font-size:11px;}")));
    runtime_->workspaceSectionValue->setAttribute(Qt::WA_TransparentForMouseEvents);
    workspaceText->addWidget(runtime_->workspaceSectionValue);
    workspaceButtonLayout->addLayout(workspaceText, 1);
    workspaceLayout->addWidget(runtime_->chooseWorkspace, 1);
    connect(runtime_->chooseWorkspace, &QPushButton::clicked, this, [this] {
        runtime_->actionPanel->hide();
        MaiSession session;
        QString initial = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), session))
            initial = fromUtf8(session.directory);
        const QString selected = QFileDialog::getExistingDirectory(this, QStringLiteral("选择创作中心工作目录"),
                                                                   initial, QFileDialog::ShowDirsOnly);
        if (!selected.isEmpty()) openSessionInDirectory(selected);
    });
    auto* openWorkspace = new QPushButton(QStringLiteral("↗"), workspaceRow);
    openWorkspace->setObjectName(QStringLiteral("agentOpenWorkspaceButton"));
    openWorkspace->setCursor(Qt::PointingHandCursor);
    openWorkspace->setFixedSize(UiZoom::s(36), UiZoom::s(36));
    openWorkspace->setAccessibleName(QStringLiteral("打开当前工作目录"));
    openWorkspace->setToolTip(QStringLiteral("打开当前工作目录"));
    workspaceLayout->addWidget(openWorkspace);
    connect(openWorkspace, &QPushButton::clicked, this, [this] {
        runtime_->actionPanel->hide();
        MaiSession session;
        if (!runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), session)) return;
        QDesktopServices::openUrl(QUrl::fromLocalFile(fromUtf8(session.directory)));
    });

    auto* messageComposerSplitter = new ComposerResizeSplitter(this);
    runtime_->composerSplitter = messageComposerSplitter;
    messageComposerSplitter->setObjectName(QStringLiteral("agentMessageComposerSplitter"));
    messageComposerSplitter->setChildrenCollapsible(false);
    messageComposerSplitter->setStyleSheet(
        QStringLiteral("QSplitter::handle{background:transparent;}"
                       "QSplitter::handle:vertical{height:1px;}"
                       "QSplitter::handle:hover{background:transparent;}"));
    messageComposerSplitter->addWidget(conversationSurface);
    messageComposerSplitter->addWidget(composerHost);
    messageComposerSplitter->setStretchFactor(0, 1);
    messageComposerSplitter->setStretchFactor(1, 0);
    messageComposerSplitter->setSizes(QList<int>() << UiZoom::s(620) << UiZoom::s(140));
    root->addWidget(messageComposerSplitter, 1);

    setRunning(false);
    connect(runtime_->send, &QPushButton::clicked, this, &AgentChatPanel::onSend);

    // ---- 接事件 ----
    // 这些信号全部已经在主线程上了（AgentController
    // 负责把它们从核心的线程搬过来）， 所以这里查存储、改部件都随意。
    // **每一条都要先认会话。**
    //
    // 子 Agent 是并发跑在另一个会话里的。不认的话它的正文会接进父的回答里、
    // 它跑完会把发送按钮从「停止」翻回「发送」、它的标题会顶掉头部那一行——
    // 而用户看到的是「AI 自己说了些我没问的话」。
    //
    // 以前只有一个会话在跑，忽略 sessionId 是无害的；现在不是了。
    connect(&controller, &AgentController::textDelta, this,
            [this](const QString& sessionId, const QString&, const QString& partId, const QString& delta) {
                if (!isCurrentSession(sessionId)) {
                    noteOtherSession(sessionId, delta, partId);
                    return;
                }
                clearPendingThinking();
                appendAnswerDelta(partId, delta);
            });
    connect(&controller, &AgentController::userMessageAdded, this,
            [this](const QString& sessionId, const QString& messageId) {
                if (!isCurrentSession(sessionId) || runtime_->renderedUserMessages.contains(messageId)) return;
                MaiSession session;
                if (!runtime_->controller->agent().getSession(toUtf8(sessionId), session)) return;
                for (const MaiMessage& message : runtime_->controller->agent().listMessagesPage(
                         toUtf8(sessionId), toUtf8(messageId) + "~", 1)) {
                    if (fromUtf8(message.id) != messageId) return;
                    appendStoredUserMessage(message, session);
                    scrollToBottom();
                    return;
                }
            });
    connect(&controller, &AgentController::toolPartChanged, this,
            [this](const QString& sessionId, const QString& messageId, const QString& partId) {
                if (!isCurrentSession(sessionId)) {
                    if (!runtime_->controller->isChildOf(sessionId, runtime_->sessionId)) return;
                    ToolCard* card = runtime_->toolCards.value(partId, nullptr);
                    ToolGroupCard* group = runtime_->toolGroups.value(messageId, nullptr);
                    if (!card || !group) return;
                    for (const MaiMessage& message : runtime_->controller->agent().listMessages(toUtf8(sessionId))) {
                        if (fromUtf8(message.id) != messageId) continue;
                        for (const MaiMessagePart& part : message.parts) {
                            if (fromUtf8(part.id) != partId) continue;
                            const auto* tool = std::get_if<MaiToolPart>(&part.body);
                            if (!tool) return;
                            card->apply(tool->state, card->isWaitingForUser() &&
                                                     tool->state == MaiToolState::Pending);
                            card->setDetail(tool->state == MaiToolState::Error ? fromUtf8(tool->error)
                                                                               : fromUtf8(tool->output));
                            group->setSummary(message, runtime_->toolCards);
                            return;
                        }
                    }
                    return;
                }
                clearPendingThinking();
                refreshToolCard(messageId, partId);
            });
    connect(&controller, &AgentController::permissionAsked, this,
            [this](const QString& permissionId, const QString& sessionId) {
                // 当前会话和它的子 Agent 可以在这里审批；其它根会话的审批留在
                // 它自己的会话中，切回去时从核心待审批列表恢复。
                (void)sessionId;
                showApproval(permissionId);
            });
    connect(&controller, &AgentController::questionAsked, this,
            [this](const QString& questionId, const QString&, const QString&) {
                // 同授权：子 Agent 问的话也得有人答。
                showQuestion(questionId);
            });
    connect(&controller, &AgentController::questionAnswered, this, [this](const QString& questionId, const QString&) {
        if (runtime_->pendingQuestionId == questionId) clearQuestion();
    });
    connect(&controller, &AgentController::turnFinished, this, [this](const QString& sessionId) {
        if (!isCurrentSession(sessionId)) {
            noteOtherSession(sessionId, QString());
            return;
        }
        clearPendingThinking();
        setRunning(false);
        emit sessionListChanged();
        flushAnswers();
        MaiSession current;
        if (runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), current)) {
            const auto messages = runtime_->controller->agent().listMessages(toUtf8(runtime_->sessionId));
            for (const MaiMessage& message : messages)
                if (message.role == MaiRole::Assistant && message.completed != 0)
                    appendQuoteAction(fromUtf8(message.id));
            for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
                if (it->role == MaiRole::Assistant && it->completed != 0) {
                    appendTextMediaCards(fromUtf8(it->id), fromUtf8(it->text()), fromUtf8(current.directory));
                    break;
                }
            }
        }
        refreshContextSize();
        scrollToBottom();
    });
    connect(&controller, &AgentController::turnFailed, this, [this](const QString& sessionId, const QString& message) {
        if (!isCurrentSession(sessionId)) {
            noteOtherSession(sessionId, message);
            return;
        }
        clearPendingThinking();
        setRunning(false);
        flushAnswers();
        appendNotice(message, true);
    });
    connect(&controller, &AgentController::sessionTitleChanged, this,
            [this](const QString&, const QString&) {
                // 列表展示所有会话。后台会话改名也要刷新，不能因用户已经切走而遗漏；
                // 当前页的标题和消息仍由各自的会话过滤处理。
                emit sessionListChanged();
            });
    connect(&controller, &AgentController::sessionStructureChanged, this, [this] {
        if (!runtime_->sessionId.isEmpty()) {
            MaiSession current;
            if (!runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), current)) {
                for (const MaiSession& session : runtime_->controller->agent().listSessions()) {
                    if (!session.isRoot()) continue;
                    openSession(fromUtf8(session.id));
                    return;
                }
                openSession();
                return;
            }
        }
        emit sessionListChanged();
    });
}

AgentChatPanel::~AgentChatPanel() {
    runtime_->importCanceled->store(true);
    for (const QString& path : runtime_->pendingImportDestinations)
        QtConcurrent::run([path] { QDir(QFileInfo(path).absolutePath()).removeRecursively(); });
    for (const QString& path : runtime_->unsentImportedPaths) {
        QFile::remove(path);
        QDir().rmdir(QFileInfo(path).absolutePath());
    }
}

bool AgentChatPanel::isCurrentSession(const QString& sessionId) const {
    return sessionId == runtime_->sessionId;
}

QString AgentChatPanel::sessionId() const {
    return runtime_->sessionId;
}

void AgentChatPanel::setModelLabel(const QString& model, bool configured) {
    runtime_->modelChip->setText(model);
    runtime_->modelConfigured = configured &&
        model != QStringLiteral("未配置模型") && !model.trimmed().isEmpty();
    runtime_->modelChip->setEnabled(runtime_->modelConfigured && !runtime_->running);
}

void AgentChatPanel::setReasoningEffortLabel(const QString& effort) {
    const QString normalized = effort == QStringLiteral("low") || effort == QStringLiteral("max")
                                   ? effort : QStringLiteral("high");
    const QString title = normalized == QStringLiteral("low") ? QStringLiteral("简单") :
        normalized == QStringLiteral("max") ? QStringLiteral("深度") : QStringLiteral("标准");
    if (runtime_->modelSectionValue) runtime_->modelSectionValue->setText(title);
    if (runtime_->modelSection)
        runtime_->modelSection->setAccessibleName(QStringLiteral("思考强度：%1").arg(title));
    for (QPushButton* option : runtime_->modelOptionButtons) {
        const QString value = option->property("reasoningEffort").toString();
        option->setChecked(value == normalized);
    }
}

bool AgentChatPanel::applySelectedModel(const QString& model) {
    const QString selected = model.trimmed();
    const bool applied = runtime_->sessionId.isEmpty() ?
        runtime_->controller->setDefaultModel(selected) :
        runtime_->controller->setModel(runtime_->sessionId, selected);
    if (!applied) {
        appendNotice(runtime_->controller->lastError(), true);
        return false;
    }
    setModelLabel(selected);
    return true;
}

void AgentChatPanel::setOpenVideoCallback(std::function<void(const QString&, const QStringList&)> callback) {
    openVideo_ = std::move(callback);
}

QStringList AgentChatPanel::currentVideoPaths() const {
    MaiSession session;
    if (!runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), session)) return {};
    const QString workspace = fromUtf8(session.directory);
    QStringList paths;
    for (const MaiMessage& message : runtime_->controller->agent().listMessages(toUtf8(runtime_->sessionId))) {
        if (message.role != MaiRole::Assistant) continue;
        for (const MaiMessagePart& part : message.parts) {
            const auto* tool = std::get_if<MaiToolPart>(&part.body);
            if (!tool || tool->tool != "agent_send_media" || tool->state != MaiToolState::Completed) continue;
            const QJsonObject artifact = QJsonDocument::fromJson(fromUtf8(tool->output).toUtf8()).object();
            if (artifact.value(QStringLiteral("delivery")).toString() != QStringLiteral("current_ai_session") ||
                artifact.value(QStringLiteral("type")).toString() != QStringLiteral("video")) continue;
            const QString path = fromUtf8(maiResolvePathWithinRoot(
                toUtf8(workspace), toUtf8(artifact.value(QStringLiteral("path")).toString())));
            if (QFileInfo(path).isFile() && !paths.contains(path)) paths.append(path);
        }
        for (const LocalMediaMention& mention : localMediaMentions(fromUtf8(message.text()), workspace)) {
            if (mention.video && QFileInfo(mention.path).isFile() && !paths.contains(mention.path))
                paths.append(mention.path);
        }
    }
    return paths;
}

void AgentChatPanel::setForwardMediaCallback(std::function<void(const QString&, const QString&)> callback) {
    forwardMedia_ = std::move(callback);
}

void AgentChatPanel::setVideoCircleAvailable(bool available) {
    runtime_->videoCircleAvailable = available;
    runtime_->videoCircleButton->setVisible(available);
    for (auto it = runtime_->videoCirclePublishButtons.begin();
         it != runtime_->videoCirclePublishButtons.end();) {
        if (!*it) {
            it = runtime_->videoCirclePublishButtons.erase(it);
        } else {
            (*it)->setVisible(available);
            ++it;
        }
    }
}

void AgentChatPanel::setCloudDriveAvailable(bool available) {
    runtime_->cloudDriveAvailable = available;
    for (auto it = runtime_->cloudDriveSaveButtons.begin();
         it != runtime_->cloudDriveSaveButtons.end();) {
        if (!*it) {
            it = runtime_->cloudDriveSaveButtons.erase(it);
        } else {
            (*it)->setVisible(available);
            ++it;
        }
    }
}

void AgentChatPanel::selectApprovalPolicy(MaiApprovalPolicy policy) {
    if (!runtime_->controller->setApprovalPolicy(policy)) {
        appendNotice(runtime_->controller->lastError(), true);
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("agent/approvalPolicy"),
                      policy == MaiApprovalPolicy::Never           ? QStringLiteral("never")
                      : policy == MaiApprovalPolicy::UnlessTrusted ? QStringLiteral("unless_trusted")
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
        runtime_->hint->setToolTip(QStringLiteral("一般操作不逐次询问；付费操作仍会确认。"));
    } else if (policy == MaiApprovalPolicy::UnlessTrusted) {
        runtime_->hint->setText(QStringLiteral("帮我批准"));
        runtime_->hint->setToolTip(
            QStringLiteral("每个文件首次变更先询问，此后本会话免问；网络和高风险命令仍会询问。"));
    } else {
        runtime_->hint->setText(QStringLiteral("请求批准"));
        runtime_->hint->setToolTip(QStringLiteral("文件变更、访问网络或执行高风险命令前会询问；可单独授权文件。"));
    }
    if (runtime_->policySectionValue)
        runtime_->policySectionValue->setText(runtime_->hint->text());
    if (runtime_->policySection)
        runtime_->policySection->setAccessibleName(
            QStringLiteral("操作权限：%1").arg(runtime_->hint->text()));
    for (QPushButton* option : runtime_->policyOptionButtons) {
        const bool selected = option->property("policyValue").toInt() ==
                              static_cast<int>(policy);
        option->setChecked(selected);
    }
}

void AgentChatPanel::openSession(const QString& sessionId) {
    if (sessionId.isEmpty()) {
        const QString directory = defaultWorkspaceDirectory(
            defaultWorkspaceDirectory_, defaultWorkspaceConfigured_);
        if (directory.isEmpty()) {
            appendNotice(QStringLiteral("无法创建创作中心工作目录"), true);
            return;
        }
        createSessionInDirectory(directory);
        return;
    }
    const QString nextSession = sessionId;
    if (runtime_->sessionId != nextSession) {
        clearQuote();
        if (!runtime_->sessionId.isEmpty()) {
            if (runtime_->editor->document()->isEmpty())
                runtime_->composerDrafts.remove(runtime_->sessionId);
            else
                runtime_->composerDrafts.insert(runtime_->sessionId,
                                                QTextDocumentFragment(runtime_->editor->document()));
        }
        runtime_->sessionId = nextSession;
        runtime_->editor->clear();
        if (runtime_->composerDrafts.contains(nextSession)) {
            QTextCursor cursor = runtime_->editor->textCursor();
            cursor.insertFragment(runtime_->composerDrafts.take(nextSession));
            cursor.movePosition(QTextCursor::End);
            runtime_->editor->setTextCursor(cursor);
        }
    }

    MaiSession session;
    if (runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), session)) {
        // 用 isUntitled() 而不是 title.isEmpty()：核心给新会话填的是一个占位标题
        // （"New session"），不是空串。判空的话那串占位符会直接显示在头部。
        runtime_->title->setText(QStringLiteral("创作中心"));
        const QString directory = QDir::toNativeSeparators(fromUtf8(session.directory));
        QString name = QFileInfo(directory).fileName();
        if (name.isEmpty()) name = directory;
        runtime_->workspaceSectionValue->setText(
            runtime_->workspaceSectionValue->fontMetrics().elidedText(
                name, Qt::ElideMiddle, runtime_->actionPanel->width() - UiZoom::s(128)));
        runtime_->chooseWorkspace->setToolTip(
            QStringLiteral("当前工作目录：%1\n点击切换当前会话工作目录").arg(directory));
    }
    reloadFromStore();
    refreshComposerMediaTray();
    emit sessionListChanged();
}

void AgentChatPanel::setDefaultWorkspaceDirectory(const QString& directory) {
    defaultWorkspaceDirectory_ = directory;
    defaultWorkspaceConfigured_ = true;
}

bool AgentChatPanel::chooseDirectoryAndCreateSession() {
    QString initial = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    MaiSession current;
    if (runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), current)) {
        initial = fromUtf8(current.directory);
    } else if (QFileInfo(defaultWorkspaceDirectory_).isDir()) {
        initial = defaultWorkspaceDirectory_;
    }
    const QString selected = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择新对话的工作目录"), initial, QFileDialog::ShowDirsOnly);
    if (selected.isEmpty()) return false;
    const QString previous = runtime_->sessionId;
    openNewSessionInDirectory(selected);
    return !runtime_->sessionId.isEmpty() && runtime_->sessionId != previous;
}

void AgentChatPanel::openNewSessionInDirectory(const QString& directory) {
    createSessionInDirectory(directory);
}

void AgentChatPanel::openSessionInDirectory(const QString& directory) {
    const QFileInfo selected(directory);
    if (!selected.isDir()) {
        appendNotice(QStringLiteral("工作目录不存在或不可访问：%1").arg(directory), true);
        return;
    }
    if (runtime_->sessionId.isEmpty()) {
        createSessionInDirectory(directory);
        return;
    }
    if (!runtime_->controller->setSessionDirectory(runtime_->sessionId, directory)) {
        appendNotice(runtime_->controller->lastError(), true);
        return;
    }
    openSession(runtime_->sessionId);
}

void AgentChatPanel::createSessionInDirectory(const QString& directory) {
    const QFileInfo selected(directory);
    if (!selected.isDir()) {
        appendNotice(QStringLiteral("工作目录不存在或不可访问：%1").arg(directory), true);
        return;
    }
    const QString sessionId = runtime_->controller->createSession(selected.canonicalFilePath());
    if (sessionId.isEmpty()) {
        appendNotice(runtime_->controller->lastError(), true);
        return;
    }
    const QString preferredModel = runtime_->modelChip->text();
    if (preferredModel == QStringLiteral("glm-5.3") ||
        preferredModel == QStringLiteral("glm-5.3-flash") ||
        preferredModel == QStringLiteral("deepseek-flash")) {
        if (!runtime_->controller->setModel(sessionId, preferredModel))
            appendNotice(runtime_->controller->lastError(), true);
    }
    openSession(sessionId);
}

// ── 重画 ────────────────────────────────────────────────────────

void AgentChatPanel::reloadFromStore() {
    // 视图自己会把嵌进去的部件删掉，这儿只要把索引清干净。
    runtime_->flushTimer->stop();
    runtime_->view->beginBatchUpdates();
    runtime_->view->clear();
    runtime_->pendingThinking = nullptr;
    runtime_->pendingThinkingItemId.clear();
    runtime_->answers.clear();
    runtime_->renderedUserMessages.clear();
    runtime_->dirtyAnswers.clear();
    runtime_->toolCards.clear();
    runtime_->toolGroups.clear();
    runtime_->subAgentCards.clear();
    runtime_->pendingQuestionId.clear();
    runtime_->editor->setPlaceholderText(QStringLiteral("交给它做点什么…"));

    MaiSession selectedSession;
    runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), selectedSession);
    const bool sessionBusy = runtime_->controller->agent().isBusy(toUtf8(runtime_->sessionId));
    bool activeAssistantHasVisibleActivity = false;

    for (const MaiMessage& message : runtime_->controller->agent().listMessages(toUtf8(runtime_->sessionId))) {
        if (message.role == MaiRole::User) {
            appendStoredUserMessage(message, selectedSession);
            continue;
        }
        QStringList textFragments;
        for (const MaiMessagePart& part : message.parts) {
            const QString partId = fromUtf8(part.id);
            if (const auto* text = std::get_if<MaiTextPart>(&part.body)) {
                const QString stored = fromUtf8(text->text);
                const QString live = message.completed == 0
                                         ? runtime_->controller->livePartText(runtime_->sessionId, partId)
                                         : QString();
                const QString content = live.isEmpty() ? stored : live;
                textFragments.push_back(content);
                if (message.completed == 0 && !content.isEmpty()) activeAssistantHasVisibleActivity = true;
                runtime_->answers.insert(partId, content);
                runtime_->view->addItem(partId, MarkdownView::Style::Document, content);
            } else if (std::get_if<MaiReasoningPart>(&part.body)) {
                // 历史思考正文不进入界面；仍在执行的会话统一显示短状态。
                continue;
            } else if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                if (message.completed == 0 &&
                    (tool->state == MaiToolState::Pending || tool->state == MaiToolState::Running))
                    activeAssistantHasVisibleActivity = true;
                if (tool->tool == "agent_send_media" && tool->state == MaiToolState::Completed &&
                    appendAgentMediaCard(partId, fromUtf8(tool->output), fromUtf8(selectedSession.directory)))
                    continue;
                ToolCard* card = toolCardFor(partId, fromUtf8(message.id));
                card->setCall(fromUtf8(tool->tool), fromUtf8(tool->input));
                card->apply(tool->state, false);
                card->setDetail(tool->state == MaiToolState::Error ? fromUtf8(tool->error) : fromUtf8(tool->output));
                if (tool->state == MaiToolState::Completed && tool->tool == "generate_pdf")
                    appendPdfPreview(partId, fromUtf8(tool->output), fromUtf8(selectedSession.directory));
            }
        }
        if (ToolGroupCard* group = runtime_->toolGroups.value(fromUtf8(message.id), nullptr))
            group->setSummary(message, runtime_->toolCards);
        if (message.completed != 0)
            appendTextMediaCards(fromUtf8(message.id), textFragments.join(QLatin1Char('\n')),
                                 fromUtf8(selectedSession.directory));
        appendQuoteAction(fromUtf8(message.id));
    }
    runtime_->view->endBatchUpdates();
    setRunning(sessionBusy);
    // 权限事件只交付一次。切换会话后从审批闸门恢复可操作状态，
    // 不能把存储中的 Pending 片段直接绘成“排队中”。
    for (const MaiPermissionRequest& pending : runtime_->controller->agent().listPendingPermissions()) {
        showApproval(fromUtf8(pending.id));
    }
    for (const MaiQuestionRequest& pending : runtime_->controller->pendingQuestions()) {
        if (fromUtf8(pending.sessionId) == runtime_->sessionId ||
            runtime_->controller->isChildOf(fromUtf8(pending.sessionId), runtime_->sessionId)) {
            showQuestion(fromUtf8(pending.id));
            break;
        }
    }
    if (sessionBusy && !activeAssistantHasVisibleActivity && runtime_->pendingQuestionId.isEmpty())
        startPendingThinking();
    updateWelcomeVisibility();
    refreshContextSize();
    // 切换会话时必须跳到该会话最新消息，不沿用上一会话的旧滚动位置。
    runtime_->view->scrollToBottom();
    const QString selected = runtime_->sessionId;
    QTimer::singleShot(0, this, [this, selected] {
        if (runtime_->sessionId == selected) runtime_->view->scrollToBottom();
    });
}

void AgentChatPanel::appendStoredUserMessage(const MaiMessage& message, const MaiSession& session) {
    if (message.role != MaiRole::User) return;
    const QString messageId = fromUtf8(message.id);
    if (runtime_->renderedUserMessages.contains(messageId)) return;
    runtime_->renderedUserMessages.insert(messageId);
    QStringList images;
    QStringList videos;
    for (const MaiMessagePart& part : message.parts) {
        if (const auto* image = std::get_if<MaiImagePart>(&part.body)) {
            const QString stored = fromUtf8(image->path);
            images.push_back(QFileInfo(stored).isAbsolute()
                                 ? stored : QDir(fromUtf8(session.directory)).filePath(stored));
        } else if (const auto* video = std::get_if<MaiVideoPart>(&part.body)) {
            const QString stored = fromUtf8(video->path);
            videos.push_back(QFileInfo(stored).isAbsolute()
                                 ? stored : QDir(fromUtf8(session.directory)).filePath(stored));
        }
    }
    const QString sourceText = fromUtf8(message.text());
    QString userText = sourceText;
    for (const MaiMessagePart& part : message.parts)
        if (const auto* quote = std::get_if<MaiQuotePart>(&part.body))
            userText = QStringLiteral("> ↩ %1\n\n%2")
                           .arg(fromUtf8(quote->preview).simplified(), userText);
    if (!images.isEmpty() && userText.trimmed() == QStringLiteral("请查看这些图片。")) userText.clear();
    if (!userText.isEmpty())
        runtime_->view->addItem(messageId, MarkdownView::Style::Bubble, userText);
    appendUserImages(images);
    appendUserVideos(videos);
    appendTextMediaCards(messageId, sourceText, fromUtf8(session.directory));
    appendQuoteAction(messageId);
    updateWelcomeVisibility();
}

// 流式期间往某条回答后面追加。第一段到的时候先把条目建出来，
// 用户立刻看得见有东西在长；后面的靠定时器攒着批量刷。
void AgentChatPanel::appendAnswerDelta(const QString& partId, const QString& delta) {
    QString& source = runtime_->answers[partId];
    const bool isNew = source.isEmpty() && !runtime_->view->contains(partId);
    source += delta;
    if (isNew) {
        runtime_->view->addItem(partId, MarkdownView::Style::Document, source);
        updateWelcomeVisibility();
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

void AgentChatPanel::appendUserBubble(const QString& text, const QString& quotedPreview) {
    // 只有用户这一侧保留气泡。人发的消息短，气泡合适；
    // 而且右侧那块底色让"谁说的"一眼可辨，不用头像也不用名字。
    //
    // 宽度不用自己量了：视图先按上限排一遍、再按**量出来的**自然宽度收窄。
    // 原来是拿 QFontMetrics 估的，估窄了最后一个字会被挤到下一行
    // （"你好"两个字排成两行就是这么来的）。
    const QString displayed = quotedPreview.isEmpty()
        ? text : QStringLiteral("> ↩ %1\n\n%2").arg(quotedPreview.simplified(), text);
    runtime_->view->addItem(QStringLiteral("local-%1").arg(++runtime_->noticeSerial),
                            MarkdownView::Style::Bubble, displayed);
    updateWelcomeVisibility();
    scrollToBottom();
}

void AgentChatPanel::appendQuoteAction(const QString& messageId) {
    if (messageId.isEmpty()) return;
    const QString id = messageId + QStringLiteral("-quote-action");
    if (runtime_->view->contains(id)) return;
    auto* button = new QPushButton(QStringLiteral("↩  引用"));
    button->setObjectName(QStringLiteral("agentQuoteAction"));
    button->setProperty("messageId", messageId);
    button->setAccessibleName(QStringLiteral("引用这条消息"));
    button->setCursor(Qt::PointingHandCursor);
    button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    button->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
        "QPushButton{background:transparent;border:none;color:#667085;"
        "font-size:12px;padding:2px 0;text-align:left;}"
        "QPushButton:hover{color:#0b67b7;}")));
    connect(button, &QPushButton::clicked, this, [this, messageId] { beginQuote(messageId); });
    runtime_->view->addWidget(id, button);
}

void AgentChatPanel::beginQuote(const QString& messageId) {
    for (const MaiMessage& message : runtime_->controller->agent().listMessages(toUtf8(runtime_->sessionId))) {
        if (fromUtf8(message.id) != messageId) continue;
        QStringList media;
        for (const MaiMessagePart& part : message.parts) {
            if (std::get_if<MaiImagePart>(&part.body)) media << QStringLiteral("图片");
            else if (std::get_if<MaiVideoPart>(&part.body)) media << QStringLiteral("视频");
            else if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                if (tool->tool != "agent_send_media" || tool->state != MaiToolState::Completed) continue;
                const QJsonObject artifact = QJsonDocument::fromJson(fromUtf8(tool->output).toUtf8()).object();
                const QString type = artifact.value(QStringLiteral("type")).toString();
                if (type == QStringLiteral("video")) media << QStringLiteral("视频");
                else if (type == QStringLiteral("image")) media << QStringLiteral("图片");
                else if (type == QStringLiteral("audio")) media << QStringLiteral("音频");
            }
        }
        const QString text = fromUtf8(message.text()).simplified();
        runtime_->quotedMessageId = messageId;
        runtime_->quotedPreview = (media.isEmpty() ? QString() :
            QStringLiteral("[%1] ").arg(media.join(QStringLiteral("、")))) +
            (text.isEmpty() ? QStringLiteral("消息") : text.left(80));
        runtime_->quoteLabel->setText(QStringLiteral("↩  %1\n%2")
            .arg(message.role == MaiRole::User ? QStringLiteral("引用你的消息")
                                           : QStringLiteral("引用 AI 消息"),
                 runtime_->quotedPreview));
        runtime_->quoteStrip->show();
        runtime_->editor->setFocus();
        return;
    }
}

void AgentChatPanel::clearQuote() {
    runtime_->quotedMessageId.clear();
    runtime_->quotedPreview.clear();
    runtime_->quoteStrip->hide();
}

void AgentChatPanel::appendNotice(const QString& text, bool isError) {
    runtime_->view->addItem(QStringLiteral("notice-%1").arg(++runtime_->noticeSerial),
                            isError ? MarkdownView::Style::Error : MarkdownView::Style::Notice, text);
    updateWelcomeVisibility();
    scrollToBottom();
}

void AgentChatPanel::startPendingThinking() {
    if (runtime_->pendingThinking != nullptr) return;
    runtime_->pendingThinkingItemId = QStringLiteral("pending-thinking-%1").arg(++runtime_->noticeSerial);
    runtime_->pendingThinking = new ThinkingLine;
    runtime_->view->addWidget(runtime_->pendingThinkingItemId, runtime_->pendingThinking);
    updateWelcomeVisibility();
}

void AgentChatPanel::clearPendingThinking() {
    if (runtime_->pendingThinking == nullptr) return;
    runtime_->pendingThinking->hide();
    runtime_->view->removeItem(runtime_->pendingThinkingItemId);
    runtime_->pendingThinking = nullptr;
    runtime_->pendingThinkingItemId.clear();
}

AgentChatPanel::ToolGroupCard* AgentChatPanel::toolGroupFor(const QString& messageId) {
    auto found = runtime_->toolGroups.constFind(messageId);
    if (found != runtime_->toolGroups.constEnd()) return found.value();
    auto* group = new ToolGroupCard;
    runtime_->view->addWidget(QStringLiteral("tool-group-") + messageId, group);
    runtime_->toolGroups.insert(messageId, group);
    return group;
}

AgentChatPanel::ToolCard* AgentChatPanel::toolCardFor(const QString& partId, const QString& messageId) {
    auto found = runtime_->toolCards.constFind(partId);
    if (found != runtime_->toolCards.constEnd()) return found.value();

    ToolGroupCard* group = toolGroupFor(messageId);
    auto* card = new ToolCard(group);
    card->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    group->addCard(card);
    runtime_->toolCards.insert(partId, card);

    connect(card->allowButton(), &QPushButton::clicked, this, [this, partId] { replyForPart(partId, false, true); });
    connect(card->alwaysButton(), &QPushButton::clicked, this, [this, partId] { replyForPart(partId, true, true); });
    connect(card->denyButton(), &QPushButton::clicked, this, [this, partId] { replyForPart(partId, false, false); });
    return card;
}

void AgentChatPanel::replyForPart(const QString& partId, bool forSession, bool approve) {
    for (const MaiPermissionRequest& pending : runtime_->controller->agent().listPendingPermissions()) {
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
    for (const MaiMessage& message : runtime_->controller->agent().listMessages(toUtf8(runtime_->sessionId))) {
        if (fromUtf8(message.id) != messageId) continue;
        for (const MaiMessagePart& part : message.parts) {
            if (fromUtf8(part.id) != partId) continue;
            const auto* tool = std::get_if<MaiToolPart>(&part.body);
            if (tool == nullptr) return;

            if (tool->tool == "agent_send_media" && tool->state == MaiToolState::Completed) {
                MaiSession session;
                if (runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), session) &&
                    appendAgentMediaCard(partId, fromUtf8(tool->output), fromUtf8(session.directory))) {
                    ToolCard* existing = runtime_->toolCards.take(partId);
                    ToolGroupCard* group = runtime_->toolGroups.value(messageId, nullptr);
                    if (existing && group) group->removeCard(existing);
                    if (group && group->cardCount() == 0) {
                        runtime_->toolGroups.remove(messageId);
                        runtime_->view->removeItem(QStringLiteral("tool-group-") + messageId);
                    } else if (group) {
                        group->setSummary(message, runtime_->toolCards);
                    }
                    scrollToBottom();
                    return;
                }
            }

            ToolCard* card = toolCardFor(partId, messageId);
            card->setCall(fromUtf8(tool->tool), fromUtf8(tool->input));
            // 还在等人点头的话，卡片保持授权态——那个状态由 permissionAsked 打开，
            // 这里不要把它关掉。
            const bool waiting = card->isWaitingForUser();
            card->apply(tool->state, waiting && tool->state == MaiToolState::Pending);
            if (tool->state == MaiToolState::Completed && !tool->output.empty()) {
                card->setDetail(fromUtf8(tool->output));
                if (tool->tool == "generate_pdf") {
                    MaiSession session;
                    if (runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), session))
                        appendPdfPreview(partId, fromUtf8(tool->output), fromUtf8(session.directory));
                }
            } else if (tool->state == MaiToolState::Error || tool->state == MaiToolState::Canceled) {
                card->setDetail(fromUtf8(tool->error));
            }
            runtime_->toolGroups.value(messageId)->setSummary(message, runtime_->toolCards);
            scrollToBottom();
            return;
        }
        return;
    }
}

void AgentChatPanel::appendPdfPreview(const QString& partId, const QString& output, const QString& workspace) {
    const QString id = partId + QStringLiteral("-pdf-preview");
    if (runtime_->view->contains(id)) return;
    const QJsonObject artifact = QJsonDocument::fromJson(output.toUtf8()).object();
    QString relative;
    if (artifact.value(QStringLiteral("mime_type")).toString() == QStringLiteral("application/pdf")) {
        relative = artifact.value(QStringLiteral("path")).toString();
    } else if (output.startsWith(QStringLiteral("Created ")) && output.endsWith(QStringLiteral(" bytes)."))) {
        const int end = output.lastIndexOf(QStringLiteral(" ("));
        if (end > 8) relative = output.mid(8, end - 8);
    }
    if (relative.isEmpty() || !relative.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) return;

    auto* button = new QPushButton(QStringLiteral("PDF  %1   ·   预览").arg(QFileInfo(relative).fileName()));
    button->setObjectName(QStringLiteral("agentPdfPreviewButton"));
    button->setCursor(Qt::PointingHandCursor);
    button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    button->setStyleSheet(
        UiZoom::scaleQss(QStringLiteral("QPushButton{background:#f3f8ff;border:1px solid "
                                        "#c9ddf7;border-radius:9px;"
                                        "color:#175a9e;padding:10px 15px;text-align:left;font-size:13px;}"
                                        "QPushButton:hover{background:#e9f3ff;border-color:#87b9ef;}")));
    connect(button, &QPushButton::clicked, this, [this, workspace, relative] {
        const QString path =
            QFileInfo(relative).isAbsolute() ? QDir::cleanPath(relative) : QDir(workspace).absoluteFilePath(relative);
        showPdfPreview(this, path, QFileInfo(relative).fileName());
    });
    runtime_->view->addWidget(id, button);
    scrollToBottom();
}

bool AgentChatPanel::appendAgentMediaCard(const QString& partId, const QString& output, const QString& workspace) {
    const QString id = partId + QStringLiteral("-agent-media");
    if (runtime_->view->contains(id)) return true;
    const QJsonObject artifact = QJsonDocument::fromJson(output.toUtf8()).object();
    if (artifact.value(QStringLiteral("delivery")).toString() != QStringLiteral("current_ai_session")) return false;
    const QString relative = artifact.value(QStringLiteral("path")).toString();
    const QString type = artifact.value(QStringLiteral("type")).toString();
    const QString mime = artifact.value(QStringLiteral("mime_type")).toString();
    if (relative.isEmpty() ||
        !QStringList{QStringLiteral("image"), QStringLiteral("video"), QStringLiteral("audio")}.contains(type) ||
        !mime.startsWith(type + QLatin1Char('/')))
        return false;
    const QString path = fromUtf8(maiResolvePathWithinRoot(toUtf8(workspace), toUtf8(relative)));
    if (path.isEmpty() || !QFileInfo(path).isFile()) return false;

    auto* card = new QWidget;
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    if (type == QStringLiteral("image") || type == QStringLiteral("video")) {
        const bool video = type == QStringLiteral("video");
        const QSize previewSize = video ? QSize(240, 135) : QSize(220, 180);
        auto* preview = new QFrame(card);
        preview->setFixedSize(previewSize);
        auto* cover = new QLabel(preview);
        if (video) {
            cover->setObjectName(QStringLiteral("agentMediaVideoCover"));
            cover->setStyleSheet(QStringLiteral("background:#111827;"));
        }
        cover->setGeometry(QRect(QPoint(0, 0), previewSize));
        cover->setAlignment(Qt::AlignCenter);
        MessageImageLoader::instance().load(path, previewSize * 3, cover,
            [cover, previewSize, video](const QPixmap& pixels) {
            cover->setPixmap(pixels.scaled(previewSize,
                video ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio,
                Qt::SmoothTransformation));
        });
        if (type == QStringLiteral("image")) {
            auto* open = new QPushButton(preview);
            open->setObjectName(QStringLiteral("agentMediaImageOpen"));
            open->setGeometry(QRect(QPoint(0, 0), previewSize));
            open->setCursor(Qt::PointingHandCursor);
            open->setStyleSheet(QStringLiteral("background:transparent;border:0"));
            connect(open, &QPushButton::clicked, this, [this, card, path] {
                const QScreen* screen = QGuiApplication::primaryScreen();
                const qreal dpr = screen ? screen->devicePixelRatio() : 1;
                const QSize size = screen ? screen->availableGeometry().size() : QSize(1200, 900);
                MessageImageLoader::instance().load(path, (QSizeF(size) * dpr).toSize(), card,
                                                    [this](const QPixmap& image) {
                                                        ImagePreviewDialog dialog(image, this);
                                                        dialog.exec();
                                                    });
            });
        } else {
            auto* play = new QPushButton(QStringLiteral("▶"), preview);
            play->setObjectName(QStringLiteral("agentMediaVideoPlay"));
            play->setGeometry(96, 52, 48, 48);
            play->setCursor(Qt::PointingHandCursor);
            play->setToolTip(QStringLiteral("播放视频"));
            play->setStyleSheet(QStringLiteral(
                "QPushButton#agentMediaVideoPlay{background:rgba(10,16,27,195);"
                "border:1px solid rgba(255,255,255,125);border-radius:24px;"
                "color:white;font-size:20px;padding-left:3px;}"
                "QPushButton#agentMediaVideoPlay:hover{background:rgba(10,16,27,230);}"));
            connect(play, &QPushButton::clicked, this, [this, path] {
                if (openVideo_) openVideo_(path, currentVideoPaths());
            });
            auto* duration = new QLabel(QStringLiteral("0:00"), preview);
            duration->setAlignment(Qt::AlignCenter);
            duration->setGeometry(186, 107, 46, 22);
            duration->setStyleSheet(QStringLiteral("color:white;background:rgba(0,0,0,175);border-radius:7px;"));
            auto* metadata = new QMediaPlayer(preview);
            connect(metadata, &QMediaPlayer::durationChanged, duration, [duration](qint64 milliseconds) {
                const qint64 seconds = qMax<qint64>(0, milliseconds / 1000);
                duration->setText(QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0')));
            });
            metadata->setMedia(QUrl::fromLocalFile(path));
        }
        layout->addWidget(preview);
    } else {
        auto* audioCard = new QPushButton(card);
        audioCard->setObjectName(QStringLiteral("agentMediaAudioPlay"));
        audioCard->setAccessibleName(QStringLiteral("播放音频"));
        audioCard->setCursor(Qt::PointingHandCursor);
        audioCard->setFixedSize(240, 58);
        audioCard->setStyleSheet(QStringLiteral(
            "QPushButton#agentMediaAudioPlay{background:#f4f6f9;border:0;"
            "border-radius:12px;text-align:left;}"
            "QPushButton#agentMediaAudioPlay:hover{background:#eaf2fa;}"));
        auto* row = new QHBoxLayout(audioCard);
        row->setContentsMargins(10, 9, 10, 9);
        row->setSpacing(12);
        auto* symbol = new QLabel(QStringLiteral("▶"), audioCard);
        symbol->setObjectName(QStringLiteral("agentMediaAudioSymbol"));
        symbol->setFixedSize(38, 38);
        symbol->setAlignment(Qt::AlignCenter);
        symbol->setStyleSheet(QStringLiteral(
            "QLabel#agentMediaAudioSymbol{background:#dff1fc;border-radius:19px;"
            "color:#1687d4;font-size:18px;}"));
        row->addWidget(symbol);
        auto* labels = new QVBoxLayout;
        labels->setContentsMargins(0, 0, 0, 0);
        labels->setSpacing(2);
        const QString fileName = QFileInfo(path).fileName();
        auto* name = new QLabel(audioCard);
        name->setText(name->fontMetrics().elidedText(fileName, Qt::ElideMiddle, 160));
        name->setToolTip(fileName);
        name->setStyleSheet(QStringLiteral("color:#172033;background:transparent;border:none;font-size:13px;"));
        auto* duration = new QLabel(QStringLiteral("0:00"), audioCard);
        duration->setObjectName(QStringLiteral("agentMediaAudioDuration"));
        duration->setStyleSheet(QStringLiteral("color:#667085;background:transparent;border:none;font-size:11px;"));
        labels->addWidget(name);
        labels->addWidget(duration);
        row->addLayout(labels, 1);
        auto* audio = new QMediaPlayer(card);
        audio->setMedia(QUrl::fromLocalFile(path));
        connect(audioCard, &QPushButton::clicked, audio, [audio] {
            if (audio->state() == QMediaPlayer::PlayingState)
                audio->pause();
            else
                audio->play();
        });
        connect(audio, &QMediaPlayer::stateChanged, audioCard, [audioCard, symbol](QMediaPlayer::State state) {
            const bool playing = state == QMediaPlayer::PlayingState;
            symbol->setText(playing ? QStringLiteral("Ⅱ") : QStringLiteral("▶"));
            audioCard->setAccessibleName(playing ? QStringLiteral("暂停音频") : QStringLiteral("播放音频"));
        });
        connect(audio, &QMediaPlayer::durationChanged, duration, [duration](qint64 milliseconds) {
            const qint64 seconds = qMax<qint64>(0, milliseconds / 1000);
            duration->setText(QStringLiteral("%1:%2").arg(seconds / 60)
                .arg(seconds % 60, 2, 10, QLatin1Char('0')));
        });
        layout->addWidget(audioCard);
    }
    const QString caption = artifact.value(QStringLiteral("caption")).toString();
    if (!caption.isEmpty()) {
        auto* label = new QLabel(caption, card);
        label->setWordWrap(true);
        layout->addWidget(label);
    }
    if (type == QStringLiteral("video")) {
        auto* saveCloud = new QPushButton(QStringLiteral("保存到视频云盘"), card);
        saveCloud->setObjectName(QStringLiteral("agentMediaCloudDriveSave"));
        saveCloud->setAccessibleName(QStringLiteral("将这个视频保存到我的私人云盘"));
        saveCloud->setCursor(Qt::PointingHandCursor);
        saveCloud->setStyleSheet(QStringLiteral(
            "QPushButton{background:#eef6fd;color:#0b67b7;border:0;border-radius:8px;"
            "padding:8px 13px;text-align:left;font-weight:600;}"
            "QPushButton:hover{background:#dceefb;}"));
        saveCloud->setVisible(runtime_->cloudDriveAvailable);
        runtime_->cloudDriveSaveButtons.append(saveCloud);
        connect(saveCloud, &QPushButton::clicked, this,
                [this, path] { emit cloudDriveSaveRequested(path); });
        layout->addWidget(saveCloud, 0, Qt::AlignLeft);
        const QJsonObject completedCard{
            {QStringLiteral("tool"), QStringLiteral("agent_send_media")},
            {QStringLiteral("state"), QStringLiteral("completed")},
            {QStringLiteral("output"), artifact}};
        const QByteArray cardBytes = QJsonDocument(completedCard).toJson(QJsonDocument::Compact);
        MaiVideoCircleResult eligibility{};
        mai_video_circle_inspect_card(cardBytes.constData(), &eligibility);
        const bool eligible = eligibility.status == MAI_VIDEO_CIRCLE_OK;
        mai_video_circle_result_free(&eligibility);
        if (eligible) {
            auto* publish = new QPushButton(QStringLiteral("发布到视频圈"), card);
            publish->setObjectName(QStringLiteral("agentMediaVideoCirclePublish"));
            publish->setAccessibleName(QStringLiteral("发布这个已完成的 AI 视频到视频圈"));
            publish->setCursor(Qt::PointingHandCursor);
            publish->setStyleSheet(QStringLiteral(
                "QPushButton{background:#eef6fd;color:#0b67b7;border:0;border-radius:8px;"
                "padding:8px 13px;text-align:left;font-weight:600;}"
                "QPushButton:hover{background:#dceefb;}"));
            publish->setVisible(runtime_->videoCircleAvailable);
            runtime_->videoCirclePublishButtons.append(publish);
            connect(publish, &QPushButton::clicked, this,
                    [this, cardBytes, path] {
                        emit videoCirclePublishRequested(QString::fromUtf8(cardBytes), path);
                    });
            layout->addWidget(publish, 0, Qt::AlignLeft);
        }
    }
    card->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(card, &QWidget::customContextMenuRequested, this, [this, card, path, type](const QPoint& point) {
        QMenu menu(card);
        QAction* save = menu.addAction(QStringLiteral("保存副本…"));
        QAction* forward = forwardMedia_ ? menu.addAction(QStringLiteral("转发给联系人…")) : nullptr;
        QAction* selected = menu.exec(card->mapToGlobal(point));
        if (selected == save) {
            const QString destination =
                QFileDialog::getSaveFileName(this, QStringLiteral("保存媒体"), QFileInfo(path).fileName());
            if (!destination.isEmpty() && QDir::cleanPath(destination) != QDir::cleanPath(path)) {
                auto* watcher = new QFutureWatcher<bool>(this);
                connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
                    const bool saved = watcher->result();
                    watcher->deleteLater();
                    if (!saved)
                        QMessageBox::warning(this, QStringLiteral("保存失败"),
                                             QStringLiteral("媒体文件无法写入所选位置。"));
                });
                watcher->setFuture(QtConcurrent::run([path, destination] {
                    QFile input(path);
                    QSaveFile output(destination);
                    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return false;
                    while (!input.atEnd()) {
                        const QByteArray bytes = input.read(1024 * 1024);
                        if (bytes.isEmpty() || output.write(bytes) != bytes.size()) return false;
                    }
                    return output.commit();
                }));
            }
        } else if (selected && selected == forward) {
            forwardMedia_(path, type);
        }
    });
    runtime_->view->addWidget(id, card);
    scrollToBottom();
    return true;
}

void AgentChatPanel::appendTextMediaCards(const QString& messageId, const QString& text, const QString& workspace) {
    // 以整条消息为单位决定：少量实际文件显示气泡，五个及以上仍是原文字清单。
    const QVector<LocalMediaMention> mentions = localMediaMentions(text, workspace);
    for (int index = 0; index < mentions.size(); ++index) {
        const QString id = messageId + QStringLiteral("-text-media-") + QString::number(index);
        if (runtime_->view->contains(id)) continue;
        const LocalMediaMention mention = mentions.at(index);
        auto* button = new QPushButton(QStringLiteral("%1  %2").arg(
            mention.video ? QStringLiteral("▶") : QStringLiteral("♫"), QFileInfo(mention.path).fileName()));
        button->setObjectName(QStringLiteral("agentTextMediaBubble"));
        button->setCursor(Qt::PointingHandCursor);
        button->setMaximumWidth(320);
        button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        button->setStyleSheet(
            UiZoom::scaleQss(QStringLiteral("QPushButton{background:#f3f8ff;border:1px solid #c9ddf7;"
                                            "border-radius:10px;color:#175a9e;padding:11px 15px;"
                                            "text-align:left;font-size:13px;}"
                                            "QPushButton:hover{background:#e9f3ff;border-color:#87b9ef;}")));
        if (mention.video) {
            connect(button, &QPushButton::clicked, this, [this, path = mention.path] {
                if (QFileInfo(path).isFile() && openVideo_)
                    openVideo_(path, currentVideoPaths());
                else
                    appendNotice(QStringLiteral("文件已被移动或删除。"), true);
            });
        } else {
            auto* audio = new QMediaPlayer(button);
            audio->setMedia(QUrl::fromLocalFile(mention.path));
            connect(button, &QPushButton::clicked, audio, [audio] {
                if (audio->state() == QMediaPlayer::PlayingState)
                    audio->pause();
                else
                    audio->play();
            });
            connect(audio, &QMediaPlayer::stateChanged, button,
                    [button, name = QFileInfo(mention.path).fileName()](QMediaPlayer::State state) {
                        button->setText(QStringLiteral("%1  %2").arg(
                            state == QMediaPlayer::PlayingState ? QStringLiteral("❚❚") : QStringLiteral("♫"), name));
                    });
        }
        runtime_->view->addWidget(id, button);
        scrollToBottom();
    }
}

void AgentChatPanel::noteOtherSession(const QString& sessionId, const QString& text,
                                    const QString& partId) {
    // 只认自己的孩子。别的根会话（用户在另一个标签页里开的）不关这儿的事。
    if (!runtime_->controller->isChildOf(sessionId, runtime_->sessionId)) return;

    SubAgentCard* card = subAgentCardFor(sessionId);
    if (card == nullptr) return;
    if (!text.isEmpty()) card->appendLatestText(partId, text);

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
        const QString owner = fromUtf8(pending.sessionId);
        if (owner != runtime_->sessionId && !runtime_->controller->isChildOf(owner, runtime_->sessionId)) return;

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
        setRunning(runtime_->running);
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
    for (const MaiPermissionRequest& pending : runtime_->controller->agent().listPendingPermissions()) {
        if (fromUtf8(pending.id) != permissionId) continue;
        const QString owner = fromUtf8(pending.sessionId);
        if (owner != runtime_->sessionId && !runtime_->controller->isChildOf(owner, runtime_->sessionId)) return;
        const QString messageId = fromUtf8(pending.messageId);
        ToolCard* card = toolCardFor(fromUtf8(pending.partId), messageId);
        card->setCall(fromUtf8(pending.toolName), fromUtf8(pending.arguments));
        const int fileCount = !pending.approvalKeys.empty() && pending.approvalKeys.front().rfind("file:", 0) == 0
                                  ? static_cast<int>(pending.approvalKeys.size())
                                  : 0;
        card->setApprovalBehavior(pending.allowForSession, pending.rememberOnApproval, fileCount);
        card->setDetail(QString());
        card->apply(MaiToolState::Pending, true);
        ToolGroupCard* group = runtime_->toolGroups.value(messageId);
        for (const MaiMessage& message : runtime_->controller->agent().listMessages(pending.sessionId)) {
            if (fromUtf8(message.id) == messageId) {
                group->setSummary(message, runtime_->toolCards);
                break;
            }
        }
        group->showApproval();
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
            if (!QFileInfo(path).isFile()) continue;
            if (!videoMimeType(path).isEmpty()) insertComposerVideoFile(path);
            else if (!QImageReader::imageFormat(path).isEmpty()) insertComposerImageFile(path);
            else continue;
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
    const QString path =
        QDir(directory).filePath(QStringLiteral("paste-%1.png").arg(QDateTime::currentMSecsSinceEpoch()));
    if (!image.save(path, "PNG")) return;
    importComposerMediaFile(path, false, true);
}

void AgentChatPanel::insertComposerImageFile(const QString& path) {
    importComposerMediaFile(path, false);
}

void AgentChatPanel::insertComposerVideoFile(const QString& path) {
    importComposerMediaFile(path, true);
}

QString AgentChatPanel::composerMediaDirectory() const {
    QString root;
    if (!defaultWorkspaceDirectory_.isEmpty()) {
        root = QFileInfo(defaultWorkspaceDirectory_).absolutePath();
    } else {
        MaiSession session;
        if (runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), session))
            root = fromUtf8(session.directory);
    }
    if (root.isEmpty() || QDir(root).isRoot()) return {};
    return QDir(root).filePath(QStringLiteral("ComposerMedia"));
}

void AgentChatPanel::insertComposerMediaMarker(const QString& path, bool video,
                                                const QString& sessionId) {
    QTextImageFormat format;
    format.setName(video ? videoResourceName(path) : path);
    format.setWidth(1);
    format.setHeight(1);
    if (sessionId == runtime_->sessionId) {
        if (video) {
            QImage marker(1, 1, QImage::Format_ARGB32_Premultiplied);
            marker.fill(Qt::transparent);
            runtime_->editor->document()->addResource(QTextDocument::ImageResource,
                                                       QUrl(format.name()), marker);
        }
        QTextCursor oldCursor = runtime_->editor->textCursor();
        QTextCursor cursor(runtime_->editor->document());
        cursor.movePosition(QTextCursor::End);
        cursor.insertImage(format);
        runtime_->editor->setTextCursor(oldCursor);
        refreshComposerMediaTray();
    } else {
        QTextDocument document;
        QTextCursor cursor(&document);
        if (runtime_->composerDrafts.contains(sessionId))
            cursor.insertFragment(runtime_->composerDrafts.value(sessionId));
        cursor.movePosition(QTextCursor::End);
        cursor.insertImage(format);
        runtime_->composerDrafts.insert(sessionId, QTextDocumentFragment(&document));
    }
}

void AgentChatPanel::importComposerMediaFile(const QString& path, bool video,
                                              bool removeSourceAfterCopy) {
    const QFileInfo file(path);
    if (!file.isFile() || (video ? videoMimeType(path).isEmpty()
                                  : !QImageReader(path).size().isValid())) {
        appendNotice(QStringLiteral("%1无法读取：%2")
            .arg(video ? QStringLiteral("视频") : QStringLiteral("图片"), file.fileName()), true);
        return;
    }
    if (runtime_->sessionId.isEmpty()) openSession();
    const QString targetSession = runtime_->sessionId;
    if (targetSession.isEmpty()) return;
    const int existing = composerImagePaths().size() + composerVideoPaths().size();
    if (existing + runtime_->pendingMediaImports.value(targetSession) >= 10) {
        appendNotice(QStringLiteral("每条消息最多添加 10 项图片或视频。"), true);
        return;
    }
    const QString storage = composerMediaDirectory();
    if (storage.isEmpty()) {
        appendNotice(QStringLiteral("无法创建创作中心媒体工作目录。"), true);
        return;
    }
    const QString directory = QDir(storage).filePath(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString destination = QDir(directory).filePath(file.fileName());
    runtime_->pendingMediaImports[targetSession] += 1;
    runtime_->pendingImportDestinations.insert(destination);
    refreshComposerMediaTray();
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
        [this, watcher, targetSession, video, destination, fileName = file.fileName()] {
            const QString imported = watcher->result();
            watcher->deleteLater();
            runtime_->pendingImportDestinations.remove(destination);
            const int remaining = runtime_->pendingMediaImports.value(targetSession) - 1;
            if (remaining > 0) runtime_->pendingMediaImports[targetSession] = remaining;
            else runtime_->pendingMediaImports.remove(targetSession);
            MaiSession session;
            if (!runtime_->controller->agent().getSession(toUtf8(targetSession), session)) {
                if (!imported.isEmpty()) QDir(QFileInfo(imported).absolutePath()).removeRecursively();
            } else if (imported.isEmpty()) {
                if (targetSession == runtime_->sessionId)
                    appendNotice(QStringLiteral("媒体导入失败：%1").arg(fileName), true);
            } else {
                runtime_->unsentImportedPaths.insert(imported);
                insertComposerMediaMarker(imported, video, targetSession);
            }
            if (targetSession == runtime_->sessionId) refreshComposerMediaTray();
        });
    watcher->setFuture(QtConcurrent::run([source = file.absoluteFilePath(),
                                          directory, destination, removeSourceAfterCopy,
                                          canceled = runtime_->importCanceled] {
        if (!QDir().mkpath(directory)) {
            if (removeSourceAfterCopy) QFile::remove(source);
            return QString();
        }
        if (!QFile::copy(source, destination)) {
            QDir(directory).removeRecursively();
            if (removeSourceAfterCopy) QFile::remove(source);
            return QString();
        }
        if (removeSourceAfterCopy) QFile::remove(source);
        if (canceled->load()) {
            QDir(directory).removeRecursively();
            return QString();
        }
        return destination;
    }));
}

QStringList AgentChatPanel::composerImagePaths() const {
    QStringList paths;
    const QTextDocument* document = runtime_->editor->document();
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) continue;
            const QString path = fragment.charFormat().toImageFormat().name();
            if (path.startsWith(kComposerVideoPrefix)) continue;
            if (QFileInfo(path).isFile() && !paths.contains(path)) paths.push_back(path);
        }
    }
    return paths;
}

QStringList AgentChatPanel::composerVideoPaths() const {
    QStringList paths;
    const QTextDocument* document = runtime_->editor->document();
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) continue;
            const QString path = videoPathFromResource(fragment.charFormat().toImageFormat().name());
            if (QFileInfo(path).isFile() && !paths.contains(path)) paths.push_back(path);
        }
    }
    return paths;
}

void AgentChatPanel::removeComposerMedia(const QString& resourceName) {
    QTextDocument* document = runtime_->editor->document();
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat() ||
                fragment.charFormat().toImageFormat().name() != resourceName) continue;
            QTextCursor cursor(document);
            cursor.setPosition(fragment.position());
            cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            refreshComposerMediaTray();
            return;
        }
    }
}

void AgentChatPanel::refreshComposerMediaTray() {
    QStringList resources;
    QTextDocument* document = runtime_->editor->document();
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) continue;
            const QString name = fragment.charFormat().toImageFormat().name();
            const QString path = name.startsWith(kComposerVideoPrefix) ? videoPathFromResource(name) : name;
            if (QFileInfo(path).isFile() && !resources.contains(name)) resources.push_back(name);
        }
    }
    if (runtime_->visibleDraftSession == runtime_->sessionId) {
        for (const QString& old : runtime_->visibleDraftMedia) {
            if (resources.contains(old)) continue;
            const QString path = old.startsWith(kComposerVideoPrefix) ? videoPathFromResource(old) : old;
            if (!runtime_->unsentImportedPaths.remove(path)) continue;
            QtConcurrent::run([path] { QDir(QFileInfo(path).absolutePath()).removeRecursively(); });
        }
    }
    runtime_->visibleDraftSession = runtime_->sessionId;
    const int pending = runtime_->pendingMediaImports.value(runtime_->sessionId);
    if (resources != runtime_->visibleDraftMedia || pending != runtime_->visiblePendingImports) {
        runtime_->visibleDraftMedia = resources;
        runtime_->visiblePendingImports = pending;
        QHBoxLayout* row = runtime_->mediaTrayLayout;
        while (QLayoutItem* item = row->takeAt(0)) {
            if (QWidget* widget = item->widget()) {
                widget->hide();
                widget->deleteLater();
            }
            delete item;
        }
        for (const QString& name : resources) {
            const bool video = name.startsWith(kComposerVideoPrefix);
            const QString path = video ? videoPathFromResource(name) : name;
            auto* card = new QWidget(runtime_->mediaTray);
            card->setObjectName(video ? QStringLiteral("agentVideoDraftCard") :
                                        QStringLiteral("agentImageDraftCard"));
            card->setFixedSize(UiZoom::s(136), UiZoom::s(90));
            card->setStyleSheet(QStringLiteral("background:#f3f7fb;border-radius:8px;"));
            auto* cover = new QLabel(card);
            cover->setGeometry(0, 0, UiZoom::s(136), UiZoom::s(70));
            cover->setScaledContents(true);
            if (video) {
                QImage marker(1, 1, QImage::Format_ARGB32_Premultiplied);
                marker.fill(Qt::transparent);
                document->addResource(QTextDocument::ImageResource, QUrl(name), marker);
                cover->setPixmap(QPixmap::fromImage(videoDraftImage({})));
                MessageImageLoader::instance().load(path, QSize(480, 270), cover,
                    [cover, path](const QPixmap& pixels) {
                        cover->setPixmap(QPixmap::fromImage(
                            videoDraftImage(pixels.toImage())));
                    });
            } else {
                MessageImageLoader::instance().loadInto(path, QSize(272, 140), cover);
            }
            auto* open = new QPushButton(card);
            open->setObjectName(video ? QStringLiteral("agentDraftVideoPreview") :
                                        QStringLiteral("agentDraftImagePreview"));
            open->setAccessibleName(QStringLiteral("预览%1 %2")
                .arg(video ? QStringLiteral("视频") : QStringLiteral("图片"), QFileInfo(path).fileName()));
            open->setGeometry(0, 0, UiZoom::s(136), UiZoom::s(70));
            open->setCursor(Qt::PointingHandCursor);
            open->setStyleSheet(QStringLiteral("QPushButton{background:transparent;border:0;}"));
            connect(open, &QPushButton::clicked, this, [this, video, path, card] {
                if (video) {
                    if (openVideo_) openVideo_(path, composerVideoPaths());
                } else {
                    const QScreen* screen = QGuiApplication::primaryScreen();
                    const QSize size = screen ? screen->availableGeometry().size() : QSize(1200, 900);
                    MessageImageLoader::instance().load(path, size, card,
                        [this](const QPixmap& pixels) {
                            ImagePreviewDialog dialog(pixels, this);
                            dialog.exec();
                        });
                }
            });
            auto* remove = new QPushButton(QStringLiteral("×"), card);
            remove->setObjectName(QStringLiteral("agentRemoveDraftMedia"));
            remove->setAccessibleName(QStringLiteral("移除%1 %2")
                .arg(video ? QStringLiteral("视频") : QStringLiteral("图片"), QFileInfo(path).fileName()));
            remove->setGeometry(UiZoom::s(109), UiZoom::s(2), UiZoom::s(25), UiZoom::s(25));
            remove->setCursor(Qt::PointingHandCursor);
            remove->setStyleSheet(QStringLiteral(
                "QPushButton{background:rgba(0,0,0,180);border:0;border-radius:12px;color:white;}"));
            connect(remove, &QPushButton::clicked, this,
                    [this, name] { removeComposerMedia(name); });
            auto* label = new QLabel(QFileInfo(path).fileName(), card);
            label->setGeometry(UiZoom::s(4), UiZoom::s(72), UiZoom::s(128), UiZoom::s(16));
            label->setText(label->fontMetrics().elidedText(label->text(), Qt::ElideMiddle, UiZoom::s(124)));
            label->setStyleSheet(QStringLiteral("color:#172033;background:transparent;font-size:11px;"));
            row->addWidget(card);
        }
        if (pending > 0) {
            auto* loading = new QLabel(QStringLiteral("正在导入媒体（%1）…").arg(pending),
                                       runtime_->mediaTray);
            loading->setFixedSize(UiZoom::s(160), UiZoom::s(90));
            loading->setAlignment(Qt::AlignCenter);
            loading->setStyleSheet(QStringLiteral(
                "QLabel{color:#667085;background:#f3f7fb;border-radius:8px;font-size:12px;}"));
            row->addWidget(loading);
        }
        row->addStretch(1);
    }
    runtime_->mediaTray->setVisible(!resources.isEmpty() || pending > 0);
    if (runtime_->composerHost && runtime_->composerSplitter) {
        const bool hasMedia = !resources.isEmpty() || pending > 0;
        const int expanded = UiZoom::s(250) +
            (runtime_->quoteStrip->isVisible() ? UiZoom::s(42) : 0);
        const QList<int> sizes = runtime_->composerSplitter->sizes();
        const int current = sizes.size() > 1 ? sizes.at(1) : 0;
        if (hasMedia && !runtime_->mediaAutoExpanded && current < expanded) {
            runtime_->mediaAutoExpanded = true;
            runtime_->composerSizeBeforeMedia = current;
        }
        runtime_->composerHost->setMinimumHeight(hasMedia ? expanded : UiZoom::s(112));
        if (hasMedia && current < expanded) {
            const int available = runtime_->composerSplitter->height() -
                runtime_->composerSplitter->handleWidth();
            runtime_->composerSplitter->setSizes(QList<int>() << qMax(0, available - expanded) << expanded);
        } else if (!hasMedia && runtime_->mediaAutoExpanded) {
            runtime_->mediaAutoExpanded = false;
            if (current <= expanded + UiZoom::s(10)) {
                const int previous = qMax(runtime_->composerSizeBeforeMedia, UiZoom::s(112));
                const int available = runtime_->composerSplitter->height() -
                    runtime_->composerSplitter->handleWidth();
                runtime_->composerSplitter->setSizes(QList<int>() << qMax(0, available - previous) << previous);
            }
        }
    }
    updateComposerPlaceholder();
}

void AgentChatPanel::updateComposerPlaceholder() {
    if (!runtime_->pendingQuestionId.isEmpty()) return;
    runtime_->editor->setPlaceholderText(composerImagePaths().isEmpty() && composerVideoPaths().isEmpty()
        && runtime_->pendingMediaImports.value(runtime_->sessionId) == 0
        ? QStringLiteral("交给它做点什么…") : QStringLiteral("添加说明（可选）"));
}

void AgentChatPanel::appendUserImages(const QStringList& paths) {
    for (const QString& path : paths) {
        QPixmap pixmap(path);
        if (pixmap.isNull()) continue;
        auto* preview = new QLabel;
        preview->setPixmap(
            pixmap.scaled(UiZoom::s(280), UiZoom::s(210), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        preview->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        preview->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        preview->setToolTip(QFileInfo(path).fileName());
        runtime_->view->addWidget(QStringLiteral("local-image-%1").arg(++runtime_->noticeSerial), preview);
    }
}

void AgentChatPanel::appendUserVideos(const QStringList& paths) {
    for (const QString& path : paths) {
        if (!QFileInfo(path).isFile()) continue;
        auto* card = new QWidget;
        card->setFixedWidth(240);
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(7);
        auto* preview = new QWidget(card);
        preview->setFixedSize(240, 135);
        auto* cover = new QLabel(preview);
        cover->setGeometry(0, 0, 240, 135);
        cover->setPixmap(QPixmap::fromImage(videoDraftImage({}))
                             .scaled(240, 135, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        MessageImageLoader::instance().load(path, QSize(480, 270), cover,
            [cover, path](const QPixmap& pixels) {
                cover->setPixmap(QPixmap::fromImage(
                    videoDraftImage(pixels.toImage()))
                    .scaled(240, 135, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            });
        auto* play = new QPushButton(preview);
        play->setObjectName(QStringLiteral("agentUserVideoPlay"));
        play->setAccessibleName(QStringLiteral("播放视频 %1").arg(QFileInfo(path).fileName()));
        play->setGeometry(96, 43, 48, 48);
        play->setCursor(Qt::PointingHandCursor);
        play->setStyleSheet(QStringLiteral(
            "QPushButton{background:transparent;border:0;}"
            "QPushButton:hover{background:rgba(255,255,255,45);border-radius:24px;}"));
        connect(play, &QPushButton::clicked, this, [this, path, paths] {
            if (openVideo_) openVideo_(path, paths);
        });
        layout->addWidget(preview);
        auto* name = new QLabel(QFileInfo(path).fileName(), card);
        name->setStyleSheet(QStringLiteral("color:#172033;background:transparent;border:0;font-size:12px;"));
        name->setText(name->fontMetrics().elidedText(name->text(), Qt::ElideMiddle, 238));
        name->setToolTip(path);
        layout->addWidget(name);
        runtime_->view->addWidget(QStringLiteral("local-video-%1").arg(++runtime_->noticeSerial), card);
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
        runtime_->view->scrollToBottom();
        return;
    }

    // 跑着的时候这个按钮是"停止"。
    if (runtime_->running) {
        runtime_->controller->interrupt(runtime_->sessionId);
        return;
    }
    if (runtime_->pendingMediaImports.value(runtime_->sessionId) > 0) {
        appendNotice(QStringLiteral("媒体还在导入，请稍候再发送。"), false);
        return;
    }
    QString text = runtime_->editor->toPlainText();
    text.remove(QChar(0xFFFC));
    text = text.trimmed();
    const QStringList imagePaths = composerImagePaths();
    const QStringList videoPaths = composerVideoPaths();
    if (text.isEmpty() && imagePaths.isEmpty() && videoPaths.isEmpty()) return;
    if (!runtime_->modelConfigured) {
        emit modelConfigurationRequested();
        return;
    }
    if (!imagePaths.isEmpty() &&
        runtime_->modelChip->text().compare(QStringLiteral("glm-5.3"), Qt::CaseInsensitive) == 0) {
        appendNotice(QStringLiteral("glm-5.3 仅支持文本，请先切换到 glm-5.3-flash。"), true);
        return;
    }

    const QString quotedMessageId = runtime_->quotedMessageId;
    if (!runtime_->controller->sendPrompt(runtime_->sessionId, text, imagePaths,
                                          videoPaths, quotedMessageId)) {
        appendNotice(runtime_->controller->lastError(), true);
        return;
    }
    for (const QString& path : imagePaths) runtime_->unsentImportedPaths.remove(path);
    for (const QString& path : videoPaths) runtime_->unsentImportedPaths.remove(path);
    clearQuote();
    runtime_->editor->clear();
    updateComposerPlaceholder();
    MaiSession current;
    runtime_->controller->agent().getSession(toUtf8(runtime_->sessionId), current);
    const auto messages = runtime_->controller->agent().listMessages(toUtf8(runtime_->sessionId));
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
        if (it->role == MaiRole::User) {
            appendStoredUserMessage(*it, current);
            break;
        }
    setRunning(true);
    startPendingThinking();
    runtime_->view->scrollToBottom();
}

void AgentChatPanel::onClear() {
    MaiResult<std::string> cleared =
        runtime_->controller->agent().submit(MaiClearMessages{toUtf8(runtime_->sessionId)});
    if (!cleared) {
        // 最常见的是 Busy：一轮还在跑。说清楚能怎么办，别只报错。
        appendNotice(fromUtf8(cleared.error().message()) + QStringLiteral("（先按停止，再清空）"), true);
        return;
    }
    runtime_->title->setText(QStringLiteral("创作中心"));
    clearQuote();
    reloadFromStore();
}

void AgentChatPanel::setRunning(bool running) {
    runtime_->running = running;
    const bool stopping = running && runtime_->pendingQuestionId.isEmpty();
    runtime_->send->setText(QString());
    runtime_->send->setIcon(
        makeComposerActionIcon(stopping, QColor(QString::fromLatin1(stopping ? kDanger : "#ffffff"))));
    runtime_->send->setToolTip(!runtime_->pendingQuestionId.isEmpty() ? QStringLiteral("回答问题")
                               : stopping                             ? QStringLiteral("停止任务")
                                                                      : QStringLiteral("发送消息"));
    runtime_->send->setAccessibleName(runtime_->send->toolTip());
    runtime_->send->setStyleSheet(
        UiZoom::scaleQss(stopping ? QStringLiteral("QPushButton#agentSendButton{background:#fff5f3;color:%1;"
                                                   "border:1px solid #f0c3bd;border-radius:18px;padding:0;}"
                                                   "QPushButton#agentSendButton:hover{background:#fee4e2;}")
                                        .arg(kDanger)
                                  : QStringLiteral("QPushButton#agentSendButton{background:%1;color:#ffffff;"
                                                   "border:none;border-radius:18px;padding:0;}"
                                                   "QPushButton#agentSendButton:hover{background:#095a9f;}"
                                                   "QPushButton#agentSendButton:pressed{background:#084d87;}"
                                                   "QPushButton#agentSendButton:disabled{background:#b8d3e8;"
                                                   "color:#eef6ff;}")
                                        .arg(kAccent)));
    runtime_->hint->setEnabled(!running);
    runtime_->modelChip->setEnabled(!running && runtime_->modelConfigured);
    if (runtime_->moreButton) runtime_->moreButton->setEnabled(!running);
    if (running && runtime_->actionPanel) runtime_->actionPanel->hide();
    if (running) {
        runtime_->hint->setText(QStringLiteral("正在执行"));
    } else {
        updateApprovalPolicyUi();
    }
    updateWelcomeVisibility();
}

void AgentChatPanel::updateWelcomeVisibility() {
    if (!runtime_->welcomeView) return;
    const bool show = !runtime_->running && runtime_->pendingQuestionId.isEmpty() && runtime_->view->isEmpty();
    runtime_->welcomeView->setVisible(show);
    if (show) runtime_->welcomeView->raise();
}

void AgentChatPanel::scrollToBottom() {
    if (runtime_->view->isAtBottom()) runtime_->view->scrollToBottom();
}

void AgentChatPanel::refreshContextSize() {
    int characters = 0;
    for (const MaiMessage& message : runtime_->controller->agent().listMessages(toUtf8(runtime_->sessionId))) {
        characters += fromUtf8(message.text()).size();
    }
    // 这是个**估数**，不是 token 数：核心现在不往外报 usage。
    // 给用户的信号是"越来越长"，这一点估数就够了。
    runtime_->contextSize->setText(characters == 0 ? QString() : QStringLiteral("已攒约 %1 字").arg(characters));
}
