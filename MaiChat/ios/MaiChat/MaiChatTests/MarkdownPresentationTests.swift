import XCTest
import SwiftUI
import UIKit
@testable import MaiChatCore

final class MarkdownPresentationTests: XCTestCase {
    @MainActor
    private struct NavigationLifetimeHarness: View {
        @State private var selection: String? = "a"
        var body: some View {
            ChatNavigationHost(root: AnyView(Text("Conversations")),
                detail: selection.map { _ in AnyView(Text(String(repeating: "历史消息\n", count: 100))) },
                selectionID: selection, onPop: { selection = nil })
        }
    }

    @MainActor
    func testRemovingNavigationHostReleasesControllers() async throws {
        for _ in 0..<3 {
            var host: UIHostingController<NavigationLifetimeHarness>? = UIHostingController(rootView: NavigationLifetimeHarness())
            let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
            window.rootViewController = host
            window.makeKeyAndVisible()
            try await Task.sleep(for: .milliseconds(150))
            func findNavigation(_ controller: UIViewController) -> ChatNavigationController? {
                if let navigation = controller as? ChatNavigationController { return navigation }
                return controller.children.lazy.compactMap { findNavigation($0) }.first
            }
            weak var navigation = findNavigation(try XCTUnwrap(host))
            XCTAssertNotNil(navigation)
            weak var chat = navigation?.topViewController
            window.isHidden = true
            window.rootViewController = nil
            host = nil
            try await Task.sleep(for: .milliseconds(500))
            XCTAssertNil(navigation, "Removing the SwiftUI navigation host must release its UIKit container")
            XCTAssertNil(chat, "Removed chats must not retain their message graphs")
        }
    }

    @MainActor
    func testNativeBackNavigationKeepsChatUntilPopCompletes() async throws {
        let navigation = ChatNavigationController()
        var popCount = 0
        let root = AnyView(Text("Conversations"))
        navigation.update(root: root, detail: nil, selectionID: nil,
                          onPop: { popCount += 1 }, animated: false)
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
        window.rootViewController = navigation
        window.makeKeyAndVisible()
        defer { window.isHidden = true; window.rootViewController = nil }
        navigation.update(root: root, detail: AnyView(Text("Chat A")), selectionID: "a",
                          onPop: { popCount += 1 }, animated: false)
        try await Task.sleep(for: .milliseconds(100))
        let chat = try XCTUnwrap(navigation.topViewController)
        let arrival = navigation.arrival
        XCTAssertTrue(arrival.hasArrived)
        let gesture = navigation.fullScreenBackGesture
        XCTAssertTrue(gesture.isEnabled)
        XCTAssertTrue(gesture.view === navigation.view)
        XCTAssertTrue(ChatNavigationController.isRightwardBackGesture(CGPoint(x: 300, y: 20)))
        XCTAssertFalse(ChatNavigationController.isRightwardBackGesture(CGPoint(x: 20, y: 300)))
        XCTAssertFalse(ChatNavigationController.isRightwardBackGesture(CGPoint(x: -300, y: 20)))
        XCTAssertEqual(navigation.viewControllers.count, 2)
        navigation.update(root: root, detail: AnyView(Text("Updated Chat A")), selectionID: "a",
                          onPop: { popCount += 1 }, animated: false)
        XCTAssertTrue(navigation.topViewController === chat, "Message updates retain the hosting controller")
        // UIKit reports the same top page after cancellation. It must not clear selection.
        navigation.navigationController(navigation, didShow: chat, animated: true)
        XCTAssertEqual(popCount, 0)
        XCTAssertTrue(navigation.arrival === arrival, "Cancelled pop must not issue a new entry/scroll intent")
        XCTAssertTrue(navigation.topViewController === chat)
        navigation.popViewController(animated: true)
        try await Task.sleep(for: .milliseconds(700))
        XCTAssertEqual(popCount, 1)
        XCTAssertEqual(navigation.viewControllers.count, 1)
        XCTAssertFalse(navigation.gestureRecognizerShouldBegin(gesture))
    }

    @MainActor
    func testFullScreenBackInteractionCanCancelAndComplete() async throws {
        let navigation = ChatNavigationController()
        var popCount = 0
        navigation.update(root: AnyView(Color.blue), detail: AnyView(Color.white), selectionID: "a",
                          onPop: { popCount += 1 }, animated: false)
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
        window.rootViewController = navigation
        window.makeKeyAndVisible()
        defer { window.isHidden = true; window.rootViewController = nil }
        try await Task.sleep(for: .milliseconds(150))
        let chat = try XCTUnwrap(navigation.topViewController)
        navigation.beginBackInteraction()
        XCTAssertTrue(navigation.arrival.isReturning)
        navigation.updateBackInteraction(progress: 0.4)
        XCTAssertEqual(navigation.transitionCoordinator?.isInteractive, true)
        navigation.endBackInteraction(progress: 0.4, velocity: 0, cancelled: true)
        try await Task.sleep(for: .milliseconds(700))
        XCTAssertTrue(navigation.topViewController === chat)
        XCTAssertFalse(navigation.arrival.isReturning)
        XCTAssertNil(navigation.transitionCoordinator, "Cancelled animation must fully finish")
        XCTAssertEqual(popCount, 0)
        XCTAssertEqual(chat.view.transform, .identity)
        navigation.beginBackInteraction()
        navigation.updateBackInteraction(progress: 0.7)
        navigation.endBackInteraction(progress: 0.7, velocity: 100)
        try await Task.sleep(for: .milliseconds(700))
        XCTAssertEqual(navigation.viewControllers.count, 1)
        XCTAssertEqual(popCount, 1)
        XCTAssertEqual(navigation.topViewController?.view.transform, .identity)
    }

    @MainActor
    func testBackStartsTogetherWithKeyboardDismissal() async throws {
        for cancelled in [true, false] {
            let navigation = ChatNavigationController()
            var popCount = 0
            navigation.update(root: AnyView(Color.blue), detail: AnyView(Color.white), selectionID: "a",
                              onPop: { popCount += 1 }, animated: false)
            let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
            window.rootViewController = navigation
            window.makeKeyAndVisible()
            defer { window.isHidden = true; window.rootViewController = nil }
            try await Task.sleep(for: .milliseconds(100))
            let chat = try XCTUnwrap(navigation.topViewController)
            let input = UITextField(frame: CGRect(x: 0, y: 100, width: 200, height: 44))
            chat.view.addSubview(input)
            input.becomeFirstResponder()
            try await Task.sleep(for: .milliseconds(350))
            XCTAssertTrue(input.isFirstResponder)
            navigation.beginBackInteraction()
            XCTAssertFalse(input.isFirstResponder)
            XCTAssertEqual(navigation.transitionCoordinator?.isInteractive, true,
                "The page must start returning without waiting for keyboardDidHide")
            navigation.updateBackInteraction(progress: 0.7)
            navigation.endBackInteraction(progress: 0.7, velocity: 100, cancelled: cancelled)
            try await Task.sleep(for: .milliseconds(700))
            XCTAssertEqual(navigation.viewControllers.count, cancelled ? 2 : 1)
            XCTAssertEqual(popCount, cancelled ? 0 : 1)
            XCTAssertFalse(navigation.arrival.isReturning)
            XCTAssertNil(navigation.transitionCoordinator)
        }
    }

    @MainActor
    func testBottomBarRemainsPartOfPreviousPageDuringPop() async throws {
        let navigation = ChatNavigationController()
        let probe = HistoryProbe()
        let root = AnyView(VStack(spacing: 0) {
            Text("Conversations").frame(maxWidth: .infinity, maxHeight: .infinity)
            Color.blue.frame(height: 50).background {
                GeometryReader { geometry in
                    Color.clear.onAppear { probe.frames[0] = geometry.frame(in: .global) }
                        .onChange(of: geometry.frame(in: .global)) { probe.frames[0] = $0 }
                }
            }
        })
        navigation.update(root: root, detail: nil, selectionID: nil, onPop: {}, animated: false)
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
        window.rootViewController = navigation
        window.makeKeyAndVisible()
        defer { window.isHidden = true; window.rootViewController = nil }
        try await Task.sleep(for: .milliseconds(150))
        let originalBar = try XCTUnwrap(probe.frames[0])
        let previousPage = try XCTUnwrap(navigation.topViewController)
        navigation.update(root: root, detail: AnyView(Color.white), selectionID: "a",
                          onPop: {}, animated: false)
        try await Task.sleep(for: .milliseconds(100))
        navigation.popViewController(animated: true)
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertNotNil(previousPage.view.superview, "Previous page, including its bar, is visible during the transition")
        XCTAssertEqual(try XCTUnwrap(probe.frames[0]).minY, originalBar.minY, accuracy: 1)
        try await Task.sleep(for: .milliseconds(600))
        XCTAssertTrue(navigation.topViewController === previousPage)
        XCTAssertEqual(try XCTUnwrap(probe.frames[0]).minY, originalBar.minY, accuracy: 1,
                       "Completing pop must not insert an outer bar and resize the page")
    }

    @MainActor
    func testNativeBackNavigationHonorsSelectionChangedDuringPop() async throws {
        let navigation = ChatNavigationController()
        var popCount = 0
        let root = AnyView(Text("Conversations"))
        navigation.update(root: root, detail: AnyView(Text("Chat A")), selectionID: "a",
                          onPop: { popCount += 1 }, animated: false)
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
        window.rootViewController = navigation
        window.makeKeyAndVisible()
        defer { window.isHidden = true; window.rootViewController = nil }
        try await Task.sleep(for: .milliseconds(100))
        navigation.popViewController(animated: true)
        navigation.update(root: root, detail: AnyView(Text("Chat B")), selectionID: "b",
                          onPop: { popCount += 1 })
        try await Task.sleep(for: .milliseconds(1200))
        XCTAssertEqual(popCount, 0, "Finishing A's pop must not clear a newly selected B")
        XCTAssertEqual(navigation.viewControllers.count, 2)
        navigation.update(root: root, detail: nil, selectionID: nil,
                          onPop: { popCount += 1 }, animated: false)
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertEqual(navigation.viewControllers.count, 1)
        XCTAssertEqual(popCount, 0, "The header's explicit back does not trigger a duplicate pop callback")
    }

    @MainActor
    func testPositioningExecutesOnceAndHasNoDelayedSecondCommand() async throws {
        let intent = MessageScrollIntent()
        var calls = 0
        intent.schedulePositioning { calls += 1 }
        try await Task.sleep(for: .milliseconds(80))
        XCTAssertEqual(calls, 1)
        try await Task.sleep(for: .milliseconds(80))
        XCTAssertEqual(calls, 1)
        intent.schedulePositioning { calls += 1 }
        intent.userDidScroll()
        try await Task.sleep(for: .milliseconds(80))
        XCTAssertEqual(calls, 1, "A manual drag cancels the pending command")
    }

    @MainActor
    func testLeavingChatCancelsQueuedPositioning() async throws {
        let intent = MessageScrollIntent()
        var calls = 0
        intent.schedulePositioning { calls += 1 }
        intent.cancelPendingPositioning()
        try await Task.sleep(for: .milliseconds(30))
        XCTAssertEqual(calls, 0)
    }

    @MainActor
    func testUserScrollCancelsPendingEntryAndSearchPositioningImmediately() {
        let intent = MessageScrollIntent()
        XCTAssertFalse(intent.userBrowsedHistory)
        let entry = intent.beginPositioning()
        XCTAssertTrue(intent.isCurrent(entry))
        intent.userDidScroll()
        XCTAssertFalse(intent.isCurrent(entry))
        XCTAssertTrue(intent.userBrowsedHistory, "Reappearance and late history loads must preserve history reading")
        let explicitJump = intent.beginPositioning()
        XCTAssertTrue(intent.isCurrent(explicitJump), "An explicit new-message/search action remains available")
        intent.userDidScroll()
        XCTAssertFalse(intent.isCurrent(explicitJump))
        XCTAssertFalse(MessageScrollIntent().userBrowsedHistory, "A newly opened conversation starts at latest")
    }

    func testVoiceCancelUsesVisibleTargetRegardlessOfPressOrigin() {
        let cancel = CGRect(x: 100, y: 600, width: 64, height: 90)
        let edit = CGRect(x: 236, y: 600, width: 64, height: 90)
        let point = CGPoint(x: 132, y: 640)
        for originX: CGFloat in [30, 130, 200, 330] {
            XCTAssertEqual(VoiceTranscriptionHitTest.target(
                translation: CGSize(width: point.x - originX, height: -100), location: point,
                cancelFrame: cancel, editFrame: edit), .cancel)
        }
        XCTAssertEqual(VoiceTranscriptionHitTest.target(
            translation: CGSize(width: -30, height: -100), location: CGPoint(x: 260, y: 640),
            cancelFrame: cancel, editFrame: edit), .edit)
        XCTAssertEqual(VoiceTranscriptionHitTest.target(
            translation: .zero, location: CGPoint(x: 195, y: 780),
            cancelFrame: cancel, editFrame: edit), .send)
        XCTAssertEqual(VoiceTranscriptionHitTest.target(
            translation: CGSize(width: -90, height: 0), location: nil,
            cancelFrame: nil, editFrame: nil), .cancel)
    }

    @MainActor
    private final class TrackingScrollView: UIScrollView {
        var trackingForTest = false
        override var isTracking: Bool { trackingForTest || super.isTracking }
    }

    @MainActor
    func testViewportResizeKeepsLatestButDoesNotMoveHistoryReaders() {
        let scroll = TrackingScrollView(frame: CGRect(x: 0, y: 0, width: 390, height: 700))
        scroll.contentSize = CGSize(width: 390, height: 3000)
        scroll.contentOffset.y = 2300
        let marker = UIView()
        scroll.addSubview(marker)
        let coordinator = MessageScrollPositionReader.Coordinator { _ in }
        coordinator.install(from: marker)
        defer { coordinator.uninstall() }
        // Keyboard opens, then closes; no new messages arrive.
        scroll.frame.size.height = 400
        XCTAssertEqual(scroll.contentOffset.y, 2600, accuracy: 1)
        scroll.frame.size.height = 700
        XCTAssertEqual(scroll.contentOffset.y, 2300, accuracy: 1)
        // Composer expands/collapses through several intermediate layout sizes.
        for height in [650.0, 550.0, 450.0, 700.0] {
            scroll.frame.size.height = height
            XCTAssertEqual(scroll.contentOffset.y, 3000 - height, accuracy: 1)
        }
        // Explicitly reading earlier history must not jump to latest on resize.
        scroll.trackingForTest = true
        scroll.contentOffset.y = 900
        scroll.trackingForTest = false
        scroll.frame.size.height = 400
        XCTAssertEqual(scroll.contentOffset.y, 900, accuracy: 1)
        scroll.frame.size.height = 700
        XCTAssertEqual(scroll.contentOffset.y, 900, accuracy: 1)
        scroll.contentOffset.y = 2300
        coordinator.allowsBottomFollowing = false
        scroll.frame.size.height = 400
        XCTAssertEqual(scroll.contentOffset.y, 2300, accuracy: 1, "Search positioning disables automatic bottom following")
    }

    @MainActor
    func testUserDragCancelsQueuedBottomRestoration() async throws {
        let scroll = UIScrollView(frame: CGRect(x: 0, y: 0, width: 390, height: 700))
        scroll.contentSize = CGSize(width: 390, height: 3000)
        scroll.contentOffset.y = 2300
        let marker = UIView(); scroll.addSubview(marker)
        var restorations = 0
        let coordinator = MessageScrollPositionReader.Coordinator { _ in }
        coordinator.onViewportResizeNeedsBottom = { restorations += 1 }
        coordinator.install(from: marker)
        defer { coordinator.uninstall() }
        scroll.frame.size.height = 400
        coordinator.userDidBeginScrolling()
        try await Task.sleep(for: .milliseconds(60))
        XCTAssertEqual(restorations, 0, "A queued layout restore must not fight a new user drag")
        scroll.contentOffset.y = 2600
        scroll.frame.size.height = 500
        try await Task.sleep(for: .milliseconds(60))
        XCTAssertEqual(restorations, 2, "A real viewport resize may restore twice, then must settle")
        try await Task.sleep(for: .milliseconds(60))
        XCTAssertEqual(restorations, 2)
    }

    @MainActor
    func testLongBubbleContentUsesAvailableWidthInBothDirections() throws {
        for width: CGFloat in [320, 393, 430, 844] {
            for outgoing in [false, true] {
                let frame = try measureBubbleContent(
                    width: width, outgoing: outgoing, shortMessage: false
                )
                XCTAssertEqual(frame.minX, MessageBubbleMetrics.horizontalInset, accuracy: 1)
                XCTAssertEqual(
                    frame.width, width - MessageBubbleMetrics.horizontalInset * 2, accuracy: 1,
                    "Sender identity and opposite-side spacers must not reserve a text column"
                )
            }
        }
    }

    @MainActor
    func testShortBubblesKeepTheirIntrinsicWidthAndDirection() throws {
        for outgoing in [false, true] {
            let frame = try measureBubbleContent(width: 393, outgoing: outgoing, shortMessage: true)
            XCTAssertGreaterThan(frame.width, 20)
            XCTAssertLessThan(frame.width, 120, "Short messages must not become full-width bars")
            if outgoing {
                XCTAssertEqual(frame.maxX, 393 - MessageBubbleMetrics.horizontalInset, accuracy: 1)
            } else {
                XCTAssertEqual(frame.minX, MessageBubbleMetrics.horizontalInset, accuracy: 1)
            }
        }
    }

    @MainActor
    private final class BubbleGeometry {
        var frame: CGRect?
    }

    @MainActor
    private func measureBubbleContent(
        width: CGFloat,
        outgoing: Bool,
        shortMessage: Bool
    ) throws -> CGRect {
        let measured = BubbleGeometry()
        let view = MessageBubbleLayout(isOutgoing: outgoing) {
            Color.blue.frame(width: MessageBubbleMetrics.avatarSize,
                             height: MessageBubbleMetrics.avatarSize)
        } metadata: {
            Text("Sender · 11:04")
        } content: {
            Group {
                if shortMessage {
                    Text("OK").padding(13)
                } else {
                    Color.blue.frame(height: 80)
                }
            }
            .background {
                GeometryReader { geometry in
                    Color.clear
                        .onAppear { measured.frame = geometry.frame(in: .named("bubble-test")) }
                        .onChange(of: geometry.size) { _ in
                            measured.frame = geometry.frame(in: .named("bubble-test"))
                        }
                }
            }
        }
        .padding(.horizontal, MessageBubbleMetrics.horizontalInset)
        .coordinateSpace(name: "bubble-test")
        .frame(width: width, height: 240, alignment: .topLeading)
        .ignoresSafeArea()
        let controller = UIHostingController(rootView: view)
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: width, height: 300))
        window.rootViewController = controller
        window.makeKeyAndVisible()
        defer {
            window.isHidden = true
            window.rootViewController = nil
        }
        controller.view.setNeedsLayout()
        controller.view.layoutIfNeeded()
        let deadline = Date().addingTimeInterval(2)
        while measured.frame == nil, Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        return try XCTUnwrap(measured.frame, "The rendered content geometry must be observed")
    }

    @MainActor
    func testLongMessageRowContainsAllParagraphsUnderConstrainedProposal() async throws {
        let probe = HistoryProbe()
        let message = MessageBubbleLayout(isOutgoing: false) {
            Color.green.frame(width: 28, height: 28)
        } metadata: { Text("好友 · 12:24") } content: {
            VStack(alignment: .leading, spacing: 12) {
                ForEach(0..<12) { index in
                    Text(String(repeating: "长消息段落 \(index) Commit: 4fe4127c6f71a9e 链接与正文。", count: 5))
                        .fixedSize(horizontal: false, vertical: true)
                        .background {
                            GeometryReader { geometry in
                                Color.clear.onAppear { probe.frames[index] = geometry.frame(in: .named("row")) }
                            }
                        }
                }
            }
        }
        .background {
            GeometryReader { geometry in
                Color.clear.onAppear { probe.frames[-1] = geometry.frame(in: .named("row")) }
            }
        }
        .coordinateSpace(name: "row")
        .frame(width: 393, height: 500, alignment: .topLeading)
        let controller = UIHostingController(rootView: message)
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
        window.rootViewController = controller; window.makeKeyAndVisible()
        defer { window.isHidden = true; window.rootViewController = nil }
        controller.view.layoutIfNeeded()
        try await Task.sleep(for: .milliseconds(150))
        let row = try XCTUnwrap(probe.frames[-1])
        for i in 0..<12 {
            let paragraph = try XCTUnwrap(probe.frames[i])
            XCTAssertLessThanOrEqual(paragraph.maxY, row.maxY + 1, "Painted text must fit its row's measured height")
            if i > 0 {
                XCTAssertGreaterThanOrEqual(paragraph.minY, try XCTUnwrap(probe.frames[i - 1]).maxY,
                    "Paragraphs must not overlap even under a short layout proposal")
            }
        }
    }

    private struct HistoryItem: Identifiable { let id: Int }

    @MainActor
    private final class HistoryModel: ObservableObject {
        @Published var locallyQueuedMessageID: Int?
        @Published var viewportHeight: CGFloat = 640
        @Published var items: [HistoryItem]
        init(_ range: Range<Int>) { items = range.map(HistoryItem.init) }
    }

    @MainActor
    private final class HistoryProbe {
        var mounted = Set<Int>()
        var frames: [Int: CGRect] = [:]
        var proxy: ScrollViewProxy?
        var nearBottom: Bool?
    }

    @MainActor
    private struct HistoryHarness: View {
        @ObservedObject var model: HistoryModel
        let probe: HistoryProbe
        var body: some View {
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(spacing: 14) {
                        MessageHistoryStack(items: model.items) { item in
                            Text(String(repeating: "Markdown line \(item.id)\n", count: abs(item.id) % 5 + 1))
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .background {
                                    GeometryReader { geometry in
                                        Color.clear.onAppear {
                                            probe.mounted.insert(item.id)
                                            probe.frames[item.id] = geometry.frame(in: .named("history-test"))
                                        }.onChange(of: geometry.frame(in: .named("history-test"))) { frame in
                                            probe.frames[item.id] = frame
                                        }
                                    }
                                }
                                .id(item.id)
                        }
                        Color.clear.frame(height: 1).id("bottom")
                    }
                    .background(MessageScrollPositionReader(onViewportResizeNeedsBottom: {
                        proxy.scrollTo("bottom", anchor: .bottom)
                    }) { probe.nearBottom = $0 })
                }
                .onAppear { probe.proxy = proxy }
                .onChange(of: model.items.last?.id) { _ in
                    // Match MessageListView's post-layout restoration on outgoing append.
                    DispatchQueue.main.async {
                        proxy.scrollTo("bottom", anchor: .bottom)
                        DispatchQueue.main.asyncAfter(deadline: .now() + 0.016) { proxy.scrollTo("bottom", anchor: .bottom) }
                    }
                }
            }
            .coordinateSpace(name: "history-test")
            .frame(width: 393, height: model.viewportHeight)
            .ignoresSafeArea()
        }
    }

    @MainActor
    private struct EntryHistoryHarness: View {
        @ObservedObject var model: HistoryModel
        @EnvironmentObject var arrival: ChatNavigationArrival
        let intent: MessageScrollIntent
        let probe: HistoryProbe
        var performsExplicitPositioning = true
        var linesPerMessage = 4

        var body: some View {
            let messages = model.items
            let displayedSendID = model.locallyQueuedMessageID.flatMap { id in
                messages.contains(where: { $0.id == id }) ? id : nil
            }
            return ScrollViewReader { proxy in
                ScrollView {
                    VStack(spacing: 14) {
                        MessageHistoryStack(items: messages) { item in
                            Text(String(repeating: "消息 \(item.id)\n", count: linesPerMessage))
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .background {
                                    GeometryReader { geometry in
                                        Color.clear.onAppear {
                                            probe.frames[item.id] = geometry.frame(in: .named("entry"))
                                        }.onChange(of: geometry.frame(in: .named("entry"))) {
                                            probe.frames[item.id] = $0
                                        }
                                    }
                                }.id(item.id)
                        }
                        Color.clear.frame(height: 1).id("bottom")
                    }
                }
                .modifier(MessageInitialScrollAnchor())
                .coordinateSpace(name: "entry")
                .onAppear {
                    probe.proxy = proxy
                    if performsExplicitPositioning && MessageInitialScrollAnchor.requiresDeferredPositioning && !intent.userBrowsedHistory { intent.positionAtBottom(proxy: proxy, id: "bottom") }
                }
                .onChange(of: displayedSendID) { targetID in
                    guard let targetID else { return }
                    intent.positionAtBottom(proxy: proxy, id: targetID)
                }
                .onChange(of: arrival.hasArrived) { arrived in
                    if performsExplicitPositioning && MessageInitialScrollAnchor.requiresDeferredPositioning && arrived && !intent.userBrowsedHistory {
                        intent.positionAtBottom(proxy: proxy, id: "bottom")
                    }
                }
            }
        }
    }

    @MainActor
    func testLocalSendPositionsItsRowButIncomingAndReceiptKeepHistory() async throws {
        let model = HistoryModel(0..<5), probe = HistoryProbe(), intent = MessageScrollIntent()
        let controller = UIHostingController(rootView: EntryHistoryHarness(
            model: model, intent: intent, probe: probe,
            performsExplicitPositioning: false, linesPerMessage: 12).environmentObject(ChatNavigationArrival()))
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
        window.rootViewController = controller
        window.makeKeyAndVisible()
        defer { window.isHidden = true; window.rootViewController = nil }
        controller.view.layoutIfNeeded()
        try await Task.sleep(for: .milliseconds(150))
        probe.proxy?.scrollTo(2, anchor: .center)
        intent.userDidScroll()
        try await Task.sleep(for: .milliseconds(150))
        let historyY = try XCTUnwrap(probe.frames[2]).minY
        model.items.append(HistoryItem(id: 5)) // Incoming: no local send event.
        try await Task.sleep(for: .milliseconds(150))
        XCTAssertEqual(try XCTUnwrap(probe.frames[2]).minY, historyY, accuracy: 2)

        model.items.append(HistoryItem(id: 6)) // Local pending message, before receipt.
        model.locallyQueuedMessageID = 6
        try await Task.sleep(for: .milliseconds(200))
        let sent = try XCTUnwrap(probe.frames[6])
        XCTAssertGreaterThan(sent.maxY, 0)
        XCTAssertLessThanOrEqual(sent.maxY, controller.view.bounds.height + 1)

        probe.proxy?.scrollTo(3, anchor: .center)
        intent.userDidScroll()
        try await Task.sleep(for: .milliseconds(150))
        let afterSendHistoryY = try XCTUnwrap(probe.frames[3]).minY
        model.objectWillChange.send() // Delivery receipt refreshes without another queue event.
        model.items.append(HistoryItem(id: 7))
        try await Task.sleep(for: .milliseconds(150))
        XCTAssertEqual(try XCTUnwrap(probe.frames[3]).minY, afterSendHistoryY, accuracy: 2)
        model.locallyQueuedMessageID = 999 // A send to a different conversation.
        try await Task.sleep(for: .milliseconds(150))
        XCTAssertEqual(try XCTUnwrap(probe.frames[3]).minY, afterSendHistoryY, accuracy: 2)
    }

    @MainActor
    func testLocalSendReachesNewRowFromLongHistory() async throws {
        let model = HistoryModel(0..<350), probe = HistoryProbe(), intent = MessageScrollIntent()
        let controller = UIHostingController(rootView: EntryHistoryHarness(
            model: model, intent: intent, probe: probe,
            performsExplicitPositioning: false).environmentObject(ChatNavigationArrival()))
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
        window.rootViewController = controller
        window.makeKeyAndVisible()
        defer { window.isHidden = true; window.rootViewController = nil }
        controller.view.layoutIfNeeded()
        try await Task.sleep(for: .milliseconds(200))
        probe.proxy?.scrollTo(100, anchor: .center)
        intent.userDidScroll()
        try await Task.sleep(for: .milliseconds(200))
        model.items.append(HistoryItem(id: 350))
        model.locallyQueuedMessageID = 350
        try await Task.sleep(for: .milliseconds(250))
        let sent = try XCTUnwrap(probe.frames[350])
        XCTAssertGreaterThan(sent.maxY, 0)
        XCTAssertLessThanOrEqual(sent.maxY, controller.view.bounds.height + 1)
    }

    @MainActor
    func testFirstLayoutStartsAtLatestWithoutAnAfterAppearanceJump() async throws {
        let model = HistoryModel(0..<350), probe = HistoryProbe()
        let controller = UIHostingController(rootView: EntryHistoryHarness(
            model: model, intent: MessageScrollIntent(), probe: probe,
            performsExplicitPositioning: false).environmentObject(ChatNavigationArrival()))
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
        window.rootViewController = controller
        window.makeKeyAndVisible()
        defer { window.isHidden = true; window.rootViewController = nil }
        controller.view.layoutIfNeeded()
        try await Task.sleep(for: .milliseconds(100))
        let last = try XCTUnwrap(probe.frames[349])
        XCTAssertGreaterThan(last.maxY, 0)
        XCTAssertLessThanOrEqual(last.maxY, controller.view.bounds.height + 1,
            "The first layout must start at the latest message without a deferred scrollTo")
    }

    @MainActor
    func testAnimatedEntryPositionsLatestAndCancelledReturnKeepsHistory() async throws {
        let navigation = ChatNavigationController()
        let root = AnyView(Text("Conversations"))
        navigation.update(root: root, detail: nil, selectionID: nil, onPop: {}, animated: false)
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
        window.rootViewController = navigation
        window.makeKeyAndVisible()
        defer { window.isHidden = true; window.rootViewController = nil }
        let model = HistoryModel(0..<350), probe = HistoryProbe(), intent = MessageScrollIntent()
        let detail = AnyView(EntryHistoryHarness(model: model, intent: intent, probe: probe))
        navigation.update(root: root, detail: detail, selectionID: "a", onPop: {})
        try await Task.sleep(for: .milliseconds(900))
        XCTAssertTrue(navigation.arrival.hasArrived)
        let last = try XCTUnwrap(probe.frames[349])
        XCTAssertGreaterThan(last.maxY, 0)
        XCTAssertLessThanOrEqual(last.maxY, navigation.view.bounds.height + 1)
        intent.userDidScroll()
        probe.proxy?.scrollTo(100, anchor: .center)
        try await Task.sleep(for: .milliseconds(300))
        let history = try XCTUnwrap(probe.frames[100])
        XCTAssertGreaterThan(history.maxY, 0)
        XCTAssertLessThan(history.minY, navigation.view.bounds.height)
        // Re-showing the same controller (UIKit cancellation) and a late message
        // must not issue a second arrival intent or move the history being read.
        navigation.navigationController(navigation, didShow: try XCTUnwrap(navigation.topViewController), animated: true)
        model.items.append(HistoryItem(id: 350))
        navigation.update(root: root, detail: detail, selectionID: "a", onPop: {})
        try await Task.sleep(for: .milliseconds(300))
        XCTAssertEqual(try XCTUnwrap(probe.frames[100]).minY, history.minY, accuracy: 2)
    }

    @MainActor
    private struct BottomTimelineHarness: View {
        @ObservedObject var model: HistoryModel
        let probe: HistoryProbe
        @StateObject private var intent = MessageScrollIntent()
        var body: some View {
            ScrollViewReader { proxy in
                ScrollView {
                    MessageHistoryStack(items: Array(model.items.reversed())) { item in
                        Text(String(repeating: "消息 \(item.id)\n", count: abs(item.id) % 5 + 1))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background {
                                GeometryReader { geometry in
                                    Color.clear.onAppear {
                                        probe.frames[item.id] = geometry.frame(in: .named("bottom-timeline"))
                                    }.onChange(of: geometry.frame(in: .named("bottom-timeline"))) {
                                        probe.frames[item.id] = $0
                                    }
                                }
                            }
                            .rotationEffect(.degrees(180))
                            .id(item.id)
                    }
                }
                .rotationEffect(.degrees(180))
                .coordinateSpace(name: "bottom-timeline")
                .onAppear { probe.proxy = proxy }
                .onChange(of: model.locallyQueuedMessageID) { id in
                    guard let id else { return }
                    intent.positionAtBottom(proxy: proxy, id: id, anchor: .top)
                }
            }
            .frame(width: 393, height: model.viewportHeight)
            .ignoresSafeArea()
        }
    }

    @MainActor
    func testBottomTimelineKeepsLatestAttachedDuringViewportResize() async throws {
        let model = HistoryModel(0..<350), probe = HistoryProbe()
        let controller = UIHostingController(rootView: BottomTimelineHarness(model: model, probe: probe))
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
        window.rootViewController = controller; window.makeKeyAndVisible()
        defer { window.isHidden = true; window.rootViewController = nil }
        try await Task.sleep(for: .milliseconds(150))
        XCTAssertEqual(try XCTUnwrap(probe.frames[349]).maxY, 640, accuracy: 1)
        for height: CGFloat in [550, 450, 360] {
            model.viewportHeight = height
            try await Task.sleep(for: .milliseconds(80))
            XCTAssertEqual(try XCTUnwrap(probe.frames[349]).maxY, height, accuracy: 1,
                "Viewport resize alone keeps the last message at its edge; no extra scroll command")
        }
    }

    @MainActor
    func testBottomTimelinePreservesHistoryForIncomingAndOlderPagesThenPositionsOwnSend() async throws {
        let model = HistoryModel(0..<50), probe = HistoryProbe()
        let controller = UIHostingController(rootView: BottomTimelineHarness(model: model, probe: probe))
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
        window.rootViewController = controller; window.makeKeyAndVisible()
        defer { window.isHidden = true; window.rootViewController = nil }
        try await Task.sleep(for: .milliseconds(150))
        probe.proxy?.scrollTo(25, anchor: .center)
        try await Task.sleep(for: .milliseconds(150))
        let previous = try XCTUnwrap(probe.frames[25]).minY
        model.items.append(HistoryItem(id: 50))
        try await Task.sleep(for: .milliseconds(150))
        XCTAssertEqual(try XCTUnwrap(probe.frames[25]).minY, previous, accuracy: 2)
        model.items.insert(contentsOf: (-20..<0).map(HistoryItem.init), at: 0)
        try await Task.sleep(for: .milliseconds(150))
        XCTAssertEqual(try XCTUnwrap(probe.frames[25]).minY, previous, accuracy: 2)
        model.items.append(HistoryItem(id: 51))
        model.locallyQueuedMessageID = 51
        try await Task.sleep(for: .milliseconds(200))
        XCTAssertEqual(try XCTUnwrap(probe.frames[51]).maxY, 640, accuracy: 1)
    }

    @MainActor
    private func historyWindow(_ model: HistoryModel, _ probe: HistoryProbe) -> UIWindow {
        let controller = UIHostingController(rootView: HistoryHarness(model: model, probe: probe))
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 393, height: 640))
        window.rootViewController = controller; window.makeKeyAndVisible()
        controller.view.layoutIfNeeded()
        return window
    }

    @MainActor
    func testSingleHistoryPageDoesNotMountEveryOffscreenRow() async throws {
        // The reported conversation had 56 loaded messages. Virtualization must
        // help the first history page too, not only conversations with 350+ rows.
        let model = HistoryModel(0..<56), probe = HistoryProbe()
        let window = historyWindow(model, probe)
        defer { window.isHidden = true; window.rootViewController = nil }
        try await Task.sleep(for: .milliseconds(250))
        print("single-page mounted=\(probe.mounted.count)")
        XCTAssertLessThan(probe.mounted.count, 35,
            "Most of a history page must remain unmounted outside the viewport")
    }

    @MainActor
    func testHistoryRenderingIsBoundedAndLatestRowRemainsReachable() async throws {
        for count in [350, 1000] {
            let probe = HistoryProbe(), model = HistoryModel(0..<count)
            let window = historyWindow(model, probe)
            defer { window.isHidden = true; window.rootViewController = nil }
            try await Task.sleep(for: .milliseconds(250))
            print("history initial loaded=\(count) mounted=\(probe.mounted.count)")
            XCTAssertLessThan(probe.mounted.count, 150, "Offscreen history must not mount every loaded row")
            probe.proxy?.scrollTo(model.items.last!.id, anchor: .bottom)
            try await Task.sleep(for: .milliseconds(200))
            probe.proxy?.scrollTo(model.items.last!.id, anchor: .bottom)
            try await Task.sleep(for: .milliseconds(200))
            let lastFrame = try XCTUnwrap(probe.frames[count - 1])
            XCTAssertGreaterThan(lastFrame.maxY, 0)
            XCTAssertLessThanOrEqual(lastFrame.maxY, 641, "Latest row must be visible without user dragging")
            XCTAssertLessThan(probe.mounted.count, 180)
            // A lazy stack can retain estimated contentSize beyond the actual
            // last row. Visibility is verified from that row's frame above;
            // this obsolete near-bottom estimate no longer drives app scrolling.
            print("history after bottom loaded=\(count) mounted=\(probe.mounted.count)")
            probe.proxy?.scrollTo(count / 2, anchor: .center)
            try await Task.sleep(for: .milliseconds(250))
            let target = try XCTUnwrap(probe.frames[count / 2])
            XCTAssertGreaterThan(target.maxY, 0)
            XCTAssertLessThan(target.minY, 640, "Search jump must materialize its older target")
            XCTAssertLessThan(probe.mounted.count, 200)
        }
    }

    @MainActor
    func testLazyHistoryStaysAtBottomAcrossKeyboardSizedChanges() async throws {
        let model = HistoryModel(0..<350), probe = HistoryProbe()
        let window = historyWindow(model, probe)
        defer { window.isHidden = true; window.rootViewController = nil }
        try await Task.sleep(for: .milliseconds(250))
        for _ in 0..<2 {
            probe.proxy?.scrollTo(model.items.last!.id, anchor: .bottom)
            try await Task.sleep(for: .milliseconds(200))
        }
        for height: CGFloat in [350, 640, 400, 640] {
            model.viewportHeight = height
            try await Task.sleep(for: .milliseconds(250))
            let last = try XCTUnwrap(probe.frames[349])
            XCTAssertGreaterThan(last.maxY, 0)
            XCTAssertLessThanOrEqual(last.maxY, height + 1)
            XCTAssertEqual(probe.nearBottom, true)
        }
    }

    @MainActor
    func testRepeatedSendsAndComposerResizesSettleAtLatest() async throws {
        let model = HistoryModel(0..<90), probe = HistoryProbe()
        let window = historyWindow(model, probe)
        defer { window.isHidden = true; window.rootViewController = nil }
        try await Task.sleep(for: .milliseconds(250))
        for _ in 0..<2 {
            probe.proxy?.scrollTo(model.items.last!.id, anchor: .bottom)
            try await Task.sleep(for: .milliseconds(150))
        }
        for id in 90..<100 {
            model.items.append(HistoryItem(id: id))
            model.viewportHeight = id % 2 == 0 ? 350 : 640
            try await Task.sleep(for: .milliseconds(200))
            XCTAssertEqual(probe.nearBottom, true)
        }
        let settled = try XCTUnwrap(probe.frames[99])
        try await Task.sleep(for: .milliseconds(300))
        let later = try XCTUnwrap(probe.frames[99])
        XCTAssertEqual(later.minY, settled.minY, accuracy: 1, "No self-sustaining scroll/layout oscillation after sending stops")
    }

    @MainActor
    func testHistoryPrependKeepsTheOldFirstRowAsAnAnchor() async throws {
        let model = HistoryModel(0..<50), probe = HistoryProbe()
        let window = historyWindow(model, probe)
        defer { window.isHidden = true; window.rootViewController = nil }
        try await Task.sleep(for: .milliseconds(200))
        probe.proxy?.scrollTo(0, anchor: .top)
        try await Task.sleep(for: .milliseconds(150))
        model.items = (-50..<50).map(HistoryItem.init)
        try await Task.sleep(for: .milliseconds(150))
        probe.proxy?.scrollTo(0, anchor: .top)
        try await Task.sleep(for: .milliseconds(250))
        XCTAssertEqual(try XCTUnwrap(probe.frames[0]).minY, 0, accuracy: 2)
        probe.proxy?.scrollTo(-50, anchor: .top)
        try await Task.sleep(for: .milliseconds(250))
        XCTAssertEqual(try XCTUnwrap(probe.frames[-50]).minY, 0, accuracy: 2)
    }

    func testConversationPreviewUsesReadableMarkdownText() {
        let cases: [(String, String)] = [
            ("## 标题\n\n**重点** 与 `code`", "标题 重点 与 code"),
            ("[文档](https://example.com) 与 ~~旧版~~", "文档 与 旧版"),
            ("```cpp\na*b*c\npath_with_under_score\n```", "a*b*c path_with_under_score"),
            ("> [!TIP]\n> 使用 **独立目录**", "建议：使用 独立目录"),
            ("- [x] 完成\n- [ ] 待办", "☑ 完成 ☐ 待办"),
            ("| A | B |\n|---|---|\n|1|2|", "A B 1 2"),
            ("[文档][ref]\n\n[ref]: https://example.com", "文档"),
            ("![替代文字](https://example.com/a.png)", "替代文字"),
            ("`**literal**` 与 `a_b`", "**literal** 与 a_b"),
            ("```text\n> [!TIP]\n```", "> [!TIP]"),
            ("> `[!TIP]`", "[!TIP]"),
            ("## 标题\r\n\r\n下一段", "标题 下一段")
        ]
        for (source, expected) in cases {
            XCTAssertEqual(MarkdownConversationPreview.plainText(source), expected, source)
            // Repeated list refreshes return the same cached presentation.
            XCTAssertEqual(MarkdownConversationPreview.plainText(source), expected, source)
        }
    }

    func testConversationPreviewDoesNotModifyMessagesOrAttachments() {
        var message = RemoteIMMessage(fromUserID: "a", toUserID: "b", text: "## **原文**",
                                      direction: .incoming, status: .received, createdAt: Date())
        XCTAssertEqual(MarkdownConversationPreview.text(for: message), "原文")
        XCTAssertEqual(message.text, "## **原文**")
        message.text = "**更新**"
        XCTAssertEqual(MarkdownConversationPreview.text(for: message), "更新")
        XCTAssertEqual(MarkdownConversationPreview.text(for: nil), "暂无消息")
        let attachment = RemoteIMMessage(fromUserID: "a", toUserID: "b", text: "[文件消息] **report**.md",
            fileAttachment: RemoteIMFileAttachment(localFilePath: "/tmp/report.md", fileName: "report.md", mimeType: "text/markdown"),
            direction: .incoming, status: .received, createdAt: Date())
        XCTAssertEqual(MarkdownConversationPreview.text(for: attachment), attachment.text)
    }

    func testConversationPreviewClampsAtWholeEmojiBoundaries() {
        let emoji = "👨‍👩‍👧‍👦"
        XCTAssertEqual(MarkdownConversationPreview.plainText(String(repeating: emoji, count: 170)),
                       String(repeating: emoji, count: 160) + "…")
        XCTAssertLessThanOrEqual(MarkdownConversationPreview.plainText(String(repeating: "长", count: 100_000)).count, 161)
        XCTAssertEqual(MarkdownConversationPreview.plainText("---"), "")
    }

    func testInlineCodeStylesEveryRangeAndPreservesOtherTextAndLinks() throws {
        let source = try AttributedString(markdown: "`one` plain `two` **`three`** [link](https://example.com)")
        let styled = MarkdownInlineStyling.apply(to: source)
        XCTAssertEqual(String(styled.characters), String(source.characters))
        var codeTexts: [String] = []
        for run in styled.runs {
            if run.inlinePresentationIntent?.contains(.code) == true {
                codeTexts.append(String(styled[run.range].characters))
                XCTAssertEqual(run.font, MarkdownInlineStyling.codeFont)
                XCTAssertEqual(run.foregroundColor, MarkdownInlineStyling.codeForeground)
                XCTAssertEqual(run.backgroundColor, MarkdownInlineStyling.codeBackground)
            } else {
                XCTAssertNil(run.backgroundColor)
                XCTAssertNil(run.font)
            }
        }
        XCTAssertEqual(codeTexts, ["one", "two", "three"])
        XCTAssertTrue(styled.runs.contains { $0.link == URL(string: "https://example.com") })
        XCTAssertTrue(styled.runs.contains { $0.inlinePresentationIntent?.contains(.stronglyEmphasized) == true })
        XCTAssertTrue(source.runs.allSatisfy { $0.backgroundColor == nil })
    }

    func testAdjacentCodeRangesCanMergeWithoutSkippingLaterRanges() throws {
        var first = try AttributedString(markdown: "`one`")
        first.foregroundColor = .red
        var second = try AttributedString(markdown: "`two`")
        second.foregroundColor = .blue
        let source = first + second + AttributedString(" plain ") + (try AttributedString(markdown: "`three`"))
        XCTAssertEqual(String(source.characters), "onetwo plain three")
        let styled = MarkdownInlineStyling.apply(to: source)
        XCTAssertEqual(String(styled.characters), "onetwo plain three")
        let codeRuns = styled.runs.filter { $0.inlinePresentationIntent?.contains(.code) == true }
        XCTAssertEqual(codeRuns.map { String(styled[$0.range].characters) }.joined(), "onetwothree")
        for run in codeRuns {
            XCTAssertEqual(run.font, MarkdownInlineStyling.codeFont)
            XCTAssertEqual(run.foregroundColor, MarkdownInlineStyling.codeForeground)
            XCTAssertEqual(run.backgroundColor, MarkdownInlineStyling.codeBackground)
        }
    }

    func testCalloutMarkerCaseAndTrailingHorizontalWhitespace() {
        for marker in ["[!Note] ", "[!note]\t", " [!NOTE]  "] {
            let quote = MarkdownQuotePresentation(marker + "\r\n正文")
            XCTAssertEqual(quote.kind, .note)
            XCTAssertEqual(quote.text, "正文")
        }
        XCTAssertNil(MarkdownQuotePresentation("[!Note]  同行正文").kind)
    }

    func testFiveCalloutsRetainBodyFormatting() {
        for kind in MarkdownQuotePresentation.Kind.allCases {
            let quote = MarkdownQuotePresentation("[!\(kind.rawValue)]\n**重点** 与 `code`\n下一行")
            XCTAssertEqual(quote.kind, kind)
            XCTAssertEqual(quote.text, "**重点** 与 `code`\n下一行")
            XCTAssertFalse(kind.title.isEmpty)
        }
    }

    func testUnknownInlineAndOrdinaryQuotesRemainLiteral() {
        for text in ["[!UNKNOWN]\n正文", "文字 [!NOTE]", "`[!WARNING]`", "[!TIP] 不独占一行", "普通引用"] {
            let quote = MarkdownQuotePresentation(text)
            XCTAssertNil(quote.kind)
            XCTAssertEqual(quote.text, text)
        }
    }

    func testEmptyCalloutAndCRLFMarker() {
        XCTAssertEqual(MarkdownQuotePresentation("[!NOTE]").text, "")
        XCTAssertEqual(MarkdownQuotePresentation("[!NOTE]\r\n正文").kind, .note)
        XCTAssertEqual(MarkdownQuotePresentation("[!TIP]\r\n第一行\n第二行\r\n第三行").text,
                       "第一行\n第二行\n第三行")
    }

    func testTaskMarkersDoNotConsumeBodyMarkdown() {
        XCTAssertEqual(MarkdownTaskPresentation.parse("[ ] **待办**")?.checked, false)
        XCTAssertEqual(MarkdownTaskPresentation.parse("[x] `done`")?.text, "`done`")
        XCTAssertEqual(MarkdownTaskPresentation.parse("[X]\t完成")?.checked, true)
        XCTAssertEqual(MarkdownTaskPresentation.parse("[x]")?.text, "")
        XCTAssertNil(MarkdownTaskPresentation.parse("[x](https://example.com)"))
        XCTAssertNil(MarkdownTaskPresentation.parse("普通 [x] 文字"))
    }
}
