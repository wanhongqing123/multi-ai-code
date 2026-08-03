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
}

@MainActor
final class RemoteDesktopSession: NSObject, ObservableObject {
    @Published private(set) var state: RemoteDesktopViewerState = .idle
    @Published private(set) var peerUserID = ""
    @Published private(set) var noticeText: String?

    private weak var client: (any RemoteIMClient)?
    private weak var renderView: UIView?
    private var credentials: Credentials?
    private var sessionID = ""
    private var roomID = ""
    private var invitationTimeoutTask: Task<Void, Never>?

    #if canImport(TXLiteAVSDK_TRTC)
    private let cloud = TRTCCloud.sharedInstance()
    private var hasEnteredRoom = false
    private var isRemoteViewStarted = false
    #endif

    private struct Credentials {
        let sdkAppID: Int
        let localUserID: String
        let userSig: String
    }

    init(client: any RemoteIMClient) {
        self.client = client
        super.init()
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
        guard canStart else { return }

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
        self.noticeText = nil
        self.state = .inviting
        scheduleInvitationTimeout(sessionID: identifier)

        let signal = RemoteDesktopSignal(
            kind: .invite,
            sessionID: identifier,
            roomID: roomID
        )
        do {
            try await send(signal, to: peerUserID)
        } catch {
            invitationTimeoutTask?.cancel()
            invitationTimeoutTask = nil
            state = .failed("远程请求发送失败：\(error.localizedDescription)")
        }
    }

    @discardableResult
    func handleIncomingText(from userID: String, text: String) -> Bool {
        guard RemoteDesktopSignal.isSignalText(text) else { return false }
        guard let signal = RemoteDesktopSignal.decodeText(text) else {
            return true
        }

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
                return true
            }
            if !signal.roomID.isEmpty {
                roomID = signal.roomID
            }
            state = .connecting
            startViewing()
        case .reject:
            guard userID == peerUserID,
                  signal.sessionID == sessionID,
                  state == .inviting
            else {
                return true
            }
            invitationTimeoutTask?.cancel()
            invitationTimeoutTask = nil
            cleanupTRTC()
            state = .failed(signal.reason.isEmpty ? "对方拒绝了请求" : signal.reason)
        case .stop:
            guard userID == peerUserID, signal.sessionID == sessionID else { return true }
            resetSession()
        case .notice:
            guard userID == peerUserID, signal.sessionID == sessionID else { return true }
            switch signal.noticeCode {
            case "secure-desktop-entered":
                noticeText = "对方进入了安全桌面，画面可能暂时不可用"
            case "secure-desktop-left":
                noticeText = nil
            default:
                break
            }
        }
        return true
    }

    func bindRenderView(_ view: UIView?) {
        renderView = view
        #if canImport(TXLiteAVSDK_TRTC)
        guard isRemoteViewStarted, !peerUserID.isEmpty else { return }
        cloud.updateRemoteView(view, streamType: .sub, forUser: peerUserID)
        #endif
    }

    func stop() async {
        let peer = peerUserID
        let currentSessionID = sessionID
        resetSession()
        guard !peer.isEmpty, !currentSessionID.isEmpty else { return }
        try? await send(
            RemoteDesktopSignal(kind: .stop, sessionID: currentSessionID),
            to: peer
        )
    }

    private func startViewing() {
        guard let credentials, !roomID.isEmpty else {
            fail("远程连接参数不完整")
            return
        }

        #if canImport(TXLiteAVSDK_TRTC)
        cloud.delegate = self
        cloud.setDefaultStreamRecvMode(false, video: false)
        cloud.muteAllRemoteAudio(true)

        let params = TRTCParams()
        params.sdkAppId = UInt32(credentials.sdkAppID)
        params.userId = credentials.localUserID
        params.userSig = credentials.userSig
        params.strRoomId = roomID
        cloud.enterRoom(params, appScene: .videoCall)
        #else
        fail("TRTC SDK 未集成，请执行 pod install 后使用 MaiChat.xcworkspace 编译")
        #endif
    }

    #if canImport(TXLiteAVSDK_TRTC)
    private func startRemoteScreenView() {
        guard hasEnteredRoom, !peerUserID.isEmpty else { return }
        let renderParams = TRTCRenderParams()
        renderParams.fillMode = .fit
        cloud.setRemoteRenderParams(peerUserID, streamType: .sub, params: renderParams)
        if isRemoteViewStarted {
            cloud.updateRemoteView(renderView, streamType: .sub, forUser: peerUserID)
        } else {
            cloud.startRemoteView(peerUserID, streamType: .sub, view: renderView)
            isRemoteViewStarted = true
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
            self.cleanupTRTC()
            self.state = .failed("对方未在规定时间内响应")
            try? await self.send(
                RemoteDesktopSignal(kind: .stop, sessionID: sessionID),
                to: peer
            )
        }
    }

    private func send(_ signal: RemoteDesktopSignal, to userID: String) async throws {
        guard let client else {
            throw RemoteIMClientError.sdkInitializationFailed
        }
        _ = try await client.sendText(to: userID, text: signal.encodedText())
    }

    private func fail(_ reason: String) {
        let peer = peerUserID
        let currentSessionID = sessionID
        invitationTimeoutTask?.cancel()
        invitationTimeoutTask = nil
        cleanupTRTC()
        state = .failed(reason)
        if !peer.isEmpty, !currentSessionID.isEmpty {
            Task { [weak self] in
                try? await self?.send(
                    RemoteDesktopSignal(kind: .stop, sessionID: currentSessionID),
                    to: peer
                )
            }
        }
    }

    private func resetSession() {
        invitationTimeoutTask?.cancel()
        invitationTimeoutTask = nil
        cleanupTRTC()
        state = .idle
        peerUserID = ""
        sessionID = ""
        roomID = ""
        credentials = nil
        noticeText = nil
    }

    private func cleanupTRTC() {
        #if canImport(TXLiteAVSDK_TRTC)
        if isRemoteViewStarted, !peerUserID.isEmpty {
            cloud.stopRemoteView(peerUserID, streamType: .sub)
        }
        if hasEnteredRoom || state == .connecting || state == .viewing {
            cloud.exitRoom()
        }
        isRemoteViewStarted = false
        hasEnteredRoom = false
        cloud.delegate = nil
        #endif
    }

    private static func newIdentifier() -> String {
        UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
    }
}

#if canImport(TXLiteAVSDK_TRTC)
extension RemoteDesktopSession: TRTCCloudDelegate {
    nonisolated func onEnterRoom(_ result: Int) {
        Task { @MainActor [weak self] in
            guard let self, self.state == .connecting else { return }
            guard result > 0 else {
                self.fail("进入远程房间失败（\(result)）")
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
            self.hasEnteredRoom = false
            if self.state.isActive {
                self.fail(reason == 0 ? "远程连接已结束" : "远程房间异常退出（\(reason)）")
            }
        }
    }

    nonisolated func onUserSubStreamAvailable(_ userId: String, available: Bool) {
        Task { @MainActor [weak self] in
            guard let self, userId == self.peerUserID else { return }
            if available {
                self.startRemoteScreenView()
            } else if self.state == .viewing {
                self.state = .connecting
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
            guard let self,
                  userId == self.peerUserID,
                  streamType == .sub,
                  self.state == .connecting
            else {
                return
            }
            self.invitationTimeoutTask?.cancel()
            self.invitationTimeoutTask = nil
            self.state = .viewing
        }
    }

    nonisolated func onError(
        _ errCode: TXLiteAVError,
        errMsg: String?,
        extInfo: [AnyHashable: Any]?
    ) {
        Task { @MainActor [weak self] in
            guard let self, self.state.isActive else { return }
            let detail = errMsg?.trimmingCharacters(in: .whitespacesAndNewlines)
            self.fail(detail?.isEmpty == false ? detail! : "TRTC 连接失败（\(errCode.rawValue)）")
        }
    }
}
#endif
