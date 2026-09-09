import SwiftUI

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
