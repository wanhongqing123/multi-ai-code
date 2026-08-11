import CoreGraphics
import Foundation

public struct CaptureGeometry: Codable, Equatable, Sendable {
    public static let fitContentMode = "fit"
    public static let maximumDimension = 65_535
    public static let maximumRevision = Int(Int32.max)

    public let sourceWidth: Int
    public let sourceHeight: Int
    public let captureX: Int
    public let captureY: Int
    public let captureWidth: Int
    public let captureHeight: Int
    public let contentMode: String
    public let revision: Int

    public init(
        sourceWidth: Int,
        sourceHeight: Int,
        captureX: Int,
        captureY: Int,
        captureWidth: Int,
        captureHeight: Int,
        contentMode: String = Self.fitContentMode,
        revision: Int
    ) {
        self.sourceWidth = sourceWidth
        self.sourceHeight = sourceHeight
        self.captureX = captureX
        self.captureY = captureY
        self.captureWidth = captureWidth
        self.captureHeight = captureHeight
        self.contentMode = contentMode
        self.revision = revision
    }

    public var validationFailureReason: String? {
        guard sourceWidth > 0, sourceHeight > 0 else {
            return "invalid-source-size"
        }
        guard sourceWidth <= Self.maximumDimension,
              sourceHeight <= Self.maximumDimension
        else {
            return "source-size-too-large"
        }
        guard captureWidth > 0, captureHeight > 0 else {
            return "invalid-capture-size"
        }
        guard captureWidth <= Self.maximumDimension,
              captureHeight <= Self.maximumDimension,
              captureX <= Self.maximumDimension,
              captureY <= Self.maximumDimension
        else {
            return "capture-rect-too-large"
        }
        guard revision > 0, revision <= Self.maximumRevision else {
            return "invalid-revision"
        }
        guard contentMode == Self.fitContentMode else {
            return "unsupported-content-mode"
        }
        guard captureX >= 0,
              captureY >= 0,
              captureX <= sourceWidth,
              captureY <= sourceHeight,
              captureWidth <= sourceWidth - captureX,
              captureHeight <= sourceHeight - captureY
        else {
            return "capture-out-of-bounds"
        }
        return nil
    }
}

public enum CaptureGeometryDisposition: Equatable, Sendable {
    case absent
    case accepted(CaptureGeometry)
    case ignored(CaptureGeometry?, reason: String)
}

public enum RemoteDesktopCoordinateMapper {
    public static func aspectFitRect(contentSize: CGSize, in container: CGRect) -> CGRect? {
        guard isValid(size: contentSize), isValid(rect: container) else { return nil }
        let scale = min(
            container.width / contentSize.width,
            container.height / contentSize.height
        )
        guard scale.isFinite, scale > 0 else { return nil }
        let size = CGSize(width: contentSize.width * scale, height: contentSize.height * scale)
        return CGRect(
            x: container.midX - size.width / 2,
            y: container.midY - size.height / 2,
            width: size.width,
            height: size.height
        )
    }

    public static func activeContentRect(
        encodedFrameRect: CGRect,
        captureGeometry: CaptureGeometry?
    ) -> CGRect? {
        guard isValid(rect: encodedFrameRect) else { return nil }
        guard let captureGeometry,
              captureGeometry.validationFailureReason == nil
        else {
            return encodedFrameRect
        }
        return aspectFitRect(
            contentSize: CGSize(
                width: captureGeometry.captureWidth,
                height: captureGeometry.captureHeight
            ),
            in: encodedFrameRect
        )
    }

    public static func normalizedPoint(
        at location: CGPoint,
        encodedFrameRect: CGRect,
        captureGeometry: CaptureGeometry?,
        clamped: Bool = false
    ) -> CGPoint? {
        guard location.x.isFinite,
              location.y.isFinite,
              let activeRect = activeContentRect(
                  encodedFrameRect: encodedFrameRect,
                  captureGeometry: captureGeometry
              )
        else {
            return nil
        }
        let isInsideContent = location.x >= activeRect.minX
            && location.x <= activeRect.maxX
            && location.y >= activeRect.minY
            && location.y <= activeRect.maxY
        guard clamped || isInsideContent else { return nil }
        let localX = clamp((location.x - activeRect.minX) / activeRect.width)
        let localY = clamp((location.y - activeRect.minY) / activeRect.height)
        guard let captureGeometry,
              captureGeometry.validationFailureReason == nil
        else {
            return CGPoint(x: localX, y: localY)
        }
        return CGPoint(
            x: clamp(
                (CGFloat(captureGeometry.captureX)
                    + localX * CGFloat(captureGeometry.captureWidth))
                    / CGFloat(captureGeometry.sourceWidth)
            ),
            y: clamp(
                (CGFloat(captureGeometry.captureY)
                    + localY * CGFloat(captureGeometry.captureHeight))
                    / CGFloat(captureGeometry.sourceHeight)
            )
        )
    }

    private static func clamp(_ value: CGFloat) -> CGFloat {
        min(max(value, 0), 1)
    }

    private static func isValid(size: CGSize) -> Bool {
        size.width.isFinite
            && size.height.isFinite
            && size.width > 0
            && size.height > 0
    }

    private static func isValid(rect: CGRect) -> Bool {
        rect.origin.x.isFinite
            && rect.origin.y.isFinite
            && isValid(size: rect.size)
    }
}

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
    public let captureGeometryDisposition: CaptureGeometryDisposition

    public var captureGeometry: CaptureGeometry? {
        guard case let .accepted(geometry) = captureGeometryDisposition else { return nil }
        return geometry
    }

    public init(
        kind: Kind,
        sessionID: String = "",
        roomID: String = "",
        authProof: String = "",
        reason: String = "",
        noticeCode: String = "",
        captureGeometry: CaptureGeometry? = nil
    ) {
        self.kind = kind
        self.sessionID = sessionID
        self.roomID = roomID
        self.authProof = authProof
        self.reason = reason
        self.noticeCode = noticeCode
        self.captureGeometryDisposition = kind == .accept
            ? Self.geometryDisposition(for: captureGeometry)
            : .absent
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
            noticeCode: noticeCode.nilIfEmpty,
            captureGeometry: captureGeometry
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
            noticeCode: payload.noticeCode ?? "",
            captureGeometryDisposition: payload.captureGeometryDisposition
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
        let captureGeometry: CaptureGeometry?
        let captureGeometryDisposition: CaptureGeometryDisposition

        init(
            version: Int,
            kind: Kind,
            sessionID: String?,
            roomID: String?,
            authProof: String?,
            reason: String?,
            noticeCode: String?,
            captureGeometry: CaptureGeometry?
        ) {
            self.version = version
            self.kind = kind
            self.sessionID = sessionID
            self.roomID = roomID
            self.authProof = authProof
            self.reason = reason
            self.noticeCode = noticeCode
            self.captureGeometry = captureGeometry
            self.captureGeometryDisposition = RemoteDesktopSignal.geometryDisposition(
                for: captureGeometry
            )
        }

        init(from decoder: Decoder) throws {
            let container = try decoder.container(keyedBy: CodingKeys.self)
            version = try container.decode(Int.self, forKey: .version)
            kind = try container.decode(Kind.self, forKey: .kind)
            sessionID = try container.decodeIfPresent(String.self, forKey: .sessionID)
            roomID = try container.decodeIfPresent(String.self, forKey: .roomID)
            authProof = try container.decodeIfPresent(String.self, forKey: .authProof)
            reason = try container.decodeIfPresent(String.self, forKey: .reason)
            noticeCode = try container.decodeIfPresent(String.self, forKey: .noticeCode)

            if kind != .accept
                || !container.contains(.captureGeometry)
                || (try? container.decodeNil(forKey: .captureGeometry)) == true
            {
                captureGeometry = nil
                captureGeometryDisposition = .absent
            } else if let decodedGeometry = try? container.decode(
                CaptureGeometry.self,
                forKey: .captureGeometry
            ) {
                let disposition = RemoteDesktopSignal.geometryDisposition(for: decodedGeometry)
                captureGeometryDisposition = disposition
                if case let .accepted(geometry) = disposition {
                    captureGeometry = geometry
                } else {
                    captureGeometry = nil
                }
            } else {
                captureGeometry = nil
                captureGeometryDisposition = .ignored(nil, reason: "malformed")
            }
        }

        func encode(to encoder: Encoder) throws {
            var container = encoder.container(keyedBy: CodingKeys.self)
            try container.encode(version, forKey: .version)
            try container.encode(kind, forKey: .kind)
            try container.encodeIfPresent(sessionID, forKey: .sessionID)
            try container.encodeIfPresent(roomID, forKey: .roomID)
            try container.encodeIfPresent(authProof, forKey: .authProof)
            try container.encodeIfPresent(reason, forKey: .reason)
            try container.encodeIfPresent(noticeCode, forKey: .noticeCode)
            try container.encodeIfPresent(captureGeometry, forKey: .captureGeometry)
        }

        enum CodingKeys: String, CodingKey {
            case version = "v"
            case kind = "type"
            case sessionID = "sessionId"
            case roomID = "roomId"
            case authProof
            case reason
            case noticeCode
            case captureGeometry
        }
    }

    private init(
        kind: Kind,
        sessionID: String,
        roomID: String,
        authProof: String,
        reason: String,
        noticeCode: String,
        captureGeometryDisposition: CaptureGeometryDisposition
    ) {
        self.kind = kind
        self.sessionID = sessionID
        self.roomID = roomID
        self.authProof = authProof
        self.reason = reason
        self.noticeCode = noticeCode
        self.captureGeometryDisposition = captureGeometryDisposition
    }

    private static func geometryDisposition(
        for captureGeometry: CaptureGeometry?
    ) -> CaptureGeometryDisposition {
        guard let captureGeometry else { return .absent }
        if let reason = captureGeometry.validationFailureReason {
            return .ignored(captureGeometry, reason: reason)
        }
        return .accepted(captureGeometry)
    }
}

private extension String {
    var nilIfEmpty: String? {
        isEmpty ? nil : self
    }
}
