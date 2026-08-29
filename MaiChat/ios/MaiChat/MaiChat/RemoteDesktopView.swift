import MaiChatCore
import SwiftUI
import UIKit

struct RemoteDesktopView: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        RemoteDesktopContent(
            session: appState.remoteDesktop,
            appState: appState
        )
    }
}

private struct RemoteDesktopContent: View {
    @ObservedObject var session: RemoteDesktopSession
    let appState: RemoteIMAppState
    @State private var keyboardActive = false
    @State private var keyboardDraft = ""
    @State private var keyboardDraftFocused = true
    @State private var keyboardSubmitRequest = 0
    @State private var zoomScale: CGFloat = 1
    @State private var zoomResetRequest = 0
    @State private var controlGestureHintVisible = false
    @State private var controlGestureHintTask: Task<Void, Never>?

    var body: some View {
        ZStack {
            if session.state.isActive {
                activeSession

                VStack(spacing: 0) {
                    header
                    Spacer(minLength: 0)
                    if session.state == .viewing {
                        controlBar
                    }
                }
            } else {
                VStack(spacing: 0) {
                    header
                    idleState
                }
            }
        }
        .ignoresSafeArea(
            .keyboard,
            edges: session.state.isActive && !keyboardActive ? .bottom : []
        )
        .background(
            (session.state.isActive ? Color.black : RemoteIMStyle.pageBackground)
                .ignoresSafeArea()
        )
        .preferredColorScheme(session.state.isActive ? .dark : nil)
        .onChange(of: session.isControlEnabled) { enabled in
            controlGestureHintTask?.cancel()
            if !enabled {
                keyboardActive = false
                keyboardDraftFocused = true
                keyboardDraft = ""
                controlGestureHintVisible = false
            } else {
                controlGestureHintVisible = true
                controlGestureHintTask = Task { @MainActor in
                    try? await Task.sleep(for: .seconds(2.4))
                    guard !Task.isCancelled else { return }
                    controlGestureHintVisible = false
                }
            }
        }
        .onChange(of: session.state.isActive) { isActive in
            if !isActive {
                controlGestureHintTask?.cancel()
                keyboardActive = false
                keyboardDraftFocused = true
                keyboardDraft = ""
                controlGestureHintVisible = false
                zoomScale = 1
                zoomResetRequest &+= 1
            }
        }
        .onDisappear {
            controlGestureHintTask?.cancel()
        }
    }

    private var header: some View {
        HStack(spacing: 0) {
            Circle()
                .fill(connectionIndicatorColor)
                .frame(width: 8, height: 8)
                .accessibilityLabel(connectionIndicatorLabel)

            Spacer(minLength: 0)

            if session.state.isActive {
                Button {
                    Task { @MainActor in
                        await appState.stopRemoteDesktopView()
                    }
                } label: {
                    Image(systemName: "xmark")
                        .font(.system(size: 12, weight: .bold))
                        .foregroundStyle(Color.white)
                        .frame(width: 28, height: 28)
                        .background(Color.red, in: Circle())
                        .frame(width: 44, height: 44)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("停止远程查看")
            }
        }
        .padding(.horizontal, 12)
        .frame(maxWidth: .infinity)
        .frame(height: 44)
    }

    private var idleState: some View {
        VStack(spacing: 12) {
            Image(systemName: "display")
                .font(.system(size: 34, weight: .medium))
                .foregroundStyle(RemoteIMStyle.textSecondary)
            Text("没有进行中的远程桌面")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(RemoteIMStyle.textPrimary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var activeSession: some View {
        ZStack {
            Color.black
            RemoteDesktopViewport(
                traceID: session.diagnosticTraceID,
                videoSize: session.remoteVideoSize,
                captureGeometry: session.captureGeometry,
                isViewing: session.state == .viewing,
                isControlEnabled: session.isControlEnabled,
                resetRequest: zoomResetRequest,
                bind: { view in
                    session.bindRenderView(view)
                },
                onMove: { x, y in
                    session.movePointer(x: x, y: y)
                },
                onClick: { button, x, y, clickCount in
                    session.clickMouse(
                        button: button,
                        x: x,
                        y: y,
                        clickCount: clickCount
                    )
                },
                onScroll: { delta, x, y in
                    session.scrollPointer(delta: delta, x: x, y: y)
                },
                onCapture: { diagnostic in
                    if keyboardActive {
                        keyboardDraftFocused = false
                    }
                    session.recordPointerCapture(diagnostic)
                },
                onZoomScaleChanged: { newScale in
                    if abs(zoomScale - newScale) > 0.01 {
                        zoomScale = newScale
                    }
                }
            )

            if session.state == .viewing, zoomScale > 1.01 {
                VStack {
                    Spacer()
                    HStack {
                        Spacer()
                        Button {
                            zoomResetRequest &+= 1
                        } label: {
                            HStack(spacing: 5) {
                                Text(String(format: "%.1f", Double(zoomScale)))
                                Text("×")
                                Image(systemName: "arrow.counterclockwise")
                            }
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(Color.white)
                            .padding(.horizontal, 11)
                            .frame(minWidth: 44, minHeight: 44)
                            .background(Color.black.opacity(0.62), in: Capsule())
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(
                            "缩放 \(Int((zoomScale * 100).rounded()))%，点按还原"
                        )
                    }
                    .padding(12)
                }
            }

            if session.state == .viewing, controlGestureHintVisible {
                VStack {
                    Spacer()
                    Text(
                        zoomScale > 1.01
                            ? "画面位置已锁定；关闭控制后可继续缩放"
                            : "关闭“控制中”后可缩放和移动画面"
                    )
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(Color.white)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(Color.black.opacity(0.7), in: Capsule())
                    .padding(.bottom, zoomScale > 1.01 ? 64 : 56)
                }
                .allowsHitTesting(false)
                .transition(.opacity)
            }

            if session.state != .viewing {
                VStack(spacing: 14) {
                    ProgressView()
                        .tint(.white)
                        .scaleEffect(1.15)
                    Text(session.state.statusText)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(Color.white.opacity(0.82))
                }
            }

            if let noticeText = session.noticeText {
                VStack {
                    Text(noticeText)
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 9)
                        .background(Color.orange.opacity(0.9), in: RoundedRectangle(cornerRadius: 8))
                        .padding(12)
                        .padding(.top, 44)
                    Spacer()
                }
                .allowsHitTesting(false)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.black)
    }

    private var controlBar: some View {
        VStack(spacing: 0) {
            if session.isControlEnabled, keyboardActive {
                remoteTextComposer
            }

            HStack(spacing: 4) {
                Spacer(minLength: 0)

                RemoteControlButton(
                    systemImage: "hand.tap",
                    selected: session.isControlEnabled,
                    accessibilityLabel: session.isControlEnabled ? "停止控制" : "开始控制"
                ) {
                    session.setControlEnabled(!session.isControlEnabled)
                }

                if session.isControlEnabled {
                    RemoteControlButton(
                        systemImage: keyboardActive ? "keyboard.chevron.compact.down" : "keyboard",
                        selected: keyboardActive,
                        accessibilityLabel: keyboardActive ? "收起键盘" : "显示键盘"
                    ) {
                        if keyboardActive {
                            keyboardActive = false
                        } else {
                            keyboardDraftFocused = true
                            keyboardActive = true
                        }
                    }

                    remoteControlMenu
                }

                Spacer(minLength: 0)
            }
            .padding(.horizontal, 8)
            .frame(height: 44)
        }
        .frame(maxWidth: .infinity)
    }

    private var remoteTextComposer: some View {
        HStack(spacing: 8) {
            ZStack(alignment: .leading) {
                RemoteKeyboardCapture(
                    traceID: session.diagnosticTraceID,
                    text: $keyboardDraft,
                    isActive: keyboardActive,
                    isDraftFocused: keyboardDraftFocused,
                    submitRequest: keyboardSubmitRequest,
                    onSubmit: sendKeyboardDraft,
                    onDirectText: { value in
                        _ = session.sendTextInput(value)
                    },
                    onDirectKey: session.sendKeyPress,
                    onDismiss: {
                        keyboardActive = false
                        keyboardDraftFocused = true
                    }
                )

                if keyboardDraftFocused, keyboardDraft.isEmpty {
                    Text("输入远程文字")
                        .font(.system(size: 15))
                        .foregroundStyle(Color.white.opacity(0.46))
                        .padding(.leading, 12)
                        .allowsHitTesting(false)
                }

                if !keyboardDraftFocused {
                    Button {
                        keyboardDraftFocused = true
                    } label: {
                        HStack(spacing: 8) {
                            Image(systemName: "keyboard")
                                .font(.system(size: 14, weight: .semibold))

                            VStack(alignment: .leading, spacing: 2) {
                                Text("键盘直接控制中")
                                    .font(.system(size: 13, weight: .semibold))

                                Text(
                                    keyboardDraft.isEmpty
                                        ? "点此编辑文字草稿"
                                        : "草稿已保留，点此继续编辑"
                                )
                                .font(.system(size: 11))
                                .foregroundStyle(Color.white.opacity(0.62))
                            }

                            Spacer(minLength: 0)
                        }
                        .foregroundStyle(Color.white)
                        .padding(.horizontal, 12)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("返回远程文字草稿编辑")
                }
            }
            .frame(height: 68)
            .background(
                keyboardDraftFocused
                    ? Color.white.opacity(0.12)
                    : Color.orange.opacity(0.2),
                in: RoundedRectangle(cornerRadius: 12)
            )
            .overlay {
                RoundedRectangle(cornerRadius: 12)
                    .stroke(Color.white.opacity(0.14), lineWidth: 1)
            }

            Button {
                keyboardDraftFocused = true
                keyboardSubmitRequest &+= 1
            } label: {
                Image(systemName: "arrow.up")
                    .font(.system(size: 15, weight: .bold))
                    .foregroundStyle(Color.white)
                    .frame(width: 36, height: 36)
                    .background(
                        keyboardDraft.isEmpty
                            ? Color.white.opacity(0.16)
                            : Color.blue,
                        in: Circle()
                    )
            }
            .buttonStyle(.plain)
            .disabled(keyboardDraft.isEmpty)
            .accessibilityLabel("发送文字到远端")
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(Color.black.opacity(0.82))
    }

    private func sendKeyboardDraft(_ draft: String) {
        guard !draft.isEmpty else { return }
        if session.sendTextInput(draft) {
            keyboardDraft = ""
        }
    }

    private var remoteControlMenu: some View {
        Menu {
            Section("鼠标") {
                Button {
                    session.setLeftButtonHeld(!session.isLeftButtonHeld)
                } label: {
                    Label(
                        session.isLeftButtonHeld ? "松开左键" : "按住左键",
                        systemImage: "cursorarrow.click"
                    )
                }
                Button {
                    session.clickMouseAtCurrentPointer(button: .right)
                } label: {
                    Label("点击右键", systemImage: "computermouse")
                }
            }

            Section("特殊按键") {
                Button("Esc") { session.sendKeyPress(0x1B) }
                Button("Tab") { session.sendKeyPress(0x09) }
                Button("退格") { session.sendKeyPress(0x08) }
                Button("回车") { session.sendKeyPress(0x0D) }
                Menu("方向键") {
                    Button("上") { session.sendKeyPress(0x26) }
                    Button("下") { session.sendKeyPress(0x28) }
                    Button("左") { session.sendKeyPress(0x25) }
                    Button("右") { session.sendKeyPress(0x27) }
                }
            }
        } label: {
            RemoteControlLabel(
                systemImage: "ellipsis",
                selected: session.isLeftButtonHeld
            )
        }
        .accessibilityLabel("更多远程控制")
        .accessibilityValue(session.isLeftButtonHeld ? "左键已按住" : "左键未按住")
    }

    private var connectionIndicatorColor: Color {
        switch session.state {
        case .viewing:
            return .green
        case .inviting, .connecting:
            return .orange
        case .failed:
            return .red
        case .idle:
            return RemoteIMStyle.textSecondary
        }
    }

    private var connectionIndicatorLabel: String {
        session.isControlEnabled ? "远程控制中" : session.state.statusText
    }
}

private struct RemoteControlButton: View {
    let systemImage: String
    let selected: Bool
    let accessibilityLabel: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            RemoteControlLabel(
                systemImage: systemImage,
                selected: selected
            )
        }
        .buttonStyle(.plain)
        .accessibilityLabel(accessibilityLabel)
    }
}

private struct RemoteControlLabel: View {
    let systemImage: String
    let selected: Bool

    var body: some View {
        Image(systemName: systemImage)
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(Color.white)
            .frame(width: 32, height: 32)
            .background(
                selected ? RemoteIMStyle.blue : Color.white.opacity(0.14),
                in: RoundedRectangle(cornerRadius: 8, style: .continuous)
            )
            .frame(width: 44, height: 44)
            .contentShape(Rectangle())
    }
}

private struct RemoteDesktopViewport: UIViewRepresentable {
    let traceID: String
    let videoSize: CGSize
    let captureGeometry: CaptureGeometry?
    let isViewing: Bool
    let isControlEnabled: Bool
    let resetRequest: Int
    let bind: (UIView?) -> Void
    let onMove: (Double, Double) -> Void
    let onClick: (RemoteMouseButton, Double, Double, Int) -> Void
    let onScroll: (Int, Double, Double) -> Void
    let onCapture: (RemotePointerCaptureDiagnostic) -> Void
    let onZoomScaleChanged: (CGFloat) -> Void

    func makeUIView(context: Context) -> RemoteDesktopViewportUIView {
        let view = RemoteDesktopViewportUIView()
        update(view)
        bind(view.renderView)
        return view
    }

    func updateUIView(_ uiView: RemoteDesktopViewportUIView, context: Context) {
        update(uiView)
    }

    static func dismantleUIView(_ uiView: RemoteDesktopViewportUIView, coordinator: Void) {
        uiView.bindRenderView(nil)
    }

    private func update(_ view: RemoteDesktopViewportUIView) {
        view.bindRenderView = bind
        view.traceID = traceID
        view.update(
            videoSize: videoSize,
            captureGeometry: captureGeometry,
            isViewing: isViewing,
            isControlEnabled: isControlEnabled,
            resetRequest: resetRequest,
            onMove: onMove,
            onClick: onClick,
            onScroll: onScroll,
            onCapture: onCapture,
            onZoomScaleChanged: onZoomScaleChanged
        )
    }
}

private final class RemoteDesktopViewportUIView: UIView, UIScrollViewDelegate {
    let renderView = UIView()
    var bindRenderView: ((UIView?) -> Void) = { _ in }
    var traceID = "none"

    private let scrollView = UIScrollView()
    private let zoomContentView = UIView()
    private let pointerView = RemotePointerUIView()
    private lazy var doubleTapRecognizer = UITapGestureRecognizer(
        target: self,
        action: #selector(handleDoubleTap(_:))
    )

    private var videoSize: CGSize = .zero
    private var captureGeometry: CaptureGeometry?
    private var baseContentSize: CGSize = .zero
    private var lastViewportSize: CGSize = .zero
    private var lastResetRequest = 0
    private var isUpdatingLayout = false
    private var isBrowsingEnabled = false
    private var isControlEnabled = false
    private var lastReportedZoomScale: CGFloat = 1
    private var onZoomScaleChanged: ((CGFloat) -> Void)?
    private var activeZoomTrigger = "pinch"
    private var layoutLogTask: Task<Void, Never>?

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        clipsToBounds = true

        scrollView.backgroundColor = .black
        scrollView.contentInsetAdjustmentBehavior = .never
        scrollView.delegate = self
        scrollView.minimumZoomScale = 1
        scrollView.maximumZoomScale = 4
        scrollView.bounces = true
        scrollView.bouncesZoom = true
        scrollView.decelerationRate = .fast
        scrollView.showsHorizontalScrollIndicator = false
        scrollView.showsVerticalScrollIndicator = false
        scrollView.delaysContentTouches = false
        addSubview(scrollView)

        zoomContentView.backgroundColor = .black
        scrollView.addSubview(zoomContentView)

        renderView.backgroundColor = .black
        zoomContentView.addSubview(renderView)

        pointerView.isUserInteractionEnabled = false
        pointerView.isAccessibilityElement = false
        zoomContentView.addSubview(pointerView)

        doubleTapRecognizer.numberOfTapsRequired = 2
        scrollView.addGestureRecognizer(doubleTapRecognizer)

        isAccessibilityElement = false
        accessibilityLabel = "远程桌面画面"
        accessibilityValue = "缩放 100%"
        accessibilityTraits = [.adjustable]
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let focus = normalizedVisibleCenter()
        scrollView.frame = bounds

        let newBaseSize = aspectFitSize(videoSize: videoSize, viewportSize: bounds.size)
        guard sizeChanged(from: lastViewportSize, to: bounds.size)
                || sizeChanged(from: baseContentSize, to: newBaseSize)
        else {
            return
        }

        reflowContent(to: newBaseSize, preserving: focus)
        lastViewportSize = bounds.size
        scheduleLayoutLog(focus: focus)
    }

    func update(
        videoSize: CGSize,
        captureGeometry: CaptureGeometry?,
        isViewing: Bool,
        isControlEnabled: Bool,
        resetRequest: Int,
        onMove: @escaping (Double, Double) -> Void,
        onClick: @escaping (RemoteMouseButton, Double, Double, Int) -> Void,
        onScroll: @escaping (Int, Double, Double) -> Void,
        onCapture: @escaping (RemotePointerCaptureDiagnostic) -> Void,
        onZoomScaleChanged: @escaping (CGFloat) -> Void
    ) {
        if sizeChanged(from: self.videoSize, to: videoSize) {
            self.videoSize = videoSize
            setNeedsLayout()
        }
        if self.captureGeometry != captureGeometry {
            self.captureGeometry = captureGeometry
            lastViewportSize = .zero
            setNeedsLayout()
        }

        pointerView.videoSize = videoSize
        pointerView.captureGeometry = captureGeometry
        pointerView.onMove = onMove
        pointerView.onClick = onClick
        pointerView.onScroll = onScroll
        pointerView.onCapture = onCapture
        self.onZoomScaleChanged = onZoomScaleChanged

        let previousControlEnabled = self.isControlEnabled
        if isControlEnabled, !previousControlEnabled {
            stopViewportMotion()
        }
        self.isControlEnabled = isControlEnabled
        isBrowsingEnabled = isViewing && !isControlEnabled
        scrollView.panGestureRecognizer.isEnabled = isBrowsingEnabled
        scrollView.pinchGestureRecognizer?.isEnabled = isBrowsingEnabled
        doubleTapRecognizer.isEnabled = isBrowsingEnabled
        pointerView.isUserInteractionEnabled = isViewing && isControlEnabled
        pointerView.isAccessibilityElement = isViewing && isControlEnabled
        isAccessibilityElement = isBrowsingEnabled

        if previousControlEnabled != isControlEnabled {
            logViewport(
                event: "mode-changed",
                fields: [
                    "control_enabled": diagnosticBool(isControlEnabled),
                    "browse_gestures_enabled": diagnosticBool(isBrowsingEnabled),
                    "scale": formatted(scrollView.zoomScale),
                ]
            )
        }

        if resetRequest != lastResetRequest {
            lastResetRequest = resetRequest
            activeZoomTrigger = "reset"
            logViewport(
                event: "zoom-reset-requested",
                fields: viewportDiagnosticFields(trigger: "reset")
            )
            scrollView.setZoomScale(
                1,
                animated: isBrowsingEnabled && !UIAccessibility.isReduceMotionEnabled
            )
            centerContent()
            reportZoomScale()
        }
    }

    func viewForZooming(in scrollView: UIScrollView) -> UIView? {
        zoomContentView
    }

    func scrollViewDidZoom(_ scrollView: UIScrollView) {
        guard !isUpdatingLayout else { return }
        centerContent()
        reportZoomScale()
    }

    func scrollViewWillBeginZooming(_ scrollView: UIScrollView, with view: UIView?) {
        activeZoomTrigger = "pinch"
        logViewport(
            event: "zoom-start",
            fields: viewportDiagnosticFields(trigger: activeZoomTrigger)
        )
    }

    func scrollViewDidEndZooming(
        _ scrollView: UIScrollView,
        with view: UIView?,
        atScale scale: CGFloat
    ) {
        centerContent()
        clampContentOffset()
        reportZoomScale()
        logViewport(
            event: "zoom-end",
            fields: viewportDiagnosticFields(trigger: activeZoomTrigger)
        )
    }

    func scrollViewDidEndDragging(_ scrollView: UIScrollView, willDecelerate decelerate: Bool) {
        guard !decelerate, scrollView.zoomScale > 1.01 else { return }
        logViewport(
            event: "pan-end",
            fields: viewportDiagnosticFields(trigger: "drag")
        )
    }

    func scrollViewDidEndDecelerating(_ scrollView: UIScrollView) {
        guard scrollView.zoomScale > 1.01 else { return }
        logViewport(
            event: "pan-end",
            fields: viewportDiagnosticFields(trigger: "deceleration")
        )
    }

    override func accessibilityIncrement() {
        guard isBrowsingEnabled else { return }
        setAccessibleZoomScale(scrollView.zoomScale + 0.5)
    }

    override func accessibilityDecrement() {
        guard isBrowsingEnabled else { return }
        setAccessibleZoomScale(scrollView.zoomScale - 0.5)
    }

    @objc private func handleDoubleTap(_ recognizer: UITapGestureRecognizer) {
        guard recognizer.state == .ended else { return }
        activeZoomTrigger = "double-tap"
        logViewport(
            event: "zoom-double-tap",
            fields: viewportDiagnosticFields(trigger: activeZoomTrigger)
        )
        let animated = !UIAccessibility.isReduceMotionEnabled
        if scrollView.zoomScale > 1.01 {
            scrollView.setZoomScale(1, animated: animated)
            return
        }

        let targetScale = min(2, scrollView.maximumZoomScale)
        let location = recognizer.location(in: zoomContentView)
        let zoomRect = CGRect(
            x: location.x - scrollView.bounds.width / targetScale / 2,
            y: location.y - scrollView.bounds.height / targetScale / 2,
            width: scrollView.bounds.width / targetScale,
            height: scrollView.bounds.height / targetScale
        )
        scrollView.zoom(to: zoomRect, animated: animated)
    }

    private func reflowContent(to newBaseSize: CGSize, preserving focus: CGPoint) {
        guard newBaseSize.width > 0, newBaseSize.height > 0 else { return }
        isUpdatingLayout = true
        defer { isUpdatingLayout = false }

        let previousScale = min(
            max(scrollView.zoomScale, scrollView.minimumZoomScale),
            scrollView.maximumZoomScale
        )
        if abs(scrollView.zoomScale - 1) > 0.001 {
            scrollView.setZoomScale(1, animated: false)
        }

        baseContentSize = newBaseSize
        zoomContentView.transform = .identity
        zoomContentView.frame = CGRect(origin: .zero, size: newBaseSize)
        renderView.frame = zoomContentView.bounds
        pointerView.frame = zoomContentView.bounds
        scrollView.contentSize = newBaseSize
        centerContent()

        if previousScale > 1.001 {
            scrollView.setZoomScale(previousScale, animated: false)
            centerContent()
            restoreVisibleCenter(focus)
        } else {
            clampContentOffset()
        }
        reportZoomScale()
    }

    private func normalizedVisibleCenter() -> CGPoint {
        guard zoomContentView.bounds.width > 0, zoomContentView.bounds.height > 0 else {
            return CGPoint(x: 0.5, y: 0.5)
        }
        let viewportCenter = CGPoint(x: scrollView.bounds.midX, y: scrollView.bounds.midY)
        let point = scrollView.convert(viewportCenter, to: zoomContentView)
        return CGPoint(
            x: min(max(point.x / zoomContentView.bounds.width, 0), 1),
            y: min(max(point.y / zoomContentView.bounds.height, 0), 1)
        )
    }

    private func restoreVisibleCenter(_ focus: CGPoint) {
        let contentPoint = CGPoint(
            x: focus.x * zoomContentView.bounds.width,
            y: focus.y * zoomContentView.bounds.height
        )
        let currentPoint = zoomContentView.convert(contentPoint, to: scrollView)
        scrollView.contentOffset.x += currentPoint.x - scrollView.bounds.midX
        scrollView.contentOffset.y += currentPoint.y - scrollView.bounds.midY
        clampContentOffset()
    }

    private func centerContent() {
        let horizontalInset = max((scrollView.bounds.width - scrollView.contentSize.width) / 2, 0)
        let verticalInset = max((scrollView.bounds.height - scrollView.contentSize.height) / 2, 0)
        scrollView.contentInset = UIEdgeInsets(
            top: verticalInset,
            left: horizontalInset,
            bottom: verticalInset,
            right: horizontalInset
        )
    }

    private func clampContentOffset() {
        let minX = -scrollView.contentInset.left
        let minY = -scrollView.contentInset.top
        let maxX = max(
            minX,
            scrollView.contentSize.width - scrollView.bounds.width + scrollView.contentInset.right
        )
        let maxY = max(
            minY,
            scrollView.contentSize.height - scrollView.bounds.height + scrollView.contentInset.bottom
        )
        scrollView.contentOffset = CGPoint(
            x: min(max(scrollView.contentOffset.x, minX), maxX),
            y: min(max(scrollView.contentOffset.y, minY), maxY)
        )
    }

    private func stopViewportMotion() {
        scrollView.layer.removeAllAnimations()
        zoomContentView.layer.removeAllAnimations()
        scrollView.setContentOffset(scrollView.contentOffset, animated: false)
        scrollView.setZoomScale(scrollView.zoomScale, animated: false)
        centerContent()
        clampContentOffset()
        reportZoomScale()
    }

    private func setAccessibleZoomScale(_ requestedScale: CGFloat) {
        let scale = min(
            max(requestedScale, scrollView.minimumZoomScale),
            scrollView.maximumZoomScale
        )
        scrollView.setZoomScale(scale, animated: !UIAccessibility.isReduceMotionEnabled)
    }

    private func reportZoomScale() {
        let clampedScale = min(
            max(scrollView.zoomScale, scrollView.minimumZoomScale),
            scrollView.maximumZoomScale
        )
        let scale = (clampedScale * 10).rounded() / 10
        guard abs(scale - lastReportedZoomScale) > 0.01 else { return }
        lastReportedZoomScale = scale
        accessibilityValue = "缩放 \(Int((scale * 100).rounded()))%"
        accessibilityCustomActions = scale > 1.01
            ? [
                UIAccessibilityCustomAction(
                    name: "还原缩放",
                    target: self,
                    selector: #selector(accessibilityResetZoom)
                ),
            ]
            : nil
        DispatchQueue.main.async { [weak self] in
            self?.onZoomScaleChanged?(scale)
        }
    }

    @objc private func accessibilityResetZoom() -> Bool {
        guard isBrowsingEnabled else { return false }
        scrollView.setZoomScale(1, animated: !UIAccessibility.isReduceMotionEnabled)
        return true
    }

    private func aspectFitSize(videoSize: CGSize, viewportSize: CGSize) -> CGSize {
        guard viewportSize.width > 0, viewportSize.height > 0 else { return .zero }
        guard videoSize.width > 0, videoSize.height > 0 else { return viewportSize }
        let scale = min(
            viewportSize.width / videoSize.width,
            viewportSize.height / videoSize.height
        )
        return CGSize(width: videoSize.width * scale, height: videoSize.height * scale)
    }

    private func sizeChanged(from oldSize: CGSize, to newSize: CGSize) -> Bool {
        abs(oldSize.width - newSize.width) > 0.5 || abs(oldSize.height - newSize.height) > 0.5
    }

    private func viewportDiagnosticFields(
        trigger: String,
        focus: CGPoint? = nil
    ) -> [String: String] {
        var fields = [
            "trigger": trigger,
            "viewport": sizeText(bounds.size),
            "video": sizeText(videoSize),
            "encoded_frame": sizeText(videoSize),
            "base": sizeText(baseContentSize),
            "scale": formatted(scrollView.zoomScale),
            "offset": pointText(scrollView.contentOffset),
            "inset": String(
                format: "%.1f,%.1f,%.1f,%.1f",
                scrollView.contentInset.top,
                scrollView.contentInset.left,
                scrollView.contentInset.bottom,
                scrollView.contentInset.right
            ),
        ]
        let encodedFrameRect = CGRect(origin: .zero, size: baseContentSize)
        if let activeContentRect = RemoteDesktopCoordinateMapper.activeContentRect(
            encodedFrameRect: encodedFrameRect,
            captureGeometry: captureGeometry
        ) {
            fields["active_content_rect"] = rectText(activeContentRect)
        }
        if let captureGeometry {
            fields["source_size"] = "\(captureGeometry.sourceWidth)x\(captureGeometry.sourceHeight)"
            fields["capture_rect"] = "\(captureGeometry.captureX),\(captureGeometry.captureY) \(captureGeometry.captureWidth)x\(captureGeometry.captureHeight)"
            fields["content_mode"] = captureGeometry.contentMode
            fields["geometry_revision"] = String(captureGeometry.revision)
            fields["geometry_fallback"] = "active-content"
        } else {
            fields["geometry_fallback"] = "encoded-frame"
        }
        if let focus {
            fields["focus"] = pointText(focus)
        }
        return fields
    }

    private func scheduleLayoutLog(focus: CGPoint) {
        layoutLogTask?.cancel()
        let fields = viewportDiagnosticFields(trigger: "reflow", focus: focus)
        let capturedTraceID = traceID
        layoutLogTask = Task { @MainActor [weak self] in
            do {
                try await Task.sleep(for: .milliseconds(250))
            } catch {
                return
            }
            guard let self else { return }
            self.layoutLogTask = nil
            self.logViewport(
                event: "layout",
                fields: fields,
                traceID: capturedTraceID
            )
        }
    }

    private func logViewport(
        event: String,
        fields: [String: String],
        traceID: String? = nil
    ) {
        var values = fields
        values["trace"] = traceID ?? self.traceID
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "remote-viewport",
            event: event,
            fields: values
        )
    }

    private func sizeText(_ size: CGSize) -> String {
        String(format: "%.1fx%.1f", size.width, size.height)
    }

    private func pointText(_ point: CGPoint) -> String {
        String(format: "%.3f,%.3f", point.x, point.y)
    }

    private func rectText(_ rect: CGRect) -> String {
        String(
            format: "%.1f,%.1f %.1fx%.1f",
            rect.origin.x,
            rect.origin.y,
            rect.width,
            rect.height
        )
    }

    private func formatted(_ value: CGFloat) -> String {
        String(format: "%.2f", value)
    }

    private func diagnosticBool(_ value: Bool) -> String {
        value ? "true" : "false"
    }
}

private final class RemotePointerUIView: UIView, UIGestureRecognizerDelegate {
    var videoSize: CGSize = .zero
    var captureGeometry: CaptureGeometry?
    var onMove: ((Double, Double) -> Void)?
    var onClick: ((RemoteMouseButton, Double, Double, Int) -> Void)?
    var onScroll: ((Int, Double, Double) -> Void)?
    var onCapture: ((RemotePointerCaptureDiagnostic) -> Void)?

    private var pointerPanStartedInContent = false
    private var lastScrollTranslationY: CGFloat = 0
    private var scrollRemainder: CGFloat = 0

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        isOpaque = false
        isAccessibilityElement = true
        accessibilityLabel = "远程桌面控制区域"

        let singleTap = UITapGestureRecognizer(target: self, action: #selector(handleSingleTap(_:)))
        let doubleTap = UITapGestureRecognizer(target: self, action: #selector(handleDoubleTap(_:)))
        doubleTap.numberOfTapsRequired = 2
        singleTap.require(toFail: doubleTap)

        let rightTap = UITapGestureRecognizer(target: self, action: #selector(handleRightTap(_:)))
        rightTap.numberOfTouchesRequired = 2

        let longPress = UILongPressGestureRecognizer(target: self, action: #selector(handleLongPress(_:)))
        longPress.minimumPressDuration = 0.5
        longPress.allowableMovement = 12

        let pointerPan = UIPanGestureRecognizer(target: self, action: #selector(handlePointerPan(_:)))
        pointerPan.minimumNumberOfTouches = 1
        pointerPan.maximumNumberOfTouches = 1

        let scrollPan = UIPanGestureRecognizer(target: self, action: #selector(handleScrollPan(_:)))
        scrollPan.minimumNumberOfTouches = 2
        scrollPan.maximumNumberOfTouches = 2

        for recognizer in [singleTap, doubleTap, rightTap, longPress, pointerPan, scrollPan] {
            recognizer.delegate = self
            addGestureRecognizer(recognizer)
        }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func gestureRecognizer(
        _ gestureRecognizer: UIGestureRecognizer,
        shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer
    ) -> Bool {
        true
    }

    @objc private func handleSingleTap(_ recognizer: UITapGestureRecognizer) {
        guard let point = normalizedPoint(at: recognizer.location(in: self)) else { return }
        onClick?(.left, point.x, point.y, 1)
    }

    @objc private func handleDoubleTap(_ recognizer: UITapGestureRecognizer) {
        guard let point = normalizedPoint(at: recognizer.location(in: self)) else { return }
        onClick?(.left, point.x, point.y, 2)
    }

    @objc private func handleRightTap(_ recognizer: UITapGestureRecognizer) {
        guard let point = normalizedPoint(at: recognizer.location(in: self)) else { return }
        onClick?(.right, point.x, point.y, 1)
    }

    @objc private func handleLongPress(_ recognizer: UILongPressGestureRecognizer) {
        guard recognizer.state == .began,
              let point = normalizedPoint(at: recognizer.location(in: self))
        else {
            return
        }
        onClick?(.right, point.x, point.y, 1)
    }

    @objc private func handlePointerPan(_ recognizer: UIPanGestureRecognizer) {
        switch recognizer.state {
        case .began:
            guard let point = normalizedPoint(at: recognizer.location(in: self)) else {
                pointerPanStartedInContent = false
                return
            }
            pointerPanStartedInContent = true
            onMove?(point.x, point.y)
        case .changed:
            guard pointerPanStartedInContent,
                  let point = normalizedPoint(at: recognizer.location(in: self), clamped: true)
            else {
                return
            }
            onMove?(point.x, point.y)
        case .ended, .cancelled, .failed:
            if pointerPanStartedInContent,
               let point = normalizedPoint(at: recognizer.location(in: self), clamped: true)
            {
                onMove?(point.x, point.y)
            }
            pointerPanStartedInContent = false
        default:
            break
        }
    }

    @objc private func handleScrollPan(_ recognizer: UIPanGestureRecognizer) {
        let translationY = recognizer.translation(in: self).y
        switch recognizer.state {
        case .began:
            lastScrollTranslationY = translationY
            scrollRemainder = 0
        case .changed, .ended:
            scrollRemainder += translationY - lastScrollTranslationY
            lastScrollTranslationY = translationY
            let steps = Int(scrollRemainder / 12)
            guard steps != 0,
                  let point = normalizedPoint(at: recognizer.location(in: self), clamped: true)
            else {
                return
            }
            scrollRemainder -= CGFloat(steps * 12)
            onScroll?(steps * 120, point.x, point.y)
        case .cancelled, .failed:
            scrollRemainder = 0
        default:
            break
        }
    }

    private func normalizedPoint(at location: CGPoint, clamped: Bool = false) -> CGPoint? {
        guard bounds.width > 0,
              bounds.height > 0,
              videoSize.width > 0,
              videoSize.height > 0
        else {
            onCapture?(
                RemotePointerCaptureDiagnostic(
                    location: location,
                    viewportSize: bounds.size,
                    videoSize: videoSize,
                    activeContentRect: nil,
                    normalizedPoint: nil,
                    dropReason: .invalidGeometry
                )
            )
            return nil
        }

        guard let encodedFrameRect = RemoteDesktopCoordinateMapper.aspectFitRect(
            contentSize: videoSize,
            in: bounds
        ), let activeContentRect = RemoteDesktopCoordinateMapper.activeContentRect(
            encodedFrameRect: encodedFrameRect,
            captureGeometry: captureGeometry
        ) else {
            onCapture?(
                RemotePointerCaptureDiagnostic(
                    location: location,
                    viewportSize: bounds.size,
                    videoSize: videoSize,
                    activeContentRect: nil,
                    normalizedPoint: nil,
                    dropReason: .invalidGeometry
                )
            )
            return nil
        }
        guard let point = RemoteDesktopCoordinateMapper.normalizedPoint(
            at: location,
            encodedFrameRect: encodedFrameRect,
            captureGeometry: captureGeometry,
            clamped: clamped
        ) else {
            onCapture?(
                RemotePointerCaptureDiagnostic(
                    location: location,
                    viewportSize: bounds.size,
                    videoSize: videoSize,
                    activeContentRect: activeContentRect,
                    normalizedPoint: nil,
                    dropReason: .letterbox
                )
            )
            return nil
        }

        onCapture?(
            RemotePointerCaptureDiagnostic(
                location: location,
                viewportSize: bounds.size,
                videoSize: videoSize,
                activeContentRect: activeContentRect,
                normalizedPoint: point,
                dropReason: nil
            )
        )
        return point
    }
}

private struct RemoteKeyboardCapture: UIViewRepresentable {
    let traceID: String
    @Binding var text: String
    let isActive: Bool
    let isDraftFocused: Bool
    let submitRequest: Int
    let onSubmit: (String) -> Void
    let onDirectText: (String) -> Void
    let onDirectKey: (UInt32) -> Void
    let onDismiss: () -> Void

    func makeUIView(context: Context) -> RemoteKeyboardInputTextView {
        let view = RemoteKeyboardInputTextView()
        update(view)
        view.lastSubmitRequest = submitRequest
        return view
    }

    func updateUIView(_ uiView: RemoteKeyboardInputTextView, context: Context) {
        update(uiView)
        uiView.wantsKeyboard = isActive
        uiView.submitIfRequested(submitRequest)
    }

    static func dismantleUIView(_ uiView: RemoteKeyboardInputTextView, coordinator: Void) {
        uiView.prepareForDismantle()
    }

    private func update(_ view: RemoteKeyboardInputTextView) {
        view.traceID = traceID
        view.onTextChange = { value in
            text = value
        }
        view.onSubmit = onSubmit
        view.onDirectText = onDirectText
        view.onDirectKey = onDirectKey
        view.onDismiss = onDismiss
        view.updateDraftMode(isDraftFocused, draftText: text)
        view.accessibilityLabel = isDraftFocused
            ? "远程文字草稿"
            : "远程键盘直接控制"
    }
}

@MainActor
private final class RemoteKeyboardInputTextView: UITextView, UITextViewDelegate {
    var traceID = "none"
    var onTextChange: ((String) -> Void)?
    var onSubmit: ((String) -> Void)?
    var onDirectText: ((String) -> Void)?
    var onDirectKey: ((UInt32) -> Void)?
    var onDismiss: (() -> Void)?
    var lastSubmitRequest = 0
    var wantsKeyboard = false {
        didSet {
            guard wantsKeyboard != oldValue else { return }
            logKeyboard(
                level: .info,
                event: "capture-state-changed",
                fields: ["active": wantsKeyboard ? "true" : "false"]
            )
            updateKeyboardFocus()
        }
    }

    private var focusRequestID = 0
    private var isDraftMode = true
    private var pendingDraftMode: Bool?
    private var modeTransitionID = 0

    override init(frame: CGRect, textContainer: NSTextContainer?) {
        super.init(frame: frame, textContainer: textContainer)
        delegate = self
        backgroundColor = .clear
        textColor = .white
        tintColor = .systemBlue
        font = .preferredFont(forTextStyle: .body)
        adjustsFontForContentSizeCategory = true
        textContainerInset = UIEdgeInsets(top: 8, left: 7, bottom: 8, right: 7)
        self.textContainer.lineFragmentPadding = 0
        autocorrectionType = .no
        autocapitalizationType = .none
        spellCheckingType = .no
        smartQuotesType = .no
        smartDashesType = .no
        smartInsertDeleteType = .no
        keyboardDismissMode = .none
        returnKeyType = .default
        accessibilityLabel = "远程键盘输入"
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(applicationDidBecomeActive),
            name: UIApplication.didBecomeActiveNotification,
            object: nil
        )
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        updateKeyboardFocus()
    }

    override func deleteBackward() {
        if !isDraftMode, text.isEmpty, markedTextRange == nil {
            onDirectKey?(0x08)
            return
        }
        super.deleteBackward()
    }

    func textViewDidChange(_ textView: UITextView) {
        if isDraftMode {
            onTextChange?(textView.text ?? "")
        } else {
            flushDirectText()
        }
    }

    func textViewDidEndEditing(_ textView: UITextView) {
        logKeyboard(
            level: wantsKeyboard ? .warning : .info,
            event: "focus-ended",
            fields: ["still_requested": wantsKeyboard ? "true" : "false"]
        )
        if wantsKeyboard {
            // The system keyboard can be dismissed without tapping our toolbar
            // button. Treat that as the source of truth instead of immediately
            // becoming first responder again, otherwise SwiftUI keeps the old
            // keyboard inset and exposes a large blank area below the desktop.
            wantsKeyboard = false
            onDismiss?()
        }
    }

    func textView(
        _ textView: UITextView,
        shouldChangeTextIn range: NSRange,
        replacementText replacement: String
    ) -> Bool {
        guard textView.markedTextRange == nil else { return true }
        if !isDraftMode {
            if replacement == "\n" || replacement == "\r\n" {
                flushDirectText()
                onDirectKey?(0x0D)
                return false
            }
            if replacement == "\t" {
                flushDirectText()
                onDirectKey?(0x09)
                return false
            }
            return true
        }
        if replacement == "\n" || replacement == "\r\n" {
            // Drafts are sent only through the explicit button. Remote Return
            // remains a separate command in the control menu, so typing cannot
            // accidentally execute a command on the controlled computer.
            return false
        }
        if replacement == "\t" {
            return false
        }
        let sanitized = replacement
            .replacingOccurrences(of: "\r\n", with: " ")
            .replacingOccurrences(of: "\r", with: " ")
            .replacingOccurrences(of: "\n", with: " ")
            .replacingOccurrences(of: "\t", with: " ")
        if sanitized != replacement,
           let start = textView.position(
               from: textView.beginningOfDocument,
               offset: range.location
           ),
           let end = textView.position(from: start, offset: range.length),
           let textRange = textView.textRange(from: start, to: end) {
            textView.replace(textRange, withText: sanitized)
            onTextChange?(textView.text ?? "")
            return false
        }
        return true
    }

    func updateDraftMode(_ draftMode: Bool, draftText: String) {
        if isDraftMode == draftMode {
            if pendingDraftMode != nil {
                modeTransitionID &+= 1
            }
            pendingDraftMode = nil
            if draftMode, markedTextRange == nil, text != draftText {
                text = draftText
                selectedRange = NSRange(
                    location: (draftText as NSString).length,
                    length: 0
                )
            }
            return
        }
        guard pendingDraftMode != draftMode else { return }
        pendingDraftMode = draftMode
        modeTransitionID &+= 1
        let transitionID = modeTransitionID
        DispatchQueue.main.async { [weak self] in
            guard let self,
                  self.modeTransitionID == transitionID,
                  self.pendingDraftMode == draftMode,
                  self.wantsKeyboard
            else {
                return
            }
            self.pendingDraftMode = nil
            if self.markedTextRange != nil {
                self.unmarkText()
            }
            if draftMode {
                self.flushDirectText()
                self.isDraftMode = true
                self.text = draftText
                self.selectedRange = NSRange(
                    location: (draftText as NSString).length,
                    length: 0
                )
            } else {
                self.isDraftMode = false
                self.text = ""
            }
        }
    }

    func submitIfRequested(_ request: Int) {
        guard request != lastSubmitRequest else { return }
        lastSubmitRequest = request
        DispatchQueue.main.async { [weak self] in
            guard let self,
                  self.lastSubmitRequest == request,
                  self.wantsKeyboard,
                  self.window != nil,
                  self.isDraftMode
            else {
                return
            }
            self.submitDraft()
        }
    }

    private func submitDraft() {
        if markedTextRange != nil {
            unmarkText()
        }
        let value = text ?? ""
        onTextChange?(value)
        onSubmit?(value)
    }

    private func flushDirectText() {
        guard !isDraftMode, markedTextRange == nil, !text.isEmpty else { return }
        let value = (text ?? "")
            .replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n")
        text = ""
        var buffered = ""

        func flushBufferedText() {
            guard !buffered.isEmpty else { return }
            onDirectText?(buffered)
            buffered = ""
        }

        for character in value {
            switch character {
            case "\n":
                flushBufferedText()
                onDirectKey?(0x0D)
            case "\t":
                flushBufferedText()
                onDirectKey?(0x09)
            default:
                buffered.append(character)
            }
        }
        flushBufferedText()
    }

    private func updateKeyboardFocus() {
        guard wantsKeyboard else {
            focusRequestID &+= 1
            modeTransitionID &+= 1
            pendingDraftMode = nil
            if markedTextRange != nil {
                unmarkText()
            }
            if isFirstResponder {
                resignFirstResponder()
            }
            return
        }
        requestKeyboardFocus()
    }

    /// Objective-C notification selectors do not preserve Swift actor isolation. UIKit normally
    /// posts this notification on the main thread, but treating that as a guarantee caused a
    /// physical-device crash when focus restoration arrived on a cooperative Task executor.
    @objc nonisolated private func applicationDidBecomeActive() {
        let arrivedOnMainThread = Thread.isMainThread
        Task { @MainActor [weak self] in
            guard let self, self.wantsKeyboard else { return }
            if !arrivedOnMainThread {
                self.logKeyboard(
                    level: .warning,
                    event: "activation-focus-rerouted-to-main",
                    fields: ["source_thread": "background"]
                )
            }
            self.requestKeyboardFocus()
        }
    }

    func prepareForDismantle() {
        NotificationCenter.default.removeObserver(self)
        focusRequestID &+= 1
        modeTransitionID &+= 1
        pendingDraftMode = nil
        wantsKeyboard = false
        delegate = nil
        onTextChange = nil
        onSubmit = nil
        onDirectText = nil
        onDirectKey = nil
        onDismiss = nil
    }

    private func requestKeyboardFocus() {
        focusRequestID &+= 1
        let requestID = focusRequestID
        attemptKeyboardFocus(requestID: requestID, remainingAttempts: 3)
    }

    private func attemptKeyboardFocus(requestID: Int, remainingAttempts: Int) {
        guard wantsKeyboard, requestID == focusRequestID, !isFirstResponder else { return }

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) { [weak self] in
            guard let self,
                  self.wantsKeyboard,
                  requestID == self.focusRequestID,
                  !self.isFirstResponder
            else {
                return
            }

            if self.window != nil, self.becomeFirstResponder() {
                self.logKeyboard(
                    level: .info,
                    event: "focus-acquired",
                    fields: ["attempts_remaining": String(remainingAttempts)]
                )
                return
            }
            if remainingAttempts > 1 {
                self.attemptKeyboardFocus(
                    requestID: requestID,
                    remainingAttempts: remainingAttempts - 1
                )
            } else {
                self.logKeyboard(
                    level: .error,
                    event: "focus-failed",
                    fields: ["window_attached": self.window == nil ? "false" : "true"]
                )
            }
        }
    }

    private func logKeyboard(
        level: DiagnosticLogLevel,
        event: String,
        fields: [String: String]
    ) {
        var values = fields
        values["trace"] = traceID
        AppDiagnosticLog.shared.record(
            level: level,
            category: "remote-keyboard",
            event: event,
            fields: values
        )
    }
}
