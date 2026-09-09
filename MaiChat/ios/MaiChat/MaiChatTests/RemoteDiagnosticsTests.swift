import XCTest
import MaiChatCore
import SwiftUI

@MainActor
private final class DiagnosticsContext: RemoteDiagnosticsContextProvider {
    var chatState: MasterChatState
    var remoteDiagnosticsIdentity = "account-generation-1"
    var connected = true
    var sentReports: [(String, String)] = []
    var requests: [String] = []
    var onRequest: ((DiagnosticsContext, UUID) throws -> Void)?
    var holdRequest = false
    var pendingRequest: CheckedContinuation<Bool, Never>?
    let directory: URL

    init(directory: URL) throws {
        self.directory = directory
        chatState = MasterChatState(ownerUserID: "phone")
        for userID in ["machine", "helper", "other"] {
            try chatState.upsertContact(userID: userID, relation: .friend)
        }
        chatState.selectPeer(userID: "machine")
    }

    func remoteDiagnosticsMayContinue(identity: String, peer: String, recipient: String) -> Bool {
        connected && identity == remoteDiagnosticsIdentity &&
            chatState.contacts.contains(where: { $0.userID == peer }) &&
            chatState.contacts.contains(where: { $0.userID == recipient })
    }
    func remoteDiagnosticsLogs(peer: String, since: Date) -> [RemoteDiagnosticsLogEntry] { [] }

    func sendRemoteDiagnosticsText(_ text: String, to peerID: String, identity: String) async -> Bool {
        requests.append(peerID)
        if holdRequest { return await withCheckedContinuation { pendingRequest = $0 } }
        guard let id = UUID(uuidString: String(text.dropFirst("/diagnostics ".count))) else { return false }
        do { try onRequest?(self, id); return true } catch { return false }
    }

    func sendRemoteDiagnosticsReport(_ file: URL, to recipient: RemoteIMContact, identity: String) async -> Bool {
        guard remoteDiagnosticsIdentity == identity else { return false }
        guard let text = try? String(contentsOf: file, encoding: .utf8) else { return false }
        sentReports.append((recipient.userID, text))
        return true
    }

    func respond(id: UUID, from: String = "machine", payloadID: UUID? = nil, createdAt: Date = Date()) throws {
        let data = try JSONSerialization.data(withJSONObject: [
            "schemaVersion": 1, "requestId": (payloadID ?? id).uuidString.lowercased(),
            "token": "TOKEN_SENTINEL", "files": [["source": "runtime", "status": "ok", "events": [
                ["event": "item_completed", "messageId": "call-async", "phase": "final_answer", "delivery": "async", "text": "BODY_SENTINEL"]
            ]]]
        ])
        let path = directory.appendingPathComponent(UUID().uuidString + ".json")
        try data.write(to: path)
        chatState.receiveFile(filePath: path.path, fromUserID: from,
            fileName: RemoteDiagnosticsProtocol.reportFileName(id: id), mimeType: "application/json", now: createdAt)
    }
}

@MainActor
final class RemoteDiagnosticsTests: XCTestCase {
    func testPresentingTheFormDoesNotAutoStartCollection() async throws {
        try await withContext { context in
            let coordinator = RemoteDiagnosticsCoordinator(appState: context)
            var appeared = false
            let view = RemoteDiagnosticsView(peer: context.chatState.contacts[0], contacts: context.chatState.contacts,
                ownerUserID: "phone", connected: true, coordinator: coordinator)
                .onAppear { appeared = true }
            let controller = UIHostingController(rootView: view)
            let window: UIWindow
            if let scene = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }).first {
                window = UIWindow(windowScene: scene)
            } else {
                window = UIWindow()
            }
            window.frame = CGRect(x: 0, y: 0, width: 393, height: 852)
            window.rootViewController = controller
            window.makeKeyAndVisible()
            defer { window.isHidden = true }
            controller.view.frame = window.bounds
            controller.view.backgroundColor = .systemBackground
            controller.beginAppearanceTransition(true, animated: false)
            controller.view.setNeedsLayout()
            controller.view.layoutIfNeeded()
            controller.endAppearanceTransition()
            try await Task.sleep(for: .milliseconds(200))
            // This logic-test host does not provide a composited screen. UI
            // rendering is verified separately in a scene-backed preview app;
            // here require the real appearance callback before testing consent.
            XCTAssertTrue(appeared)
            XCTAssertFalse(coordinator.isRunning)
            XCTAssertTrue(context.requests.isEmpty)
            XCTAssertTrue(context.sentReports.isEmpty)
        }
    }
    private func withContext(_ body: (DiagnosticsContext) async throws -> Void) async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("diagnostics-test-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        try await body(DiagnosticsContext(directory: directory))
    }

    private func waitForCompletion(_ coordinator: RemoteDiagnosticsCoordinator) async throws {
        let limit = ContinuousClock.now.advanced(by: .seconds(3))
        while coordinator.isRunning && ContinuousClock.now < limit { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertFalse(coordinator.isRunning)
    }

    func testMergesTheActualAttachmentAndKeepsRecipientAfterSwitchingChats() async throws {
        try await withContext { context in
            context.onRequest = { context, id in
                try context.respond(id: id)
                try context.respond(id: id) // Duplicate response must not create a second report.
                context.chatState.selectPeer(userID: "other")
            }
            let coordinator = RemoteDiagnosticsCoordinator(appState: context, timeout: .milliseconds(100), pollInterval: .milliseconds(5))
            coordinator.start(peer: context.chatState.contacts[0], recipient: context.chatState.contacts[1])
            try await waitForCompletion(coordinator)
            XCTAssertEqual(context.requests, ["machine"])
            XCTAssertEqual(context.sentReports.map(\.0), ["helper"])
            let report = try XCTUnwrap(context.sentReports.first?.1)
            XCTAssertTrue(report.contains("call-async"))
            XCTAssertTrue(report.contains("final_answer"))
            XCTAssertFalse(report.contains("TOKEN_SENTINEL"))
            XCTAssertFalse(report.contains("BODY_SENTINEL"))
        }
    }

    func testWrongSenderAndWrongRequestIdProduceAnExplicitPartialReport() async throws {
        try await withContext { context in
            context.onRequest = { context, id in
                try context.respond(id: id, from: "other")
                try context.respond(id: id, payloadID: UUID())
            }
            let coordinator = RemoteDiagnosticsCoordinator(appState: context, timeout: .milliseconds(70), pollInterval: .milliseconds(5))
            coordinator.start(peer: context.chatState.contacts[0], recipient: context.chatState.contacts[1])
            try await waitForCompletion(coordinator)
            let report = try XCTUnwrap(context.sentReports.first?.1)
            XCTAssertEqual(context.sentReports.map(\.0), ["helper"])
            XCTAssertTrue(report.contains("远端附件无效"))
            XCTAssertFalse(report.contains("call-async"))
        }
    }

    func testAccountChangeCancelsForwarding() async throws {
        try await withContext { context in
            context.onRequest = { context, id in
                try context.respond(id: id)
                context.remoteDiagnosticsIdentity = "account-generation-2"
            }
            let coordinator = RemoteDiagnosticsCoordinator(appState: context)
            coordinator.start(peer: context.chatState.contacts[0], recipient: context.chatState.contacts[1])
            try await waitForCompletion(coordinator)
            XCTAssertTrue(context.sentReports.isEmpty)
            XCTAssertTrue(coordinator.status.contains("取消"))
        }
    }

    func testFreshNonceStillMatchesWhenRemoteClockIsBehind() async throws {
        try await withContext { context in
            context.onRequest = { context, id in try context.respond(id: id, createdAt: Date().addingTimeInterval(-3600)) }
            let coordinator = RemoteDiagnosticsCoordinator(appState: context, timeout: .milliseconds(100), pollInterval: .milliseconds(5))
            coordinator.start(peer: context.chatState.contacts[0], recipient: context.chatState.contacts[1])
            try await waitForCompletion(coordinator)
            XCTAssertTrue(try XCTUnwrap(context.sentReports.first?.1).contains("call-async"))
        }
    }

    func testMissingSDKCallbackDoesNotHoldTheCoordinatorOpen() async throws {
        try await withContext { context in
            context.holdRequest = true
            defer { context.pendingRequest?.resume(returning: true); context.pendingRequest = nil }
            let coordinator = RemoteDiagnosticsCoordinator(appState: context, timeout: .milliseconds(50), pollInterval: .milliseconds(5), deliveryTimeout: .milliseconds(20))
            coordinator.start(peer: context.chatState.contacts[0], recipient: context.chatState.contacts[1])
            try await waitForCompletion(coordinator)
            XCTAssertEqual(context.sentReports.count, 1)
            XCTAssertTrue(try XCTUnwrap(context.sentReports.first?.1).contains("未回传有效报告"))
        }
    }

    func testCancelBeforeTheTaskStartsSendsNothing() async throws {
        try await withContext { context in
            let coordinator = RemoteDiagnosticsCoordinator(appState: context)
            coordinator.start(peer: context.chatState.contacts[0], recipient: context.chatState.contacts[1])
            coordinator.cancel()
            try await waitForCompletion(coordinator)
            XCTAssertTrue(context.requests.isEmpty)
            XCTAssertTrue(context.sentReports.isEmpty)
        }
    }

    func testTimeoutStillSendsLocalEvidenceWithMissingReason() async throws {
        try await withContext { context in
            let coordinator = RemoteDiagnosticsCoordinator(appState: context, timeout: .milliseconds(30), pollInterval: .milliseconds(5))
            coordinator.start(peer: context.chatState.contacts[0], recipient: context.chatState.contacts[1])
            try await waitForCompletion(coordinator)
            let report = try XCTUnwrap(context.sentReports.first?.1)
            XCTAssertTrue(report.contains("未回传有效报告"))
            XCTAssertTrue(report.contains("ownerUserID"))
        }
    }

    func testOversizedAndUnknownSchemaReportsAreRejected() throws {
        let id = UUID()
        XCTAssertThrowsError(try RemoteDiagnosticsProtocol.sanitizedReport(Data(repeating: 32, count: RemoteDiagnosticsProtocol.maximumBytes + 1), requestID: id))
        let unknown = try JSONSerialization.data(withJSONObject: ["schemaVersion": 2, "requestId": id.uuidString.lowercased(), "files": []])
        XCTAssertThrowsError(try RemoteDiagnosticsProtocol.sanitizedReport(unknown, requestID: id))
        let boolean = try JSONSerialization.data(withJSONObject: ["schemaVersion": true, "requestId": id.uuidString.lowercased(), "files": []])
        XCTAssertThrowsError(try RemoteDiagnosticsProtocol.sanitizedReport(boolean, requestID: id))
    }

    func testSDKAndLocalIdsSurviveSanitizingAsDistinctTypes() throws {
        let id = UUID()
        let raw = try JSONSerialization.data(withJSONObject: ["schemaVersion": 1, "requestId": id.uuidString.lowercased(), "files": [
            ["source": "runtime", "events": [["messageId": 1637, "detail": ["ID": "sdk-remote-id", "attempt": 1, "token": "TOKEN_SENTINEL"]]]]
        ]])
        let sanitized = try RemoteDiagnosticsProtocol.sanitizedReport(raw, requestID: id)
        let root = try XCTUnwrap(JSONSerialization.jsonObject(with: sanitized) as? [String: Any])
        let files = try XCTUnwrap(root["files"] as? [[String: Any]])
        let events = try XCTUnwrap(files[0]["events"] as? [[String: Any]])
        XCTAssertEqual(events[0]["messageId"] as? Int, 1637)
        let detail = try XCTUnwrap(events[0]["detail"] as? [String: Any])
        XCTAssertEqual(detail["ID"] as? String, "sdk-remote-id")
        XCTAssertFalse(String(decoding: sanitized, as: UTF8.self).contains("TOKEN_SENTINEL"))
    }

    func testFailureReceiptsRequireTheExactNonceAndDoNotCopyRemoteText() {
        let id = UUID()
        let nonce = id.uuidString.lowercased()
        XCTAssertNotNil(RemoteDiagnosticsProtocol.failureReceipt("不支持的 IM 控制命令：/diagnostics \(nonce)\n可用命令…", requestID: id))
        XCTAssertNotNil(RemoteDiagnosticsProtocol.failureReceipt("远程排障请求过于频繁，请稍后重试（\(nonce)）。", requestID: id))
        XCTAssertNil(RemoteDiagnosticsProtocol.failureReceipt("远程排障请求过于频繁，请稍后重试（\(UUID().uuidString.lowercased())）。", requestID: id))
        let reason = RemoteDiagnosticsProtocol.failureReceipt("远程排障采集失败（编号 \(nonce)）：PRIVATE_ERROR_SENTINEL", requestID: id)
        XCTAssertNotNil(reason)
        XCTAssertFalse(reason?.contains("PRIVATE_ERROR_SENTINEL") ?? true)
    }

    func testSharedFailureReceiptContract() throws {
        let url = try XCTUnwrap(Bundle(for: Self.self).url(forResource: "remote-diagnostics-failure-receipts", withExtension: "json"))
        let root = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any])
        let id = try XCTUnwrap(UUID(uuidString: try XCTUnwrap(root["requestId"] as? String)))
        let cases = try XCTUnwrap(root["cases"] as? [[String: Any]])
        XCTAssertEqual(cases.count, 6)
        for row in cases {
            let text = try XCTUnwrap(row["text"] as? String)
            XCTAssertEqual(RemoteDiagnosticsProtocol.failureReceipt(text, requestID: id) != nil,
                row["recognized"] as? Bool, row["category"] as? String ?? "unknown")
        }
    }
}
