// 文件作用：扫描电脑登录二维码，在手机或平板上核对并批准一次性登录请求。
// 二维码只包含请求编号；登录凭据始终由账号服务签发，不在二维码中传递。
import AVFoundation
import SwiftUI
import UIKit

private enum DesktopLoginQRCode {
    static func requestID(from text: String) -> String? {
        guard let url = URLComponents(string: text),
              url.scheme?.lowercased() == "maichat",
              url.host?.lowercased() == "auth",
              url.path == "/desktop",
              let items = url.queryItems,
              items.count == 1,
              items[0].name == "request_id",
              let value = items[0].value,
              let identifier = UUID(uuidString: value) else { return nil }
        return identifier.uuidString.lowercased()
    }
}

private enum DesktopLoginCameraAccess {
    case checking
    case authorized
    case denied
    case unavailable
}

private enum DesktopLoginStage {
    case scanning
    case loading(String)
    case confirming(String, MaiChatDesktopLoginRequestInfo, Date)
    case deciding(String, MaiChatDesktopLoginRequestInfo, Date, Bool)
    case approved
    case denied
    case expired
    case failed(String)
}

struct DesktopLoginScanView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase
    @State private var cameraAccess: DesktopLoginCameraAccess = .checking
    @State private var stage: DesktopLoginStage = .scanning
    @State private var now = Date()
    @State private var generation = 0

    /// 扫码、核对、请求中和结果状态都留在同一导航页，避免转场时丢失请求上下文。
    var body: some View {
        GeometryReader { geometry in
            let landscape = geometry.size.width > geometry.size.height
            ScrollView {
                VStack(spacing: 24) {
                    switch stage {
                    case .scanning:
                        scanContent(height: min(max(geometry.size.height * 0.5, 260), 460))
                    case .loading:
                        statusContent(
                            symbol: "qrcode.viewfinder", title: "正在检查登录请求",
                            message: "请稍候…", showProgress: true
                        )
                    case let .confirming(id, info, expiresAt):
                        confirmationContent(id: id, info: info, expiresAt: expiresAt,
                                            isDeciding: false, landscape: landscape)
                    case let .deciding(id, info, expiresAt, approved):
                        confirmationContent(id: id, info: info, expiresAt: expiresAt,
                                            isDeciding: true, approving: approved,
                                            landscape: landscape)
                    case .approved:
                        statusContent(
                            symbol: "checkmark.circle.fill", title: "已允许电脑登录",
                            message: "电脑会继续完成登录。", actionTitle: "返回设置"
                        )
                    case .denied:
                        statusContent(
                            symbol: "xmark.circle.fill", title: "已取消本次登录",
                            message: "这张二维码不能再用于登录。", actionTitle: "返回设置"
                        )
                    case .expired:
                        statusContent(
                            symbol: "clock.badge.exclamationmark", title: "二维码已过期",
                            message: "请在电脑上刷新二维码，再重新扫描。", actionTitle: "重新扫码"
                        )
                    case let .failed(message):
                        statusContent(
                            symbol: "exclamationmark.circle", title: "无法处理登录请求",
                            message: message, actionTitle: "重新扫码"
                        )
                    }
                }
                .frame(maxWidth: 520)
                .padding(.horizontal, 24)
                .padding(.vertical, 24)
                .frame(maxWidth: .infinity, minHeight: geometry.size.height,
                       alignment: landscape && isConfirming ? .center : .top)
            }
        }
        .background(Color(uiColor: .systemGroupedBackground).ignoresSafeArea())
        .navigationTitle("扫码登录电脑")
        .navigationBarTitleDisplayMode(.inline)
        .navigationBarBackButtonHidden(true)
        .toolbar {
            ToolbarItem(placement: .navigationBarLeading) {
                Button("关闭") { close() }
                    .disabled(isDeciding)
            }
        }
        .onAppear {
            updateCameraAccess()
            #if DEBUG && targetEnvironment(simulator)
            // 本机账号模拟服务的 UI 验收入口；真机与发布包没有此路径。
            let environment = ProcessInfo.processInfo.environment
            if environment["MAICHAT_LOCAL_UI_PHONE"] != nil,
               let testURI = environment["MAICHAT_LOCAL_UI_DESKTOP_QR"],
               !testURI.isEmpty {
                receiveQRCode(testURI)
            }
            #endif
        }
        .onDisappear { generation &+= 1 }
        .onChange(of: scenePhase) { phase in
            if phase == .active {
                now = Date()
                expireIfNeeded()
                if case .scanning = stage { updateCameraAccess() }
            }
        }
        .onReceive(Timer.publish(every: 1, on: .main, in: .common).autoconnect()) { date in
            now = date
            expireIfNeeded()
        }
    }

    private var isDeciding: Bool {
        if case .deciding = stage { return true }
        return false
    }

    private var isScanning: Bool {
        if case .scanning = stage { return true }
        return false
    }

    private var isConfirming: Bool {
        switch stage {
        case .confirming, .deciding: return true
        default: return false
        }
    }

    /// 相机授权、无摄像头和扫描取景共用同一尺寸，状态切换时不改变页面结构。
    @ViewBuilder
    private func scanContent(height: CGFloat) -> some View {
        VStack(spacing: 22) {
            VStack(spacing: 8) {
                Text("扫描电脑上的二维码")
                    .font(.title2.weight(.semibold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                Text("打开电脑端 MaiChat 的扫码登录页面")
                    .font(.subheadline)
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
            ZStack {
                RoundedRectangle(cornerRadius: 24)
                    .fill(Color(red: 0.08, green: 0.12, blue: 0.19))
                switch cameraAccess {
                case .authorized:
                    DesktopQRCodeCameraView(
                        isActive: isScanning && scenePhase == .active,
                        onCode: { receiveQRCode($0) },
                        onUnavailable: { cameraAccess = .unavailable }
                    )
                    .clipShape(RoundedRectangle(cornerRadius: 24))
                    RoundedRectangle(cornerRadius: 18)
                        .strokeBorder(.white.opacity(0.85), lineWidth: 3)
                        .frame(width: min(height * 0.65, 280),
                               height: min(height * 0.65, 280))
                        .allowsHitTesting(false)
                case .checking:
                    ProgressView("正在打开相机")
                        .tint(.white)
                        .foregroundStyle(.white)
                case .denied:
                    cameraMessage(symbol: "camera.fill", title: "相机权限未开启",
                                  detail: "请在系统设置中允许 MaiChat 使用相机。")
                case .unavailable:
                    cameraMessage(symbol: "camera.fill", title: "相机不可用",
                                  detail: "请在有摄像头的手机或平板上扫描。")
                }
            }
            .frame(height: height)
            .clipShape(RoundedRectangle(cornerRadius: 24))
            if case .denied = cameraAccess {
                Link("打开系统设置", destination: URL(string: UIApplication.openSettingsURLString)!)
                    .font(.body.weight(.semibold))
            }
            Text("扫描后请核对电脑类型和 6 位核对码，再确认登录。")
                .font(.footnote)
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .multilineTextAlignment(.center)
        }
    }

    private func cameraMessage(symbol: String, title: String, detail: String) -> some View {
        VStack(spacing: 12) {
            Image(systemName: symbol).font(.system(size: 42))
            Text(title).font(.headline)
            Text(detail).font(.subheadline).multilineTextAlignment(.center)
        }
        .foregroundStyle(.white)
        .padding(24)
    }

    /// 横屏并排显示设备信息、核对码和操作，保持关键按钮在可见区域内。
    @ViewBuilder
    private func confirmationContent(
        id: String, info: MaiChatDesktopLoginRequestInfo, expiresAt: Date,
        isDeciding: Bool, approving: Bool = false, landscape: Bool
    ) -> some View {
        if landscape {
            HStack(alignment: .center, spacing: 24) {
                confirmationHeading(info: info, compact: true)
                    .frame(maxWidth: .infinity)
                VStack(spacing: 10) {
                    verificationCard(info: info, expiresAt: expiresAt, compact: true)
                    decisionButtons(id: id, info: info, expiresAt: expiresAt,
                                    isDeciding: isDeciding, approving: approving,
                                    compact: true)
                }
                .frame(maxWidth: .infinity)
            }
        } else {
            VStack(spacing: 24) {
                confirmationHeading(info: info, compact: false)
                verificationCard(info: info, expiresAt: expiresAt, compact: false)
                decisionButtons(id: id, info: info, expiresAt: expiresAt,
                                isDeciding: isDeciding, approving: approving,
                                compact: false)
            }
        }
    }

    private func confirmationHeading(
        info: MaiChatDesktopLoginRequestInfo, compact: Bool
    ) -> some View {
        VStack(spacing: compact ? 8 : 24) {
            Image(systemName: info.platform == "mac" ? "laptopcomputer" : "desktopcomputer")
                .font(.system(size: compact ? 38 : 52, weight: .light))
                .foregroundStyle(.blue)
                .frame(width: compact ? 70 : 96, height: compact ? 70 : 96)
                .background(Color.blue.opacity(0.08), in: RoundedRectangle(cornerRadius: 24))
            VStack(spacing: 8) {
                Text("确认电脑登录")
                    .font((compact ? Font.title3 : Font.title2).weight(.semibold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                Text("正在登录 \(info.platform == "mac" ? "Mac" : "Windows") 版 MaiChat")
                    .font(.subheadline)
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
        }
    }

    private func verificationCard(
        info: MaiChatDesktopLoginRequestInfo, expiresAt: Date, compact: Bool
    ) -> some View {
        VStack(spacing: compact ? 6 : 12) {
            Text("请比对电脑上显示的 6 位核对码")
                .font(.subheadline)
                .foregroundStyle(RemoteIMStyle.textSecondary)
            Text(info.displayCode)
                .font(.system(size: compact ? 32 : 40,
                              weight: .semibold, design: .monospaced))
                .tracking(compact ? 3 : 5)
                .foregroundStyle(RemoteIMStyle.textPrimary)
                .accessibilityIdentifier("desktop-login-code")
            Text("剩余 \(max(0, Int(ceil(expiresAt.timeIntervalSince(now))))) 秒")
                .font(.footnote)
                .foregroundStyle(RemoteIMStyle.textSecondary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, compact ? 14 : 30)
        .background(.white, in: RoundedRectangle(cornerRadius: 20))
    }

    /// 批准和取消在请求处理中同步禁用，避免重复提交与状态回跳。
    private func decisionButtons(
        id: String, info: MaiChatDesktopLoginRequestInfo, expiresAt: Date,
        isDeciding: Bool, approving: Bool, compact: Bool
    ) -> some View {
        VStack(spacing: compact ? 4 : 12) {
            Button {
                decide(id: id, info: info, expiresAt: expiresAt, approve: true)
            } label: {
                HStack(spacing: 8) {
                    if isDeciding && approving { ProgressView().tint(.white) }
                    Text(isDeciding && approving ? "正在确认…" : "确认登录")
                }
                .font(.body.weight(.semibold))
                .frame(maxWidth: .infinity, minHeight: compact ? 44 : 54)
            }
            .buttonStyle(.borderedProminent)
            .disabled(isDeciding)
            .accessibilityIdentifier("desktop-login-approve")

            Button("取消本次登录") {
                decide(id: id, info: info, expiresAt: expiresAt, approve: false)
            }
            .font(.body.weight(.medium))
            .foregroundStyle(.red)
            .frame(minHeight: compact ? 36 : 44)
            .disabled(isDeciding)
            .accessibilityIdentifier("desktop-login-deny")
        }
    }

    /// 完成、过期和失败都显示明确结果，并给出返回或重新扫描操作。
    private func statusContent(
        symbol: String, title: String, message: String,
        showProgress: Bool = false, actionTitle: String? = nil
    ) -> some View {
        VStack(spacing: 18) {
            if showProgress {
                ProgressView().controlSize(.large).frame(height: 70)
            } else {
                Image(systemName: symbol)
                    .font(.system(size: 58, weight: .light))
                    .foregroundStyle(title == "已允许电脑登录" ? .green : .blue)
                    .frame(height: 70)
            }
            Text(title)
                .font(.title2.weight(.semibold))
                .foregroundStyle(RemoteIMStyle.textPrimary)
            Text(message)
                .font(.subheadline)
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .multilineTextAlignment(.center)
            if let actionTitle {
                Button(actionTitle) {
                    if actionTitle == "重新扫码" {
                        generation &+= 1
                        stage = .scanning
                        updateCameraAccess()
                    } else {
                        dismiss()
                    }
                }
                .buttonStyle(.borderedProminent)
                .frame(minHeight: 54)
                .padding(.top, 8)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.horizontal, 20)
        .padding(.vertical, 52)
        .background(.white, in: RoundedRectangle(cornerRadius: 20))
    }

    private func updateCameraAccess() {
        guard AVCaptureDevice.default(for: .video) != nil else {
            cameraAccess = .unavailable
            return
        }
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            cameraAccess = .authorized
        case .denied, .restricted:
            cameraAccess = .denied
        case .notDetermined:
            cameraAccess = .checking
            AVCaptureDevice.requestAccess(for: .video) { granted in
                Task { @MainActor in
                    cameraAccess = granted ? .authorized : .denied
                }
            }
        @unknown default:
            cameraAccess = .unavailable
        }
    }

    /// 只接受含请求编号的 MaiChat URI，再从服务端读取平台、核对码和剩余时间。
    private func receiveQRCode(_ text: String) {
        guard isScanning else { return }
        guard let id = DesktopLoginQRCode.requestID(from: text) else {
            stage = .failed("这不是 MaiChat 的电脑登录二维码，请重新扫描。")
            return
        }
        stage = .loading(id)
        let currentGeneration = generation
        Task { @MainActor in
            do {
                let info = try await appState.desktopLoginRequestInfo(id: id, code: "")
                guard generation == currentGeneration else { return }
                guard info.canDecide, info.expiresIn > 0,
                      ["mac", "windows"].contains(info.platform),
                      info.displayCode.count == 6,
                      info.displayCode.allSatisfy(\.isNumber) else {
                    stage = .failed("登录请求数据无效或已处理，请刷新电脑上的二维码。")
                    return
                }
                stage = .confirming(id, info,
                                    Date().addingTimeInterval(TimeInterval(info.expiresIn)))
            } catch {
                guard generation == currentGeneration else { return }
                stage = isExpired(error) ? .expired : .failed(error.localizedDescription)
            }
        }
    }

    /// 用户明确批准或拒绝后提交服务端；过期与网络失败保留可恢复的状态。
    private func decide(
        id: String, info: MaiChatDesktopLoginRequestInfo,
        expiresAt: Date, approve: Bool
    ) {
        guard Date() < expiresAt else { stage = .expired; return }
        stage = .deciding(id, info, expiresAt, approve)
        let currentGeneration = generation
        Task { @MainActor in
            do {
                if approve {
                    try await appState.approveDesktopLoginRequest(
                        id: id, code: info.displayCode)
                } else {
                    try await appState.denyDesktopLoginRequest(
                        id: id, code: info.displayCode)
                }
                guard generation == currentGeneration else { return }
                stage = approve ? .approved : .denied
            } catch {
                guard generation == currentGeneration else { return }
                stage = isExpired(error) ? .expired : .failed(error.localizedDescription)
            }
        }
    }

    private func expireIfNeeded() {
        if case let .confirming(_, _, expiresAt) = stage, now >= expiresAt {
            stage = .expired
        }
    }

    private func close() {
        switch stage {
        case let .confirming(id, info, expiresAt):
            decide(id: id, info: info, expiresAt: expiresAt, approve: false)
        case .deciding:
            break
        default:
            generation &+= 1
            dismiss()
        }
    }

    private func isExpired(_ error: Error) -> Bool {
        if case MaiChatAuthError.requestFailed("Desktop login request expired") = error {
            return true
        }
        return false
    }
}

private final class DesktopQRCodePreview: UIView {
    override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }

    var previewLayer: AVCaptureVideoPreviewLayer {
        layer as! AVCaptureVideoPreviewLayer
    }
}

private struct DesktopQRCodeCameraView: UIViewRepresentable {
    let isActive: Bool
    let onCode: @MainActor @Sendable (String) -> Void
    let onUnavailable: @MainActor @Sendable () -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(onCode: onCode, onUnavailable: onUnavailable)
    }

    func makeUIView(context: Context) -> DesktopQRCodePreview {
        let view = DesktopQRCodePreview()
        view.previewLayer.session = context.coordinator.session
        view.previewLayer.videoGravity = .resizeAspectFill
        context.coordinator.setRunning(isActive)
        return view
    }

    func updateUIView(_ view: DesktopQRCodePreview, context: Context) {
        context.coordinator.setRunning(isActive)
    }

    static func dismantleUIView(_ view: DesktopQRCodePreview, coordinator: Coordinator) {
        coordinator.stopAndDetachPreview(view)
    }

    /// 一个视图生命周期只使用一个采集会话；重复二维码只交给界面一次。
    final class Coordinator: NSObject, AVCaptureMetadataOutputObjectsDelegate,
                             @unchecked Sendable {
        let session = AVCaptureSession()
        private let queue = DispatchQueue(label: "maichat.desktop-qr.camera")
        private let onCode: @MainActor @Sendable (String) -> Void
        private let onUnavailable: @MainActor @Sendable () -> Void
        private var requestedRunning = false
        private var configured = false
        private var delivered = false

        init(
            onCode: @escaping @MainActor @Sendable (String) -> Void,
            onUnavailable: @escaping @MainActor @Sendable () -> Void
        ) {
            self.onCode = onCode
            self.onUnavailable = onUnavailable
        }

        // SwiftUI 只在主线程调用；AVCaptureSession 的配置和运行都交给同一串行队列。
        func setRunning(_ running: Bool) {
            guard requestedRunning != running else { return }
            requestedRunning = running
            queue.async { [weak self] in
                guard let self else { return }
                if running {
                    self.delivered = false
                    if !self.configured { self.configure() }
                    if self.configured && !self.session.isRunning {
                        self.session.startRunning()
                    }
                } else if self.session.isRunning {
                    self.session.stopRunning()
                }
            }
        }

        // 扫码后 SwiftUI 会立即拆掉取景视图。预览层的 session setter 内部也会
        // 提交采集配置，不能在主线程解绑的同时从采集队列调用 stopRunning。
        // 保留视图到停止完成，再回主线程解绑；队列顺序还保证先前的启动已结束。
        func stopAndDetachPreview(_ view: DesktopQRCodePreview) {
            requestedRunning = false
            queue.async { [self, view] in
                if session.isRunning { session.stopRunning() }
                DispatchQueue.main.async {
                    view.previewLayer.session = nil
                }
            }
        }

        /// 在采集队列配置视频输入与二维码输出；失败时回主线程显示相机状态。
        private func configure() {
            guard let device = AVCaptureDevice.default(for: .video),
                  let input = try? AVCaptureDeviceInput(device: device) else {
                reportUnavailable()
                return
            }
            let output = AVCaptureMetadataOutput()
            session.beginConfiguration()
            defer { session.commitConfiguration() }
            guard session.canAddInput(input), session.canAddOutput(output) else {
                reportUnavailable()
                return
            }
            session.addInput(input)
            session.addOutput(output)
            output.setMetadataObjectsDelegate(self, queue: queue)
            output.metadataObjectTypes = [.qr]
            configured = true
        }

        private func reportUnavailable() {
            DispatchQueue.main.async { [weak self] in self?.onUnavailable() }
        }

        func metadataOutput(
            _ output: AVCaptureMetadataOutput,
            didOutput metadataObjects: [AVMetadataObject],
            from connection: AVCaptureConnection
        ) {
            guard !delivered,
                  let code = metadataObjects.compactMap({
                      ($0 as? AVMetadataMachineReadableCodeObject)?.stringValue
                  }).first else { return }
            delivered = true
            DispatchQueue.main.async { [weak self] in self?.onCode(code) }
        }
    }
}
