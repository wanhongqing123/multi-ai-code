import Foundation
import XCTest
@testable import MaiChatCore

final class DiagnosticLogTests: XCTestCase {
    func testEntrySanitizesRedactsAndRoundTrips() throws {
        let entry = DiagnosticLogEntry(
            sequence: 7,
            createdAt: "2026-08-11T12:34:56.789+08:00",
            level: .warning,
            category: "remote\ninput",
            event: "packet\trejected",
            fields: [
                "userSig": "fixture-user-sig",
                "secret_key": "fixture-secret",
                "access-token": "fixture-token",
                "note\nkey": "line one\nline two",
            ]
        )

        XCTAssertEqual(entry.category, "remote input")
        XCTAssertEqual(entry.event, "packet rejected")
        XCTAssertEqual(entry.fields["userSig"], "<redacted>")
        XCTAssertEqual(entry.fields["secret_key"], "<redacted>")
        XCTAssertEqual(entry.fields["access-token"], "<redacted>")
        XCTAssertEqual(entry.fields["note key"], "line one line two")

        let data = try JSONEncoder().encode(entry)
        let raw = try XCTUnwrap(String(data: data, encoding: .utf8))
        XCTAssertFalse(raw.contains("fixture-user-sig"))
        XCTAssertFalse(raw.contains("fixture-secret"))
        XCTAssertFalse(raw.contains("fixture-token"))
        XCTAssertEqual(try JSONDecoder().decode(DiagnosticLogEntry.self, from: data), entry)
    }

    func testStableTagIsRepeatableWithoutLeakingIdentifier() {
        let first = DiagnosticLogPrivacy.stableTag("whq-iphone", prefix: "peer")
        XCTAssertEqual(first, DiagnosticLogPrivacy.stableTag("whq-iphone", prefix: "peer"))
        XCTAssertNotEqual(first, DiagnosticLogPrivacy.stableTag("other", prefix: "peer"))
        XCTAssertFalse(first.contains("whq-iphone"))
    }

    func testSanitizerHandlesZeroAndNegativeLimits() {
        XCTAssertEqual(DiagnosticLogPrivacy.sanitized("value", maximumLength: 0), "…")
        XCTAssertEqual(DiagnosticLogPrivacy.sanitized("value", maximumLength: -10), "…")
    }

    func testInputAccumulatorSnapshotsAndResetsCounters() {
        var value = RemoteInputDiagnosticAccumulator()
        value.recordPointerSeen()
        value.recordPointerSeen()
        value.recordPointerDroppedInvalidGeometry()
        value.recordPointerDroppedLetterbox()
        value.recordCoalescedMove()
        value.recordMove(x: -1, y: 2)
        value.recordClick(x: 0.25, y: 0.75)
        value.recordWheel(x: 0.4, y: 0.6)
        value.recordKey()
        value.recordText(characterCount: 2, utf8Bytes: 6)
        value.recordSent(reliable: true, eventCount: 2, byteCount: 100)
        value.recordSent(reliable: false, eventCount: 1, byteCount: 70)
        value.recordSDKRejection()
        value.recordBlockedByState()
        value.recordBlockedNotInRoom()
        value.recordEncodingFailure()
        value.recordOversizedPacket()
        value.recordRetry()

        let snapshot = value.takeSnapshot()
        XCTAssertTrue(snapshot.hasActivity)
        XCTAssertEqual(snapshot.pointerEventsSeen, 2)
        XCTAssertEqual(snapshot.droppedInvalidGeometry, 1)
        XCTAssertEqual(snapshot.droppedLetterbox, 1)
        XCTAssertEqual(snapshot.coalescedMoves, 1)
        XCTAssertEqual(snapshot.capturedMoves, 1)
        XCTAssertEqual(snapshot.capturedClicks, 1)
        XCTAssertEqual(snapshot.capturedWheels, 1)
        XCTAssertEqual(snapshot.capturedKeys, 1)
        XCTAssertEqual(snapshot.capturedTextCharacters, 2)
        XCTAssertEqual(snapshot.capturedTextUTF8Bytes, 6)
        XCTAssertEqual(snapshot.sentReliablePackets, 1)
        XCTAssertEqual(snapshot.sentUnreliablePackets, 1)
        XCTAssertEqual(snapshot.sentEvents, 3)
        XCTAssertEqual(snapshot.sentBytes, 170)
        XCTAssertEqual(snapshot.rejectedBySDK, 1)
        XCTAssertEqual(snapshot.blockedByState, 1)
        XCTAssertEqual(snapshot.blockedNotInRoom, 1)
        XCTAssertEqual(snapshot.encodingFailures, 1)
        XCTAssertEqual(snapshot.oversizedPackets, 1)
        XCTAssertEqual(snapshot.retries, 1)
        XCTAssertEqual(snapshot.lastKnownX, 0.4)
        XCTAssertEqual(snapshot.lastKnownY, 0.6)

        let reset = value.takeSnapshot()
        XCTAssertFalse(reset.hasActivity)
        XCTAssertEqual(reset.pointerEventsSeen, 0)
        XCTAssertEqual(reset.droppedInvalidGeometry, 0)
        XCTAssertEqual(reset.droppedLetterbox, 0)
        XCTAssertEqual(reset.coalescedMoves, 0)
        XCTAssertEqual(reset.sentEvents, 0)
        XCTAssertEqual(reset.lastKnownX, 0.4)
        XCTAssertEqual(reset.lastKnownY, 0.6)
    }

    func testUTF8BytesAloneCountAsInputActivity() {
        var value = RemoteInputDiagnosticAccumulator()
        value.recordText(characterCount: 0, utf8Bytes: 3)
        XCTAssertTrue(value.takeSnapshot().hasActivity)
    }

    func testFileStoreWritesReadableRedactedLogAndExportsIt() async throws {
        let directory = temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = DiagnosticLogFileStore(directoryURL: directory)
        let entry = DiagnosticLogEntry(
            sequence: 1,
            createdAt: "2026-08-11T10:00:00.000+08:00",
            level: .error,
            category: "remote-input",
            event: "packet-rejected",
            fields: [
                "userSig": "fixture-secret",
                "sdk_rejected": "1",
            ]
        )

        try await store.append([entry], at: fixedDate("2026-08-11T02:00:00Z"))
        let files = await store.logFileURLs()
        XCTAssertEqual(files.count, 1)
        let content = try String(contentsOf: XCTUnwrap(files.first), encoding: .utf8)
        XCTAssertTrue(content.contains("[E] seq=1 [remote-input] event=packet-rejected"))
        XCTAssertTrue(content.contains("userSig=<redacted>"))
        XCTAssertFalse(content.contains("fixture-secret"))

        let exportDirectory = directory.appendingPathComponent("exports", isDirectory: true)
        let exportURL = try await store.makeExportSnapshot(
            in: exportDirectory,
            at: fixedDate("2026-08-11T02:00:01Z")
        )
        let exported = try String(contentsOf: exportURL, encoding: .utf8)
        XCTAssertEqual(exported, content)
    }

    func testFileStoreRotatesAndKeepsSequenceOrderInExport() async throws {
        let directory = temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = DiagnosticLogFileStore(
            directoryURL: directory,
            maximumFileBytes: 180,
            maximumTotalBytes: 10_000,
            retentionDays: 7
        )
        let date = fixedDate("2026-08-11T02:00:00Z")
        let first = DiagnosticLogEntry(
            sequence: 1,
            createdAt: "2026-08-11T10:00:00.000+08:00",
            level: .info,
            category: "test",
            event: "first",
            fields: ["padding": String(repeating: "a", count: 80)]
        )
        let second = DiagnosticLogEntry(
            sequence: 2,
            createdAt: "2026-08-11T10:00:01.000+08:00",
            level: .info,
            category: "test",
            event: "second",
            fields: ["padding": String(repeating: "b", count: 80)]
        )

        try await store.append([first], at: date)
        try await store.append([second], at: date.addingTimeInterval(1))
        let rotatedFiles = await store.logFileURLs()
        XCTAssertEqual(rotatedFiles.count, 2)

        let exportURL = try await store.makeExportSnapshot(
            in: directory.appendingPathComponent("exports", isDirectory: true),
            at: date.addingTimeInterval(2)
        )
        let exported = try String(contentsOf: exportURL, encoding: .utf8)
        let firstRange = try XCTUnwrap(exported.range(of: "event=first"))
        let secondRange = try XCTUnwrap(exported.range(of: "event=second"))
        XCTAssertLessThan(firstRange.lowerBound, secondRange.lowerBound)
    }

    func testFileStoreRotatesEntriesWithinOneBatch() async throws {
        let directory = temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = DiagnosticLogFileStore(
            directoryURL: directory,
            maximumFileBytes: 180,
            maximumTotalBytes: 10_000,
            retentionDays: 7
        )
        let date = fixedDate("2026-08-11T02:00:00Z")
        let first = DiagnosticLogEntry(
            sequence: 1,
            createdAt: "2026-08-11T10:00:00.000+08:00",
            level: .info,
            category: "test",
            event: "batch-first",
            fields: ["padding": String(repeating: "a", count: 72)]
        )
        let second = DiagnosticLogEntry(
            sequence: 2,
            createdAt: "2026-08-11T10:00:01.000+08:00",
            level: .info,
            category: "test",
            event: "batch-second",
            fields: ["padding": String(repeating: "b", count: 72)]
        )

        try await store.append([first, second], at: date)

        let files = await store.logFileURLs()
        XCTAssertEqual(files.count, 2)
        for file in files {
            let attributes = try FileManager.default.attributesOfItem(atPath: file.path)
            let size = try XCTUnwrap(attributes[.size] as? NSNumber).uint64Value
            XCTAssertLessThanOrEqual(size, 180)
        }
    }

    func testFileStorePrunesExpiredFilesWhenDayChanges() async throws {
        let directory = temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = DiagnosticLogFileStore(
            directoryURL: directory,
            maximumFileBytes: 1_024,
            maximumTotalBytes: 10_000,
            retentionDays: 7
        )
        let oldDate = fixedDate("2026-08-01T02:00:00Z")
        let newDate = fixedDate("2026-08-11T02:00:00Z")

        try await store.append([testEntry(sequence: 1, event: "old")], at: oldDate)
        try await store.append([testEntry(sequence: 2, event: "new")], at: newDate)

        let files = await store.logFileURLs()
        XCTAssertEqual(files.count, 1)
        let content = try String(contentsOf: XCTUnwrap(files.first), encoding: .utf8)
        XCTAssertFalse(content.contains("event=old"))
        XCTAssertTrue(content.contains("event=new"))
    }

    func testFileStoreDoesNotExportAnExpiredIdleCurrentFile() async throws {
        let directory = temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = DiagnosticLogFileStore(
            directoryURL: directory,
            maximumFileBytes: 1_024,
            maximumTotalBytes: 10_000,
            retentionDays: 7
        )
        let oldDate = fixedDate("2026-08-01T02:00:00Z")
        let exportDate = fixedDate("2026-08-11T02:00:00Z")

        try await store.append([testEntry(sequence: 1, event: "expired")], at: oldDate)
        let exportURL = try await store.makeExportSnapshot(
            in: directory.appendingPathComponent("exports", isDirectory: true),
            at: exportDate
        )

        let exported = try String(contentsOf: exportURL, encoding: .utf8)
        XCTAssertEqual(exported, "No diagnostic log entries are available.\n")
        let files = await store.logFileURLs()
        XCTAssertTrue(files.isEmpty)
    }

    private func temporaryDirectory() -> URL {
        FileManager.default.temporaryDirectory
            .appendingPathComponent("maichat-log-tests-\(UUID().uuidString)", isDirectory: true)
    }

    private func fixedDate(_ value: String) -> Date {
        ISO8601DateFormatter().date(from: value)!
    }

    private func testEntry(sequence: UInt64, event: String) -> DiagnosticLogEntry {
        DiagnosticLogEntry(
            sequence: sequence,
            createdAt: "2026-08-11T10:00:00.000+08:00",
            level: .info,
            category: "test",
            event: event
        )
    }
}
