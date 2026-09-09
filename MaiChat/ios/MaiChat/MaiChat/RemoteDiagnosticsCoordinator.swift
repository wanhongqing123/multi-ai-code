import Foundation
import Combine
import MaiChatCore

/// Account-scoped message operations used by the coordinator. The application
/// implements these with its normal persisted send/receipt path.
@MainActor
protocol RemoteDiagnosticsContextProvider: AnyObject {
    var chatState: MasterChatState { get }
    var remoteDiagnosticsIdentity: String { get }
    func remoteDiagnosticsMayContinue(identity: String, peer: String, recipient: String) -> Bool
    func remoteDiagnosticsLogs(peer: String, since: Date) -> [RemoteDiagnosticsLogEntry]
    func sendRemoteDiagnosticsText(_ text: String, to peerID: String, identity: String) async -> Bool
    func sendRemoteDiagnosticsReport(_ file: URL, to recipient: RemoteIMContact, identity: String) async -> Bool
}

@MainActor
private final class DiagnosticDeliveryRace {
    var continuation: CheckedContinuation<Bool?, Never>?
    var work: Task<Void, Never>?
    var timer: Task<Void, Never>?

    func finish(_ value: Bool?) {
        guard let continuation else { return }
        self.continuation = nil
        timer?.cancel()
        work?.cancel()
        timer = nil
        work = nil
        continuation.resume(returning: value)
    }
}

@MainActor
final class RemoteDiagnosticsCoordinator: ObservableObject {
    @Published private(set) var status = "选择接收报告的好友后，确认收集并发送。"
    @Published private(set) var isRunning = false
    @Published private(set) var isSending = false
    private weak var appState: (any RemoteDiagnosticsContextProvider)?
    private var task: Task<Void, Never>?
    private let timeout: Duration
    private let pollInterval: Duration
    private let deliveryTimeout: Duration
    var contextIdentity: String { appState?.remoteDiagnosticsIdentity ?? "" }

    init(appState: any RemoteDiagnosticsContextProvider, timeout: Duration = .seconds(60), pollInterval: Duration = .milliseconds(500), deliveryTimeout: Duration = .seconds(30)) {
        self.appState = appState
        self.timeout = timeout
        self.pollInterval = pollInterval
        self.deliveryTimeout = deliveryTimeout
    }

    // SDK continuations may ignore Task cancellation. Do not use a task group
    // that would wait forever for the losing send; release the UI exactly once.
    private func deliver(_ operation: @escaping @MainActor () async -> Bool) async -> Bool? {
        let race = DiagnosticDeliveryRace()
        return await withTaskCancellationHandler {
            await withCheckedContinuation { continuation in
                race.continuation = continuation
                guard !Task.isCancelled else { race.finish(nil); return }
                race.work = Task { race.finish(await operation()) }
                race.timer = Task {
                    do {
                        try await Task.sleep(for: deliveryTimeout)
                        race.finish(nil)
                    } catch { }
                }
            }
        } onCancel: {
            Task { @MainActor in race.finish(nil) }
        }
    }

    func cancel() {
        guard !isSending else { return }
        task?.cancel()
        status = "已取消回传；已经发出的采集请求无法撤回。"
    }

    func start(peer: RemoteIMContact, recipient: RemoteIMContact, expectedIdentity: String? = nil) {
        guard !isRunning, let appState else { return }
        let identity = appState.remoteDiagnosticsIdentity
        if let expectedIdentity, expectedIdentity != identity {
            status = "确认期间账号或连接已变更，未发起采集，请关闭后重新确认。"
            return
        }
        guard appState.remoteDiagnosticsMayContinue(identity: identity, peer: peer.userID, recipient: recipient.userID) else {
            status = "当前账号、连接或好友已失效，未发起采集。"
            return
        }
        let requestID = UUID()
        let began = Date()
        let messages = appState.chatState.messages(with: peer.userID)
        let displayedConversation = appState.chatState.selectedPeerID == peer.userID
        isRunning = true
        status = "正在向 \(peer.displayName) 请求现场，最长等待 60 秒…"
        task = Task { [weak self, weak appState] in
            guard let self, let appState else { return }
            defer { self.isRunning = false; self.isSending = false; self.task = nil }
            do {
                try Task.checkCancellation()
                let deadline = ContinuousClock.now.advanced(by: self.timeout)
                let requested = await self.deliver {
                    await appState.sendRemoteDiagnosticsText(
                        RemoteDiagnosticsProtocol.requestText(id: requestID), to: peer.userID, identity: identity
                    )
                }
                try Task.checkCancellation()
                var remote: Data?
                var missing = requested == false ? "采集请求发送失败，未取得远端报告。" : "等待结束仍未收到有效报告；可能是对方未响应、版本不支持、采集失败或附件下载失败。"
                if requested == nil { missing += "采集请求发送结果也尚未确认，不能据此断言未送达。" }
                var inspected = Set<UUID>()
                var remoteFailed = false
                while requested != false && ContinuousClock.now < deadline {
                    try Task.checkCancellation()
                    guard appState.remoteDiagnosticsMayContinue(identity: identity, peer: peer.userID, recipient: recipient.userID) else {
                        self.status = "账号、连接或好友已变更，已取消回传。"
                        return
                    }
                    for message in appState.chatState.messages(with: peer.userID) {
                        // The fresh nonce and authenticated sender identify the
                        // response. A remote wall clock can legitimately differ.
                        guard message.direction == .incoming, message.fromUserID == peer.userID else { continue }
                        if let reason = RemoteDiagnosticsProtocol.failureReceipt(message.text, requestID: requestID) {
                            missing = reason
                            remoteFailed = true
                            break
                        }
                        guard let file = message.fileAttachment,
                              file.fileName == RemoteDiagnosticsProtocol.reportFileName(id: requestID),
                              !inspected.contains(message.id),
                              FileManager.default.fileExists(atPath: file.localFilePath)
                        else { continue }
                        inspected.insert(message.id)
                        do {
                            let path = file.localFilePath
                            remote = try await Task.detached {
                                let attributes = try FileManager.default.attributesOfItem(atPath: path)
                                guard let size = attributes[.size] as? NSNumber,
                                      size.intValue <= RemoteDiagnosticsProtocol.maximumBytes
                                else { throw RemoteDiagnosticsError.oversizedReport }
                                let handle = try FileHandle(forReadingFrom: URL(fileURLWithPath: path))
                                defer { try? handle.close() }
                                let data = try handle.read(upToCount: RemoteDiagnosticsProtocol.maximumBytes + 1) ?? Data()
                                return try RemoteDiagnosticsProtocol.sanitizedReport(data, requestID: requestID)
                            }.value
                            break
                        } catch {
                            missing = "收到的远端附件无效或超限，已拒绝合并；报告只包含本地现场。"
                        }
                    }
                    if remote != nil || remoteFailed { break }
                    if RemoteDiagnosticsProtocol.reportDownloadFailed(
                        appState.remoteDiagnosticsLogs(peer: peer.userID, since: began),
                        requestID: requestID, peerUserID: peer.userID
                    ) {
                        missing = "已收到本次报告的附件通知，但附件下载失败；本报告仅含本地现场。"
                        break
                    }
                    try await Task.sleep(for: self.pollInterval)
                }
                try Task.checkCancellation()
                guard appState.remoteDiagnosticsMayContinue(identity: identity, peer: peer.userID, recipient: recipient.userID) else {
                    self.status = "账号、连接或好友已变更，已取消回传。"
                    return
                }
                let sanitizedRemote = remote
                let missingReason = remote == nil ? missing : nil
                // Keep the original conversation snapshot, but include the
                // request/download observations made during this collection.
                let local = RemoteDiagnosticsLocalEvidence(
                    appVersion: Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "unknown",
                    ownerUserID: appState.chatState.ownerUserID, peerUserID: peer.userID, collectedAt: began,
                    messages: messages, displayedConversation: displayedConversation,
                    logs: appState.remoteDiagnosticsLogs(peer: peer.userID, since: began.addingTimeInterval(-1800))
                )
                let file = try await Task.detached {
                    let data = try RemoteDiagnosticsProtocol.mergedReport(requestID: requestID, local: local, remote: sanitizedRemote, missingReason: missingReason)
                    let folder = FileManager.default.temporaryDirectory.appendingPathComponent("MaiChatRemoteDiagnostics", isDirectory: true)
                    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
                    let url = folder.appendingPathComponent("maichat-diagnostics-\(requestID.uuidString.lowercased()).md")
                    try data.write(to: url, options: .atomic)
                    try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
                    return url
                }.value
                try Task.checkCancellation()
                guard appState.remoteDiagnosticsMayContinue(identity: identity, peer: peer.userID, recipient: recipient.userID) else {
                    self.status = "账号或好友已变更，报告未发送。"
                    return
                }
                self.isSending = true
                self.status = "正在将\(remote == nil ? "部分" : "合并")报告发送给 \(recipient.displayName)…"
                let sent = await self.deliver { await appState.sendRemoteDiagnosticsReport(file, to: recipient, identity: identity) }
                if sent == true {
                    self.status = "已发送给 \(recipient.displayName)\(remote == nil ? "（远端现场缺失，原因已写入报告）" : "，缺失或截断项目已在报告中标明")。"
                } else if sent == nil {
                    self.status = "发送结果尚未确认，请查看排查好友聊天中的消息状态，避免连续重发。"
                } else {
                    self.status = "报告发送失败，没有确认送达。可重新发起；不需要到故障机器搬日志。"
                }
            } catch is CancellationError {
                self.status = "已取消回传。"
            } catch {
                self.status = "无法生成排障报告，未发送附件。"
            }
        }
    }
}
