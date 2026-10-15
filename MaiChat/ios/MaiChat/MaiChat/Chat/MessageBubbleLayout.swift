// 文件作用：集中实现 IM 与创作中心共用的消息气泡尺寸、消息列表位置观察和 UIKit 导航。
// 消息列表只允许停在已排版内容的范围内；输入、分页和分屏改变可视高度时重新校正偏移。
// ChatNavigationController 保留已打开联系人的页面实例，避免返回会话时重建草稿和滚动状态。
// 本文件不读写消息数据库；可见消息与分页边界由上层状态对象提供。
import SwiftUI
import UIKit
import UniformTypeIdentifiers

enum MessageBubbleMetrics {
    static let horizontalInset: CGFloat = 12
    static let avatarSize: CGFloat = 28
}

enum RemoteIMActivityDurationFormatter {
    static func text(seconds: Int) -> String {
        let value = max(0, seconds)
        guard value >= 60 else { return "\(value)秒" }
        return "\(value / 60)分\(value % 60)秒"
    }
}

enum RemoteIMClipboard {
    static func writeText(_ text: String, to pasteboard: UIPasteboard = .general) {
        pasteboard.setItems(
            [[
                UTType.utf8PlainText.identifier: text,
                UTType.plainText.identifier: text,
            ]]
        )
    }
}

/// Sender identity occupies a header row, not a column beside every line of text.
/// Content keeps its intrinsic width for short messages, but can use the entire
/// proposed width for long text, code and tables. No screen-global width lookup.
struct MessageBubbleLayout<Avatar: View, Metadata: View, Content: View>: View {
    let isOutgoing: Bool
    let showsHeader: Bool
    let avatar: Avatar
    let metadata: Metadata
    let content: Content

    init(
        isOutgoing: Bool,
        showsHeader: Bool = true,
        @ViewBuilder avatar: () -> Avatar,
        @ViewBuilder metadata: () -> Metadata,
        @ViewBuilder content: () -> Content
    ) {
        self.isOutgoing = isOutgoing
        self.showsHeader = showsHeader
        self.avatar = avatar()
        self.metadata = metadata()
        self.content = content()
    }

    var body: some View {
        VStack(alignment: isOutgoing ? .trailing : .leading, spacing: 6) {
            if showsHeader {
                HStack(spacing: 8) {
                    if !isOutgoing { avatar }
                    metadata
                        .layoutPriority(1)
                    if isOutgoing { avatar }
                }
                .frame(maxWidth: .infinity, alignment: isOutgoing ? .trailing : .leading)
            }

            content
        }
        .frame(maxWidth: .infinity, alignment: isOutgoing ? .trailing : .leading)
    }
}

struct MessageHistoryStack<Item: Identifiable, Row: View>: View {
    let items: [Item]
    let row: (Item) -> Row
    init(items: [Item], @ViewBuilder row: @escaping (Item) -> Row) {
        self.items = items; self.row = row
    }
    var body: some View {
        // 上层只传入当前已加载的一页；直接排版这一页，滚动高度才与实际消息一致。
        // 旧页读取完成后才追加到此容器，未加载历史不预留可滑入的空白区域。
        VStack(alignment: .leading, spacing: 14) {
            ForEach(items) { item in row(item) }
        }
    }
}

/// Establish the initial offset during layout, before any frame is presented.
/// Restrict this to initialOffset: size changes must not pull history readers
/// back to the bottom. Older systems retain the explicit positioning fallback.
struct MessageInitialScrollAnchor: ViewModifier {
    static var requiresDeferredPositioning: Bool {
        if #available(iOS 18.0, *) { return false }
        return true
    }

    var startsAtLatest = true

    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(iOS 18.0, *) {
            content.defaultScrollAnchor(startsAtLatest ? .bottom : .top, for: .initialOffset)
        } else {
            content
        }
    }
}


/// Scroll intent is not published per pixel. UIKit gesture callbacks can cancel
/// queued positioning immediately without causing a SwiftUI layout update.
@MainActor
final class MessageScrollIntent: ObservableObject {
    @Published private(set) var userBrowsedHistory = false

    private var generation = 0

    func userDidScroll() {
        if !userBrowsedHistory { userBrowsedHistory = true }
        generation &+= 1
    }

    func followLatest() {
        if userBrowsedHistory { userBrowsedHistory = false }
    }

    func didReachLatest() {
        if userBrowsedHistory { userBrowsedHistory = false }
    }

    func beginPositioning() -> Int {
        generation &+= 1
        return generation
    }

    func isCurrent(_ token: Int) -> Bool { token == generation }

    func cancelPendingPositioning() { generation &+= 1 }

    func positionAtBottom<ID: Hashable>(proxy: ScrollViewProxy, id: ID, anchor: UnitPoint = .bottom) {
        schedulePositioning {
            var transaction = Transaction(animation: nil)
            transaction.disablesAnimations = true
            withTransaction(transaction) { proxy.scrollTo(id, anchor: anchor) }
        }
    }

    func schedulePositioning(_ action: @escaping () -> Void) {
        let token = beginPositioning()
        DispatchQueue.main.async { [weak self] in
            guard let self, self.isCurrent(token) else { return }
            action()
        }
    }

}

/// Saves the bottom proximity before keyboard layout changes the viewport.
final class MessageScrollViewportState {
    private struct Resize {
        let fromHeight: Int
        var toHeight: Int
        let nearBottomBefore: Bool
    }

    private var visibleHeight = -1
    private var pendingResize: Resize?
    private(set) var nearBottom = true

    func record(visible: CGFloat, nearBottom: Bool) {
        let height = Int(visible.rounded())
        if visibleHeight >= 0 && height != visibleHeight {
            if pendingResize == nil {
                pendingResize = Resize(fromHeight: visibleHeight, toHeight: height,
                                       nearBottomBefore: self.nearBottom)
            } else {
                pendingResize?.toHeight = height
            }
        }
        visibleHeight = height
        self.nearBottom = nearBottom
    }

    func nearBottomBeforeKeyboard(opening: Bool) -> Bool {
        defer { pendingResize = nil }
        guard let resize = pendingResize,
              opening ? resize.toHeight < resize.fromHeight :
                        resize.toHeight > resize.fromHeight else { return nearBottom }
        return resize.nearBottomBefore
    }

    func finishKeyboardTransition() { pendingResize = nil }

    func reset() {
        visibleHeight = -1
        pendingResize = nil
        nearBottom = true
    }
}

/// Reads the scroll view's actual position without publishing per-pixel SwiftUI state.
@MainActor
struct MessageScrollPositionReader: UIViewRepresentable {
    final class MarkerView: UIView {
        var onGeometryChange: (() -> Void)?
        override var frame: CGRect {
            didSet { if frame != oldValue { onGeometryChange?() } }
        }
        override var bounds: CGRect {
            didSet { if bounds != oldValue { onGeometryChange?() } }
        }
    }

    var restoreInitialScrollableHistory = false
    var allowsBottomFollowing = true
    var bottomCorrectionRequest = 0
    var userScrollAwayThreshold: CGFloat = 0
    var minimumOlderDragDistance: CGFloat = 0
    var onViewportResizeNeedsBottom: (() -> Void)? = nil
    var onUserScroll: (() -> Void)? = nil
    var onUserReachedTop: (() -> Void)? = nil
    var onViewportMeasured: ((CGFloat, CGFloat, CGFloat, CGFloat, Bool) -> Void)? = nil
    let onNearBottomChanged: (Bool) -> Void

    func makeCoordinator() -> Coordinator {
        let coordinator = Coordinator(onNearBottomChanged)
        coordinator.restoreInitialScrollableHistory = restoreInitialScrollableHistory
        coordinator.allowsBottomFollowing = allowsBottomFollowing
        coordinator.userScrollAwayThreshold = userScrollAwayThreshold
        coordinator.minimumOlderDragDistance = minimumOlderDragDistance
        coordinator.onViewportResizeNeedsBottom = onViewportResizeNeedsBottom
        coordinator.onUserScroll = onUserScroll
        coordinator.onUserReachedTop = onUserReachedTop
        coordinator.onViewportMeasured = onViewportMeasured
        return coordinator
    }
    func makeUIView(context: Context) -> UIView {
        let view = MarkerView(frame: .zero)
        view.isUserInteractionEnabled = false
        install(view, context.coordinator)
        return view
    }
    func updateUIView(_ view: UIView, context: Context) {
        context.coordinator.onChanged = onNearBottomChanged
        context.coordinator.onViewportResizeNeedsBottom = onViewportResizeNeedsBottom
        context.coordinator.onUserScroll = onUserScroll
        context.coordinator.onUserReachedTop = onUserReachedTop
        context.coordinator.onViewportMeasured = onViewportMeasured
        context.coordinator.userScrollAwayThreshold = userScrollAwayThreshold
        context.coordinator.minimumOlderDragDistance = minimumOlderDragDistance
        context.coordinator.setAllowsBottomFollowing(allowsBottomFollowing)
        context.coordinator.setRestoreInitialScrollableHistory(restoreInitialScrollableHistory)
        install(view, context.coordinator)
        context.coordinator.requestBottomCorrection(bottomCorrectionRequest)
    }
    static func dismantleUIView(_ view: UIView, coordinator: Coordinator) { coordinator.uninstall() }

    private func install(_ view: UIView, _ coordinator: Coordinator) {
        DispatchQueue.main.async { [weak view, weak coordinator] in
            guard let view else { return }
            coordinator?.install(from: view)
        }
    }

    @MainActor
    final class Coordinator: NSObject {
        var restoreInitialScrollableHistory = false
        var allowsBottomFollowing = true
        var onChanged: (Bool) -> Void
        var onViewportResizeNeedsBottom: (() -> Void)?
        var onUserScroll: (() -> Void)?
        var onUserReachedTop: (() -> Void)?
        var onViewportMeasured: ((CGFloat, CGFloat, CGFloat, CGFloat, Bool) -> Void)?
        var userScrollAwayThreshold: CGFloat = 0
        var minimumOlderDragDistance: CGFloat = 0
        private weak var scroll: UIScrollView?
        private weak var contentMarker: UIView?
        private var observations: [NSKeyValueObservation] = []
        private var lastNearBottom: Bool?
        private var previousViewportSize: CGSize?
        private var previousContentSize: CGSize?
        private var previousMarkerBottom: CGFloat?
        private var needsBoundaryCorrectionAfterLayoutChange = false
        private var markerUpdateScheduled = false
        private var correctingViewport = false
        private var followsLatest = false
        private var notifiedUserScroll = false
        private var requestedTopForGesture = false
        private var draggedTowardOlderEnough = false
        private var restoreGeneration: UInt64 = 0
        private var lastBottomCorrectionRequest = 0
        init(_ onChanged: @escaping (Bool) -> Void) { self.onChanged = onChanged }

        func setRestoreInitialScrollableHistory(_ enabled: Bool) {
            guard restoreInitialScrollableHistory != enabled else { return }
            restoreInitialScrollableHistory = enabled
            guard enabled, allowsBottomFollowing, !notifiedUserScroll else { return }
            followsLatest = true
            if let scroll,
               scroll.contentSize.height + scroll.adjustedContentInset.top +
                   scroll.adjustedContentInset.bottom > scroll.bounds.height + 1 {
                scheduleBottomCorrection()
            }
        }

        func setAllowsBottomFollowing(_ enabled: Bool) {
            guard allowsBottomFollowing != enabled else { return }
            allowsBottomFollowing = enabled
            restoreGeneration &+= 1
            followsLatest = enabled
            if enabled, let scroll,
               scroll.contentSize.height + scroll.adjustedContentInset.top +
                   scroll.adjustedContentInset.bottom > scroll.bounds.height + 1 {
                scheduleBottomCorrection()
            }
        }

        func requestBottomCorrection(_ request: Int) {
            guard request != lastBottomCorrectionRequest else { return }
            lastBottomCorrectionRequest = request
            guard allowsBottomFollowing else { return }
            followsLatest = true
            scheduleBottomCorrection(allowDeceleration: true)
        }

        func install(from view: UIView) {
            var parent = view.superview
            while let candidate = parent, !(candidate is UIScrollView) { parent = candidate.superview }
            guard let next = parent as? UIScrollView else { return }
            guard next !== scroll else { return }
            uninstall()
            scroll = next
            contentMarker = view
            (view as? MarkerView)?.onGeometryChange = { [weak self] in
                guard let self, !self.markerUpdateScheduled else { return }
                self.markerUpdateScheduled = true
                DispatchQueue.main.async { [weak self] in
                    guard let self else { return }
                    self.markerUpdateScheduled = false
                    self.updateMessageScrollPosition()
                }
            }
            followsLatest = allowsBottomFollowing && restoreInitialScrollableHistory
            next.panGestureRecognizer.addTarget(self, action: #selector(panChanged(_:)))
            observations = [
                next.observe(\.contentOffset, options: [.new]) { [weak self] _, _ in
                    MainActor.assumeIsolated { self?.updateMessageScrollPosition() }
                },
                next.observe(\.contentSize, options: [.new]) { [weak self] _, _ in
                    MainActor.assumeIsolated { self?.updateMessageScrollPosition() }
                },
                next.observe(\.bounds, options: [.new]) { [weak self] _, _ in
                    MainActor.assumeIsolated { self?.updateMessageScrollPosition() }
                }
            ]
            updateMessageScrollPosition()
        }

        func uninstall() {
            restoreGeneration &+= 1
            scroll?.panGestureRecognizer.removeTarget(self, action: #selector(panChanged(_:)))
            observations.forEach { $0.invalidate() }
            observations.removeAll()
            scroll = nil
            (contentMarker as? MarkerView)?.onGeometryChange = nil
            contentMarker = nil
            lastNearBottom = nil
            previousViewportSize = nil
            previousContentSize = nil
            previousMarkerBottom = nil
            needsBoundaryCorrectionAfterLayoutChange = false
            correctingViewport = false
            followsLatest = false
            notifiedUserScroll = false
            requestedTopForGesture = false
            draggedTowardOlderEnough = false
            lastBottomCorrectionRequest = 0
        }

        @objc private func panChanged(_ gesture: UIPanGestureRecognizer) {
            if gesture.state == .began {
                notifiedUserScroll = false
                requestedTopForGesture = false
                draggedTowardOlderEnough = false
            }
            if gesture.state == .changed, let scroll,
               gesture.translation(in: scroll).y >= minimumOlderDragDistance {
                draggedTowardOlderEnough = true
            }
            if gesture.state == .began || gesture.state == .changed {
                userDidBeginScrolling()
            }
            if gesture.state == .changed, !requestedTopForGesture,
               let scroll, gesture.translation(in: scroll).y > 0,
               scroll.contentOffset.y <= -scroll.adjustedContentInset.top + 1 {
                requestedTopForGesture = true
                onUserReachedTop?()
            }
            if (gesture.state == .ended || gesture.state == .cancelled),
               !notifiedUserScroll, allowsBottomFollowing,
               restoreInitialScrollableHistory {
                followsLatest = true
                scheduleBottomCorrection(allowDeceleration: true)
            }
        }

        func userDidBeginScrolling() {
            let leftBottom = userScrollAwayThreshold <= 0 ||
                (draggedTowardOlderEnough &&
                 (remainingDistance() ?? 0) > userScrollAwayThreshold)
            if !notifiedUserScroll && leftBottom {
                notifiedUserScroll = true
                // 回调只使非发布的滚动意图失效；同步取消可阻止排队中的跳转。
                onUserScroll?()
            }
            followsLatest = false
            restoreGeneration &+= 1
        }

        private func remainingDistance() -> CGFloat? {
            guard let scroll else { return nil }
            let bottom = contentMarker.flatMap { marker in
                marker.bounds.height > 0 ? marker.convert(marker.bounds, to: scroll).maxY : nil
            } ?? scroll.contentSize.height
            return min(bottom, scroll.contentSize.height) + scroll.adjustedContentInset.bottom
                - scroll.contentOffset.y - scroll.bounds.height
        }

        private func scheduleBottomCorrection(allowDeceleration: Bool = false) {
            restoreGeneration &+= 1
            let generation = restoreGeneration
            DispatchQueue.main.async { [weak self] in
                guard let self, let scroll = self.scroll,
                      self.restoreGeneration == generation, self.followsLatest,
                      self.allowsBottomFollowing, !scroll.isTracking,
                      !scroll.isDragging,
                      (allowDeceleration || !scroll.isDecelerating) else { return }
                scroll.layoutIfNeeded()
                guard self.restoreGeneration == generation else { return }
                let top = -scroll.adjustedContentInset.top
                let contentMaximum = max(top, scroll.contentSize.height +
                    scroll.adjustedContentInset.bottom - scroll.bounds.height)
                let markerBottom: CGFloat
                if let marker = self.contentMarker, marker.bounds.height > 0 {
                    markerBottom = marker.convert(marker.bounds, to: scroll).maxY
                } else {
                    markerBottom = scroll.contentSize.height
                }
                let target = max(top, min(contentMaximum,
                    markerBottom + scroll.adjustedContentInset.bottom - scroll.bounds.height))
                guard target.isFinite, abs(scroll.contentOffset.y - target) > 1 else { return }
                self.correctingViewport = true
                scroll.setContentOffset(CGPoint(x: scroll.contentOffset.x, y: target),
                                        animated: false)
                self.correctingViewport = false
                self.updateMessageScrollPosition()
                self.onViewportResizeNeedsBottom?()
            }
        }

        /// 读取当前可视区和真实内容边界，修正尺寸变化后的越界偏移。
        /// 手指拖动时不抢控制权；松手后的惯性滚动、键盘和播放器分屏都必须守住边界。
        private func updateMessageScrollPosition() {
            guard let scroll, scroll.bounds.height > 0, !correctingViewport else { return }
            let firstMeasurement = previousViewportSize == nil
            let viewportSize = CGSize(width: scroll.bounds.width.rounded(),
                                      height: scroll.bounds.height.rounded())
            let viewportChanged = previousViewportSize.map { $0 != viewportSize } ?? false
            previousViewportSize = viewportSize
            let contentSize = CGSize(width: scroll.contentSize.width.rounded(),
                                     height: scroll.contentSize.height.rounded())
            let contentChanged = previousContentSize.map { $0 != contentSize } ?? false
            previousContentSize = contentSize
            if firstMeasurement || viewportChanged || contentChanged {
                needsBoundaryCorrectionAfterLayoutChange = true
            }
            // 播放器进入分屏、键盘出现或流式消息收缩时，SwiftUI 可能先保留旧偏移，
            // 再缩小 contentSize。此时偏移已经落在真实内容之外，视口会整块显示空白。
            // 普通拖动或回弹不会改变内容尺寸，不应触发强制夹取；由 UIScrollView
            // 自己完成边界回弹。尺寸变化期间的修正延后到手势及惯性滚动结束。
            if needsBoundaryCorrectionAfterLayoutChange &&
                !scroll.isTracking && !scroll.isDragging && !scroll.isDecelerating &&
                scroll.contentSize.height > 0 {
                needsBoundaryCorrectionAfterLayoutChange = false
                let top = -scroll.adjustedContentInset.top
                let bottom = max(top, scroll.contentSize.height +
                    scroll.adjustedContentInset.bottom - scroll.bounds.height)
                let clamped = min(max(scroll.contentOffset.y, top), bottom)
                if clamped.isFinite && abs(scroll.contentOffset.y - clamped) > 1 {
                    AppDiagnosticLog.shared.record(
                        level: .warning,
                        category: "message-viewport",
                        event: "scroll-offset-clamped",
                        fields: [
                            "old": String(format: "%.1f", scroll.contentOffset.y),
                            "new": String(format: "%.1f", clamped),
                            "content": String(format: "%.1f", scroll.contentSize.height),
                            "viewport": String(format: "%.1f", scroll.bounds.height)
                        ]
                    )
                    correctingViewport = true
                    scroll.setContentOffset(CGPoint(x: scroll.contentOffset.x, y: clamped),
                                            animated: false)
                    correctingViewport = false
                }
            }
            let scrollable = scroll.contentSize.height + scroll.adjustedContentInset.top +
                scroll.adjustedContentInset.bottom > scroll.bounds.height + 1
            let bottom = contentMarker.flatMap { marker in
                marker.bounds.height > 0 ? marker.convert(marker.bounds, to: scroll).maxY : nil
            } ?? scroll.contentSize.height
            let markerChanged = previousMarkerBottom.map { abs($0 - bottom) > 1 } ?? false
            previousMarkerBottom = bottom
            let needsBottom = (viewportChanged || contentChanged || markerChanged) ||
                (firstMeasurement && restoreInitialScrollableHistory)
            if needsBottom, scrollable, followsLatest, allowsBottomFollowing,
               !scroll.isTracking, !scroll.isDragging, !scroll.isDecelerating {
                // 根据实际排版后的几何位置响应变化，将连续 KVO 事件合并为一次校正。
                scheduleBottomCorrection()
            }
            let remaining = min(bottom, scroll.contentSize.height) +
                scroll.adjustedContentInset.bottom
                - scroll.contentOffset.y - scroll.bounds.height
            let nearBottom = remaining <= 60
            onViewportMeasured?(scroll.contentSize.height, scroll.bounds.height,
                                scroll.contentOffset.y, remaining, nearBottom)
            // 排版可能先改变内容尺寸、再改变视口；过渡期间只有用户主动滚动
            // 才能取消跟随最新消息的意图。
            if scroll.isTracking || scroll.isDragging ||
               (userScrollAwayThreshold > 0 && scroll.isDecelerating &&
                draggedTowardOlderEnough && !notifiedUserScroll) {
                userDidBeginScrolling()
            } else if nearBottom && restoreInitialScrollableHistory && allowsBottomFollowing {
                followsLatest = true
            }
            guard nearBottom != lastNearBottom else { return }
            lastNearBottom = nearBottom
            DispatchQueue.main.async { [weak self, weak scroll] in
                guard let self, let scroll, self.scroll === scroll,
                      self.lastNearBottom == nearBottom else { return }
                self.onChanged(nearBottom)
            }
        }
    }
}


enum VoiceTranscriptionTarget: Hashable {
    case send, cancel, edit, finishingSend, finishingEdit
}

enum VoiceTranscriptionHitTest {
    static func target(translation: CGSize, location: CGPoint?,
                       cancelFrame: CGRect?, editFrame: CGRect?,
                       cancelDragDistance: CGFloat = 70) -> VoiceTranscriptionTarget {
        if let location {
            if let frame = cancelFrame, !frame.isEmpty,
               frame.insetBy(dx: -16, dy: -16).contains(location) { return .cancel }
            if let frame = editFrame, !frame.isEmpty,
               frame.insetBy(dx: -16, dy: -16).contains(location) { return .edit }
        }
        if translation.width < -cancelDragDistance { return .cancel }
        if translation.width > 70, translation.height < -35 { return .edit }
        return .send
    }
}

enum RemoteIMScreenshotRecencyPolicy {
    static func accepts(creationDate: Date?, screenshotDate: Date) -> Bool {
        guard let creationDate else { return false }
        return creationDate >= screenshotDate.addingTimeInterval(-3)
            && creationDate <= screenshotDate.addingTimeInterval(10)
    }
}


/// Keep both pages alive during UIKit's native interactive pop. The selection
/// changes only after a completed pop; a cancelled gesture keeps the chat intact.
struct ChatNavigationHost: UIViewControllerRepresentable {
    let root: AnyView
    let detail: AnyView?
    let selectionID: String?
    let onPop: () -> Void

    func makeUIViewController(context: Context) -> ChatNavigationController {
        let controller = ChatNavigationController()
        controller.update(root: root, detail: detail, selectionID: selectionID, onPop: onPop,
                          animated: false)
        return controller
    }

    func updateUIViewController(_ controller: ChatNavigationController, context: Context) {
        controller.update(root: root, detail: detail, selectionID: selectionID, onPop: onPop)
    }
}

@MainActor
final class ChatNavigationArrival: ObservableObject {
    @Published private(set) var hasArrived = false
    // 不发布为界面状态：只过滤交互式返回期间的键盘事件。
    var isReturning = false
    weak var scrollIntent: MessageScrollIntent?
    weak var composerView: UIView?
    func complete() {
        if !hasArrived { hasArrived = true }
    }
}

@MainActor
final class ChatNavigationController: UINavigationController,
    UINavigationControllerDelegate, UIGestureRecognizerDelegate {
    private struct CachedChat {
        let controller: UIHostingController<AnyView>
        let arrival: ChatNavigationArrival
        var lastVisited: UInt64
    }

    private let listController = UIHostingController(rootView: AnyView(EmptyView()))
    private var chatController: UIHostingController<AnyView>?
    private var displayedID: String?
    private var cachedChats: [String: CachedChat] = [:]
    private var visitSequence: UInt64 = 0
    private let cachedChatLimit = 24
    private(set) var arrival = ChatNavigationArrival()
    private var requestedID: String?
    private var requestedDetail: AnyView?
    private var onPop: (() -> Void)?
    private var popInteraction: UIPercentDrivenInteractiveTransition?
    private(set) lazy var fullScreenBackGesture = UIPanGestureRecognizer(
        target: self, action: #selector(handleBackPan(_:)))

    override func viewDidLoad() {
        super.viewDidLoad()
        setNavigationBarHidden(true, animated: false)
        delegate = self
        interactivePopGestureRecognizer?.isEnabled = false
        fullScreenBackGesture.maximumNumberOfTouches = 1
        fullScreenBackGesture.delegate = self
        view.addGestureRecognizer(fullScreenBackGesture)

    }

    func update(root: AnyView, detail: AnyView?, selectionID: String?,
                onPop: @escaping () -> Void, animated: Bool = true) {
        loadViewIfNeeded()
        self.onPop = onPop
        requestedID = selectionID
        requestedDetail = detail
        listController.rootView = root
        if viewControllers.isEmpty {
            setViewControllers([listController], animated: false)
        }
        synchronizeNavigationStack(animated: animated)
    }

    /// 把请求的联系人映射到稳定的聊天页控制器，并与 UIKit 导航栈同步。
    /// 同一联系人重复进入时复用页面；手势转场期间延后处理，避免半途替换视图。
    /// 缓存超过上限时按最久未访问回收，不触碰当前显示的页面。
    private func synchronizeNavigationStack(animated: Bool) {
        // 手指控制返回转场时不替换或移除当前页；didShow 再合并手势期间的新输入。
        guard transitionCoordinator == nil else { return }
        guard let requestedID, let requestedDetail else {
            if viewControllers.count > 1 { popToRootViewController(animated: animated) }
            return
        }
        visitSequence &+= 1
        if let chatController, displayedID == requestedID,
           viewControllers.contains(where: { $0 === chatController }) {
            chatController.rootView = AnyView(requestedDetail.environmentObject(arrival))
            cachedChats[requestedID]?.lastVisited = visitSequence
        } else {
            // 已打开过的联系人保留 UIHostingController 和其 SwiftUI 状态。
            // 返回该聊天时复用原页，输入草稿、滚动位置和已绘制消息不会重新初始化。
            let next: UIHostingController<AnyView>
            if var cached = cachedChats[requestedID] {
                cached.lastVisited = visitSequence
                cachedChats[requestedID] = cached
                arrival = cached.arrival
                next = cached.controller
                next.rootView = AnyView(requestedDetail.environmentObject(arrival))
            } else {
                arrival = ChatNavigationArrival()
                next = UIHostingController(rootView: AnyView(requestedDetail.environmentObject(arrival)))
                next.view.backgroundColor = .systemBackground
                cachedChats[requestedID] = CachedChat(
                    controller: next, arrival: arrival, lastVisited: visitSequence)
            }
            chatController = next
            displayedID = requestedID
            if viewControllers.count == 1 {
                pushViewController(next, animated: animated)
            } else {
                setViewControllers([listController, next], animated: false)
            }
            // 只回收长时间未访问的旧页，不因打开新联系人就销毁上一页。
            if cachedChats.count > cachedChatLimit {
                let evictable = cachedChats
                    .filter { $0.key != requestedID && $0.value.controller !== next }
                    .min { $0.value.lastVisited < $1.value.lastVisited }
                if let evictable { cachedChats.removeValue(forKey: evictable.key) }
            }
        }
    }

    func gestureRecognizerShouldBegin(_ gestureRecognizer: UIGestureRecognizer) -> Bool {
        guard viewControllers.count > 1, transitionCoordinator == nil,
              presentedViewController == nil else { return false }
        if let pan = gestureRecognizer as? UIPanGestureRecognizer {
            let velocity = pan.velocity(in: view)
            guard Self.isRightwardBackGesture(velocity) else { return false }
        }
        return true
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer,
        shouldBeRequiredToFailBy otherGestureRecognizer: UIGestureRecognizer) -> Bool {
        gestureRecognizer === fullScreenBackGesture &&
            otherGestureRecognizer is UIPanGestureRecognizer &&
            otherGestureRecognizer.view?.isDescendant(of: view) == true
    }

    static func isRightwardBackGesture(_ velocity: CGPoint) -> Bool {
        velocity.x > 0 && velocity.x > abs(velocity.y) * 1.2
    }

    @objc private func handleBackPan(_ pan: UIPanGestureRecognizer) {
        let progress = min(1, max(0, pan.translation(in: view).x / max(view.bounds.width, 1)))
        switch pan.state {
        case .began:
            beginBackInteraction()
        case .changed:
            updateBackInteraction(progress: progress)
        case .ended:
            endBackInteraction(progress: progress, velocity: pan.velocity(in: view).x)
        case .cancelled, .failed:
            endBackInteraction(progress: progress, velocity: 0, cancelled: true)
        default: break
        }
    }

    func beginBackInteraction() {
        guard viewControllers.count > 1, transitionCoordinator == nil else { return }
        arrival.isReturning = true
        arrival.scrollIntent?.cancelPendingPositioning()
        // 同一事件里开始收键盘和交互式返回，不等待 keyboardDidHide 才移动页面。
        chatController?.view.endEditing(true)
        startBackTransition()
    }

    private func startBackTransition() {
        popInteraction = UIPercentDrivenInteractiveTransition()
        popInteraction?.completionCurve = .easeOut
        popViewController(animated: true)
    }

    func updateBackInteraction(progress: CGFloat) {
        let progress = min(1, max(0, progress))
        popInteraction?.update(progress)
    }

    private static func shouldFinishBack(progress: CGFloat, velocity: CGFloat, cancelled: Bool) -> Bool {
        !cancelled && (velocity >= 700 || (progress >= 0.33 && velocity > -200))
    }

    func endBackInteraction(progress: CGFloat, velocity: CGFloat, cancelled: Bool = false) {
        if Self.shouldFinishBack(progress: progress, velocity: velocity, cancelled: cancelled) {
            popInteraction?.finish()
        } else {
            popInteraction?.cancel()
        }
    }

    func navigationController(_ navigationController: UINavigationController,
        animationControllerFor operation: UINavigationController.Operation,
        from fromVC: UIViewController, to toVC: UIViewController) -> UIViewControllerAnimatedTransitioning? {
        operation == .pop && popInteraction != nil ? ChatBackAnimator(onCompletion: { [weak self] in
            self?.popInteraction = nil
            self?.arrival.isReturning = false
        }) : nil
    }

    func navigationController(_ navigationController: UINavigationController,
        interactionControllerFor animationController: UIViewControllerAnimatedTransitioning)
        -> UIViewControllerInteractiveTransitioning? {
        popInteraction
    }

    func navigationController(_ navigationController: UINavigationController,
                              didShow viewController: UIViewController, animated: Bool) {
        popInteraction = nil
        arrival.isReturning = false
        if viewController === chatController {
            // SwiftUI 的 onAppear 可能早于 push 完成最终排版；
            // 只通知全新进入，不把取消的返回手势算作一次进入。
            arrival.complete()
        }
        if viewController === listController, let poppedID = displayedID {
            displayedID = nil
            chatController = nil
            if requestedID == poppedID {
                // 这是已完成的交互式返回，不是外部选中状态变化。
                requestedID = nil
                requestedDetail = nil
                onPop?()
            }
        }
        // 等 UIKit 移除已完成的转场协调器后再更新导航状态。
        DispatchQueue.main.async { [weak self] in self?.synchronizeNavigationStack(animated: true) }
    }
}


/// Public UIKit transition APIs: no private gesture targets or selectors.
@MainActor
final class ChatBackAnimator: NSObject, UIViewControllerAnimatedTransitioning {
    private let onCompletion: () -> Void
    init(onCompletion: @escaping () -> Void) { self.onCompletion = onCompletion }
    func transitionDuration(using transitionContext: UIViewControllerContextTransitioning?) -> TimeInterval { 0.3 }

    func animateTransition(using context: UIViewControllerContextTransitioning) {
        guard let from = context.view(forKey: .from), let to = context.view(forKey: .to),
              let target = context.viewController(forKey: .to) else {
            context.completeTransition(false)
            return
        }
        let container = context.containerView
        let width = container.bounds.width
        to.frame = context.finalFrame(for: target)
        container.insertSubview(to, belowSubview: from)
        to.transform = CGAffineTransform(translationX: -width * 0.25, y: 0)
        UIView.animate(withDuration: transitionDuration(using: context), delay: 0,
                       options: [.curveLinear, .beginFromCurrentState]) {
            from.transform = CGAffineTransform(translationX: width, y: 0)
            to.transform = .identity
        } completion: { _ in
            let completed = !context.transitionWasCancelled
            from.transform = .identity
            to.transform = .identity
            context.completeTransition(completed)
            self.onCompletion()
        }
    }
}

#if targetEnvironment(simulator)
/// Test-only UIKit geometry surface: accessibility on SwiftUI containers can
/// resolve to a child Text instead of the message row's allocated rectangle.
struct MessageGeometryProbe: UIViewRepresentable {
    let identifier: String
    func makeUIView(context: Context) -> UIView {
        let view = UIView()
        view.isUserInteractionEnabled = false
        view.isAccessibilityElement = true
        view.accessibilityIdentifier = identifier
        view.accessibilityLabel = "Message row bounds"
        return view
    }
    func updateUIView(_ view: UIView, context: Context) {
        view.accessibilityIdentifier = identifier
    }
}
#endif
