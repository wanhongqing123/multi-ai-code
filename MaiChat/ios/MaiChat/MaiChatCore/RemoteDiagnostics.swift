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
        let allowed: Set<String> = ["peer", "message", "launch", "kind", "result", "code", "duration_ms", "operation", "cached_messages", "near_bottom", "scroll_action", "app_version", "build", "ios", "pid", "scope"]
        let numeric: Set<String> = ["samples", "slow_samples", "slow_threshold_ms", "max_ms", "average_ms", "late_ms", "sample_interval_ms", "mutation_count", "upserted_count", "removed_count", "message_count", "limit", "dropped_entries", "export_truncated", "retained_entries", "capacity"]
        fields = entry.fields.filter { key, value in
            if numeric.contains(key) {
                guard value.count <= 30, let number = Double(value), number.isFinite, number >= 0 else { return false }
                return true
            }
            if key == "requestId" {
                return UUID(uuidString: value)?.uuidString.lowercased() == value
            }
            return allowed.contains(key) && value.count <= 120 &&
                value.range(of: "^[a-zA-Z0-9_.:#/-]+$", options: .regularExpression) != nil
        }
    }
}

public struct RemoteDiagnosticsLocalEvidence: Codable, Sendable {
    public let appVersion: String
    public let platform: String
    public let osVersion: String
    public let ownerUserID: String
    public let peerUserID: String
    public let collectedAt: Double
    public let messages: [RemoteDiagnosticsMessage]
    public let displayedConversation: Bool
    public let timeZone: String
    public let processId: Int32
    public let loadedMessageCount: Int
    public let excludedMessageCount: Int
    public let logs: [RemoteDiagnosticsLogEntry]
    public let logCoverage: String
    public let performanceCoverage: [String: String]

    public init(appVersion: String, ownerUserID: String, peerUserID: String,
                collectedAt: Date, messages: [RemoteIMMessage], displayedConversation: Bool,
                logs: [RemoteDiagnosticsLogEntry] = []) {
        self.appVersion = appVersion
        platform = "iOS"
        osVersion = ProcessInfo.processInfo.operatingSystemVersionString
        self.ownerUserID = ownerUserID
        self.peerUserID = peerUserID
        self.collectedAt = collectedAt.timeIntervalSince1970 * 1000
        let observedTags = Set(logs.compactMap { $0.fields["message"] })
        let selected = messages.filter {
            ($0.createdAt >= collectedAt.addingTimeInterval(-1800) && $0.createdAt <= collectedAt) ||
                observedTags.contains(DiagnosticLogPrivacy.stableTag($0.remoteID ?? $0.id.uuidString, prefix: "m"))
        }
        self.messages = selected.suffix(1000).map(RemoteDiagnosticsMessage.init)
        loadedMessageCount = messages.count
        excludedMessageCount = messages.count - self.messages.count
        self.displayedConversation = displayedConversation
        timeZone = TimeZone.current.identifier
        processId = ProcessInfo.processInfo.processIdentifier
        self.logs = logs
        let performance = logs.filter { RemoteDiagnosticsProtocol.performanceEvents.contains($0.event) }
        performanceCoverage = [
            "status": performance.isEmpty ? "no-retained-samples" : "sampled",
            "scope": "account-process", "eventCount": String(performance.count),
            "firstEventAt": performance.first?.createdAt ?? "unavailable",
            "lastEventAt": performance.last?.createdAt ?? "unavailable",
            "mainRunloopIntervalMs": "250", "mainRunloopLogThrottleMs": "2000",
            "dropCounterScope": "since-last-account-activation", "mainRunloopDelayThresholdMs": "150",
            "operationSlowThresholdMs": "16", "stackTraces": "unavailable",
            "retention": "in-memory-current-launch-bounded-2048-entries-export-last-1000",
            "droppedEntries": logs.last(where: { $0.event == "diagnostic-log-coverage" })?.fields["dropped_entries"] ?? "unknown",
            "exportTruncated": logs.last(where: { $0.event == "diagnostic-log-coverage" })?.fields["export_truncated"] ?? "unknown",
            "interpretation": "App-wide timings are not proof this conversation caused a stall; absent events are not zero latency."
        ]
        logCoverage = "当前账号本次启动以来保留的近期元数据；保留近期本地观察到的消息，不用服务端时钟排除这些消息。较早条目可能受容量上限影响。row-presented 只证明视图挂载，不证明屏幕像素已绘制。"
    }
}

public enum RemoteDiagnosticsProtocol {
    public static let performanceEvents: Set<String> = ["composer-edit", "composer-text-mutation", "composer-update", "composer-layout", "composer-height-queue", "asr-main-actor-wait", "main-runloop-delay"]
    public static let historyEvents: Set<String> = ["history-save-slow", "history-save-failed", "history-load-failed", "history-load-completed"]
    public static let maximumBytes = 2 * 1024 * 1024

    public static func accountTag(sdkAppID: Int, ownerUserID: String) -> String {
        DiagnosticLogPrivacy.stableTag("\(sdkAppID):\(ownerUserID)", prefix: "a")
    }

    public static func scopedLogs(_ entries: [DiagnosticLogEntry], accountTag: String,
                                  peerUserID: String, since: Date, limit: Int = 1000) -> [RemoteDiagnosticsLogEntry] {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        let peer = DiagnosticLogPrivacy.stableTag(peerUserID, prefix: "u")
        let connectionEvents: Set<String> = ["login-start", "login-finished", "connect-start", "connect-finished", "disconnect-start", "disconnect-finished", "sdk-connecting", "sdk-connected", "sdk-connect-failed", "sdk-kicked-offline", "sdk-user-sig-expired"]
        return entries.filter { entry in
            guard !accountTag.isEmpty, entry.fields["account"] == accountTag,
                  let date = formatter.date(from: entry.createdAt), date >= since else { return false }
            return entry.fields["peer"] == peer || connectionEvents.contains(entry.event) ||
                (entry.category == "interaction-performance" && performanceEvents.contains(entry.event) && entry.fields["scope"] == "account-process") ||
                (entry.fields["peer"] == nil && historyEvents.contains(entry.event))
        }.suffix(max(0, min(limit, 2048))).map(RemoteDiagnosticsLogEntry.init)
    }

    public static func requestText(id: UUID) -> String {
        "/diagnostics \(id.uuidString.lowercased())"
    }

    public static func reportFileName(id: UUID) -> String {
        "remote-diagnostics-\(id.uuidString.lowercased()).json"
    }

    public static func requestID(reportFileName name: String) -> UUID? {
        let prefix = "remote-diagnostics-"
        guard name.hasPrefix(prefix), name.hasSuffix(".json"),
              let id = UUID(uuidString: String(name.dropFirst(prefix.count).dropLast(5))),
              reportFileName(id: id) == name else { return nil }
        return id
    }

    /// Entries must already be account-scoped by the context provider. Require
    /// this peer and nonce too; unrelated failed attachments cannot end a wait.
    public static func reportDownloadFailed(_ logs: [RemoteDiagnosticsLogEntry], requestID: UUID, peerUserID: String) -> Bool {
        logs.contains {
            $0.event == "media-download-finished" && $0.fields["kind"] == "file" &&
                $0.fields["result"] == "failed" &&
                $0.fields["peer"] == DiagnosticLogPrivacy.stableTag(peerUserID, prefix: "u") &&
                $0.fields["requestId"] == requestID.uuidString.lowercased()
        }
    }

    /// Recognize only our host's nonce-bound failure receipts (including the
    /// older unsupported-command reply). Never copy free-form remote text.
    public static func failureReceipt(_ text: String, requestID: UUID) -> String? {
        let id = requestID.uuidString.lowercased()
        if text == "远程排障请求过于频繁，请稍后重试（\(id)）。" {
            return "远端拒绝了过于频繁的采集请求；本报告仅含本地现场。"
        }
        if text.hasPrefix("远程排障采集失败（编号 \(id)）：") {
            return "远端报告采集失败；本报告仅含本地现场。"
        }
        if text == "当前 MultiAICode 尚未接入远程排障（\(id)），请升级后重试。" {
            return "远端排障服务尚未接入，暂未取得远端现场；本报告仅含本地现场。"
        }
        if text.components(separatedBy: "\n").first == "不支持的 IM 控制命令：/diagnostics \(id)" {
            return "远端版本不支持远程排障；本报告仅含本地现场。"
        }
        return nil
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
        if let coverage = raw["sourceCoverage"] as? [String: Any], coverage["uiPerformance"] as? String == "unavailable" {
            var clean = result["sourceCoverage"] as? [String: Any] ?? [:]
            clean["uiPerformance"] = "unavailable"
            result["sourceCoverage"] = clean
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
        for key in ["id", "ID", "remoteMessageId", "callId", "sessionId", "taskId", "replyId", "messageId", "partId", "threadId", "turnId", "eventTaskId", "eventReplyId", "sourceCommit", "onDiskBinarySha256", "onDiskBinaryStatus", "stage", "event", "kind", "type", "phase", "delivery", "sourceKind", "cli", "status", "terminalKind", "hostStopReason", "stopReasonRequested", "spawnErrorCode", "signal", "exitCodeHex", "appVersion"] {
            if let value = safeString(raw[key]) { result[key] = value }
        }
        for key in ["createdAt", "startedAt", "pid", "hostPid", "lifetimeMs", "lastInputAt", "lastOutputAt", "lastEtxAt", "stopRequestedAt", "exitCode", "textLength", "inputLength", "resolvedLength", "forwardedChunks", "code", "errorCode", "messageId", "attempt", "onDiskBinaryBytes", "duration_ms", "samples", "slow_samples", "slow_threshold_ms", "max_ms", "average_ms", "late_ms", "sample_interval_ms", "visibleLength", "cellCount", "replacedCells"] {
            if let value = finiteNumber(raw[key]) { result[key] = value }
        }
        for key in ["ok", "sourceStarted", "autoReplyToIm", "sdkReady", "isReady", "accepted", "hasStream", "replay"] {
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

        请协助排查这段聊天近期的消息收发、键盘输入、界面响应或展示异常；以下是自动收集的现场，未预判根因。

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
