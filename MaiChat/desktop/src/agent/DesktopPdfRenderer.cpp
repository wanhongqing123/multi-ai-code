#include "agent/DesktopPdfRenderer.h"

#include <QFile>
#include <QFont>
#include <QMarginsF>
#include <QPdfWriter>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>

#include <string>

namespace {

class SelfContainedPdfDocument final : public QTextDocument {
protected:
  QVariant loadResource(int type, const QUrl &name) override {
    // 模型写的 HTML 不得借 PDF 渲染读取工作区外的 file: 路径或发网络请求。
    if (name.scheme() != QStringLiteral("data"))
      return {};
    return QTextDocument::loadResource(type, name);
  }
};

} // namespace

MaiToolResult renderDesktopPdf(const std::string &htmlUtf8,
                               const std::string &absoluteOutputPath,
                               const std::atomic<bool> *cancel) {
  if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
    return MaiToolResult::failure(MaiErrorCode::Canceled,
                                  "PDF generation was canceled");
  SelfContainedPdfDocument document;
  QFont font;
  font.setPointSize(10);
  document.setDefaultFont(font);
  document.setHtml(
      QString::fromUtf8(htmlUtf8.data(), static_cast<int>(htmlUtf8.size())));

  QFile output(QString::fromUtf8(absoluteOutputPath.c_str()));
  if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return MaiToolResult::failure(
        MaiErrorCode::InvalidInput,
        "PDF output could not be created as a new file");
  QPdfWriter writer(&output);
  writer.setPageSize(QPagedPaintDevice::A4);
  writer.setPageMargins(QMarginsF(18, 18, 18, 18));
  writer.setResolution(144);
  document.print(&writer);
  if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
    return MaiToolResult::failure(MaiErrorCode::Canceled,
                                  "PDF generation was canceled");
  return MaiToolResult::success("PDF rendering completed");
}
