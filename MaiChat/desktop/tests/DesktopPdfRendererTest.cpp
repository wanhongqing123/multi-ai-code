#include <QtTest/QtTest>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "MaiPdfTool.h"
#include "agent/DesktopPdfRenderer.h"

class DesktopPdfRendererTest : public QObject {
  Q_OBJECT

private slots:
  void generatesPdfDirectlyFromChineseMarkdown();
  void generatesAPaginatedPdfFromChineseHtml();
};

void DesktopPdfRendererTest::generatesPdfDirectlyFromChineseMarkdown() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto tool = makeMaiPdfTool(renderDesktopPdf);
  MaiToolContext context;
  context.root = directory.path().toUtf8().toStdString();
  const QByteArray arguments = QJsonDocument(QJsonObject{
      {QStringLiteral("content"),
       QStringLiteral("# PDF 报告\n\n| 项目 | 结果 |\n| --- | --- |\n| 构建 | **通过** |")},
      {QStringLiteral("output_path"), QStringLiteral("报告.pdf")},
  }).toJson(QJsonDocument::Compact);
  const MaiToolResult result = tool->execute(arguments.toStdString(), context);
  QVERIFY2(!result.hasError(), result.error().message().c_str());
  QFile output(directory.filePath(QStringLiteral("报告.pdf")));
  QVERIFY(output.open(QIODevice::ReadOnly));
  const QByteArray bytes = output.readAll();
  QVERIFY(bytes.startsWith("%PDF-"));
  QVERIFY(bytes.contains("%%EOF"));
  QVERIFY(bytes.size() > 1000);
  QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("报告.html"))));
}

void DesktopPdfRendererTest::generatesAPaginatedPdfFromChineseHtml() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  QFile source(directory.filePath(QStringLiteral("报告.html")));
  QVERIFY(source.open(QIODevice::WriteOnly));
  QByteArray html =
      "<html><head><meta charset='utf-8'></head><body>"
      "<h1>PDF 报告</h1><table><tr><th>项目</th><th>结果</th></tr>";
  for (int i = 0; i < 100; ++i)
    html +=
        "<tr><td>第 " + QByteArray::number(i) + " 项</td><td>通过</td></tr>";
  html += "</table></body></html>";
  source.write(html);
  source.close();

  auto tool = makeMaiPdfTool(renderDesktopPdf);
  MaiToolContext context;
  context.root = directory.path().toUtf8().toStdString();
  const MaiToolResult result = tool->execute(
      R"({"source_path":"报告.html","output_path":"结果.pdf"})", context);
  QVERIFY2(!result.hasError(), result.error().message().c_str());
  QFile output(directory.filePath(QStringLiteral("结果.pdf")));
  QVERIFY(output.open(QIODevice::ReadOnly));
  const QByteArray bytes = output.readAll();
  QVERIFY(bytes.startsWith("%PDF-"));
  QVERIFY(bytes.contains("%%EOF"));
  QVERIFY(bytes.size() > 1000);
}

QTEST_MAIN(DesktopPdfRendererTest)
#include "DesktopPdfRendererTest.moc"
