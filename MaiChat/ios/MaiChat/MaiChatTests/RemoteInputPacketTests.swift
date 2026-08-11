import XCTest
@testable import MaiChatCore

final class RemoteInputPacketTests: XCTestCase {
    func testEncodesDesktopCompatibleEventTagsAndFields() throws {
        let packet = RemoteInputPacket(
            sessionID: "session-1",
            sequence: 7,
            events: [
                .mouseMove(x: 0.25, y: 0.75),
                .mouseButton(button: .right, pressed: true, x: 0.4, y: 0.6),
                .mouseWheel(delta: -120, x: 0.5, y: 0.5),
                .key(code: 0x0D, pressed: true),
                .text("你好"),
                .releaseAll,
            ]
        )

        let object = try XCTUnwrap(
            JSONSerialization.jsonObject(with: packet.encodedData()) as? [String: Any]
        )
        XCTAssertEqual(object["v"] as? Int, 1)
        XCTAssertEqual(object["s"] as? String, "session-1")
        XCTAssertEqual(object["n"] as? Int, 7)

        let events = try XCTUnwrap(object["e"] as? [[String: Any]])
        XCTAssertEqual(events.map { $0["t"] as? String }, ["m", "b", "w", "k", "x", "r"])
        XCTAssertEqual(events[1]["b"] as? Int, RemoteMouseButton.right.rawValue)
        XCTAssertEqual(events[1]["d"] as? Bool, true)
        XCTAssertEqual(events[2]["w"] as? Int, -120)
        XCTAssertEqual(events[3]["k"] as? Int, 0x0D)
        XCTAssertEqual(events[4]["s"] as? String, "你好")
    }

    func testClampsCoordinatesToDesktopProtocolRange() throws {
        let packet = RemoteInputPacket(
            sessionID: "session-2",
            sequence: 1,
            events: [.mouseMove(x: -.infinity, y: 4)]
        )
        let object = try XCTUnwrap(
            JSONSerialization.jsonObject(with: packet.encodedData()) as? [String: Any]
        )
        let events = try XCTUnwrap(object["e"] as? [[String: Any]])
        XCTAssertEqual(events[0]["x"] as? Double, 0)
        XCTAssertEqual(events[0]["y"] as? Double, 1)
        XCTAssertTrue(packet.fitsInOnePacket())
    }

    func testEncodesTextBackspaceAndReturnForDesktopInjection() throws {
        let packet = RemoteInputPacket(
            sessionID: "session-keyboard",
            sequence: 9,
            events: [
                .text("你好"),
                .key(code: 0x08, pressed: true),
                .key(code: 0x08, pressed: false),
                .key(code: 0x0D, pressed: true),
                .key(code: 0x0D, pressed: false),
            ]
        )

        let object = try XCTUnwrap(
            JSONSerialization.jsonObject(with: packet.encodedData()) as? [String: Any]
        )
        let events = try XCTUnwrap(object["e"] as? [[String: Any]])
        XCTAssertEqual(events.map { $0["t"] as? String }, ["x", "k", "k", "k", "k"])
        XCTAssertEqual(events[0]["s"] as? String, "你好")
        XCTAssertEqual(events.dropFirst().compactMap { $0["k"] as? Int }, [8, 8, 13, 13])
        XCTAssertEqual(
            events.dropFirst().compactMap { $0["d"] as? Bool },
            [true, false, true, false]
        )
    }

    func testEscapedTextMustFitEncodedPacketBudget() {
        let oversized = RemoteInputPacket(
            sessionID: "session-escaped-text",
            sequence: 1,
            events: [.text(String(repeating: "\\", count: 700))]
        )
        let fitting = RemoteInputPacket(
            sessionID: "session-escaped-text",
            sequence: 1,
            events: [.text(String(repeating: "\\", count: 300))]
        )

        XCTAssertFalse(oversized.fitsInOnePacket())
        XCTAssertTrue(fitting.fitsInOnePacket())
    }
}
