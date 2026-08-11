import XCTest
import CoreGraphics
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

    func testDecodesDesktopCaptureGeometryFixture() throws {
        let text = RemoteDesktopSignal.prefix
            + #"{"v":1,"type":"accept","sessionId":"session-geometry-1","roomId":"mc-room-1","captureGeometry":{"sourceWidth":2560,"sourceHeight":1600,"captureX":0,"captureY":0,"captureWidth":2560,"captureHeight":1600,"contentMode":"fit","revision":1}}"#
        let expectedGeometry = makeGeometry()

        let signal = try XCTUnwrap(RemoteDesktopSignal.decodeText(text))

        XCTAssertEqual(signal.kind, .accept)
        XCTAssertEqual(signal.sessionID, "session-geometry-1")
        XCTAssertEqual(signal.roomID, "mc-room-1")
        XCTAssertEqual(signal.captureGeometry, expectedGeometry)
        XCTAssertEqual(signal.captureGeometryDisposition, .accepted(expectedGeometry))
    }

    func testCaptureGeometryRoundTrips() throws {
        let geometry = makeGeometry()
        let signal = RemoteDesktopSignal(
            kind: .accept,
            sessionID: "session-geometry-1",
            roomID: "mc-room-1",
            captureGeometry: geometry
        )

        let decoded = RemoteDesktopSignal.decodeText(try signal.encodedText())

        XCTAssertEqual(decoded, signal)
        XCTAssertEqual(decoded?.captureGeometry, geometry)
    }

    func testLegacyAcceptWithoutCaptureGeometryUsesEncodedFrameFallback() throws {
        let text = RemoteDesktopSignal.prefix
            + #"{"v":1,"type":"accept","sessionId":"legacy-session","roomId":"legacy-room"}"#

        let signal = try XCTUnwrap(RemoteDesktopSignal.decodeText(text))

        XCTAssertEqual(signal.kind, .accept)
        XCTAssertNil(signal.captureGeometry)
        XCTAssertEqual(signal.captureGeometryDisposition, .absent)
    }

    func testNonAcceptSignalIgnoresCaptureGeometry() throws {
        let geometry = makeGeometry()
        let constructed = RemoteDesktopSignal(
            kind: .invite,
            sessionID: "invite-with-geometry",
            captureGeometry: geometry
        )
        let encoded = try constructed.encodedText()
        let maliciousText = RemoteDesktopSignal.prefix
            + #"{"v":1,"type":"invite","sessionId":"invite-with-geometry","captureGeometry":{"sourceWidth":2560,"sourceHeight":1600,"captureX":0,"captureY":0,"captureWidth":2560,"captureHeight":1600,"contentMode":"fit","revision":1}}"#

        XCTAssertNil(constructed.captureGeometry)
        XCTAssertEqual(constructed.captureGeometryDisposition, .absent)
        XCTAssertFalse(encoded.contains("captureGeometry"))
        let decoded = try XCTUnwrap(RemoteDesktopSignal.decodeText(maliciousText))
        XCTAssertEqual(decoded.kind, .invite)
        XCTAssertNil(decoded.captureGeometry)
        XCTAssertEqual(decoded.captureGeometryDisposition, .absent)
    }

    func testInvalidRevisionDropsOnlyCaptureGeometry() throws {
        let text = RemoteDesktopSignal.prefix
            + #"{"v":1,"type":"accept","sessionId":"invalid-revision","roomId":"mc-room-1","captureGeometry":{"sourceWidth":2560,"sourceHeight":1600,"captureX":0,"captureY":0,"captureWidth":2560,"captureHeight":1600,"contentMode":"fit","revision":0}}"#
        let candidate = makeGeometry(revision: 0)

        let signal = try XCTUnwrap(RemoteDesktopSignal.decodeText(text))

        XCTAssertEqual(signal.kind, .accept)
        XCTAssertNil(signal.captureGeometry)
        XCTAssertEqual(
            signal.captureGeometryDisposition,
            .ignored(candidate, reason: "invalid-revision")
        )
    }

    func testOutOfBoundsCaptureDropsOnlyCaptureGeometry() throws {
        let text = RemoteDesktopSignal.prefix
            + #"{"v":1,"type":"accept","sessionId":"out-of-bounds","roomId":"mc-room-1","captureGeometry":{"sourceWidth":2560,"sourceHeight":1600,"captureX":2000,"captureY":0,"captureWidth":1000,"captureHeight":1600,"contentMode":"fit","revision":1}}"#
        let candidate = makeGeometry(captureX: 2_000, captureWidth: 1_000)

        let signal = try XCTUnwrap(RemoteDesktopSignal.decodeText(text))

        XCTAssertEqual(signal.kind, .accept)
        XCTAssertNil(signal.captureGeometry)
        XCTAssertEqual(
            signal.captureGeometryDisposition,
            .ignored(candidate, reason: "capture-out-of-bounds")
        )
    }

    func testUnsupportedModeDropsOnlyCaptureGeometry() throws {
        let text = RemoteDesktopSignal.prefix
            + #"{"v":1,"type":"accept","sessionId":"bad-mode","roomId":"mc-room-1","captureGeometry":{"sourceWidth":2560,"sourceHeight":1600,"captureX":0,"captureY":0,"captureWidth":2560,"captureHeight":1600,"contentMode":"fill","revision":1}}"#
        let candidate = makeGeometry(contentMode: "fill")

        let signal = try XCTUnwrap(RemoteDesktopSignal.decodeText(text))

        XCTAssertEqual(signal.kind, .accept)
        XCTAssertNil(signal.captureGeometry)
        XCTAssertEqual(
            signal.captureGeometryDisposition,
            .ignored(candidate, reason: "unsupported-content-mode")
        )
    }

    func testOversizedGeometryDropsOnlyCaptureGeometry() throws {
        let text = RemoteDesktopSignal.prefix
            + #"{"v":1,"type":"accept","sessionId":"oversized","roomId":"mc-room-1","captureGeometry":{"sourceWidth":65536,"sourceHeight":1600,"captureX":0,"captureY":0,"captureWidth":65536,"captureHeight":1600,"contentMode":"fit","revision":1}}"#
        let candidate = makeGeometry(sourceWidth: 65_536, captureWidth: 65_536)

        let signal = try XCTUnwrap(RemoteDesktopSignal.decodeText(text))

        XCTAssertEqual(signal.kind, .accept)
        XCTAssertNil(signal.captureGeometry)
        XCTAssertEqual(
            signal.captureGeometryDisposition,
            .ignored(candidate, reason: "source-size-too-large")
        )
    }

    func testMalformedCaptureGeometryDropsOnlyCaptureGeometry() throws {
        let text = RemoteDesktopSignal.prefix
            + #"{"v":1,"type":"accept","sessionId":"malformed","roomId":"mc-room-1","captureGeometry":"not-an-object"}"#

        let signal = try XCTUnwrap(RemoteDesktopSignal.decodeText(text))

        XCTAssertEqual(signal.kind, .accept)
        XCTAssertNil(signal.captureGeometry)
        XCTAssertEqual(
            signal.captureGeometryDisposition,
            .ignored(nil, reason: "malformed")
        )
    }

    func testCaptureGeometryHandlesFixedAndAdjustedEncoderFrames() throws {
        let geometry = makeGeometry()
        let fixedEncoderFrame = CGRect(x: 0, y: 0, width: 1_920, height: 1_080)
        let adjustedEncoderFrame = CGRect(x: 0, y: 0, width: 1_728, height: 1_080)

        let fixedActiveRect = try XCTUnwrap(
            RemoteDesktopCoordinateMapper.activeContentRect(
                encodedFrameRect: fixedEncoderFrame,
                captureGeometry: geometry
            )
        )
        let adjustedActiveRect = try XCTUnwrap(
            RemoteDesktopCoordinateMapper.activeContentRect(
                encodedFrameRect: adjustedEncoderFrame,
                captureGeometry: geometry
            )
        )

        assertRect(
            fixedActiveRect,
            equals: CGRect(x: 96, y: 0, width: 1_728, height: 1_080)
        )
        assertRect(adjustedActiveRect, equals: adjustedEncoderFrame)
        XCTAssertNil(
            RemoteDesktopCoordinateMapper.normalizedPoint(
                at: CGPoint(x: 50, y: 540),
                encodedFrameRect: fixedEncoderFrame,
                captureGeometry: geometry
            )
        )
        let fixedPoint = try XCTUnwrap(
            RemoteDesktopCoordinateMapper.normalizedPoint(
                at: CGPoint(x: 528, y: 810),
                encodedFrameRect: fixedEncoderFrame,
                captureGeometry: geometry
            )
        )
        let adjustedPoint = try XCTUnwrap(
            RemoteDesktopCoordinateMapper.normalizedPoint(
                at: CGPoint(x: 432, y: 810),
                encodedFrameRect: adjustedEncoderFrame,
                captureGeometry: geometry
            )
        )
        assertPoint(
            fixedPoint,
            equals: CGPoint(x: 0.25, y: 0.75)
        )
        assertPoint(adjustedPoint, equals: fixedPoint)
    }

    func testPartialCaptureMapsActiveRectIntoSourceNormalizedCoordinates() throws {
        let geometry = makeGeometry(
            captureX: 640,
            captureY: 400,
            captureWidth: 1_280,
            captureHeight: 800
        )
        let encodedFrame = CGRect(x: 0, y: 0, width: 1_920, height: 1_080)

        let point = try XCTUnwrap(
            RemoteDesktopCoordinateMapper.normalizedPoint(
                at: CGPoint(x: 528, y: 810),
                encodedFrameRect: encodedFrame,
                captureGeometry: geometry
            )
        )

        assertPoint(point, equals: CGPoint(x: 0.375, y: 0.625))
    }

    func testCoordinateMappingIsStableAcrossZoomAndPan() throws {
        let geometry = makeGeometry()
        let oneXFrame = CGRect(x: 0, y: 0, width: 960, height: 540)
        let zoomedAndPannedFrame = CGRect(x: -300, y: -120, width: 1_920, height: 1_080)

        let oneXPoint = try XCTUnwrap(
            RemoteDesktopCoordinateMapper.normalizedPoint(
                at: CGPoint(x: 264, y: 405),
                encodedFrameRect: oneXFrame,
                captureGeometry: geometry
            )
        )
        let zoomedAndPannedPoint = try XCTUnwrap(
            RemoteDesktopCoordinateMapper.normalizedPoint(
                at: CGPoint(x: 228, y: 690),
                encodedFrameRect: zoomedAndPannedFrame,
                captureGeometry: geometry
            )
        )

        assertPoint(oneXPoint, equals: CGPoint(x: 0.25, y: 0.75))
        assertPoint(zoomedAndPannedPoint, equals: oneXPoint)
    }

    func testClampedPartialCaptureStaysWithinSourceCaptureRect() throws {
        let geometry = makeGeometry(
            captureX: 640,
            captureY: 400,
            captureWidth: 1_280,
            captureHeight: 800
        )
        let encodedFrame = CGRect(x: 0, y: 0, width: 1_920, height: 1_080)

        let left = try XCTUnwrap(
            RemoteDesktopCoordinateMapper.normalizedPoint(
                at: CGPoint(x: -100, y: 540),
                encodedFrameRect: encodedFrame,
                captureGeometry: geometry,
                clamped: true
            )
        )
        let right = try XCTUnwrap(
            RemoteDesktopCoordinateMapper.normalizedPoint(
                at: CGPoint(x: 2_000, y: 540),
                encodedFrameRect: encodedFrame,
                captureGeometry: geometry,
                clamped: true
            )
        )

        assertPoint(left, equals: CGPoint(x: 0.25, y: 0.5))
        assertPoint(right, equals: CGPoint(x: 0.75, y: 0.5))
    }

    func testMissingGeometryPreservesLegacyEncodedFrameMapping() throws {
        let encodedFrame = CGRect(x: 0, y: 0, width: 1_920, height: 1_080)

        let activeRect = try XCTUnwrap(
            RemoteDesktopCoordinateMapper.activeContentRect(
                encodedFrameRect: encodedFrame,
                captureGeometry: nil
            )
        )
        let point = try XCTUnwrap(
            RemoteDesktopCoordinateMapper.normalizedPoint(
                at: CGPoint(x: 480, y: 270),
                encodedFrameRect: encodedFrame,
                captureGeometry: nil
            )
        )

        assertRect(activeRect, equals: encodedFrame)
        assertPoint(point, equals: CGPoint(x: 0.25, y: 0.25))
    }

    private func makeGeometry(
        sourceWidth: Int = 2_560,
        sourceHeight: Int = 1_600,
        captureX: Int = 0,
        captureY: Int = 0,
        captureWidth: Int = 2_560,
        captureHeight: Int = 1_600,
        contentMode: String = CaptureGeometry.fitContentMode,
        revision: Int = 1
    ) -> CaptureGeometry {
        CaptureGeometry(
            sourceWidth: sourceWidth,
            sourceHeight: sourceHeight,
            captureX: captureX,
            captureY: captureY,
            captureWidth: captureWidth,
            captureHeight: captureHeight,
            contentMode: contentMode,
            revision: revision
        )
    }

    private func assertPoint(
        _ actual: CGPoint,
        equals expected: CGPoint,
        accuracy: CGFloat = 0.000_001,
        file: StaticString = #filePath,
        line: UInt = #line
    ) {
        XCTAssertEqual(actual.x, expected.x, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(actual.y, expected.y, accuracy: accuracy, file: file, line: line)
    }

    private func assertRect(
        _ actual: CGRect,
        equals expected: CGRect,
        accuracy: CGFloat = 0.000_001,
        file: StaticString = #filePath,
        line: UInt = #line
    ) {
        XCTAssertEqual(actual.minX, expected.minX, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(actual.minY, expected.minY, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(actual.width, expected.width, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(actual.height, expected.height, accuracy: accuracy, file: file, line: line)
    }
}
