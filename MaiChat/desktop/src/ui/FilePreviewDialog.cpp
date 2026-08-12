#include "ui/FilePreviewDialog.h"

#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSizeGrip>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "ui/UiZoom.h"

FilePreviewDialog::FilePreviewDialog(const QString& displayName, const QString& html, QWidget* parent)
    : QDialog(parent) {
    buildUi(displayName, html);
    applyStyle();
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

    auto* closeIcon = new QPushButton(QStringLiteral("✕"), header_);
    closeIcon->setObjectName(QStringLiteral("filePreviewCloseIcon"));
    closeIcon->setCursor(Qt::PointingHandCursor);
    closeIcon->setFixedSize(UiZoom::s(28), UiZoom::s(28));
    closeIcon->setToolTip(QStringLiteral("关闭"));
    connect(closeIcon, &QPushButton::clicked, this, &QDialog::accept);
    headerRow->addWidget(closeIcon, 0, Qt::AlignVCenter);

    layout->addWidget(header_);

    content_ = new QTextBrowser(panel);
    content_->setObjectName(QStringLiteral("filePreviewContent"));
    content_->setOpenExternalLinks(true);
    content_->setReadOnly(true);
    content_->setFrameShape(QFrame::NoFrame);
    content_->setHtml(html);
    layout->addWidget(content_, 1);

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
    grip->setFixedSize(UiZoom::s(16), UiZoom::s(16));
    footerRow->addWidget(grip, 0, Qt::AlignBottom);

    layout->addLayout(footerRow);

    QSize target(UiZoom::s(900), UiZoom::s(660));
    if (parentWidget()) {
        target = QSize(qMax(UiZoom::s(720), parentWidget()->width() * 2 / 3),
                       qMax(UiZoom::s(520), parentWidget()->height() * 3 / 4));
    }
    resize(target);
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
        #filePreviewCloseIcon {
            background: transparent;
            border: 0;
            border-radius: 8px;
            color: #64748b;
            font-size: 15px;
            font-weight: 700;
        }
        #filePreviewCloseIcon:hover {
            background: #f1f5f9;
            color: #0f172a;
        }
        #filePreviewContent {
            background: #f8fafc;
            border: 1px solid #e6edf5;
            border-radius: 12px;
            color: #172033;
            font-size: 14px;
            padding: 16px;
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

void FilePreviewDialog::updateElidedTitle() {
    if (!title_) return;
    const int available = title_->width();
    if (available <= 0) return;
    // 中间省略：文件名的扩展名往往比中段更重要，末尾省略会把 .md 吃掉。
    title_->setText(title_->fontMetrics().elidedText(fullTitle_, Qt::ElideMiddle, available));
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
