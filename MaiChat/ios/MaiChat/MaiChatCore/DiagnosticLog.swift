import CryptoKit
import Foundation

public enum DiagnosticLogLevel: String, Codable, Sendable {
    case debug
    case info
    case warning
    case error
}

public struct DiagnosticLogEntry: Codable, Equatable, Sendable {
    public let sequence: UInt64
    public let createdAt: String
    public let level: DiagnosticLogLevel
    public let category: String
    public let event: String
    public let fields: [String: String]

    public init(
        sequence: UInt64,
        createdAt: String,
        level: DiagnosticLogLevel,
        category: String,
        event: String,
        fields: [String: String] = [:]
    ) {
        self.sequence = sequence
        self.createdAt = createdAt
        self.level = level
        self.category = DiagnosticLogPrivacy.sanitized(category, maximumLength: 64)
        self.event = DiagnosticLogPrivacy.sanitized(event, maximumLength: 96)
        self.fields = DiagnosticLogPrivacy.redacted(fields)
    }

    public var summary: String {
        let fieldsSummary = fieldsSummary
        guard !fieldsSummary.isEmpty else {
            return "[\(category)] event=\(event)"
        }
        return "[\(category)] event=\(event) \(fieldsSummary)"
    }

    public var fieldsSummary: String {
        let values = fields.keys.sorted().map { key in
            "\(key)=\(Self.displayValue(fields[key] ?? ""))"
        }
        return values.joined(separator: " ")
    }

    private static func displayValue(_ value: String) -> String {
        guard value.contains(where: { $0.isWhitespace || $0 == "\"" || $0 == "=" }) else {
            return value
        }
        let escaped = value
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        return "\"\(escaped)\""
    }
}

@MainActor
public protocol DiagnosticLogSink: AnyObject {
    func record(
        level: DiagnosticLogLevel,
        category: String,
        event: String,
        fields: [String: String]
    )
}

public enum DiagnosticLogPrivacy {
    private static let sensitiveKeyFragments = [
        "authproof",
        "credential",
        "password",
        "secret",
        "token",
        "usersig",
    ]

    public static func stableTag(_ value: String, prefix: String) -> String {
        let cleanValue = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanValue.isEmpty else { return "<empty>" }
        let digest = SHA256.hash(data: Data(cleanValue.utf8))
        let shortHash = digest.prefix(8).map { String(format: "%02x", $0) }.joined()
        return "\(sanitized(prefix, maximumLength: 12))#\(shortHash)"
    }

    public static func sanitized(_ value: String, maximumLength: Int = 256) -> String {
        let safeMaximumLength = max(0, maximumLength)
        let singleLine = value
            .replacingOccurrences(of: "\r", with: " ")
            .replacingOccurrences(of: "\n", with: " ")
            .replacingOccurrences(of: "\t", with: " ")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard singleLine.count > safeMaximumLength else { return singleLine }
        return String(singleLine.prefix(safeMaximumLength)) + "…"
    }

    public static func redacted(_ fields: [String: String]) -> [String: String] {
        fields.reduce(into: [:]) { result, item in
            let safeKey = sanitized(item.key, maximumLength: 64)
            let normalizedKey = item.key.lowercased().filter { $0.isLetter || $0.isNumber }
            if sensitiveKeyFragments.contains(where: normalizedKey.contains) {
                result[safeKey] = "<redacted>"
            } else {
                result[safeKey] = sanitized(item.value)
            }
        }
    }
}

public struct RemoteInputDiagnosticSnapshot: Equatable, Sendable {
    public let pointerEventsSeen: Int
    public let droppedInvalidGeometry: Int
    public let droppedLetterbox: Int
    public let coalescedMoves: Int
    public let capturedMoves: Int
    public let capturedClicks: Int
    public let capturedWheels: Int
    public let capturedKeys: Int
    public let capturedTextCharacters: Int
    public let capturedTextUTF8Bytes: Int
    public let sentReliablePackets: Int
    public let sentUnreliablePackets: Int
    public let sentEvents: Int
    public let sentBytes: Int
    public let rejectedBySDK: Int
    public let blockedByState: Int
    public let blockedNotInRoom: Int
    public let encodingFailures: Int
    public let oversizedPackets: Int
    public let retries: Int
    public let lastKnownX: Double?
    public let lastKnownY: Double?

    public var hasActivity: Bool {
        pointerEventsSeen > 0 || droppedInvalidGeometry > 0 || droppedLetterbox > 0
            || coalescedMoves > 0 || capturedMoves > 0 || capturedClicks > 0
            || capturedWheels > 0 || capturedKeys > 0
            || capturedTextCharacters > 0 || capturedTextUTF8Bytes > 0 || sentReliablePackets > 0
            || sentUnreliablePackets > 0 || rejectedBySDK > 0 || blockedByState > 0
            || blockedNotInRoom > 0 || encodingFailures > 0 || oversizedPackets > 0
            || retries > 0
    }
}

public struct RemoteInputDiagnosticAccumulator: Sendable {
    private var pointerEventsSeen = 0
    private var droppedInvalidGeometry = 0
    private var droppedLetterbox = 0
    private var coalescedMoves = 0
    private var capturedMoves = 0
    private var capturedClicks = 0
    private var capturedWheels = 0
    private var capturedKeys = 0
    private var capturedTextCharacters = 0
    private var capturedTextUTF8Bytes = 0
    private var sentReliablePackets = 0
    private var sentUnreliablePackets = 0
    private var sentEvents = 0
    private var sentBytes = 0
    private var rejectedBySDK = 0
    private var blockedByState = 0
    private var blockedNotInRoom = 0
    private var encodingFailures = 0
    private var oversizedPackets = 0
    private var retries = 0
    private var lastX: Double?
    private var lastY: Double?

    public init() {}

    public mutating func recordPointerSeen() {
        pointerEventsSeen += 1
    }

    public mutating func recordPointerDroppedInvalidGeometry() {
        droppedInvalidGeometry += 1
    }

    public mutating func recordPointerDroppedLetterbox() {
        droppedLetterbox += 1
    }

    public mutating func recordCoalescedMove() {
        coalescedMoves += 1
    }

    public mutating func recordMove(x: Double, y: Double) {
        capturedMoves += 1
        recordPoint(x: x, y: y)
    }

    public mutating func recordClick(x: Double, y: Double) {
        capturedClicks += 1
        recordPoint(x: x, y: y)
    }

    public mutating func recordWheel(x: Double, y: Double) {
        capturedWheels += 1
        recordPoint(x: x, y: y)
    }

    public mutating func recordKey() {
        capturedKeys += 1
    }

    public mutating func recordText(characterCount: Int, utf8Bytes: Int) {
        capturedTextCharacters += max(characterCount, 0)
        capturedTextUTF8Bytes += max(utf8Bytes, 0)
    }

    public mutating func recordSent(reliable: Bool, eventCount: Int, byteCount: Int) {
        if reliable {
            sentReliablePackets += 1
        } else {
            sentUnreliablePackets += 1
        }
        sentEvents += max(eventCount, 0)
        sentBytes += max(byteCount, 0)
    }

    public mutating func recordSDKRejection() {
        rejectedBySDK += 1
    }

    public mutating func recordBlockedByState() {
        blockedByState += 1
    }

    public mutating func recordBlockedNotInRoom() {
        blockedNotInRoom += 1
    }

    public mutating func recordEncodingFailure() {
        encodingFailures += 1
    }

    public mutating func recordOversizedPacket() {
        oversizedPackets += 1
    }

    public mutating func recordRetry() {
        retries += 1
    }

    public mutating func takeSnapshot() -> RemoteInputDiagnosticSnapshot {
        let snapshot = RemoteInputDiagnosticSnapshot(
            pointerEventsSeen: pointerEventsSeen,
            droppedInvalidGeometry: droppedInvalidGeometry,
            droppedLetterbox: droppedLetterbox,
            coalescedMoves: coalescedMoves,
            capturedMoves: capturedMoves,
            capturedClicks: capturedClicks,
            capturedWheels: capturedWheels,
            capturedKeys: capturedKeys,
            capturedTextCharacters: capturedTextCharacters,
            capturedTextUTF8Bytes: capturedTextUTF8Bytes,
            sentReliablePackets: sentReliablePackets,
            sentUnreliablePackets: sentUnreliablePackets,
            sentEvents: sentEvents,
            sentBytes: sentBytes,
            rejectedBySDK: rejectedBySDK,
            blockedByState: blockedByState,
            blockedNotInRoom: blockedNotInRoom,
            encodingFailures: encodingFailures,
            oversizedPackets: oversizedPackets,
            retries: retries,
            lastKnownX: lastX,
            lastKnownY: lastY
        )
        pointerEventsSeen = 0
        droppedInvalidGeometry = 0
        droppedLetterbox = 0
        coalescedMoves = 0
        capturedMoves = 0
        capturedClicks = 0
        capturedWheels = 0
        capturedKeys = 0
        capturedTextCharacters = 0
        capturedTextUTF8Bytes = 0
        sentReliablePackets = 0
        sentUnreliablePackets = 0
        sentEvents = 0
        sentBytes = 0
        rejectedBySDK = 0
        blockedByState = 0
        blockedNotInRoom = 0
        encodingFailures = 0
        oversizedPackets = 0
        retries = 0
        return snapshot
    }

    private mutating func recordPoint(x: Double, y: Double) {
        lastX = x.isFinite ? min(max(x, 0), 1) : 0
        lastY = y.isFinite ? min(max(y, 0), 1) : 0
    }
}
