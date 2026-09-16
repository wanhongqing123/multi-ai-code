import SwiftUI
import UIKit

enum MessageBubbleMetrics {
    static let horizontalInset: CGFloat = 12
    static let avatarSize: CGFloat = 28
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
        // Keep every message in one identity/layout domain. Appending must not
        // move a row from an eager tail into a separately estimated lazy prefix.
        LazyVStack(alignment: .leading, spacing: 14) {
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
    private(set) var userBrowsedHistory = false

    private var generation = 0

    func userDidScroll() {
        userBrowsedHistory = true
        generation &+= 1
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

    func positionWithKeyboard<ID: Hashable>(proxy: ScrollViewProxy, id: ID,
                                            notification: Notification, anchor: UnitPoint = .bottom) {
        let duration = (notification.userInfo?[UIResponder.keyboardAnimationDurationUserInfoKey] as? NSNumber)?.doubleValue ?? 0.25
        schedulePositioning {
            withAnimation(.easeOut(duration: duration)) { proxy.scrollTo(id, anchor: anchor) }
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

/// Reads the scroll view's actual position without publishing per-pixel SwiftUI state.
@MainActor
struct MessageScrollPositionReader: UIViewRepresentable {
    var allowsBottomFollowing = true
    var onViewportResizeNeedsBottom: (() -> Void)? = nil
    var onUserScroll: (() -> Void)? = nil
    let onNearBottomChanged: (Bool) -> Void

    func makeCoordinator() -> Coordinator {
        let coordinator = Coordinator(onNearBottomChanged)
        coordinator.allowsBottomFollowing = allowsBottomFollowing
        coordinator.onViewportResizeNeedsBottom = onViewportResizeNeedsBottom
        coordinator.onUserScroll = onUserScroll
        return coordinator
    }
    func makeUIView(context: Context) -> UIView {
        let view = UIView(frame: .zero)
        view.isUserInteractionEnabled = false
        install(view, context.coordinator)
        return view
    }
    func updateUIView(_ view: UIView, context: Context) {
        context.coordinator.onChanged = onNearBottomChanged
        context.coordinator.allowsBottomFollowing = allowsBottomFollowing
        context.coordinator.onViewportResizeNeedsBottom = onViewportResizeNeedsBottom
        context.coordinator.onUserScroll = onUserScroll
        install(view, context.coordinator)
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
        var allowsBottomFollowing = true
        var onChanged: (Bool) -> Void
        var onViewportResizeNeedsBottom: (() -> Void)?
        var onUserScroll: (() -> Void)?
        private weak var scroll: UIScrollView?
        private var observations: [NSKeyValueObservation] = []
        private var lastNearBottom: Bool?
        private var previousViewportSize: CGSize?
        private var correctingViewport = false
        private var followsLatest = false
        private var notifiedUserScroll = false
        private var restoreGeneration: UInt64 = 0
        init(_ onChanged: @escaping (Bool) -> Void) { self.onChanged = onChanged }

        func install(from view: UIView) {
            var parent = view.superview
            while let candidate = parent, !(candidate is UIScrollView) { parent = candidate.superview }
            guard let next = parent as? UIScrollView else { return }
            guard next !== scroll else { return }
            uninstall()
            scroll = next
            next.panGestureRecognizer.addTarget(self, action: #selector(panChanged(_:)))
            observations = [
                next.observe(\.contentOffset, options: [.new]) { [weak self] _, _ in
                    MainActor.assumeIsolated { self?.updatePosition() }
                },
                next.observe(\.contentSize, options: [.new]) { [weak self] _, _ in
                    MainActor.assumeIsolated { self?.updatePosition() }
                },
                next.observe(\.bounds, options: [.new]) { [weak self] _, _ in
                    MainActor.assumeIsolated { self?.updatePosition() }
                }
            ]
            updatePosition()
        }

        func uninstall() {
            restoreGeneration &+= 1
            scroll?.panGestureRecognizer.removeTarget(self, action: #selector(panChanged(_:)))
            observations.forEach { $0.invalidate() }
            observations.removeAll()
            scroll = nil
            lastNearBottom = nil
            previousViewportSize = nil
            correctingViewport = false
            followsLatest = false
            notifiedUserScroll = false
        }

        @objc private func panChanged(_ gesture: UIPanGestureRecognizer) {
            if gesture.state == .began { notifiedUserScroll = false }
            if gesture.state == .began || gesture.state == .changed { userDidBeginScrolling() }
        }

        func userDidBeginScrolling() {
            if !notifiedUserScroll {
                notifiedUserScroll = true
                // The callback only invalidates non-published scroll intent;
                // cancelling synchronously prevents an already queued jump.
                onUserScroll?()
            }
            followsLatest = false
            restoreGeneration &+= 1
        }

        private func scheduleBottomRestore(generation: UInt64, passes: Int) {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.016) { [weak self] in
                guard let self, let scroll = self.scroll,
                      self.restoreGeneration == generation, self.followsLatest,
                      self.allowsBottomFollowing, !scroll.isTracking,
                      !scroll.isDragging, !scroll.isDecelerating else { return }
                self.onViewportResizeNeedsBottom?()
                if passes > 1 {
                    self.scheduleBottomRestore(generation: generation, passes: passes - 1)
                }
            }
        }

        private func updatePosition() {
            guard let scroll, scroll.bounds.height > 0, !correctingViewport else { return }
            let viewportSize = CGSize(width: scroll.bounds.width.rounded(), height: scroll.bounds.height.rounded())
            let viewportChanged = previousViewportSize.map { $0 != viewportSize } ?? false
            previousViewportSize = viewportSize
            if viewportChanged, followsLatest, allowsBottomFollowing,
               !scroll.isTracking, !scroll.isDragging, !scroll.isDecelerating {
                if onViewportResizeNeedsBottom != nil {
                    // Never mutate UIScrollView's offset synchronously inside SwiftUI's
                    // bounds/content-size KVO. Let its stable ID resolve the lazy layout.
                    restoreGeneration &+= 1
                    scheduleBottomRestore(generation: restoreGeneration, passes: 2)
                } else {
                    correctingViewport = true
                    let bottom = max(-scroll.adjustedContentInset.top,
                        scroll.contentSize.height + scroll.adjustedContentInset.bottom - scroll.bounds.height)
                    scroll.setContentOffset(CGPoint(x: scroll.contentOffset.x, y: bottom), animated: false)
                    correctingViewport = false
                }
            }
            let remaining = scroll.contentSize.height + scroll.adjustedContentInset.bottom
                - scroll.contentOffset.y - scroll.bounds.height
            let nearBottom = remaining <= 60
            // Layout can temporarily change contentSize before bounds. Only a real
            // user scroll relinquishes the latest-message intent during that transition.
            if scroll.isTracking || scroll.isDragging || scroll.isDecelerating {
                userDidBeginScrolling()
            } else if nearBottom {
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
                       cancelFrame: CGRect?, editFrame: CGRect?) -> VoiceTranscriptionTarget {
        if let location {
            if let frame = cancelFrame, !frame.isEmpty,
               frame.insetBy(dx: -16, dy: -16).contains(location) { return .cancel }
            if let frame = editFrame, !frame.isEmpty,
               frame.insetBy(dx: -16, dy: -16).contains(location) { return .edit }
        }
        if translation.width < -70 { return .cancel }
        if translation.width > 70, translation.height < -35 { return .edit }
        return .send
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
    // Not published: this only filters keyboard events during interactive back.
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
    private let listController = UIHostingController(rootView: AnyView(EmptyView()))
    private var chatController: UIHostingController<AnyView>?
    private var displayedID: String?
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
        reconcile(animated: animated)
    }

    private func reconcile(animated: Bool) {
        // Do not replace/remove the outgoing page while a finger owns the
        // transition. didShow reconciles any input received during the gesture.
        guard transitionCoordinator == nil else { return }
        guard let requestedID, let requestedDetail else {
            if viewControllers.count > 1 { popToRootViewController(animated: animated) }
            return
        }
        if let chatController, displayedID == requestedID,
           viewControllers.contains(where: { $0 === chatController }) {
            chatController.rootView = AnyView(requestedDetail.environmentObject(arrival))
        } else {
            arrival = ChatNavigationArrival()
            let next = UIHostingController(rootView: AnyView(requestedDetail.environmentObject(arrival)))
            next.view.backgroundColor = .systemBackground
            chatController = next
            displayedID = requestedID
            if viewControllers.count == 1 {
                pushViewController(next, animated: animated)
            } else {
                setViewControllers([listController, next], animated: false)
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
        // Start keyboard dismissal and the interactive pop in the same event.
        // Never wait for keyboardDidHide before moving the page.
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
            // SwiftUI onAppear can run before the push has laid out the final
            // viewport. Signal only a fresh entry, never a cancelled back swipe.
            arrival.complete()
        }
        if viewController === listController, let poppedID = displayedID {
            displayedID = nil
            chatController = nil
            if requestedID == poppedID {
                // A completed interactive pop, not an external selection change.
                requestedID = nil
                requestedDetail = nil
                onPop?()
            }
        }
        // Defer until UIKit has detached its completed transition coordinator.
        DispatchQueue.main.async { [weak self] in self?.reconcile(animated: true) }
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
