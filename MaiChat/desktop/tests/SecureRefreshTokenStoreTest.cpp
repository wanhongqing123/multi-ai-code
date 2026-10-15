// 文件作用：用独立命名空间验证桌面刷新令牌在系统安全存储中的写入、读取和删除。
// 运行边界：只使用随机测试 service 与假令牌；析构时再次删除，绝不读取正式登录项。
#include <QTest>
#include <QUuid>

#include "auth/SecureRefreshTokenStore.h"

class SecureRefreshTokenStoreTest : public QObject {
    Q_OBJECT

private slots:
    void roundTripsDummyTokenAndRemovesIt();
};

void SecureRefreshTokenStoreTest::roundTripsDummyTokenAndRemovesIt() {
    const QString service = QStringLiteral("com.maichat.desktop.auth.test.") +
                            QUuid::createUuid().toString(QUuid::WithoutBraces);
    SecureRefreshTokenStore store(service, QStringLiteral("round_trip_token"));
    struct Cleanup {
        SecureRefreshTokenStore& store;
        ~Cleanup() { store.remove(); }
    } cleanup{store};
    const QString dummyToken(43, QLatin1Char('T'));

    QVERIFY(store.read().isEmpty());
    QVERIFY(store.write(dummyToken));
    QCOMPARE(store.read(), dummyToken);
    QVERIFY(store.remove());
    QVERIFY(store.read().isEmpty());
}

QTEST_GUILESS_MAIN(SecureRefreshTokenStoreTest)
#include "SecureRefreshTokenStoreTest.moc"
