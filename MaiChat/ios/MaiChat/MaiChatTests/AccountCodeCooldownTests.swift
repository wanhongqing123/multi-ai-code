// 文件作用：验证生产短信门禁的成功、失败、并发与绝对时间边界；不调用短信服务。
import XCTest
import MaiChatCore

final class AccountCodeCooldownTests: XCTestCase {
    func testSuccessRequiresSixtySecondsAndKeepsThePhoneAcrossFormatChanges() {
        var cooldown = AccountCodeCooldown()
        let sentAt = Date(timeIntervalSince1970: 1_000)
        XCTAssertTrue(cooldown.beginRequest(phone: "13900000001", at: sentAt))
        XCTAssertFalse(cooldown.beginRequest(phone: "+86 139-0000-0001", at: sentAt))
        cooldown.finishRequest(phone: "+86 139-0000-0001", resendAfter: 10, at: sentAt)

        XCTAssertEqual(cooldown.retryAfter(phone: "13900000001", at: sentAt), 60)
        XCTAssertEqual(cooldown.retryAfter(phone: "13900000001",
                                            at: sentAt.addingTimeInterval(59.1)), 1)
        XCTAssertFalse(cooldown.beginRequest(phone: "+8613900000001",
                                             at: sentAt.addingTimeInterval(59.99)))
        XCTAssertTrue(cooldown.beginRequest(phone: "13900000001",
                                            at: sentAt.addingTimeInterval(60)))
    }

    func testFailureReleasesTheRequestWithoutStartingCooldown() {
        var cooldown = AccountCodeCooldown()
        let now = Date(timeIntervalSince1970: 1_000)
        XCTAssertTrue(cooldown.beginRequest(phone: "13900000001", at: now))
        XCTAssertTrue(cooldown.isRequesting(phone: "13900000001"))
        cooldown.finishRequest(phone: "13900000001", resendAfter: nil, at: now)

        XCTAssertFalse(cooldown.isRequesting(phone: "13900000001"))
        XCTAssertEqual(cooldown.retryAfter(phone: "13900000001", at: now), 0)
        XCTAssertTrue(cooldown.beginRequest(phone: "13900000001", at: now))
    }

    func testChangingPhoneKeepsIndependentDeadlinesAndHonorsLongerServerWait() {
        var cooldown = AccountCodeCooldown()
        let sentAt = Date(timeIntervalSince1970: 1_000)
        XCTAssertTrue(cooldown.beginRequest(phone: "13900000001", at: sentAt))
        cooldown.finishRequest(phone: "13900000001", resendAfter: 120, at: sentAt)
        XCTAssertTrue(cooldown.beginRequest(phone: "13900000002", at: sentAt))
        cooldown.finishRequest(phone: "13900000002", resendAfter: 60, at: sentAt)

        // 界面在后台不逐秒运行；恢复后直接按当前时间计算，改回旧号也保留期限。
        let resumedAt = sentAt.addingTimeInterval(90)
        XCTAssertEqual(cooldown.retryAfter(phone: "13900000001", at: resumedAt), 30)
        XCTAssertEqual(cooldown.retryAfter(phone: "13900000002", at: resumedAt), 0)
        XCTAssertFalse(cooldown.beginRequest(phone: "13900000001", at: resumedAt))
        XCTAssertTrue(cooldown.beginRequest(phone: "13900000002", at: resumedAt))
        XCTAssertTrue(cooldown.beginRequest(phone: "13900000001",
                                            at: sentAt.addingTimeInterval(120)))
    }
}
