#include <QtTest/QtTest>

#include <QFile>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "MaiTool.h"
#include "MaiChatTools/DesktopVisionTools.h"

class DesktopVisionToolsTest : public QObject {
    Q_OBJECT

private slots:
    void detectsFacesAndWritesPersonMaskFromWorkspaceImage();
};

void DesktopVisionToolsTest::detectsFacesAndWritesPersonMaskFromWorkspaceImage() {
    Q_INIT_RESOURCE(resources);
    QFile faceModel(QStringLiteral(":/maichat/models/face_detection_yunet_2023mar.onnx"));
    QFile personModel(QStringLiteral(":/maichat/models/human_segmentation_pphumanseg_2023mar.onnx"));
    QVERIFY(faceModel.size() > 200000);
    QVERIFY(personModel.size() > 6000000);

    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString inputPath = workspace.filePath(QStringLiteral("人像测试.jpg"));
    QVERIFY(QFile::copy(QString::fromUtf8(MAICHAT_LENA_PATH), inputPath));

    MaiToolRegistry registry;
    registerDesktopVisionTools(registry);
    MaiTool* detect = registry.find("detect_faces");
    MaiTool* segment = registry.find("segment_person");
    QVERIFY(detect != nullptr);
    QVERIFY(segment != nullptr);
    QVERIFY(!detect->requiresApproval("{}"));
    QVERIFY(segment->requiresApproval("{}"));

    MaiToolContext context;
    context.root = workspace.path().toUtf8().toStdString();
    const MaiToolResult faces = detect->execute(u8R"({"path":"人像测试.jpg"})", context);
    QVERIFY2(!faces.hasError(), faces.error().message().c_str());
    const QJsonObject found = QJsonDocument::fromJson(QByteArray::fromStdString(faces.output())).object();
    QVERIFY(found.value(QStringLiteral("faces")).toArray().size() >= 1);
    QCOMPARE(found.value(QStringLiteral("source_path")).toString(), QStringLiteral("人像测试.jpg"));
    QCOMPARE(found.value(QStringLiteral("coordinate_origin")).toString(), QStringLiteral("top_left"));

    const MaiToolResult mask = segment->execute(u8R"({"path":"人像测试.jpg"})", context);
    QVERIFY2(!mask.hasError(), mask.error().message().c_str());
    const QJsonObject saved = QJsonDocument::fromJson(QByteArray::fromStdString(mask.output())).object();
    const QString maskPath = workspace.filePath(saved.value(QStringLiteral("path")).toString());
    QVERIFY(QFile::exists(maskPath));
    QImageReader reader(maskPath);
    QCOMPARE(reader.size(), QSize(512, 512));
    QVERIFY(detect->execute(R"({"path":"../lena.jpg"})", context).hasError());
}

QTEST_MAIN(DesktopVisionToolsTest)
#include "DesktopVisionToolsTest.moc"
