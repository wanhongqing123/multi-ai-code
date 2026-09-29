#include "ui/PdfPreviewDialog.h"

#include <QDesktopServices>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#ifdef MAICHAT_PDF_WINDOWS_NATIVE
#include <QFutureWatcher>
#include <QScrollArea>
#include <QtConcurrent/QtConcurrentRun>
#endif

#ifdef MAICHAT_PDF_MAC_NATIVE
bool showMacPdfPreview(const QString &path, const QString &displayName);
#endif

namespace {

constexpr qint64 kMaxPreviewBytes = 100LL * 1024 * 1024;

#ifdef MAICHAT_PDF_WINDOWS_NATIVE
class WindowsPdfDialog final : public QDialog {
public:
  WindowsPdfDialog(const QString &path, const QString &displayName,
                   QWidget *parent)
      : QDialog(parent), path_(path) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(displayName);
    resize(1000, 760);
    setMinimumSize(640, 480);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 12);
    auto *toolbar = new QHBoxLayout;
    auto *title = new QLabel(displayName, this);
    title->setStyleSheet(
        QStringLiteral("font-size:15px;font-weight:600;color:#172033;"));
    toolbar->addWidget(title);
    toolbar->addStretch();
    previous_ = new QPushButton(QStringLiteral("上一页"), this);
    next_ = new QPushButton(QStringLiteral("下一页"), this);
    zoomOut_ = new QPushButton(QStringLiteral("−"), this);
    zoomIn_ = new QPushButton(QStringLiteral("+"), this);
    pageLabel_ = new QLabel(QStringLiteral("正在读取 PDF…"), this);
    pageLabel_->setObjectName(QStringLiteral("pdfPreviewPageLabel"));
    for (QPushButton *button : {previous_, next_, zoomOut_, zoomIn_})
      button->setEnabled(false);
    toolbar->addWidget(previous_);
    toolbar->addWidget(pageLabel_);
    toolbar->addWidget(next_);
    toolbar->addSpacing(12);
    toolbar->addWidget(zoomOut_);
    toolbar->addWidget(zoomIn_);
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    toolbar->addWidget(close);
    layout->addLayout(toolbar);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(false);
    scroll_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    scroll_->setStyleSheet(
        QStringLiteral("QScrollArea{background:#e9edf2;border:none;}"));
    image_ = new QLabel(scroll_);
    image_->setObjectName(QStringLiteral("pdfPreviewPageImage"));
    image_->setAlignment(Qt::AlignCenter);
    image_->setStyleSheet(QStringLiteral("background:white;"));
    scroll_->setWidget(image_);
    layout->addWidget(scroll_, 1);

    connect(previous_, &QPushButton::clicked, this,
            [this] { requestPage(currentPage_ - 1); });
    connect(next_, &QPushButton::clicked, this,
            [this] { requestPage(currentPage_ + 1); });
    connect(zoomOut_, &QPushButton::clicked, this, [this] {
      scale_ = qMax(0.25, scale_ / 1.25);
      updateImage();
    });
    connect(zoomIn_, &QPushButton::clicked, this, [this] {
      scale_ = qMin(3.0, scale_ * 1.25);
      updateImage();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(&watcher_, &QFutureWatcher<PdfPreviewPage>::finished, this, [this] {
      const PdfPreviewPage page = watcher_.result();
      if (!page.error.isEmpty() || page.image.isNull()) {
        pageLabel_->setText(
            page.error.isEmpty() ? QStringLiteral("页面无法预览") : page.error);
        return;
      }
      source_ = page.image;
      pageCount_ = page.pageCount;
      currentPage_ = loadingPage_;
      if (scale_ == 0)
        scale_ = qMin(1.0, qMax(0.25, (scroll_->viewport()->width() - 20.0) /
                                          source_.width()));
      updateImage();
      pageLabel_->setText(
          QStringLiteral("%1 / %2").arg(currentPage_ + 1).arg(pageCount_));
      previous_->setEnabled(currentPage_ > 0);
      next_->setEnabled(currentPage_ + 1 < pageCount_);
      zoomOut_->setEnabled(true);
      zoomIn_->setEnabled(true);
      if (queuedPage_ >= 0 && queuedPage_ != currentPage_) {
        const int nextPage = queuedPage_;
        queuedPage_ = -1;
        requestPage(nextPage);
      }
    });
    requestPage(0);
  }

private:
  void requestPage(int index) {
    if (index < 0 || (pageCount_ > 0 && index >= pageCount_))
      return;
    if (watcher_.isRunning()) {
      queuedPage_ = index;
      return;
    }
    loadingPage_ = index;
    pageLabel_->setText(QStringLiteral("正在读取第 %1 页…").arg(index + 1));
    previous_->setEnabled(false);
    next_->setEnabled(false);
    watcher_.setFuture(QtConcurrent::run(
        [path = path_, index] { return renderWindowsPdfPage(path, index); }));
  }

  void updateImage() {
    if (source_.isNull())
      return;
    const QSize size(qMax(1, qRound(source_.width() * scale_)),
                     qMax(1, qRound(source_.height() * scale_)));
    image_->setPixmap(QPixmap::fromImage(source_).scaled(
        size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    image_->resize(size);
  }

  QString path_;
  QScrollArea *scroll_ = nullptr;
  QLabel *image_ = nullptr;
  QLabel *pageLabel_ = nullptr;
  QPushButton *previous_ = nullptr;
  QPushButton *next_ = nullptr;
  QPushButton *zoomOut_ = nullptr;
  QPushButton *zoomIn_ = nullptr;
  QFutureWatcher<PdfPreviewPage> watcher_;
  QImage source_;
  int loadingPage_ = 0;
  int currentPage_ = 0;
  int pageCount_ = 0;
  int queuedPage_ = -1;
  qreal scale_ = 0;
};
#endif

} // namespace

bool isPreviewablePdfFile(const QString &path, QString *error) {
  const QFileInfo info(path);
  if (!info.isFile()) {
    if (error)
      *error = QStringLiteral("PDF 文件不存在。");
    return false;
  }
  if (info.size() < 8 || info.size() > kMaxPreviewBytes) {
    if (error)
      *error = QStringLiteral("PDF 文件大小不在可预览范围内。");
    return false;
  }
  QFile file(info.absoluteFilePath());
  if (!file.open(QIODevice::ReadOnly) || file.read(5) != "%PDF-") {
    if (error)
      *error = QStringLiteral("文件不是有效的 PDF。");
    return false;
  }
  if (error)
    error->clear();
  return true;
}

void showPdfPreview(QWidget *parent, const QString &path,
                    const QString &displayName) {
  QString error;
  if (!isPreviewablePdfFile(path, &error)) {
    QMessageBox::warning(parent, QStringLiteral("无法预览 PDF"), error);
    return;
  }
#ifdef MAICHAT_PDF_WINDOWS_NATIVE
  const QString title =
      displayName.isEmpty() ? QFileInfo(path).fileName() : displayName;
  auto *dialog = new WindowsPdfDialog(path, title, parent);
  dialog->show();
  dialog->raise();
  dialog->activateWindow();
#elif defined(MAICHAT_PDF_MAC_NATIVE)
  if (!showMacPdfPreview(path, displayName))
    QMessageBox::warning(parent, QStringLiteral("无法预览 PDF"),
                         QStringLiteral("PDF 内容无法读取。"));
#else
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
    QMessageBox::warning(parent, QStringLiteral("无法预览 PDF"),
                         QStringLiteral("未找到可打开 PDF 的应用。"));
#endif
}
