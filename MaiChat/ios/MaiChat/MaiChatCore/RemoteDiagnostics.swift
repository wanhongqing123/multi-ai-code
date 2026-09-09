import Foundation
import CoreFoundation

public enum RemoteDiagnosticsError: Error, LocalizedError {
    case invalidReport
    case oversizedReport
    public var errorDescription: String? {
        switch self {
        case .invalidReport: return "远端报告格式或排障编号不匹配"
        case .oversizedReport: return "远端报告超过 2 MiB 限制"
        }
    }
}

public struct RemoteDiagnosticsMessage: Codable, Sendable {
    public let id: String
    public let remoteID: String?
    public let direction: String
    public let status: String
    public let createdAt: Double
    public let kind: String
    public let logMessageTag: String

    public init(_ message: RemoteIMMessage) {
        id = message.id.uuidString
        remoteID = message.remoteID
        direction = message.direction.rawValue
        status = message.status.rawValue
        createdAt = message.createdAt.timeIntervalSince1970 * 1000
        kind = message.fileAttachment != nil ? "file" : message.imageAttachment != nil ? "image" : "message"
        logMessageTag = DiagnosticLogPrivacy.stableTag(message.remoteID ?? message.id.uuidString, prefix: "m")
    }
}

public struct RemoteDiagnosticsLogEntry: Codable, Sendable {
    public let createdAt: String
    public let event: String
    public let fields: [String: String]

    public init(_ entry: DiagnosticLogEntry) {
        createdAt = entry.createdAt
        event = entry.event
        let allowed: Set<String> = ["peer", "message", "launch", "kind", "result", "code", "duration_ms", "operation", "cached_messages", "near_bottom", "scroll_action", "app_version", "build", "ios", "pid"]
        fields = entry.fields.filter { key, value in
            allowed.contains(key) && value.count <= 120 &&
                value.range(of: "^[a-zA-Z0-9_.:#/-]+$", options: .regularExpression) != nil
        }
    }
}

public struct RemoteDiagnosticsLocalEvidence: Codable, Sendable {
    public let appVersion: String
    public let platform: String
    public let ownerUserID: String
    public let peerUserID: String
    public let collectedAt: Double
    public let messages: [RemoteDiagnosticsMessage]
    public let displayedConversation: Bool
    public let timeZone: String
    public let processId: Int32
    public let logs: [RemoteDiagnosticsLogEntry]
    public let logCoverage: String

    public init(appVersion: String, ownerUserID: String, peerUserID: String,
                collectedAt: Date, messages: [RemoteIMMessage], displayedConversation: Bool,
                logs: [RemoteDiagnosticsLogEntry] = []) {
        self.appVersion = appVersion
        platform = "iOS"
        self.ownerUserID = ownerUserID
        self.peerUserID = peerUserID
        self.collectedAt = collectedAt.timeIntervalSince1970 * 1000
        self.messages = messages.filter {
            $0.createdAt >= collectedAt.addingTimeInterval(-1800) && $0.createdAt <= collectedAt
        }.suffix(1000).map(RemoteDiagnosticsMessage.init)
        self.displayedConversation = displayedConversation
        timeZone = TimeZone.current.identifier
        processId = ProcessInfo.processInfo.processIdentifier
        self.logs = logs
        logCoverage = "当前启动以来保留的近期元数据；较早条目可能受容量上限影响。row-presented 只证明视图挂载，不证明屏幕像素已绘制。"
    }
}

public enum RemoteDiagnosticsProtocol {
    public static let maximumBytes = 2 * 1024 * 1024

    public static func requestText(id: UUID) -> String {
        "/diagnostics \(id.uuidString.lowercased())"
    }

    public static func reportFileName(id: UUID) -> String {
        "remote-diagnostics-\(id.uuidString.lowercased()).json"
    }

    /// Only expected, bounded metadata survives decoding; unknown fields and
    /// free-form text cannot enter the final report through a remote attachment.
    public static func sanitizedReport(_ data: Data, requestID: UUID) throws -> Data {
        guard data.count <= maximumBytes else { throw RemoteDiagnosticsError.oversizedReport }
        guard let raw = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              finiteNumber(raw["schemaVersion"])?.doubleValue == 1,
              raw["requestId"] as? String == requestID.uuidString.lowercased(),
              let rawFiles = raw["files"] as? [[String: Any]], rawFiles.count <= 16
        else { throw RemoteDiagnosticsError.invalidReport }
        var result: [String: Any] = ["schemaVersion": 1, "requestId": requestID.uuidString.lowercased()]
        for key in ["appVersion", "platform", "projectId", "electronVersion", "timeZone"] {
            if let value = safeString(raw[key]) { result[key] = value }
        }
        for key in ["exportedAt", "from"] {
            if let value = finiteNumber(raw[key]) { result[key] = value }
        }
        result["files"] = rawFiles.map { file -> [String: Any] in
            var clean: [String: Any] = [:]
            for key in ["source", "status"] { if let value = safeString(file[key]) { clean[key] = value } }
            let events = file["events"] as? [Any] ?? []
            clean["truncated"] = (file["truncated"] as? Bool ?? false) || events.count > 1000
            clean["invalidLines"] = max(0, finiteNumber(file["invalidLines"])?.intValue ?? 0)
            clean["events"] = events.prefix(1000).map { metadata($0) }
            return clean
        }
        result["activeSessions"] = (raw["activeSessions"] as? [Any] ?? []).prefix(32).map { metadata($0) }
        if let coverage = raw["sourceCoverage"] as? [String: Any], let original = coverage["aicliOriginalEvents"] as? String,
           ["see-codex-original-events", "unavailable"].contains(original) {
            result["sourceCoverage"] = ["aicliOriginalEvents": original]
        }
        return try JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys])
    }

    private static func safeString(_ value: Any?) -> String? {
        guard let text = value as? String, !text.isEmpty, text.count <= 200,
              text.range(of: "^[a-zA-Z0-9_.:@/-]+$", options: .regularExpression) != nil
        else { return nil }
        return text
    }

    private static func finiteNumber(_ value: Any?) -> NSNumber? {
        guard let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
              number.doubleValue.isFinite else { return nil }
        return number
    }

    private static func metadata(_ value: Any, depth: Int = 0) -> [String: Any] {
        guard depth <= 3, let raw = value as? [String: Any] else { return [:] }
        var result: [String: Any] = [:]
        for key in ["id", "ID", "remoteMessageId", "callId", "sessionId", "taskId", "replyId", "messageId", "partId", "threadId", "turnId", "eventTaskId", "eventReplyId", "sourceCommit", "onDiskBinarySha256", "onDiskBinaryStatus", "event", "kind", "type", "phase", "delivery", "sourceKind", "cli", "status", "terminalKind", "hostStopReason", "stopReasonRequested", "spawnErrorCode", "signal", "exitCodeHex", "appVersion"] {
            if let value = safeString(raw[key]) { result[key] = value }
        }
        for key in ["createdAt", "startedAt", "pid", "hostPid", "lifetimeMs", "lastInputAt", "lastOutputAt", "lastEtxAt", "stopRequestedAt", "exitCode", "textLength", "inputLength", "resolvedLength", "forwardedChunks", "code", "errorCode", "messageId", "attempt", "onDiskBinaryBytes"] {
            if let value = finiteNumber(raw[key]) { result[key] = value }
        }
        for key in ["ok", "sourceStarted", "autoReplyToIm", "sdkReady", "isReady", "accepted"] {
            if let value = raw[key] as? Bool { result[key] = value }
        }
        if let detail = raw["detail"] { result["detail"] = metadata(detail, depth: depth + 1) }
        if let candidates = raw["candidates"] as? [Any] {
            result["candidates"] = candidates.prefix(20).map { metadata($0, depth: depth + 1) }
        }
        return result
    }

    public static func mergedReport(requestID: UUID, local: RemoteDiagnosticsLocalEvidence,
                                    remote: Data?, missingReason: String?) throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let localJSON = String(decoding: try encoder.encode(local), as: UTF8.self)
        let remoteJSON = try remote.map {
            String(decoding: try sanitizedReport($0, requestID: requestID), as: UTF8.self)
        } ?? "null"
        let note = missingReason ?? "已收到远端报告；各源的缺失、损坏或截断情况见报告字段。"
        let text = """
        # MaiChat 跨端远程排障报告

        请协助排查这段聊天近期的消息收发或展示异常；以下是自动收集的现场，未预判根因。

        排障编号：\(requestID.uuidString.lowercased())

        \(note)

        ## A 端现场（iOS MaiChat）

        以下是本地消息记录及采集时的会话选择状态，不是历史屏幕实际绘制完成的证明。不包含消息正文。

        ```json
        \(localJSON)
        ```

        ## B 端现场（MultiAICode）

        ```json
        \(remoteJSON)
        ```

        缺失记录不代表没有故障；两端时钟可能不同，请结合原始消息 ID 与排障编号分析。
        sourceCommit 是进程启动时记录的 manifest 提交；onDiskBinarySha256 是采集时的磁盘文件，不证明旧进程已经加载它。
        """
        return Data(text.utf8)
    }
}
