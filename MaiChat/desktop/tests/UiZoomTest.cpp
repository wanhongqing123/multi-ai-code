#include <QtTest/QtTest>

#include "ui/UiZoom.h"

class UiZoomTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup();
    void clampsFactorRange();
    void scalesPixelValues();
    void scalesPixelValuesInQss();
};

void UiZoomTest::cleanup() {
    UiZoom::setFactor(1.0);
}

void UiZoomTest::clampsFactorRange() {
    QCOMPARE(UiZoom::setFactor(1.0), 1.0);
    QCOMPARE(UiZoom::setFactor(9.0), UiZoom::maxFactor());
    QCOMPARE(UiZoom::setFactor(0.1), UiZoom::minFactor());
    QCOMPARE(UiZoom::setFactor(1.3), 1.3);
}

void UiZoomTest::scalesPixelValues() {
    UiZoom::setFactor(1.5);
    QCOMPARE(UiZoom::s(10), 15);
    QCOMPARE(UiZoom::s(0), 0);
    // 1px 边框等非零最小值不被缩没。
    UiZoom::setFactor(0.8);
    QCOMPARE(UiZoom::s(1), 1);
}

void UiZoomTest::scalesPixelValuesInQss() {
    UiZoom::setFactor(1.5);
    const QString scaled = UiZoom::scaleQss(
        QStringLiteral("font-size: 13px; padding: 0px 8px; border: 1px solid #000;"));
    QCOMPARE(scaled, QStringLiteral("font-size: 20px; padding: 0px 12px; border: 2px solid #000;"));

    // 倍率 1.0 时原样返回。
    UiZoom::setFactor(1.0);
    QCOMPARE(UiZoom::scaleQss(QStringLiteral("margin: 7px;")), QStringLiteral("margin: 7px;"));

    // Markdown 渲染器输出的 HTML 内嵌 <style> 也按同一套规则缩放（消息正文随整体缩放）。
    UiZoom::setFactor(1.5);
    QCOMPARE(UiZoom::scaleQss(QStringLiteral("<style>body{font-size:14px;}h1{font-size:22px;}</style>")),
             QStringLiteral("<style>body{font-size:21px;}h1{font-size:33px;}</style>"));
}

QTEST_MAIN(UiZoomTest)
#include "UiZoomTest.moc"
