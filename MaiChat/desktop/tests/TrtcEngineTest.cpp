#include <QtTest/QtTest>

#include <QJsonDocument>
#include <QJsonObject>
#include <memory>

#include "remote/TrtcEngine.h"

using namespace RemoteDesktop;

class TrtcEngineTest : public QObject {
    Q_OBJECT

private slots:
    void reportsSdkVersionWhenAvailable();
    void rejectsInvalidProxyWithoutFallingBackToDirect();
    void acceptsValidProxyConfigWhenAvailable();
    void refusesIncompleteRoomParams();
    void stopIsIdempotent();
    void screenShareEncodingPolicyKeepsStableCapWithoutDynamicResolution();
    void screenShareEncodingPolicyInvokesExperimentalApiWithSourceAspect();
};

namespace {

TrtcRoomParams validParams() {
    TrtcRoomParams params;
    params.sdkAppId = 1600148979;
    params.userId = QStringLiteral("desktop-im");
    params.userSig = QStringLiteral("fake-usersig-for-param-validation");
    params.roomId = QStringLiteral("mc-test-0001");
    return params;
}

}  // namespace

void TrtcEngineTest::reportsSdkVersionWhenAvailable() {
    std::unique_ptr<ITrtcEngine> engine(createTrtcEngine());
    QVERIFY(engine != nullptr);

    if (!isTrtcAvailable()) {
        // 未 vendoring SDK 的平台落到空实现，其余功能不应被影响。
        QVERIFY(engine->sdkVersion().isEmpty());
        QVERIFY(!engine->isActive());
        return;
    }

    // 能取到版本号即证明头文件、导入库、运行时 DLL 三者都接通了——
    // 这正是本用例作为 SDK 接入 smoke 的价值。
    const QString version = engine->sdkVersion();
    QVERIFY2(!version.isEmpty(), "TRTC SDK 已启用但取不到版本号，检查 liteav.dll 是否可加载");
    QVERIFY(version.contains(QLatin1Char('.')));
    QVERIFY(!engine->isActive());
}

void TrtcEngineTest::rejectsInvalidProxyWithoutFallingBackToDirect() {
    if (!isTrtcAvailable()) return;

    TrtcNetworkProxyConfig proxy;
    proxy.enabled = true;
    proxy.host.clear();
    std::unique_ptr<ITrtcEngine> engine(createTrtcEngine(proxy));
    QVERIFY(engine != nullptr);
    QVERIFY(!engine->initializationError().isEmpty());
    QVERIFY(engine->sdkVersion().isEmpty());
    QVERIFY(!engine->isActive());
}

void TrtcEngineTest::acceptsValidProxyConfigWhenAvailable() {
    if (!isTrtcAvailable()) return;

    TrtcNetworkProxyConfig proxy;
    proxy.enabled = true;
    proxy.host = QStringLiteral("127.0.0.1");
    proxy.port = 1082;
    std::unique_ptr<ITrtcEngine> engine(createTrtcEngine(proxy));
    QVERIFY2(engine->initializationError().isEmpty(),
             qPrintable(engine->initializationError()));
    QVERIFY(!engine->sdkVersion().isEmpty());
}

void TrtcEngineTest::refusesIncompleteRoomParams() {
    std::unique_ptr<ITrtcEngine> engine(createTrtcEngine());

    // 参数缺失时必须直接失败，绝不能带着空 userSig 去进房。
    TrtcRoomParams noSig = validParams();
    noSig.userSig.clear();
    QVERIFY(!engine->startScreenShare(noSig));

    TrtcRoomParams noRoom = validParams();
    noRoom.roomId.clear();
    QVERIFY(!engine->startViewing(noRoom, nullptr));

    TrtcRoomParams noApp = validParams();
    noApp.sdkAppId = 0;
    QVERIFY(!engine->startScreenShare(noApp));

    QVERIFY(!engine->isActive());
}

void TrtcEngineTest::stopIsIdempotent() {
    std::unique_ptr<ITrtcEngine> engine(createTrtcEngine());
    // 未开始时 stop、以及重复 stop 都不得崩溃（会话异常路径会反复调用）。
    engine->stop();
    engine->stop();
    QVERIFY(!engine->isActive());
}

void TrtcEngineTest::screenShareEncodingPolicyKeepsStableCapWithoutDynamicResolution() {
    const TrtcEngineInternal::ScreenShareEncodingPolicy& policy =
        TrtcEngineInternal::screenShareEncodingPolicy();

    // 114/0 是 TRTCVideoResolution_1920_1080 + Landscape 的公开稳定枚举值。
    QCOMPARE(policy.videoResolution, 114);
    QCOMPARE(policy.resolutionMode, 0);
    QCOMPARE(policy.maxWidth, 1920);
    QCOMPARE(policy.maxHeight, 1080);
    QCOMPARE(policy.frameRate, 10);
    QCOMPARE(policy.bitrateKbps, 1600);
    QVERIFY(!policy.enableAdjustResolution);
}

void TrtcEngineTest::screenShareEncodingPolicyInvokesExperimentalApiWithSourceAspect() {
    int callCount = 0;
    QByteArray capturedRequest;
    const QByteArray expectedResponse("sdk-response");

    const QByteArray response =
        TrtcEngineInternal::applyScreenShareEncodingExperimentalPolicy(
            [&](const QByteArray& request) {
                ++callCount;
                capturedRequest = request;
                return expectedResponse;
            });

    QCOMPARE(callCount, 1);
    QCOMPARE(response, expectedResponse);

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(capturedRequest, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(document.isObject());

    const QJsonObject request = document.object();
    QCOMPARE(request.value(QStringLiteral("api")).toString(),
             QStringLiteral("setVideoEncodeParamEx"));
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.size(), 2);
    QCOMPARE(params.value(QStringLiteral("streamType")).toInt(), 2);  // Sub
    QCOMPARE(params.value(QStringLiteral("screenEncodingAspectRatio")).toInt(), 0);
}

QTEST_MAIN(TrtcEngineTest)
#include "TrtcEngineTest.moc"
