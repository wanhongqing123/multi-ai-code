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
    let avatar: Avatar
    let metadata: Metadata
    let content: Content

    init(
        isOutgoing: Bool,
        @ViewBuilder avatar: () -> Avatar,
        @ViewBuilder metadata: () -> Metadata,
        @ViewBuilder content: () -> Content
    ) {
        self.isOutgoing = isOutgoing
        self.avatar = avatar()
        self.metadata = metadata()
        self.content = content()
    }

    var body: some View {
        VStack(alignment: isOutgoing ? .trailing : .leading, spacing: 6) {
            HStack(spacing: 8) {
                if !isOutgoing { avatar }
                metadata
                    .layoutPriority(1)
                if isOutgoing { avatar }
            }
            .frame(maxWidth: .infinity, alignment: isOutgoing ? .trailing : .leading)

            content
        }
        .frame(maxWidth: .infinity, alignment: isOutgoing ? .trailing : .leading)
    }
}

enum MessageHistoryMetrics {
    static let eagerTailCount = 50
}

/// A bounded eager tail keeps the latest-page bottom anchor measurable.
/// Older history can be virtualized independently of that tail.
struct MessageHistoryStack<Item: Identifiable, Row: View>: View {
    let items: [Item]
    let row: (Item) -> Row

    init(items: [Item], @ViewBuilder row: @escaping (Item) -> Row) {
        self.items = items
        self.row = row
    }

    var body: some View {
        let eagerCount = min(MessageHistoryMetrics.eagerTailCount, items.count)
        VStack(alignment: .leading, spacing: 14) {
            if items.count > eagerCount {
                LazyVStack(alignment: .leading, spacing: 14) {
                    ForEach(items.dropLast(eagerCount)) { item in row(item) }
                }
            }
            VStack(alignment: .leading, spacing: 14) {
                ForEach(items.suffix(eagerCount)) { item in row(item) }
            }
        }
    }
}

/// Reads the scroll view's actual position without publishing per-pixel SwiftUI state.
@MainActor
struct MessageScrollPositionReader: UIViewRepresentable {
    let onNearBottomChanged: (Bool) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(onNearBottomChanged) }
    func makeUIView(context: Context) -> UIView {
        let view = UIView(frame: .zero)
        view.isUserInteractionEnabled = false
        install(view, context.coordinator)
        return view
    }
    func updateUIView(_ view: UIView, context: Context) {
        context.coordinator.onChanged = onNearBottomChanged
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
        var onChanged: (Bool) -> Void
        private weak var scroll: UIScrollView?
        private var observations: [NSKeyValueObservation] = []
        private var lastNearBottom: Bool?
        init(_ onChanged: @escaping (Bool) -> Void) { self.onChanged = onChanged }

        func install(from view: UIView) {
            var parent = view.superview
            while let candidate = parent, !(candidate is UIScrollView) { parent = candidate.superview }
            guard let next = parent as? UIScrollView, next !== scroll else { return }
            uninstall()
            scroll = next
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
            observations.forEach { $0.invalidate() }
            observations.removeAll()
            scroll = nil
            lastNearBottom = nil
        }

        private func updatePosition() {
            guard let scroll, scroll.bounds.height > 0 else { return }
            let remaining = scroll.contentSize.height + scroll.adjustedContentInset.bottom
                - scroll.contentOffset.y - scroll.bounds.height
            let nearBottom = remaining <= 60
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
