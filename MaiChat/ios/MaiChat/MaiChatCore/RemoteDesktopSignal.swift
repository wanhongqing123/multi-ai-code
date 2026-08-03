import Foundation

public struct RemoteDesktopSignal: Equatable, Sendable {
    public enum Kind: String, Codable, Sendable {
        case invite
        case accept
        case reject
        case stop
        case notice
    }

    public static let protocolVersion = 1
    public static let prefix = "\u{2063}\u{200B}[remote-desktop]"

    public let kind: Kind
    public let sessionID: String
    public let roomID: String
    public let authProof: String
    public let reason: String
    public let noticeCode: String

    public init(
        kind: Kind,
        sessionID: String = "",
        roomID: String = "",
        authProof: String = "",
        reason: String = "",
        noticeCode: String = ""
    ) {
        self.kind = kind
        self.sessionID = sessionID
        self.roomID = roomID
        self.authProof = authProof
        self.reason = reason
        self.noticeCode = noticeCode
    }

    public static func isSignalText(_ text: String) -> Bool {
        text.hasPrefix(prefix)
    }

    public func encodedText() throws -> String {
        let payload = Payload(
            version: Self.protocolVersion,
            kind: kind,
            sessionID: sessionID.nilIfEmpty,
            roomID: roomID.nilIfEmpty,
            authProof: authProof.nilIfEmpty,
            reason: reason.nilIfEmpty,
            noticeCode: noticeCode.nilIfEmpty
        )
        let data = try JSONEncoder().encode(payload)
        guard let json = String(data: data, encoding: .utf8) else {
            throw EncodingError.invalidValue(
                payload,
                EncodingError.Context(codingPath: [], debugDescription: "Invalid UTF-8 signal")
            )
        }
        return Self.prefix + json
    }

    public static func decodeText(_ text: String) -> RemoteDesktopSignal? {
        guard isSignalText(text) else { return nil }
        let payloadText = String(text.dropFirst(prefix.count))
        guard let data = payloadText.data(using: .utf8),
              let payload = try? JSONDecoder().decode(Payload.self, from: data),
              payload.version == protocolVersion
        else {
            return nil
        }
        if payload.kind == .notice, payload.noticeCode?.isEmpty != false {
            return nil
        }
        return RemoteDesktopSignal(
            kind: payload.kind,
            sessionID: payload.sessionID ?? "",
            roomID: payload.roomID ?? "",
            authProof: payload.authProof ?? "",
            reason: payload.reason ?? "",
            noticeCode: payload.noticeCode ?? ""
        )
    }

    private struct Payload: Codable {
        let version: Int
        let kind: Kind
        let sessionID: String?
        let roomID: String?
        let authProof: String?
        let reason: String?
        let noticeCode: String?

        enum CodingKeys: String, CodingKey {
            case version = "v"
            case kind = "type"
            case sessionID = "sessionId"
            case roomID = "roomId"
            case authProof
            case reason
            case noticeCode
        }
    }
}

private extension String {
    var nilIfEmpty: String? {
        isEmpty ? nil : self
    }
}
