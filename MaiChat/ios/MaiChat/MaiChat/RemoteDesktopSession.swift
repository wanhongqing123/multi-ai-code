import Foundation
import MaiChatCore
import SwiftUI
import UIKit

#if canImport(TXLiteAVSDK_TRTC)
@preconcurrency import TXLiteAVSDK_TRTC
#endif

enum RemoteDesktopViewerState: Equatable {
    case idle
    case inviting
    case connecting
    case viewing
    case failed(String)

    var isActive: Bool {
        switch self {
        case .inviting, .connecting, .viewing:
            return true
        case .idle, .failed:
            return false
        }
    }

    var statusText: String {
        switch self {
        case .idle:
            return "选择一台设备开始查看"
        case .inviting:
            return "等待对方确认..."
        case .connecting:
            return "正在连接对方屏幕..."
        case .viewing:
            return "远程画面已连接"
        case let .failed(reason):
            return reason
        }
    }

    var diagnosticName: String {
        switch self {
        case .idle:
            return "idle"
        case .inviting:
            return "inviting"
        case .connecting:
            return "connecting"
        case .viewing:
            return "viewing"
        case .failed:
            return "failed"
        }
    }
}

enum RemotePointerCaptureDropReason: String {
    case invalidGeometry = "invalid-geometry"
    case letterbox
}

struct RemotePointerCaptureDiagnostic {
    let location: CGPoint
    let viewportSize: CGSize
    let videoSize: CGSize
    let activeContentRect: CGRect?
    let normalizedPoint: CGPoint?
    let dropReason: RemotePointerCaptureDropReason?
}

@MainActor
final class RemoteDesktopSession: NSObject, ObservableObject {
    @Published private(set) var state: RemoteDesktopViewerState = .idle
    @Published private(set) var peerUserID = ""
    @Published private(set) var noticeText: String?
    @Published private(set) var remoteVideoSize: CGSize = .zero
    @Published private(set) var captureGeometry: CaptureGeometry?
    @Published private(set) var isControlEnabled = false
    @Published private(set) var isLeftButtonHeld = false

    private weak var client: (any RemoteIMClient)?
    private let diagnosticLog: any DiagnosticLogSink
    private weak var renderView: UIView?
    private var credentials: Credentials?
    private var sessionID = ""
    private var roomID = ""
    private var invitationTimeoutTask: Task<Void, Never>?
    private var pointerFlushTask: Task<Void, Never>?
    private var wheelFlushTask: Task<Void, Never>?
    private var textFlushTask: Task<Void, Never>?
    private var pendingPointerPath: [CGPoint] = []
    private var pendingWheelDelta = 0
    private var pendingWheelPoint = CGPoint(x: 0.5, y: 0.5)
    private var pendingTextInput = ""
    private var lastPointer = CGPoint(x: 0.5, y: 0.5)
    private var lastPointerSentAt: TimeInterval = 0
    private var unreliableInputSequence: UInt32 = 0
    private var reliableInputSequence: UInt32 = 0
    private var requestStartedUptime: TimeInterval?
    private var enterRoomStartedUptime: TimeInterval?
    private var inputDiagnostics = RemoteInputDiagnosticAccumulator()
    private var inputDiagnosticTask: Task<Void, Never>?
    private var viewerHeartbeatTask: Task<Void, Never>?
    private var quietInputWindows = 0
    private var lastPointerCaptureDiagnostic: RemotePointerCaptureDiagnostic?

    #if canImport(TXLiteAVSDK_TRTC)
    private struct TRTCExitContext {
        let generation: UInt64
        let sessionID: String
        let peerUserID: String
        let roomID: String
        let requestedAt: TimeInterval
        var timedOut: Bool
    }

    private struct SubstreamRecoveryContext {
        let generation: UInt64
        let sessionID: String
        let peerUserID: String
        let roomID: String
        let startedAt: TimeInterval
    }

    private let cloud = TRTCCloud.sharedInstance()
    private var hasEnteredRoom = false
    private var isEnterRoomInFlight = false
    private var isRemoteViewStarted = false
    private var pendingStartAfterExit = false
    private var hasRegisteredTRTCDelegate = false
    private var nextTRTCExitGeneration: UInt64 = 0
    private var trtcExitContext: TRTCExitContext?
    private var trtcExitWatchdogTask: Task<Void, Never>?
    private var substreamRecoveryTask: Task<Void, Never>?
    private var nextSubstreamRecoveryGeneration: UInt64 = 0
    private var substreamRecoveryContext: SubstreamRecoveryContext?
    private var connectionLostUptime: TimeInterval?
    private var reconnectAttemptCount = 0
    private var warningLastLogUptimeByCode: [Int: TimeInterval] = [:]

    private var isTRTCExitPending: Bool {
        trtcExitContext != nil
    }
    #endif

    private struct Credentials {
        let sdkAppID: Int
        let localUserID: String
        let userSig: String
    }

    init(
        client: any RemoteIMClient,
        diagnosticLog: any DiagnosticLogSink = AppDiagnosticLog.shared
    ) {
        self.client = client
        self.diagnosticLog = diagnosticLog
        super.init()
    }

    var diagnosticTraceID: String {
        Self.traceTag(for: sessionID)
    }

    var canStart: Bool {
        switch state {
        case .idle, .failed:
            return true
        case .inviting, .connecting, .viewing:
            return false
        }
    }

    func requestView(
        peerUserID: String,
        sdkAppID: Int,
        localUserID: String,
        userSig: String
    ) async {
        guard canStart else {
            log(
                level: .warning,
                category: "remote-desktop",
                event: "request-blocked",
                fields: ["reason": "session-active"]
            )
            return
        }

        #if canImport(TXLiteAVSDK_TRTC)
        if trtcExitContext?.timedOut == true {
            log(
                level: .error,
                category: "trtc",
                event: "request-blocked",
                fields: ["reason": "previous-room-exit-timeout"]
            )
            transition(
                to: .failed("远程组件仍在释放，请稍后重试或重新打开 App"),
                cause: "previous-room-exit-timeout"
            )
            return
        }
        #endif

        cleanupTRTC()
        let identifier = Self.newIdentifier()
        let roomIdentifier = Self.newIdentifier()
        self.peerUserID = peerUserID
        self.sessionID = identifier
        self.roomID = "mc-\(localUserID)-\(roomIdentifier)"
        self.credentials = Credentials(
            sdkAppID: sdkAppID,
            localUserID: localUserID,
            userSig: userSig
        )
        captureGeometry = nil
        requestStartedUptime = ProcessInfo.processInfo.systemUptime
        enterRoomStartedUptime = nil
        self.noticeText = nil
        transition(to: .inviting, cause: "user-request")
        scheduleInvitationTimeout(sessionID: identifier)

        let signal = RemoteDesktopSignal(
            kind: .invite,
            sessionID: identifier,
            roomID: roomID
        )
        do {
            try await send(signal, to: peerUserID)
        } catch {
            guard sessionID == identifier, state == .inviting else {
                log(
                    level: .warning,
                    category: "remote-desktop",
                    event: "invite-send-completion-ignored",
                    fields: ["reason": "stale-session"],
                    traceSource: identifier,
                    peerSource: peerUserID
                )
                return
            }
            invitationTimeoutTask?.cancel()
            invitationTimeoutTask = nil
            transition(to: .failed("远程请求发送失败：\(error.localizedDescription)"), cause: "invite-send-failed")
        }
    }

    @discardableResult
    func handleIncomingText(from userID: String, text: String) -> Bool {
        guard RemoteDesktopSignal.isSignalText(text) else { return false }
        guard let signal = RemoteDesktopSignal.decodeText(text) else {
            log(
                level: .warning,
                category: "remote-desktop",
                event: "signal-decode-failed",
                fields: [
                    "bytes": String(text.lengthOfBytes(using: .utf8)),
                    "from": Self.peerTag(for: userID),
                ]
            )
            return true
        }

        log(
            level: .info,
            category: "remote-desktop",
            event: "signal-received",
            fields: [
                "kind": signal.kind.rawValue,
                "from": Self.peerTag(for: userID),
                "incoming_trace": Self.traceTag(for: signal.sessionID),
            ]
        )

        switch signal.kind {
        case .invite:
            let rejection = RemoteDesktopSignal(
                kind: .reject,
                sessionID: signal.sessionID,
                reason: "iOS 暂不支持共享本机屏幕"
            )
            Task { [weak self] in
                try? await self?.send(rejection, to: userID)
            }
        case .accept:
            guard userID == peerUserID,
                  signal.sessionID == sessionID,
                  state == .inviting
            else {
                logIgnoredSignal(signal, from: userID)
                return true
            }
            if !signal.roomID.isEmpty {
                roomID = signal.roomID
            }
            applyCaptureGeometry(from: signal)
            transition(to: .connecting, cause: "peer-accepted")
            startViewing()
        case .reject:
            guard userID == peerUserID,
                  signal.sessionID == sessionID,
                  state == .inviting
            else {
                logIgnoredSignal(signal, from: userID)
                return true
            }
            invitationTimeoutTask?.cancel()
            invitationTimeoutTask = nil
            cleanupTRTC()
            transition(
                to: .failed(signal.reason.isEmpty ? "对方拒绝了请求" : signal.reason),
                cause: "peer-rejected"
            )
        case .stop:
            guard userID == peerUserID, signal.sessionID == sessionID else {
                logIgnoredSignal(signal, from: userID)
                return true
            }
            resetSession(cause: "peer-stop")
        case .notice:
            guard userID == peerUserID, signal.sessionID == sessionID else {
                logIgnoredSignal(signal, from: userID)
                return true
            }
            switch signal.noticeCode {
            case "secure-desktop-entered":
                noticeText = "对方进入了安全桌面，画面可能暂时不可用"
                log(
                    level: .warning,
                    category: "remote-desktop",
                    event: "secure-desktop-entered"
                )
            case "secure-desktop-left":
                noticeText = nil
                log(
                    level: .info,
                    category: "remote-desktop",
                    event: "secure-desktop-left"
                )
            default:
                log(
                    level: .debug,
                    category: "remote-desktop",
                    event: "notice-ignored",
                    fields: ["reason": "unknown-code"]
                )
                break
            }
        }
        return true
    }

    func bindRenderView(_ view: UIView?) {
        guard renderView !== view else { return }
        renderView = view
        log(
            level: .info,
            category: "remote-desktop",
            event: view == nil ? "render-unbound" : "render-bound",
            fields: ["remote_view_started": diagnosticBool(isRemoteViewStartedForDiagnostics)]
        )
        #if canImport(TXLiteAVSDK_TRTC)
        guard isRemoteViewStarted, !peerUserID.isEmpty else { return }
        cloud.updateRemoteView(view, streamType: .sub, forUser: peerUserID)
        #endif
    }

    func setControlEnabled(_ enabled: Bool) {
        guard enabled != isControlEnabled else { return }
        guard !enabled || state == .viewing else {
            log(
                level: .warning,
                category: "remote-input",
                event: "control-blocked",
                fields: ["reason": "not-viewing"]
            )
            return
        }
        if enabled {
            isControlEnabled = true
            startInputDiagnostics()
            log(
                level: .info,
                category: "remote-input",
                event: "control-start",
                fields: ["entered_room": diagnosticBool(hasEnteredRoomForDiagnostics)]
            )
        } else {
            disableControl(sendRelease: true, cause: "user-disabled")
        }
    }

    func movePointer(x: Double, y: Double) {
        guard isControlEnabled else { return }
        guard state == .viewing else {
            inputDiagnostics.recordBlockedByState()
            return
        }
        let point = normalizedPoint(x: x, y: y)
        inputDiagnostics.recordMove(x: point.x, y: point.y)
        lastPointer = point
        pendingPointerPath.append(point)
        if pendingPointerPath.count > 12 {
            pendingPointerPath.remove(at: pendingPointerPath.count / 2)
            inputDiagnostics.recordCoalescedMove()
        }
        flushPointerIfReady()
    }

    func recordPointerCapture(_ diagnostic: RemotePointerCaptureDiagnostic) {
        guard isControlEnabled else { return }
        inputDiagnostics.recordPointerSeen()
        switch diagnostic.dropReason {
        case .invalidGeometry:
            inputDiagnostics.recordPointerDroppedInvalidGeometry()
        case .letterbox:
            inputDiagnostics.recordPointerDroppedLetterbox()
        case nil:
            break
        }
        lastPointerCaptureDiagnostic = diagnostic
    }

    func clickMouse(
        button: RemoteMouseButton,
        x: Double,
        y: Double,
        clickCount: Int = 1
    ) {
        guard clickCount > 0 else { return }
        guard isControlEnabled else { return }
        guard state == .viewing else {
            inputDiagnostics.recordBlockedByState()
            return
        }
        let point = normalizedPoint(x: x, y: y)
        inputDiagnostics.recordClick(x: point.x, y: point.y)
        lastPointer = point
        clearPendingPointer()
        flushPendingTextInput()

        var events: [RemoteInputEvent] = []
        for _ in 0..<min(clickCount, 2) {
            events.append(.mouseButton(button: button, pressed: true, x: point.x, y: point.y))
            events.append(.mouseButton(button: button, pressed: false, x: point.x, y: point.y))
        }
        _ = sendInputEvents(events, channel: .reliable)
    }

    func clickMouseAtCurrentPointer(button: RemoteMouseButton) {
        clickMouse(button: button, x: lastPointer.x, y: lastPointer.y)
    }

    func setLeftButtonHeld(_ held: Bool) {
        guard isControlEnabled else { return }
        guard state == .viewing else {
            inputDiagnostics.recordBlockedByState()
            return
        }
        guard held != isLeftButtonHeld else { return }
        inputDiagnostics.recordClick(x: lastPointer.x, y: lastPointer.y)
        clearPendingPointer()
        flushPendingTextInput()
        let event = RemoteInputEvent.mouseButton(
            button: .left,
            pressed: held,
            x: lastPointer.x,
            y: lastPointer.y
        )
        if sendInputEvents([event], channel: .reliable) {
            isLeftButtonHeld = held
        }
    }

    func scrollPointer(delta: Int, x: Double, y: Double) {
        guard delta != 0 else { return }
        guard isControlEnabled else { return }
        guard state == .viewing else {
            inputDiagnostics.recordBlockedByState()
            return
        }
        let point = normalizedPoint(x: x, y: y)
        inputDiagnostics.recordWheel(x: point.x, y: point.y)
        lastPointer = point
        pendingWheelDelta += delta
        pendingWheelPoint = point
        scheduleWheelFlush()
    }

    @discardableResult
    func sendTextInput(_ text: String) -> Bool {
        guard !text.isEmpty else { return false }
        guard isControlEnabled else { return false }
        guard state == .viewing else {
            inputDiagnostics.recordBlockedByState()
            return false
        }
        inputDiagnostics.recordText(
            characterCount: text.count,
            utf8Bytes: text.lengthOfBytes(using: .utf8)
        )
        pendingTextInput.append(text)
        if pendingTextInput.lengthOfBytes(using: .utf8) >= 700 {
            flushPendingTextInput()
        } else {
            scheduleTextFlush()
        }
        return true
    }

    func sendKeyPress(_ keyCode: UInt32) {
        guard isControlEnabled else { return }
        guard state == .viewing else {
            inputDiagnostics.recordBlockedByState()
            return
        }
        flushPendingTextInput()
        inputDiagnostics.recordKey()
        _ = sendInputEvents(
            [.key(code: keyCode, pressed: true), .key(code: keyCode, pressed: false)],
            channel: .reliable
        )
    }

    func stop(cause: String = "user") async {
        let peer = peerUserID
        let currentSessionID = sessionID
        log(
            level: .info,
            category: "remote-desktop",
            event: "stop-requested",
            fields: ["source": cause]
        )
        resetSession(cause: cause == "user" ? "user-stop" : cause)
        guard !peer.isEmpty, !currentSessionID.isEmpty else { return }
        do {
            try await send(
                RemoteDesktopSignal(kind: .stop, sessionID: currentSessionID),
                to: peer
            )
        } catch {
            // send(_:to:) already records the safe error type and numeric code.
        }
    }

    private func startViewing() {
        guard let credentials, !roomID.isEmpty else {
            log(
                level: .error,
                category: "remote-desktop",
                event: "enter-room-blocked",
                fields: ["reason": "missing-parameters"]
            )
            fail("远程连接参数不完整")
            return
        }

        startViewerHeartbeat()

        #if canImport(TXLiteAVSDK_TRTC)
        if isTRTCExitPending {
            pendingStartAfterExit = true
            var fields = ["reason": "previous-room-exiting"]
            if let exitContext = trtcExitContext {
                fields["waiting_for_trace"] = Self.traceTag(for: exitContext.sessionID)
                fields["exit_generation"] = String(exitContext.generation)
            }
            log(
                level: .info,
                category: "trtc",
                event: "enter-room-deferred",
                fields: fields
            )
            return
        }
        pendingStartAfterExit = false
        registerTRTCDelegateIfNeeded()
        cloud.setDefaultStreamRecvMode(false, video: false)
        cloud.muteAllRemoteAudio(true)

        let params = TRTCParams()
        params.sdkAppId = UInt32(credentials.sdkAppID)
        params.userId = credentials.localUserID
        params.userSig = credentials.userSig
        params.strRoomId = roomID
        enterRoomStartedUptime = ProcessInfo.processInfo.systemUptime
        log(
            level: .info,
            category: "trtc",
            event: "enter-room-requested",
            fields: [
                "sdk_app_id": String(credentials.sdkAppID),
                "render_bound": diagnosticBool(renderView != nil),
            ]
        )
        isEnterRoomInFlight = true
        cloud.enterRoom(params, appScene: .videoCall)
        #else
        log(
            level: .error,
            category: "trtc",
            event: "sdk-unavailable"
        )
        fail("TRTC SDK 未集成，请执行 pod install 后使用 MaiChat.xcworkspace 编译")
        #endif
    }

    private enum InputChannel {
        case unreliable
        case reliable
    }

    private func normalizedPoint(x: Double, y: Double) -> CGPoint {
        CGPoint(x: Self.clamped(x), y: Self.clamped(y))
    }

    private static func clamped(_ value: Double) -> Double {
        guard value.isFinite else { return 0 }
        return min(max(value, 0), 1)
    }

    private func flushPointerIfReady() {
        guard pointerFlushTask == nil, !pendingPointerPath.isEmpty else { return }
        let elapsed = ProcessInfo.processInfo.systemUptime - lastPointerSentAt
        // Leave headroom under TRTC's shared 30 custom-message/s budget for
        // reliable keyboard and click events while the pointer is moving.
        let delay = max(0, (1.0 / 15.0) - elapsed)
        if delay == 0 {
            flushPendingPointer()
            return
        }
        pointerFlushTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(delay))
            guard !Task.isCancelled, let self else { return }
            self.pointerFlushTask = nil
            self.flushPendingPointer()
        }
    }

    private func flushPendingPointer() {
        guard isControlEnabled, state == .viewing, !pendingPointerPath.isEmpty else {
            clearPendingPointer()
            return
        }
        let events = pendingPointerPath.map { point in
            RemoteInputEvent.mouseMove(x: point.x, y: point.y)
        }
        if sendInputEvents(events, channel: .unreliable) {
            pendingPointerPath.removeAll(keepingCapacity: true)
            lastPointerSentAt = ProcessInfo.processInfo.systemUptime
        } else {
            inputDiagnostics.recordRetry()
            pointerFlushTask = Task { [weak self] in
                try? await Task.sleep(for: .milliseconds(50))
                guard !Task.isCancelled, let self else { return }
                self.pointerFlushTask = nil
                self.flushPendingPointer()
            }
        }
    }

    private func clearPendingPointer() {
        pointerFlushTask?.cancel()
        pointerFlushTask = nil
        pendingPointerPath.removeAll(keepingCapacity: true)
    }

    private func scheduleWheelFlush() {
        guard wheelFlushTask == nil else { return }
        wheelFlushTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(40))
            guard !Task.isCancelled, let self else { return }
            self.wheelFlushTask = nil
            self.flushPendingWheel()
        }
    }

    private func flushPendingWheel() {
        guard isControlEnabled, state == .viewing, pendingWheelDelta != 0 else {
            clearPendingWheel()
            return
        }
        let delta = pendingWheelDelta
        let point = pendingWheelPoint
        if sendInputEvents(
            [.mouseWheel(delta: delta, x: point.x, y: point.y)],
            channel: .reliable
        ) {
            pendingWheelDelta = 0
        }
        if pendingWheelDelta != 0 {
            inputDiagnostics.recordRetry()
            scheduleWheelFlush()
        }
    }

    private func clearPendingWheel() {
        wheelFlushTask?.cancel()
        wheelFlushTask = nil
        pendingWheelDelta = 0
    }

    private func scheduleTextFlush() {
        guard textFlushTask == nil, !pendingTextInput.isEmpty else { return }
        textFlushTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(20))
            guard !Task.isCancelled, let self else { return }
            self.textFlushTask = nil
            self.flushPendingTextInput()
        }
    }

    private func flushPendingTextInput() {
        textFlushTask?.cancel()
        textFlushTask = nil
        guard !pendingTextInput.isEmpty else { return }
        guard isControlEnabled, state == .viewing else {
            pendingTextInput.removeAll(keepingCapacity: true)
            return
        }

        let text = pendingTextInput
        pendingTextInput.removeAll(keepingCapacity: true)
        for chunk in Self.textChunks(text, maximumUTF8Bytes: 700) {
            if !sendTextChunk(chunk) {
                break
            }
        }
    }

    private func clearPendingTextInput() {
        textFlushTask?.cancel()
        textFlushTask = nil
        pendingTextInput.removeAll(keepingCapacity: true)
    }

    private func sendTextChunk(_ text: String) -> Bool {
        guard !text.isEmpty else { return true }
        let events: [RemoteInputEvent] = [.text(text)]
        if inputPacketFits(events, channel: .reliable) {
            return sendInputEvents(events, channel: .reliable)
        }

        guard text.count > 1 else {
            inputDiagnostics.recordOversizedPacket()
            return false
        }
        let midpoint = text.index(text.startIndex, offsetBy: text.count / 2)
        let firstHalf = String(text[..<midpoint])
        let secondHalf = String(text[midpoint...])
        return sendTextChunk(firstHalf) && sendTextChunk(secondHalf)
    }

    private func inputPacketFits(_ events: [RemoteInputEvent], channel: InputChannel) -> Bool {
        let currentSequence = channel == .reliable
            ? reliableInputSequence
            : unreliableInputSequence
        return RemoteInputPacket(
            sessionID: sessionID,
            sequence: currentSequence &+ 1,
            events: events
        ).fitsInOnePacket()
    }

    @discardableResult
    private func sendInputEvents(_ events: [RemoteInputEvent], channel: InputChannel) -> Bool {
        guard !events.isEmpty else {
            log(
                level: .warning,
                category: "remote-input",
                event: "packet-blocked",
                fields: ["reason": "events-empty"]
            )
            return false
        }
        guard !sessionID.isEmpty, state == .viewing else {
            inputDiagnostics.recordBlockedByState()
            return false
        }

        #if canImport(TXLiteAVSDK_TRTC)
        guard hasEnteredRoom else {
            inputDiagnostics.recordBlockedNotInRoom()
            return false
        }
        let currentSequence = channel == .reliable
            ? reliableInputSequence
            : unreliableInputSequence
        let nextSequence = currentSequence &+ 1
        let packet = RemoteInputPacket(
            sessionID: sessionID,
            sequence: nextSequence,
            events: events
        )
        let data: Data
        do {
            data = try packet.encodedData()
        } catch {
            inputDiagnostics.recordEncodingFailure()
            return false
        }
        guard data.count <= RemoteInputPacket.maximumPacketBytes else {
            inputDiagnostics.recordOversizedPacket()
            return false
        }

        let reliable = channel == .reliable
        let commandID = reliable
            ? RemoteInputPacket.reliableCommandID
            : RemoteInputPacket.unreliableCommandID
        guard cloud.sendCustomCmdMsg(
            commandID,
            data: data,
            reliable: reliable,
            ordered: reliable
        ) else {
            inputDiagnostics.recordSDKRejection()
            return false
        }
        if reliable {
            reliableInputSequence = nextSequence
        } else {
            unreliableInputSequence = nextSequence
        }
        inputDiagnostics.recordSent(
            reliable: reliable,
            eventCount: events.count,
            byteCount: data.count
        )
        return true
        #else
        inputDiagnostics.recordBlockedNotInRoom()
        return false
        #endif
    }

    private func disableControl(sendRelease: Bool, cause: String) {
        let cancelledPendingMoves = pendingPointerPath.count
        let cancelledPendingWheel = pendingWheelDelta == 0 ? 0 : 1
        var cancelledPendingTextBytes = 0
        clearPendingPointer()
        clearPendingWheel()
        if isControlEnabled, state == .viewing {
            flushPendingTextInput()
        } else {
            cancelledPendingTextBytes = pendingTextInput.lengthOfBytes(using: .utf8)
            clearPendingTextInput()
        }
        var releaseSent = false
        if sendRelease, isControlEnabled, state == .viewing {
            releaseSent = sendInputEvents([.releaseAll], channel: .reliable)
        }
        let wasEnabled = isControlEnabled
        isControlEnabled = false
        isLeftButtonHeld = false
        if wasEnabled {
            flushInputDiagnostics(forceHeartbeat: false)
            stopInputDiagnostics()
            log(
                level: .info,
                category: "remote-input",
                event: "control-stop",
                fields: [
                    "cause": cause,
                    "release_requested": diagnosticBool(sendRelease),
                    "release_sent": diagnosticBool(releaseSent),
                    "cancelled_pending_move": String(cancelledPendingMoves),
                    "cancelled_pending_wheel": String(cancelledPendingWheel),
                    "cancelled_pending_text_bytes": String(cancelledPendingTextBytes),
                ]
            )
        }
    }

    private static func textChunks(_ text: String, maximumUTF8Bytes: Int) -> [String] {
        guard !text.isEmpty else { return [] }
        var chunks: [String] = []
        var current = ""
        var currentBytes = 0
        for character in text {
            let value = String(character)
            let byteCount = value.lengthOfBytes(using: .utf8)
            if !current.isEmpty, currentBytes + byteCount > maximumUTF8Bytes {
                chunks.append(current)
                current = ""
                currentBytes = 0
            }
            current.append(character)
            currentBytes += byteCount
        }
        if !current.isEmpty {
            chunks.append(current)
        }
        return chunks
    }

    #if canImport(TXLiteAVSDK_TRTC)
    private func startRemoteScreenView() {
        guard hasEnteredRoom, !peerUserID.isEmpty else {
            log(
                level: .debug,
                category: "trtc",
                event: "remote-view-blocked",
                fields: ["reason": hasEnteredRoom ? "peer-empty" : "not-in-room"]
            )
            return
        }
        let renderParams = TRTCRenderParams()
        renderParams.fillMode = .fit
        cloud.setRemoteRenderParams(peerUserID, streamType: .sub, params: renderParams)
        if isRemoteViewStarted {
            cloud.updateRemoteView(renderView, streamType: .sub, forUser: peerUserID)
            log(
                level: .info,
                category: "trtc",
                event: "remote-view-updated",
                fields: ["render_bound": diagnosticBool(renderView != nil)]
            )
        } else {
            cloud.startRemoteView(peerUserID, streamType: .sub, view: renderView)
            isRemoteViewStarted = true
            log(
                level: .info,
                category: "trtc",
                event: "remote-view-started",
                fields: ["render_bound": diagnosticBool(renderView != nil)]
            )
        }
    }
    #endif

    private func scheduleInvitationTimeout(sessionID: String) {
        invitationTimeoutTask?.cancel()
        invitationTimeoutTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(30))
            guard !Task.isCancelled, let self,
                  self.sessionID == sessionID,
                  self.state == .inviting || self.state == .connecting
            else {
                return
            }
            let peer = self.peerUserID
            self.log(
                level: .error,
                category: "remote-desktop",
                event: "invitation-timeout",
                fields: ["elapsed_ms": self.elapsedMilliseconds(since: self.requestStartedUptime)]
            )
            self.cleanupTRTC()
            self.transition(to: .failed("对方未在规定时间内响应"), cause: "invitation-timeout")
            do {
                try await self.send(
                    RemoteDesktopSignal(kind: .stop, sessionID: sessionID),
                    to: peer
                )
            } catch {
                // send(_:to:) records a safe failure entry.
            }
        }
    }

    private func send(_ signal: RemoteDesktopSignal, to userID: String) async throws {
        let startedAt = ProcessInfo.processInfo.systemUptime
        log(
            level: .info,
            category: "remote-desktop",
            event: "signal-send-start",
            fields: ["kind": signal.kind.rawValue],
            traceSource: signal.sessionID,
            peerSource: userID
        )
        guard let client else {
            log(
                level: .error,
                category: "remote-desktop",
                event: "signal-send-failed",
                fields: [
                    "kind": signal.kind.rawValue,
                    "reason": "client-unavailable",
                ],
                traceSource: signal.sessionID,
                peerSource: userID
            )
            throw RemoteIMClientError.sdkInitializationFailed
        }
        do {
            _ = try await client.sendText(
                to: userID,
                text: signal.encodedText(),
                origin: .machine
            )
            log(
                level: .info,
                category: "remote-desktop",
                event: "signal-send-finished",
                fields: [
                    "kind": signal.kind.rawValue,
                    "result": "ok",
                    "duration_ms": elapsedMilliseconds(since: startedAt),
                ],
                traceSource: signal.sessionID,
                peerSource: userID
            )
        } catch {
            let errorValue = error as NSError
            log(
                level: .error,
                category: "remote-desktop",
                event: "signal-send-failed",
                fields: [
                    "kind": signal.kind.rawValue,
                    "error_domain": errorValue.domain,
                    "error_code": String(errorValue.code),
                    "duration_ms": elapsedMilliseconds(since: startedAt),
                ],
                traceSource: signal.sessionID,
                peerSource: userID
            )
            throw error
        }
    }

    private func fail(_ reason: String, cause: String = "session-failure", code: Int? = nil) {
        let peer = peerUserID
        let currentSessionID = sessionID
        invitationTimeoutTask?.cancel()
        invitationTimeoutTask = nil
        cleanupTRTC()
        #if canImport(TXLiteAVSDK_TRTC)
        pendingStartAfterExit = false
        #endif
        transition(to: .failed(reason), cause: cause, code: code)
        if !peer.isEmpty, !currentSessionID.isEmpty {
            Task { [weak self] in
                do {
                    try await self?.send(
                        RemoteDesktopSignal(kind: .stop, sessionID: currentSessionID),
                        to: peer
                    )
                } catch {
                    // send(_:to:) records a safe failure entry.
                }
            }
        }
    }

    private func resetSession(cause: String) {
        invitationTimeoutTask?.cancel()
        invitationTimeoutTask = nil
        cleanupTRTC()
        #if canImport(TXLiteAVSDK_TRTC)
        pendingStartAfterExit = false
        #endif
        transition(to: .idle, cause: cause)
        peerUserID = ""
        sessionID = ""
        roomID = ""
        credentials = nil
        noticeText = nil
        remoteVideoSize = .zero
        captureGeometry = nil
        requestStartedUptime = nil
        enterRoomStartedUptime = nil
    }

    private func cleanupTRTC() {
        stopViewerHeartbeat()
        var isTRTCExitOutstanding = false
        let hadActiveResources = isControlEnabled
            || hasEnteredRoomForDiagnostics
            || isEnterRoomInFlightForDiagnostics
            || isRemoteViewStartedForDiagnostics
        if hadActiveResources {
            log(
                level: .info,
                category: "trtc",
                event: "cleanup-start",
                fields: [
                    "entered_room": diagnosticBool(hasEnteredRoomForDiagnostics),
                    "remote_view_started": diagnosticBool(isRemoteViewStartedForDiagnostics),
                ]
            )
        }
        disableControl(sendRelease: true, cause: "session-cleanup")
        #if canImport(TXLiteAVSDK_TRTC)
        cancelSubstreamRecoveryWatchdog()
        if isRemoteViewStarted, !peerUserID.isEmpty {
            cloud.stopRemoteView(peerUserID, streamType: .sub)
        }
        let shouldExitRoom = !isTRTCExitPending
            && (hasEnteredRoom || isEnterRoomInFlight)
        if shouldExitRoom {
            nextTRTCExitGeneration &+= 1
            let context = TRTCExitContext(
                generation: nextTRTCExitGeneration,
                sessionID: sessionID,
                peerUserID: peerUserID,
                roomID: roomID,
                requestedAt: ProcessInfo.processInfo.systemUptime,
                timedOut: false
            )
            trtcExitContext = context
            log(
                level: .info,
                category: "trtc",
                event: "exit-room-requested",
                fields: ["exit_generation": String(context.generation)],
                traceSource: context.sessionID,
                peerSource: context.peerUserID,
                roomSource: context.roomID
            )
            scheduleTRTCExitWatchdog(generation: context.generation)
            cloud.exitRoom()
        } else if !isTRTCExitPending {
            unregisterTRTCDelegateIfNeeded()
        }
        isTRTCExitOutstanding = shouldExitRoom || isTRTCExitPending
        isRemoteViewStarted = false
        hasEnteredRoom = false
        isEnterRoomInFlight = false
        connectionLostUptime = nil
        reconnectAttemptCount = 0
        warningLastLogUptimeByCode.removeAll(keepingCapacity: true)
        #endif
        unreliableInputSequence = 0
        reliableInputSequence = 0
        lastPointer = CGPoint(x: 0.5, y: 0.5)
        lastPointerSentAt = 0
        stopInputDiagnostics()
        inputDiagnostics = RemoteInputDiagnosticAccumulator()
        lastPointerCaptureDiagnostic = nil
        quietInputWindows = 0
        if hadActiveResources {
            log(
                level: .info,
                category: "trtc",
                event: isTRTCExitOutstanding
                    ? "local-cleanup-issued"
                    : "cleanup-finished"
            )
        }
    }

    private func transition(
        to nextState: RemoteDesktopViewerState,
        cause: String,
        code: Int? = nil
    ) {
        var fields = [
            "from": state.diagnosticName,
            "to": nextState.diagnosticName,
            "cause": cause,
        ]
        if let code {
            fields["code"] = String(code)
        }
        log(
            level: nextState.diagnosticName == "failed" ? .error : .info,
            category: "remote-desktop",
            event: "state-transition",
            fields: fields
        )
        state = nextState
    }

    private func applyCaptureGeometry(from signal: RemoteDesktopSignal) {
        var fields: [String: String]
        switch signal.captureGeometryDisposition {
        case .absent:
            captureGeometry = nil
            fields = [
                "result": "absent",
                "reason": "missing",
                "fallback": "encoded-frame",
            ]
        case let .accepted(geometry):
            captureGeometry = geometry
            fields = captureGeometryDiagnosticFields(geometry)
            fields["result"] = "accepted"
            fields["fallback"] = "active-content"
        case let .ignored(candidate, reason):
            captureGeometry = nil
            fields = candidate.map(captureGeometryDiagnosticFields) ?? [:]
            fields["result"] = "ignored"
            fields["reason"] = reason
            fields["fallback"] = "encoded-frame"
        }
        log(
            level: fields["result"] == "ignored" ? .warning : .info,
            category: "remote-desktop",
            event: "capture-geometry",
            fields: fields
        )
    }

    private func logIgnoredSignal(_ signal: RemoteDesktopSignal, from userID: String) {
        let reason: String
        if userID != peerUserID {
            reason = "peer-mismatch"
        } else if signal.sessionID != sessionID {
            reason = "session-mismatch"
        } else {
            reason = "state-mismatch"
        }
        log(
            level: .warning,
            category: "remote-desktop",
            event: "signal-ignored",
            fields: [
                "kind": signal.kind.rawValue,
                "reason": reason,
                "from": Self.peerTag(for: userID),
                "incoming_trace": Self.traceTag(for: signal.sessionID),
            ]
        )
    }

    private func startInputDiagnostics() {
        stopInputDiagnostics()
        inputDiagnostics = RemoteInputDiagnosticAccumulator()
        quietInputWindows = 0
        inputDiagnosticTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(1))
                guard !Task.isCancelled, let self else { return }
                self.flushInputDiagnostics(forceHeartbeat: false)
            }
        }
    }

    private func startViewerHeartbeat() {
        stopViewerHeartbeat()
        let heartbeatSessionID = sessionID
        guard !heartbeatSessionID.isEmpty else { return }
        viewerHeartbeatTask = Task { [weak self] in
            while !Task.isCancelled {
                do {
                    try await Task.sleep(for: .seconds(5))
                } catch {
                    return
                }
                guard let self,
                      self.sessionID == heartbeatSessionID,
                      self.state == .connecting || self.state == .viewing
                else {
                    return
                }
                self.log(
                    level: .debug,
                    category: "remote-desktop",
                    event: "viewer-heartbeat",
                    fields: [
                        "entered_room": self.diagnosticBool(self.hasEnteredRoomForDiagnostics),
                        "enter_in_flight": self.diagnosticBool(self.isEnterRoomInFlightForDiagnostics),
                        "remote_view_started": self.diagnosticBool(self.isRemoteViewStartedForDiagnostics),
                        "render_bound": self.diagnosticBool(self.renderView != nil),
                        "video_size": "\(Int(self.remoteVideoSize.width))x\(Int(self.remoteVideoSize.height))",
                        "control_enabled": self.diagnosticBool(self.isControlEnabled),
                        "exit_pending": self.diagnosticBool(self.isTRTCExitPendingForDiagnostics),
                    ]
                )
            }
        }
    }

    private func stopViewerHeartbeat() {
        viewerHeartbeatTask?.cancel()
        viewerHeartbeatTask = nil
    }

    private func stopInputDiagnostics() {
        inputDiagnosticTask?.cancel()
        inputDiagnosticTask = nil
    }

    private func flushInputDiagnostics(forceHeartbeat: Bool) {
        let snapshot = inputDiagnostics.takeSnapshot()
        if snapshot.hasActivity {
            quietInputWindows = 0
            var fields: [String: String] = ["window_ms": "1000"]
            fields["pointer_seen"] = String(snapshot.pointerEventsSeen)
            fields["dropped_invalid_geometry"] = String(snapshot.droppedInvalidGeometry)
            fields["dropped_letterbox"] = String(snapshot.droppedLetterbox)
            fields["coalesced_move"] = String(snapshot.coalescedMoves)
            fields["captured_move"] = String(snapshot.capturedMoves)
            fields["captured_click"] = String(snapshot.capturedClicks)
            fields["captured_wheel"] = String(snapshot.capturedWheels)
            fields["captured_key"] = String(snapshot.capturedKeys)
            fields["captured_text_chars"] = String(snapshot.capturedTextCharacters)
            fields["captured_text_bytes"] = String(snapshot.capturedTextUTF8Bytes)
            fields["sent_reliable"] = String(snapshot.sentReliablePackets)
            fields["sent_unreliable"] = String(snapshot.sentUnreliablePackets)
            fields["sent_events"] = String(snapshot.sentEvents)
            fields["sent_bytes"] = String(snapshot.sentBytes)
            fields["sdk_rejected"] = String(snapshot.rejectedBySDK)
            fields["blocked_state"] = String(snapshot.blockedByState)
            fields["blocked_not_in_room"] = String(snapshot.blockedNotInRoom)
            fields["encode_failed"] = String(snapshot.encodingFailures)
            fields["oversized"] = String(snapshot.oversizedPackets)
            fields["retry"] = String(snapshot.retries)
            fields["pending_move"] = String(pendingPointerPath.count)
            fields["pending_wheel"] = String(pendingWheelDelta)
            fields["encoded_frame"] = sizeDiagnosticText(remoteVideoSize)
            fields["coordinate_space"] = captureGeometry == nil
                ? "encoded-frame-normalized"
                : "source-normalized"
            if let activeContentRect = RemoteDesktopCoordinateMapper.activeContentRect(
                encodedFrameRect: CGRect(origin: .zero, size: remoteVideoSize),
                captureGeometry: captureGeometry
            ) {
                fields["active_content_rect"] = rectDiagnosticText(activeContentRect)
            }
            if let captureGeometry {
                fields.merge(
                    captureGeometryDiagnosticFields(captureGeometry),
                    uniquingKeysWith: { _, new in new }
                )
                fields["geometry_fallback"] = "active-content"
            } else {
                fields["geometry_fallback"] = "encoded-frame"
            }
            let capturedPosition = snapshot.capturedMoves
                + snapshot.capturedClicks
                + snapshot.capturedWheels
            if capturedPosition > 0,
               let x = snapshot.lastKnownX,
               let y = snapshot.lastKnownY
            {
                fields["last_point"] = String(format: "%.4f,%.4f", x, y)
            }
            if snapshot.pointerEventsSeen > 0,
               let capture = lastPointerCaptureDiagnostic
            {
                fields["capture_raw"] = String(
                    format: "%.1f,%.1f",
                    capture.location.x,
                    capture.location.y
                )
                fields["capture_viewport"] = String(
                    format: "%.1fx%.1f",
                    capture.viewportSize.width,
                    capture.viewportSize.height
                )
                fields["capture_video"] = String(
                    format: "%.0fx%.0f",
                    capture.videoSize.width,
                    capture.videoSize.height
                )
                if let activeContentRect = capture.activeContentRect {
                    fields["capture_active_content_rect"] = rectDiagnosticText(activeContentRect)
                }
                if let point = capture.normalizedPoint {
                    fields["capture_normalized"] = String(
                        format: "%.4f,%.4f",
                        point.x,
                        point.y
                    )
                    fields["capture_result"] = "mapped"
                } else {
                    fields["capture_result"] = capture.dropReason?.rawValue ?? "dropped"
                }
            }
            let hasFailure = snapshot.rejectedBySDK > 0
                || snapshot.blockedByState > 0
                || snapshot.blockedNotInRoom > 0
                || snapshot.encodingFailures > 0
                || snapshot.oversizedPackets > 0
                || snapshot.droppedInvalidGeometry > 0
                || snapshot.droppedLetterbox > 0
            log(
                level: hasFailure ? .warning : .info,
                category: "remote-input",
                event: "summary",
                fields: fields
            )
            return
        }

        quietInputWindows += 1
        guard forceHeartbeat || quietInputWindows >= 5 else { return }
        quietInputWindows = 0
        log(
            level: .debug,
            category: "remote-input",
            event: "heartbeat",
            fields: [
                "control_enabled": diagnosticBool(isControlEnabled),
                "entered_room": diagnosticBool(hasEnteredRoomForDiagnostics),
                "pending_move": String(pendingPointerPath.count),
                "pending_wheel": String(pendingWheelDelta),
            ]
        )
    }

    private func log(
        level: DiagnosticLogLevel,
        category: String,
        event: String,
        fields: [String: String] = [:],
        traceSource: String? = nil,
        peerSource: String? = nil,
        roomSource: String? = nil
    ) {
        var context = fields
        context["state"] = state.diagnosticName
        context["trace"] = Self.traceTag(for: traceSource ?? sessionID)
        let peer = peerSource ?? peerUserID
        if !peer.isEmpty {
            context["peer"] = Self.peerTag(for: peer)
        }
        let room = roomSource ?? roomID
        if !room.isEmpty {
            context["room"] = Self.roomTag(for: room)
        }
        diagnosticLog.record(
            level: level,
            category: category,
            event: event,
            fields: context
        )
    }

    private func elapsedMilliseconds(since startedAt: TimeInterval?) -> String {
        guard let startedAt else { return "unknown" }
        let elapsed = max(ProcessInfo.processInfo.systemUptime - startedAt, 0)
        return String(Int((elapsed * 1_000).rounded()))
    }

    private func diagnosticBool(_ value: Bool) -> String {
        value ? "true" : "false"
    }

    private func captureGeometryDiagnosticFields(
        _ geometry: CaptureGeometry
    ) -> [String: String] {
        [
            "source_size": "\(geometry.sourceWidth)x\(geometry.sourceHeight)",
            "capture_rect": "\(geometry.captureX),\(geometry.captureY) \(geometry.captureWidth)x\(geometry.captureHeight)",
            "content_mode": geometry.contentMode,
            "geometry_revision": String(geometry.revision),
        ]
    }

    private func sizeDiagnosticText(_ size: CGSize) -> String {
        String(format: "%.0fx%.0f", size.width, size.height)
    }

    private func rectDiagnosticText(_ rect: CGRect) -> String {
        String(
            format: "%.1f,%.1f %.1fx%.1f",
            rect.origin.x,
            rect.origin.y,
            rect.width,
            rect.height
        )
    }

    private var hasEnteredRoomForDiagnostics: Bool {
        #if canImport(TXLiteAVSDK_TRTC)
        hasEnteredRoom
        #else
        false
        #endif
    }

    private var isRemoteViewStartedForDiagnostics: Bool {
        #if canImport(TXLiteAVSDK_TRTC)
        isRemoteViewStarted
        #else
        false
        #endif
    }

    private var isEnterRoomInFlightForDiagnostics: Bool {
        #if canImport(TXLiteAVSDK_TRTC)
        isEnterRoomInFlight
        #else
        false
        #endif
    }

    private var isTRTCExitPendingForDiagnostics: Bool {
        #if canImport(TXLiteAVSDK_TRTC)
        isTRTCExitPending
        #else
        false
        #endif
    }

    #if canImport(TXLiteAVSDK_TRTC)
    private func logIgnoredTRTCCallbackDuringExit(
        _ callback: String,
        level: DiagnosticLogLevel = .debug,
        additionalFields: [String: String] = [:]
    ) {
        guard let context = trtcExitContext else { return }
        var fields = additionalFields
        fields["callback"] = callback
        fields["exit_generation"] = String(context.generation)
        fields["elapsed_ms"] = elapsedMilliseconds(since: context.requestedAt)
        fields["current_trace"] = diagnosticTraceID
        log(
            level: level,
            category: "trtc",
            event: "callback-ignored-during-exit",
            fields: fields,
            traceSource: context.sessionID,
            peerSource: context.peerUserID,
            roomSource: context.roomID
        )
    }

    private func scheduleTRTCExitWatchdog(generation: UInt64) {
        trtcExitWatchdogTask?.cancel()
        trtcExitWatchdogTask = Task { [weak self] in
            do {
                try await Task.sleep(for: .seconds(5))
            } catch {
                return
            }
            guard let self,
                  var context = self.trtcExitContext,
                  context.generation == generation,
                  !context.timedOut
            else {
                return
            }
            context.timedOut = true
            self.trtcExitContext = context
            self.pendingStartAfterExit = false
            self.log(
                level: .error,
                category: "trtc",
                event: "exit-room-timeout",
                fields: [
                    "exit_generation": String(context.generation),
                    "elapsed_ms": self.elapsedMilliseconds(since: context.requestedAt),
                    "current_trace": self.diagnosticTraceID,
                ],
                traceSource: context.sessionID,
                peerSource: context.peerUserID,
                roomSource: context.roomID
            )
            if self.state.isActive {
                self.fail(
                    "远程组件释放超时，请稍后重试或重新打开 App",
                    cause: "previous-room-exit-timeout"
                )
            } else if self.state == .idle {
                self.transition(
                    to: .failed("远程组件释放超时，请稍后重试或重新打开 App"),
                    cause: "previous-room-exit-timeout"
                )
            }
        }
    }

    private func scheduleSubstreamRecoveryWatchdog() {
        cancelSubstreamRecoveryWatchdog()
        guard !sessionID.isEmpty else { return }
        nextSubstreamRecoveryGeneration &+= 1
        let context = SubstreamRecoveryContext(
            generation: nextSubstreamRecoveryGeneration,
            sessionID: sessionID,
            peerUserID: peerUserID,
            roomID: roomID,
            startedAt: ProcessInfo.processInfo.systemUptime
        )
        substreamRecoveryContext = context
        log(
            level: .warning,
            category: "trtc",
            event: "substream-recovery-started",
            fields: [
                "generation": String(context.generation),
                "timeout_seconds": "15",
            ]
        )
        substreamRecoveryTask = Task { [weak self] in
            do {
                try await Task.sleep(for: .seconds(15))
            } catch {
                return
            }
            guard let self,
                  let currentContext = self.substreamRecoveryContext,
                  currentContext.generation == context.generation,
                  self.sessionID == context.sessionID,
                  self.peerUserID == context.peerUserID,
                  self.roomID == context.roomID,
                  self.state == .connecting,
                  self.hasEnteredRoom,
                  !self.isTRTCExitPending
            else {
                return
            }
            self.log(
                level: .error,
                category: "trtc",
                event: "substream-recovery-timeout",
                fields: [
                    "generation": String(context.generation),
                    "elapsed_ms": self.elapsedMilliseconds(since: context.startedAt),
                ],
                traceSource: context.sessionID,
                peerSource: context.peerUserID,
                roomSource: context.roomID
            )
            self.fail(
                "远程画面恢复超时",
                cause: "substream-recovery-timeout"
            )
        }
    }

    private func cancelSubstreamRecoveryWatchdog() {
        substreamRecoveryTask?.cancel()
        substreamRecoveryTask = nil
        substreamRecoveryContext = nil
    }

    private func shouldLogTRTCWarning(code: Int, now: TimeInterval) -> Bool {
        if let lastLoggedAt = warningLastLogUptimeByCode[code],
           now - lastLoggedAt < 5
        {
            return false
        }
        warningLastLogUptimeByCode[code] = now
        return true
    }

    private func registerTRTCDelegateIfNeeded() {
        guard !hasRegisteredTRTCDelegate else { return }
        cloud.addDelegate(self)
        hasRegisteredTRTCDelegate = true
    }

    private func unregisterTRTCDelegateIfNeeded() {
        guard hasRegisteredTRTCDelegate else { return }
        cloud.removeDelegate(self)
        hasRegisteredTRTCDelegate = false
    }
    #endif

    private static func traceTag(for sessionID: String) -> String {
        guard !sessionID.isEmpty else { return "none" }
        return DiagnosticLogPrivacy.stableTag(sessionID, prefix: "s")
    }

    private static func peerTag(for userID: String) -> String {
        guard !userID.isEmpty else { return "none" }
        return DiagnosticLogPrivacy.stableTag(userID, prefix: "u")
    }

    private static func roomTag(for roomID: String) -> String {
        guard !roomID.isEmpty else { return "none" }
        return DiagnosticLogPrivacy.stableTag(roomID, prefix: "r")
    }

    private static func newIdentifier() -> String {
        UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
    }
}

#if canImport(TXLiteAVSDK_TRTC)
extension RemoteDesktopSession: TRTCCloudDelegate {
    nonisolated func onEnterRoom(_ result: Int) {
        Task { @MainActor [weak self] in
            guard let self else { return }
            if self.isTRTCExitPending {
                self.logIgnoredTRTCCallbackDuringExit("enter-room", level: .warning)
                return
            }
            self.isEnterRoomInFlight = false
            self.log(
                level: result > 0 ? .info : .error,
                category: "trtc",
                event: "enter-room-callback",
                fields: [
                    "result": String(result),
                    "expected_state": self.diagnosticBool(self.state == .connecting),
                    "wait_ms": self.elapsedMilliseconds(since: self.enterRoomStartedUptime),
                ]
            )
            guard self.state == .connecting else { return }
            guard result > 0 else {
                self.fail(
                    "进入远程房间失败（\(result)）",
                    cause: "enter-room-failed",
                    code: result
                )
                return
            }
            self.hasEnteredRoom = true
            self.cloud.muteAllRemoteAudio(true)
            self.startRemoteScreenView()
        }
    }

    nonisolated func onExitRoom(_ reason: Int) {
        Task { @MainActor [weak self] in
            guard let self else { return }
            let exitContext = self.trtcExitContext
            let wasExpected = exitContext != nil
            var fields = [
                "reason_code": String(reason),
                "expected": self.diagnosticBool(wasExpected),
            ]
            if let exitContext {
                fields["exit_generation"] = String(exitContext.generation)
                fields["elapsed_ms"] = self.elapsedMilliseconds(since: exitContext.requestedAt)
                fields["timed_out"] = self.diagnosticBool(exitContext.timedOut)
                fields["current_trace"] = self.diagnosticTraceID
            }
            self.log(
                level: reason == 0 ? .info : .warning,
                category: "trtc",
                event: "exit-room-callback",
                fields: fields,
                traceSource: exitContext?.sessionID,
                peerSource: exitContext?.peerUserID,
                roomSource: exitContext?.roomID
            )
            self.hasEnteredRoom = false
            self.isEnterRoomInFlight = false
            if let exitContext {
                self.trtcExitWatchdogTask?.cancel()
                self.trtcExitWatchdogTask = nil
                self.trtcExitContext = nil
                self.unregisterTRTCDelegateIfNeeded()
                let shouldStartDeferredRoom = !exitContext.timedOut
                    && self.pendingStartAfterExit
                    && self.state == .connecting
                self.pendingStartAfterExit = false
                self.log(
                    level: exitContext.timedOut ? .warning : .info,
                    category: "trtc",
                    event: "cleanup-finished",
                    fields: [
                        "exit_generation": String(exitContext.generation),
                        "timed_out": self.diagnosticBool(exitContext.timedOut),
                        "current_trace": self.diagnosticTraceID,
                    ],
                    traceSource: exitContext.sessionID,
                    peerSource: exitContext.peerUserID,
                    roomSource: exitContext.roomID
                )
                if shouldStartDeferredRoom {
                    self.log(
                        level: .info,
                        category: "trtc",
                        event: "deferred-enter-resumed"
                    )
                    self.startViewing()
                }
                return
            }
            self.unregisterTRTCDelegateIfNeeded()
            if self.state.isActive {
                self.fail(
                    reason == 0 ? "远程连接已结束" : "远程房间异常退出（\(reason)）",
                    cause: reason == 0 ? "room-ended" : "room-exit-error",
                    code: reason
                )
            }
        }
    }

    nonisolated func onUserSubStreamAvailable(_ userId: String, available: Bool) {
        Task { @MainActor [weak self] in
            guard let self else { return }
            if self.isTRTCExitPending {
                self.logIgnoredTRTCCallbackDuringExit("substream-availability")
                return
            }
            let matchesPeer = userId == self.peerUserID
            self.log(
                level: .info,
                category: "trtc",
                event: "substream-availability",
                fields: [
                    "available": self.diagnosticBool(available),
                    "peer_match": self.diagnosticBool(matchesPeer),
                ]
            )
            guard matchesPeer else { return }
            if available {
                self.startRemoteScreenView()
            } else if self.state == .viewing {
                self.disableControl(sendRelease: true, cause: "substream-unavailable")
                self.transition(to: .connecting, cause: "substream-unavailable")
                self.scheduleSubstreamRecoveryWatchdog()
            }
        }
    }

    nonisolated func onFirstVideoFrame(
        _ userId: String,
        streamType: TRTCVideoStreamType,
        width: Int32,
        height: Int32
    ) {
        Task { @MainActor [weak self] in
            guard let self else { return }
            if self.isTRTCExitPending {
                self.logIgnoredTRTCCallbackDuringExit("first-video-frame")
                return
            }
            let matchesPeer = userId == self.peerUserID
            let matchesStream = streamType == .sub
            self.log(
                level: .info,
                category: "trtc",
                event: "first-video-frame",
                fields: [
                    "peer_match": self.diagnosticBool(matchesPeer),
                    "stream_match": self.diagnosticBool(matchesStream),
                    "width": String(width),
                    "height": String(height),
                    "request_wait_ms": self.elapsedMilliseconds(since: self.requestStartedUptime),
                ]
            )
            guard matchesPeer, matchesStream, self.state == .connecting else { return }
            guard width > 0, height > 0 else {
                self.log(
                    level: .warning,
                    category: "trtc",
                    event: "first-video-frame-ignored",
                    fields: ["reason": "invalid-size"]
                )
                return
            }
            self.remoteVideoSize = CGSize(width: Int(width), height: Int(height))
            self.invitationTimeoutTask?.cancel()
            self.invitationTimeoutTask = nil
            if let recoveryContext = self.substreamRecoveryContext {
                self.log(
                    level: .info,
                    category: "trtc",
                    event: "substream-recovered",
                    fields: [
                        "generation": String(recoveryContext.generation),
                        "elapsed_ms": self.elapsedMilliseconds(since: recoveryContext.startedAt),
                        "width": String(width),
                        "height": String(height),
                    ],
                    traceSource: recoveryContext.sessionID,
                    peerSource: recoveryContext.peerUserID,
                    roomSource: recoveryContext.roomID
                )
            }
            self.cancelSubstreamRecoveryWatchdog()
            self.transition(to: .viewing, cause: "first-video-frame")
        }
    }

    nonisolated func onUserVideoSizeChanged(
        _ userId: String,
        streamType: TRTCVideoStreamType,
        newWidth: Int32,
        newHeight: Int32
    ) {
        Task { @MainActor [weak self] in
            guard let self else { return }
            if self.isTRTCExitPending { return }
            guard userId == self.peerUserID,
                  streamType == .sub,
                  newWidth > 0,
                  newHeight > 0
            else {
                return
            }
            let oldSize = self.remoteVideoSize
            self.remoteVideoSize = CGSize(width: Int(newWidth), height: Int(newHeight))
            if abs(oldSize.width - CGFloat(newWidth)) > 0.5
                || abs(oldSize.height - CGFloat(newHeight)) > 0.5
            {
                self.log(
                    level: .info,
                    category: "trtc",
                    event: "video-size-changed",
                    fields: [
                        "old": "\(Int(oldSize.width))x\(Int(oldSize.height))",
                        "new": "\(newWidth)x\(newHeight)",
                    ]
                )
            }
        }
    }

    nonisolated func onError(
        _ errCode: TXLiteAVError,
        errMsg: String?,
        extInfo: [AnyHashable: Any]?
    ) {
        let code = Int(errCode.rawValue)
        let failureText = errMsg?.trimmingCharacters(in: .whitespacesAndNewlines)
        Task { @MainActor [weak self] in
            guard let self else { return }
            if self.isTRTCExitPending {
                self.logIgnoredTRTCCallbackDuringExit("error", level: .warning)
                return
            }
            guard self.state.isActive else { return }
            self.log(
                level: .error,
                category: "trtc",
                event: "error",
                fields: ["code": String(code)]
            )
            self.fail(
                failureText?.isEmpty == false ? failureText! : "TRTC 连接失败（\(code)）",
                cause: "trtc-error",
                code: code
            )
        }
    }

    nonisolated func onWarning(
        _ warningCode: TXLiteAVWarning,
        warningMsg: String?,
        extInfo: [AnyHashable: Any]?
    ) {
        let code = Int(warningCode.rawValue)
        Task { @MainActor [weak self] in
            guard let self else { return }
            guard self.isTRTCExitPending || self.state.isActive else { return }
            let now = ProcessInfo.processInfo.systemUptime
            guard self.shouldLogTRTCWarning(code: code, now: now) else { return }
            if self.isTRTCExitPending {
                self.logIgnoredTRTCCallbackDuringExit(
                    "warning",
                    additionalFields: ["code": String(code)]
                )
                return
            }
            self.log(
                level: .warning,
                category: "trtc",
                event: "warning",
                fields: ["code": String(code)]
            )
        }
    }

    nonisolated func onConnectionLost() {
        Task { @MainActor [weak self] in
            guard let self else { return }
            if self.isTRTCExitPending {
                self.logIgnoredTRTCCallbackDuringExit("connection-lost", level: .warning)
                return
            }
            guard self.state.isActive else { return }
            self.connectionLostUptime = ProcessInfo.processInfo.systemUptime
            self.reconnectAttemptCount = 0
            self.log(level: .warning, category: "trtc", event: "connection-lost")
        }
    }

    nonisolated func onTryToReconnect() {
        Task { @MainActor [weak self] in
            guard let self else { return }
            if self.isTRTCExitPending {
                self.logIgnoredTRTCCallbackDuringExit("reconnect-attempt")
                return
            }
            guard self.state.isActive else { return }
            self.reconnectAttemptCount += 1
            guard self.reconnectAttemptCount == 1 || self.reconnectAttemptCount.isMultiple(of: 5) else {
                return
            }
            self.log(
                level: .info,
                category: "trtc",
                event: "reconnect-attempt",
                fields: ["attempt": String(self.reconnectAttemptCount)]
            )
        }
    }

    nonisolated func onConnectionRecovery() {
        Task { @MainActor [weak self] in
            guard let self else { return }
            if self.isTRTCExitPending {
                self.logIgnoredTRTCCallbackDuringExit("connection-recovered")
                return
            }
            guard self.state.isActive else { return }
            self.log(
                level: .info,
                category: "trtc",
                event: "connection-recovered",
                fields: [
                    "outage_ms": self.elapsedMilliseconds(since: self.connectionLostUptime),
                    "attempts": String(self.reconnectAttemptCount),
                ]
            )
            self.connectionLostUptime = nil
            self.reconnectAttemptCount = 0
        }
    }
}
#endif
