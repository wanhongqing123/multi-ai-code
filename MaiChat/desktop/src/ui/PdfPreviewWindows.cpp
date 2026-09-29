#include "ui/PdfPreviewDialog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <QByteArray>
#include <QFile>

#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

PdfPreviewPage renderWindowsPdfPage(const QString &path, int pageIndex) {
  PdfPreviewPage result;
  bool apartmentInitialized = false;
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    apartmentInitialized = true;
    using namespace winrt::Windows;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 100LL * 1024 * 1024)
      throw std::runtime_error("PDF file could not be read");
    const QByteArray pdfBytes = file.readAll();
    Storage::Streams::InMemoryRandomAccessStream source;
    {
      Storage::Streams::DataWriter writer(source);
      const auto *begin =
          reinterpret_cast<const std::uint8_t *>(pdfBytes.constData());
      writer.WriteBytes(winrt::array_view<const std::uint8_t>(
          begin, begin + pdfBytes.size()));
      writer.StoreAsync().get();
      writer.FlushAsync().get();
      writer.DetachStream();
    }
    source.Seek(0);
    const auto document =
        Data::Pdf::PdfDocument::LoadFromStreamAsync(source).get();
    result.pageCount = static_cast<int>(document.PageCount());
    if (pageIndex < 0 || pageIndex >= result.pageCount) {
      result.error = QStringLiteral("PDF 页码无效。");
    } else {
      const auto page = document.GetPage(static_cast<std::uint32_t>(pageIndex));
      const auto size = page.Size();
      if (size.Width <= 0 || size.Height <= 0) {
        result.error = QStringLiteral("PDF 页面尺寸无效。");
      } else {
        const auto width = static_cast<std::uint32_t>(std::min(
            1800.0,
            std::max(720.0, std::ceil(static_cast<double>(size.Width) * 2.0))));
        const auto height = static_cast<std::uint32_t>(std::min(
            3000.0, std::max(1.0, std::round(static_cast<double>(size.Height) *
                                             width / size.Width))));
        Data::Pdf::PdfPageRenderOptions options;
        options.DestinationWidth(width);
        options.DestinationHeight(height);
        Storage::Streams::InMemoryRandomAccessStream stream;
        page.RenderToStreamAsync(stream, options).get();
        const std::uint64_t bytes = stream.Size();
        if (bytes == 0 || bytes > 50u * 1024 * 1024) {
          result.error = QStringLiteral("PDF 页面图像过大。");
        } else {
          auto reader =
              Storage::Streams::DataReader(stream.GetInputStreamAt(0));
          if (reader.LoadAsync(static_cast<std::uint32_t>(bytes)).get() !=
              bytes) {
            result.error = QStringLiteral("PDF 页面读取不完整。");
          } else {
            std::vector<std::uint8_t> imageBytes(
                static_cast<std::size_t>(bytes));
            reader.ReadBytes(winrt::array_view<std::uint8_t>(imageBytes));
            result.image = QImage::fromData(
                imageBytes.data(), static_cast<int>(imageBytes.size()));
            if (result.image.isNull())
              result.error = QStringLiteral("PDF 页面无法解码。");
          }
        }
      }
    }
  } catch (const winrt::hresult_error &failure) {
    result.error = QString::fromWCharArray(failure.message().c_str());
  } catch (const std::exception &failure) {
    result.error = QString::fromUtf8(failure.what());
  }
  if (apartmentInitialized)
    winrt::uninit_apartment();
  return result;
}
