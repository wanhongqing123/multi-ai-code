import Foundation

public enum RemoteMouseButton: Int, Equatable, Sendable {
    case left = 0
    case right = 1
    case middle = 2
}

public enum RemoteInputEvent: Equatable, Sendable {
    case mouseMove(x: Double, y: Double)
    case mouseButton(button: RemoteMouseButton, pressed: Bool, x: Double, y: Double)
    case mouseWheel(delta: Int, x: Double, y: Double)
    case key(code: UInt32, pressed: Bool)
    case text(String)
    case releaseAll
}

public struct RemoteInputPacket: Equatable, Sendable {
    public static let protocolVersion = 1
    public static let unreliableCommandID = 2
    public static let reliableCommandID = 3
    public static let maximumPacketBytes = 1_024

    public let sessionID: String
    public let sequence: UInt32
    public let events: [RemoteInputEvent]

    public init(sessionID: String, sequence: UInt32, events: [RemoteInputEvent]) {
        self.sessionID = sessionID
        self.sequence = sequence
        self.events = events
    }

    public func encodedData() throws -> Data {
        try JSONEncoder().encode(
            Payload(
                version: Self.protocolVersion,
                sessionID: sessionID,
                sequence: sequence,
                events: events.map(EncodedEvent.init)
            )
        )
    }

    public func fitsInOnePacket() -> Bool {
        guard let data = try? encodedData() else { return false }
        return data.count <= Self.maximumPacketBytes
    }

    private struct Payload: Encodable {
        let version: Int
        let sessionID: String
        let sequence: UInt32
        let events: [EncodedEvent]

        enum CodingKeys: String, CodingKey {
            case version = "v"
            case sessionID = "s"
            case sequence = "n"
            case events = "e"
        }
    }

    private struct EncodedEvent: Encodable {
        let event: RemoteInputEvent

        init(_ event: RemoteInputEvent) {
            self.event = event
        }

        enum CodingKeys: String, CodingKey {
            case type = "t"
            case x
            case y
            case button = "b"
            case pressed = "d"
            case wheelDelta = "w"
            case keyCode = "k"
            case text = "s"
        }

        func encode(to encoder: Encoder) throws {
            var container = encoder.container(keyedBy: CodingKeys.self)
            switch event {
            case let .mouseMove(x, y):
                try container.encode("m", forKey: .type)
                try container.encode(Self.clamped(x), forKey: .x)
                try container.encode(Self.clamped(y), forKey: .y)
            case let .mouseButton(button, pressed, x, y):
                try container.encode("b", forKey: .type)
                try container.encode(Self.clamped(x), forKey: .x)
                try container.encode(Self.clamped(y), forKey: .y)
                try container.encode(button.rawValue, forKey: .button)
                try container.encode(pressed, forKey: .pressed)
            case let .mouseWheel(delta, x, y):
                try container.encode("w", forKey: .type)
                try container.encode(Self.clamped(x), forKey: .x)
                try container.encode(Self.clamped(y), forKey: .y)
                try container.encode(delta, forKey: .wheelDelta)
            case let .key(code, pressed):
                try container.encode("k", forKey: .type)
                try container.encode(code, forKey: .keyCode)
                try container.encode(pressed, forKey: .pressed)
            case let .text(text):
                try container.encode("x", forKey: .type)
                try container.encode(text, forKey: .text)
            case .releaseAll:
                try container.encode("r", forKey: .type)
            }
        }

        private static func clamped(_ value: Double) -> Double {
            guard value.isFinite else { return 0 }
            return min(max(value, 0), 1)
        }
    }
}
