import XCTest
@testable import MaiChatCore

final class RemoteDesktopSignalTests: XCTestCase {
    func testEncodesAndDecodesDesktopCompatibleInvite() throws {
        let signal = RemoteDesktopSignal(
            kind: .invite,
            sessionID: "session-1",
            roomID: "mc-ios-user-room-1"
        )

        let text = try signal.encodedText()

        XCTAssertTrue(text.hasPrefix("\u{2063}\u{200B}[remote-desktop]"))
        XCTAssertEqual(RemoteDesktopSignal.decodeText(text), signal)
    }

    func testRejectsUnsupportedProtocolVersionButStillRecognizesSignalPrefix() {
        let text = RemoteDesktopSignal.prefix + #"{"v":2,"type":"stop","sessionId":"old"}"#

        XCTAssertTrue(RemoteDesktopSignal.isSignalText(text))
        XCTAssertNil(RemoteDesktopSignal.decodeText(text))
    }

    func testNoticeWithoutCodeIsInvalid() {
        let text = RemoteDesktopSignal.prefix + #"{"v":1,"type":"notice","sessionId":"s1"}"#

        XCTAssertNil(RemoteDesktopSignal.decodeText(text))
    }

    func testNormalChatMessageIsNotRemoteSignal() {
        XCTAssertFalse(RemoteDesktopSignal.isSignalText("你好"))
        XCTAssertNil(RemoteDesktopSignal.decodeText("你好"))
    }
}
