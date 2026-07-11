import AVFoundation
import MultiAIIMCore
import Photos
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

enum RemoteIMStyle {
    static let pageBackground = Color(red: 0.966, green: 0.976, blue: 0.988)
    static let panelBackground = Color.white
    static let border = Color(red: 0.855, green: 0.894, blue: 0.941)
    static let textPrimary = Color(red: 0.055, green: 0.081, blue: 0.145)
    static let textSecondary = Color(red: 0.392, green: 0.459, blue: 0.561)
    static let blue = Color(red: 0.059, green: 0.553, blue: 0.867)
    static let blueSoft = Color(red: 0.882, green: 0.957, blue: 1.0)
    static let green = Color(red: 0.063, green: 0.596, blue: 0.325)
    static let greenSoft = Color(red: 0.848, green: 0.984, blue: 0.902)
    static let yellowBorder = Color(red: 0.992, green: 0.812, blue: 0.345)
    static let yellowSoft = Color(red: 1.0, green: 0.984, blue: 0.913)
}

struct ChatView: View {
    @Binding var activeContact: RemoteIMContact?

    var body: some View {
        Group {
            if let activeContact {
                ChatDetailView(contact: activeContact, activeContact: $activeContact)
            } else {
                VStack(spacing: 0) {
                    HeaderView()
                    ConversationListView(activeContact: $activeContact)
                }
            }
        }
        .background(RemoteIMStyle.pageBackground.ignoresSafeArea())
    }
}

struct RemoteIMContactAvatar: View {
    let isSelected: Bool
    let presenceStatus: RemoteIMPresenceStatus
    let size: CGFloat
    let iconSize: CGFloat

    var body: some View {
        ZStack(alignment: .bottomTrailing) {
            Image(systemName: "desktopcomputer")
                .font(.system(size: iconSize, weight: .semibold))
                .foregroundStyle(isSelected ? Color(red: 0.035, green: 0.376, blue: 0.667) : RemoteIMStyle.textSecondary)
                .frame(width: size, height: size)
                .background(
                    isSelected ? RemoteIMStyle.blueSoft : Color(red: 0.953, green: 0.961, blue: 0.973),
                    in: RoundedRectangle(cornerRadius: size >= 40 ? 10 : 8, style: .continuous)
                )
                .overlay(
                    RoundedRectangle(
                        cornerRadius: size >= 40 ? 10 : 8,
                        style: .continuous
                    )
                    .stroke(
                        presenceStatus.isOnline
                            ? Color(red: 0.118, green: 0.737, blue: 0.408)
                            : Color.clear,
                        lineWidth: presenceStatus.isOnline ? 2 : 0
                    )
                )

            if presenceStatus.isOnline {
                Circle()
                    .fill(Color(red: 0.118, green: 0.737, blue: 0.408))
                    .frame(width: 11, height: 11)
                    .overlay(Circle().stroke(Color.white, lineWidth: 2))
                    .offset(x: 2, y: 2)
            }
        }
    }
}

struct RemoteIMPresenceBadge: View {
    let status: RemoteIMPresenceStatus

    var body: some View {
        switch status {
        case .online:
            Text("在线")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(Color(red: 0.047, green: 0.518, blue: 0.29))
                .padding(.horizontal, 7)
                .padding(.vertical, 3)
                .background(RemoteIMStyle.greenSoft, in: Capsule())
        case .offline:
            Text("离线")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .padding(.horizontal, 7)
                .padding(.vertical, 3)
                .background(Color(red: 0.945, green: 0.953, blue: 0.965), in: Capsule())
        case .unknown:
            EmptyView()
        }
    }
}

private struct HeaderView: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                Text("远程 IM")
                    .font(.system(size: 21, weight: .bold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                Text(appState.masterUserID.isEmpty ? "未设置账号" : appState.masterUserID)
                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
                    .foregroundStyle(RemoteIMStyle.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }

            Spacer(minLength: 8)

            StatusPill(state: appState.connectionState)
        }
        .padding(.horizontal, 16)
        .padding(.top, 8)
        .padding(.bottom, 10)
        .background(RemoteIMStyle.panelBackground)
        .overlay(alignment: .bottom) {
            Divider().background(RemoteIMStyle.border)
        }
    }
}

private struct StatusPill: View {
    let state: RemoteIMAppState.ConnectionState

    var body: some View {
        HStack(spacing: 7) {
            Circle()
                .fill(dotColor)
                .frame(width: 8, height: 8)
            Text(state.rawValue)
                .font(.system(size: 13, weight: .semibold))
        }
        .foregroundStyle(textColor)
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
        .background(backgroundColor, in: Capsule())
    }

    private var dotColor: Color {
        switch state {
        case .connected:
            return RemoteIMStyle.green
        case .connecting:
            return .orange
        case .failed:
            return .red
        case .disconnected:
            return RemoteIMStyle.textSecondary
        }
    }

    private var textColor: Color {
        state == .connected ? RemoteIMStyle.green : RemoteIMStyle.textSecondary
    }

    private var backgroundColor: Color {
        state == .connected ? RemoteIMStyle.greenSoft : Color(.secondarySystemBackground)
    }
}

private struct ConversationListView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Binding var activeContact: RemoteIMContact?

    var body: some View {
        List {
            if appState.chatState.contacts.isEmpty {
                EmptyConversationListView()
                    .padding(.top, 96)
                    .listRowInsets(EdgeInsets())
                    .listRowSeparator(.hidden)
                    .listRowBackground(RemoteIMStyle.panelBackground)
            } else {
                ForEach(appState.chatState.contacts) { contact in
                    Button {
                        appState.selectContact(contact)
                        activeContact = contact
                    } label: {
                        ConversationRow(
                            contact: contact,
                            latestMessage: appState.chatState.latestMessage(with: contact.userID),
                            selected: contact.userID == appState.chatState.selectedPeerID,
                            presenceStatus: appState.presenceStatus(for: contact)
                        )
                    }
                    .buttonStyle(.plain)
                    .listRowInsets(EdgeInsets())
                    .listRowSeparator(.hidden)
                    .listRowBackground(RemoteIMStyle.panelBackground)
                    .swipeActions(edge: .trailing, allowsFullSwipe: true) {
                        Button(role: .destructive) {
                            appState.deleteContact(contact)
                            if activeContact?.userID == contact.userID {
                                activeContact = nil
                            }
                        } label: {
                            Label("删除", systemImage: "trash")
                        }
                    }
                }
            }
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
        .background(RemoteIMStyle.panelBackground)
    }
}

private struct ConversationRow: View {
    let contact: RemoteIMContact
    let latestMessage: RemoteIMMessage?
    let selected: Bool
    let presenceStatus: RemoteIMPresenceStatus

    var body: some View {
        HStack(spacing: 12) {
            RemoteIMContactAvatar(
                isSelected: selected,
                presenceStatus: presenceStatus,
                size: 42,
                iconSize: 18
            )

            VStack(alignment: .leading, spacing: 5) {
                Text(contact.displayName)
                    .font(.system(size: 16, weight: .bold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .lineLimit(1)
                    .truncationMode(.middle)

                if let latestMessage {
                    Text(latestMessage.text)
                        .font(.system(size: 13))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                        .lineLimit(1)
                        .truncationMode(.tail)
                }
            }

            Spacer(minLength: 8)

            VStack(alignment: .trailing, spacing: 7) {
                if let latestMessage {
                    Text(RemoteIMTimestampTextPolicy.displayText(for: latestMessage.createdAt))
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                }
                RemoteIMPresenceBadge(status: presenceStatus)
                RelationBadge(text: contact.relation.displayName)
            }
        }
        .padding(.horizontal, 16)
        .frame(height: 72)
        .contentShape(Rectangle())
    }

}

private struct EmptyConversationListView: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "bubble.left")
                .font(.system(size: 28))
                .foregroundStyle(Color(red: 0.56, green: 0.59, blue: 0.64))
            Text("暂无会话")
                .font(.system(size: 16, weight: .bold))
                .foregroundStyle(RemoteIMStyle.textPrimary)
            Text("到通讯录添加好友账号后即可开始聊天。")
                .font(.system(size: 13))
                .foregroundStyle(RemoteIMStyle.textSecondary)
        }
        .frame(maxWidth: .infinity)
    }
}

private final class VoiceMessagePlayer: NSObject, ObservableObject, AVAudioPlayerDelegate {
    @Published var playingMessageID: UUID?

    private var audioPlayer: AVAudioPlayer?

    func toggle(message: RemoteIMMessage) {
        guard let attachment = message.voiceAttachment else { return }
        if playingMessageID == message.id {
            stop()
            return
        }

        do {
            try AVAudioSession.sharedInstance().setCategory(.playback, mode: .spokenAudio)
            try AVAudioSession.sharedInstance().setActive(true)
            let nextPlayer = try AVAudioPlayer(contentsOf: URL(fileURLWithPath: attachment.localFilePath))
            nextPlayer.delegate = self
            nextPlayer.prepareToPlay()
            audioPlayer = nextPlayer
            playingMessageID = message.id
            nextPlayer.play()
        } catch {
            stop()
        }
    }

    func stop() {
        audioPlayer?.stop()
        audioPlayer = nil
        playingMessageID = nil
    }

    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        stop()
    }
}

private struct ChatDetailView: View {
    let contact: RemoteIMContact
    @Binding var activeContact: RemoteIMContact?
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        VStack(spacing: 0) {
            ChatDetailHeader(contact: contact, activeContact: $activeContact)
            MessageListView(
                messages: appState.chatState.messages(with: contact.userID),
                peerRelation: contact.relation
            )
            ComposerView()
        }
        .background(RemoteIMStyle.pageBackground.ignoresSafeArea())
        .toolbar(.hidden, for: .navigationBar)
        .simultaneousGesture(edgeSwipeBackGesture)
        .onAppear {
            appState.selectContact(contact)
        }
    }

    private var edgeSwipeBackGesture: some Gesture {
        DragGesture(minimumDistance: 20, coordinateSpace: .local)
            .onEnded { value in
                guard ChatDetailSwipeBackPolicy.shouldReturnToConversationList(
                    startX: Double(value.startLocation.x),
                    translationWidth: Double(value.translation.width),
                    translationHeight: Double(value.translation.height)
                ) else { return }

                dismissKeyboard()
                withAnimation(.easeOut(duration: 0.18)) {
                    activeContact = nil
                }
            }
    }
}

private struct ChatDetailHeader: View {
    let contact: RemoteIMContact
    @Binding var activeContact: RemoteIMContact?
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        HStack(spacing: 10) {
            Button {
                activeContact = nil
            } label: {
                Image(systemName: "chevron.left")
                    .font(.system(size: 18, weight: .semibold))
                    .frame(width: 34, height: 34)
            }
            .buttonStyle(.plain)
            .foregroundStyle(RemoteIMStyle.textPrimary)

            VStack(alignment: .leading, spacing: 2) {
                Text(contact.displayName)
                    .font(.system(size: 18, weight: .bold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Text(contact.userID)
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .foregroundStyle(RemoteIMStyle.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }

            Spacer(minLength: 8)

            StatusPill(state: appState.connectionState)
        }
        .padding(.horizontal, 10)
        .padding(.top, 8)
        .padding(.bottom, 10)
        .background(RemoteIMStyle.panelBackground)
        .overlay(alignment: .bottom) {
            Divider().background(RemoteIMStyle.border)
        }
    }
}

struct RelationBadge: View {
    let text: String

    var body: some View {
        Text(text)
            .font(.system(size: 11, weight: .bold))
            .foregroundStyle(Color(red: 0.706, green: 0.324, blue: 0.035))
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(Color(red: 1.0, green: 0.963, blue: 0.862), in: Capsule())
    }
}

private struct MessageListView: View {
    let messages: [RemoteIMMessage]
    let peerRelation: RemoteIMContactRelation
    @StateObject private var voicePlayer = VoiceMessagePlayer()
    @State private var imagePreviewItem: RemoteIMImagePreviewItem?

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 14) {
                    if messages.isEmpty {
                        EmptyMessagesView()
                            .padding(.top, 72)
                    } else {
                        ForEach(messages) { message in
                            MessageBubbleView(
                                message: message,
                                incomingRelation: peerRelation,
                                isVoicePlaying: voicePlayer.playingMessageID == message.id,
                                playVoice: {
                                    voicePlayer.toggle(message: message)
                                },
                                previewImage: {
                                    imagePreviewItem = RemoteIMImagePreviewPolicy.previewItem(for: message)
                                }
                            )
                                .id(message.id)
                        }
                    }
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 18)
            }
            .scrollDismissesKeyboard(.interactively)
            .background(RemoteIMStyle.panelBackground)
            .simultaneousGesture(
                TapGesture().onEnded {
                    dismissKeyboard()
                }
            )
            .onAppear {
                scrollToLatestMessage(proxy: proxy)
            }
            .onChange(of: messages.count) { _ in
                scrollToLatestMessage(proxy: proxy)
            }
        }
        .fullScreenCover(item: $imagePreviewItem) { item in
            FullScreenImagePreviewView(item: item) {
                imagePreviewItem = nil
            }
        }
    }

    private func scrollToLatestMessage(proxy: ScrollViewProxy) {
        guard let latestMessageID = MessageListAutoScrollPolicy.latestMessageID(from: messages) else {
            return
        }
        DispatchQueue.main.async {
            proxy.scrollTo(latestMessageID, anchor: .bottom)
        }
    }
}

@MainActor
private func dismissKeyboard() {
    UIApplication.shared.sendAction(
        #selector(UIResponder.resignFirstResponder),
        to: nil,
        from: nil,
        for: nil
    )
}

private struct EmptyMessagesView: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "message")
                .font(.system(size: 28))
                .foregroundStyle(Color(red: 0.56, green: 0.59, blue: 0.64))
            Text("暂无消息")
                .font(.system(size: 16, weight: .bold))
                .foregroundStyle(RemoteIMStyle.textPrimary)
            Text("发送一条消息开始远程任务。")
                .font(.system(size: 13))
                .foregroundStyle(RemoteIMStyle.textSecondary)
        }
        .frame(maxWidth: .infinity)
    }
}

private struct MessageBubbleView: View {
    let message: RemoteIMMessage
    let incomingRelation: RemoteIMContactRelation
    let isVoicePlaying: Bool
    let playVoice: () -> Void
    let previewImage: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 0) {
            if message.direction == .outgoing {
                Spacer(minLength: 42)
            }

            VStack(alignment: .leading, spacing: 7) {
                HStack(spacing: 8) {
                    Text(displayUserID)
                        .font(.system(size: 13, weight: .bold, design: .monospaced))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    if let relationText {
                        RelationBadge(text: relationText)
                    }
                    Text(RemoteIMTimestampTextPolicy.displayText(for: message.createdAt))
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                    Spacer(minLength: 0)
                }

                HStack(alignment: .bottom, spacing: 10) {
                    if let imageAttachment = message.imageAttachment {
                        Button(action: previewImage) {
                            ImageBubbleContent(attachment: imageAttachment)
                        }
                        .buttonStyle(.plain)
                    } else if let voiceAttachment = message.voiceAttachment {
                        Button(action: playVoice) {
                            VoiceBubbleContent(
                                attachment: voiceAttachment,
                                isPlaying: isVoicePlaying
                            )
                        }
                        .buttonStyle(.plain)
                    } else {
                        MarkdownLikeText(message.text)
                            .font(.system(size: 13, weight: .regular))
                            .lineSpacing(3)
                            .foregroundStyle(RemoteIMStyle.textPrimary)
                    }

                    if message.direction == .outgoing {
                        StatusIcon(status: message.status)
                    }
                }
            }
            .padding(.horizontal, 13)
            .padding(.vertical, 11)
            .frame(maxWidth: 620, alignment: .leading)
            .background(bubbleBackground, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .stroke(bubbleBorder, lineWidth: 1)
            )

            if message.direction == .incoming {
                Spacer(minLength: 42)
            }
        }
        .frame(maxWidth: .infinity, alignment: message.direction == .outgoing ? .trailing : .leading)
    }

    private var displayUserID: String {
        message.direction == .outgoing ? message.fromUserID : message.fromUserID
    }

    private var relationText: String? {
        message.direction == .outgoing ? nil : incomingRelation.displayName
    }

    private var bubbleBackground: Color {
        message.direction == .outgoing ? Color.white : RemoteIMStyle.yellowSoft
    }

    private var bubbleBorder: Color {
        message.direction == .outgoing ? Color(red: 0.764, green: 0.873, blue: 0.996) : RemoteIMStyle.yellowBorder
    }
}

private struct FullScreenImagePreviewView: View {
    let item: RemoteIMImagePreviewItem
    let close: () -> Void

    var body: some View {
        ZStack(alignment: .topTrailing) {
            Color.black.ignoresSafeArea()

            if let image = UIImage(contentsOfFile: item.localFilePath) {
                ZoomableImagePreview(image: image)
                    .ignoresSafeArea()
                    .accessibilityLabel("图片预览")
            } else {
                VStack(spacing: 12) {
                    Image(systemName: "photo")
                        .font(.system(size: 34, weight: .semibold))
                    Text("图片暂不可预览")
                        .font(.system(size: 15, weight: .semibold))
                    Text(URL(fileURLWithPath: item.localFilePath).lastPathComponent)
                        .font(.system(size: 12, weight: .medium))
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                .foregroundStyle(.white.opacity(0.86))
                .padding(.horizontal, 28)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }

            Button(action: close) {
                Image(systemName: "xmark")
                    .font(.system(size: 16, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 44, height: 44)
                    .background(.black.opacity(0.45), in: Circle())
            }
            .buttonStyle(.plain)
            .padding(.top, 18)
            .padding(.trailing, 18)
            .accessibilityLabel("关闭图片预览")
        }
        .statusBarHidden(true)
    }
}

private struct ZoomableImagePreview: UIViewRepresentable {
    let image: UIImage

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    func makeUIView(context: Context) -> UIScrollView {
        let scrollView = UIScrollView()
        scrollView.backgroundColor = .black
        scrollView.delegate = context.coordinator
        scrollView.minimumZoomScale = 1
        scrollView.maximumZoomScale = 4
        scrollView.bouncesZoom = true
        scrollView.showsHorizontalScrollIndicator = false
        scrollView.showsVerticalScrollIndicator = false

        let imageView = UIImageView(image: image)
        imageView.contentMode = .scaleAspectFit
        imageView.isUserInteractionEnabled = true
        imageView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.addSubview(imageView)

        NSLayoutConstraint.activate([
            imageView.leadingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.leadingAnchor),
            imageView.trailingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.trailingAnchor),
            imageView.topAnchor.constraint(equalTo: scrollView.contentLayoutGuide.topAnchor),
            imageView.bottomAnchor.constraint(equalTo: scrollView.contentLayoutGuide.bottomAnchor),
            imageView.widthAnchor.constraint(equalTo: scrollView.frameLayoutGuide.widthAnchor),
            imageView.heightAnchor.constraint(equalTo: scrollView.frameLayoutGuide.heightAnchor)
        ])

        let doubleTap = UITapGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.handleDoubleTap(_:)))
        doubleTap.numberOfTapsRequired = 2
        scrollView.addGestureRecognizer(doubleTap)

        context.coordinator.scrollView = scrollView
        context.coordinator.imageView = imageView
        return scrollView
    }

    func updateUIView(_ scrollView: UIScrollView, context: Context) {
        context.coordinator.imageView?.image = image
        if scrollView.zoomScale < scrollView.minimumZoomScale {
            scrollView.setZoomScale(scrollView.minimumZoomScale, animated: false)
        }
    }

    final class Coordinator: NSObject, UIScrollViewDelegate {
        weak var scrollView: UIScrollView?
        weak var imageView: UIImageView?

        func viewForZooming(in scrollView: UIScrollView) -> UIView? {
            imageView
        }

        @objc func handleDoubleTap(_ recognizer: UITapGestureRecognizer) {
            guard let scrollView, let imageView else { return }

            if scrollView.zoomScale > scrollView.minimumZoomScale {
                scrollView.setZoomScale(scrollView.minimumZoomScale, animated: true)
                return
            }

            let tapPoint = recognizer.location(in: imageView)
            let targetScale = min(scrollView.maximumZoomScale, scrollView.minimumZoomScale * 2)
            let zoomRect = CGRect(
                x: tapPoint.x - scrollView.bounds.width / targetScale / 2,
                y: tapPoint.y - scrollView.bounds.height / targetScale / 2,
                width: scrollView.bounds.width / targetScale,
                height: scrollView.bounds.height / targetScale
            )
            scrollView.zoom(to: zoomRect, animated: true)
        }
    }
}

private struct ImageBubbleContent: View {
    let attachment: RemoteIMImageAttachment

    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            if let image = UIImage(contentsOfFile: attachment.localFilePath) {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFit()
                    .frame(maxWidth: 220, maxHeight: 180)
                    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                    .background(Color(red: 0.945, green: 0.957, blue: 0.973), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
            } else {
                HStack(spacing: 8) {
                    Image(systemName: "photo")
                    Text("图片暂不可预览")
                        .font(.system(size: 13, weight: .semibold))
                }
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .frame(width: 180, height: 120)
                .background(Color(red: 0.945, green: 0.957, blue: 0.973), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
            }
            Text(URL(fileURLWithPath: attachment.localFilePath).lastPathComponent)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .lineLimit(1)
                .truncationMode(.middle)
        }
    }
}

private struct VoiceBubbleContent: View {
    let attachment: RemoteIMVoiceAttachment
    let isPlaying: Bool

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                .font(.system(size: 13, weight: .bold))
                .frame(width: 18)
            HStack(spacing: 3) {
                ForEach(0..<7, id: \.self) { index in
                    Capsule()
                        .fill(RemoteIMStyle.textPrimary.opacity(isPlaying ? 0.85 : 0.55))
                        .frame(width: 3, height: CGFloat([8, 14, 10, 18, 12, 15, 9][index]))
                }
            }
            Text("\(attachment.durationSeconds)s")
                .font(.system(size: 13, weight: .semibold))
        }
        .foregroundStyle(RemoteIMStyle.textPrimary)
        .frame(minWidth: 116, alignment: .leading)
        .contentShape(Rectangle())
    }
}

private struct StatusIcon: View {
    let status: RemoteIMMessageStatus

    var body: some View {
        Image(systemName: systemName)
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(color)
            .accessibilityLabel(accessibilityText)
    }

    private var systemName: String {
        switch status {
        case .pending:
            return "clock.fill"
        case .sent, .received:
            return "checkmark.circle.fill"
        case .failed:
            return "exclamationmark.circle.fill"
        }
    }

    private var color: Color {
        switch status {
        case .pending:
            return .orange
        case .sent, .received:
            return RemoteIMStyle.green
        case .failed:
            return .red
        }
    }

    private var accessibilityText: String {
        switch status {
        case .pending:
            return "发送中"
        case .sent:
            return "已发送"
        case .received:
            return "已收到"
        case .failed:
            return "发送失败"
        }
    }
}

private struct MarkdownLikeText: View {
    private let blocks: [MarkdownBlock]

    init(_ text: String) {
        self.blocks = parseMarkdownBlocks(text)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(blocks) { block in
                switch block.kind {
                case .markdown(let text):
                    MarkdownInlineText(text: text)
                case .heading(let level, let text):
                    MarkdownHeadingView(level: level, text: text)
                case .unorderedList(let list):
                    MarkdownListView(list: list)
                case .code(let code):
                    MarkdownCodeBlock(code: code)
                case .table(let table):
                    MarkdownTableView(table: table)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .textSelection(.enabled)
    }
}

private struct MarkdownBlock: Identifiable {
    let id = UUID()
    let kind: MarkdownBlockKind
}

private enum MarkdownBlockKind {
    case markdown(String)
    case heading(level: Int, text: String)
    case unorderedList(MarkdownList)
    case code(String)
    case table(MarkdownTable)
}

private struct MarkdownList {
    let items: [MarkdownListItem]
}

private struct MarkdownListItem: Identifiable {
    let id = UUID()
    var text: String
}

private struct MarkdownTable {
    let headers: [String]
    let rows: [[String]]
}

private struct MarkdownInlineText: View {
    let text: String

    var body: some View {
        if let attributed = try? AttributedString(markdown: text) {
            Text(attributed)
        } else {
            Text(text)
        }
    }
}

private struct MarkdownHeadingView: View {
    let level: Int
    let text: String

    var body: some View {
        MarkdownInlineText(text: text)
            .font(.system(size: fontSize, weight: .bold))
            .foregroundStyle(RemoteIMStyle.textPrimary)
            .padding(.top, level <= 2 ? 2 : 0)
    }

    private var fontSize: CGFloat {
        switch level {
        case 1:
            return 17
        case 2:
            return 15
        default:
            return 14
        }
    }
}

private struct MarkdownListView: View {
    let list: MarkdownList

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(list.items) { item in
                HStack(alignment: .top, spacing: 7) {
                    Text("•")
                        .font(.system(size: 13, weight: .bold))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                        .frame(width: 10, alignment: .center)
                        .padding(.top, 1)
                    MarkdownInlineText(text: item.text)
                        .font(.system(size: 13, weight: .regular))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                        .lineSpacing(3)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
    }
}

private struct MarkdownCodeBlock: View {
    let code: String

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            Text(code.isEmpty ? " " : code)
                .font(.system(size: 12, weight: .regular, design: .monospaced))
                .foregroundStyle(RemoteIMStyle.textPrimary)
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .background(Color(red: 0.945, green: 0.957, blue: 0.973), in: RoundedRectangle(cornerRadius: 7, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .stroke(RemoteIMStyle.border, lineWidth: 1)
        )
    }
}

private struct MarkdownTableView: View {
    let table: MarkdownTable

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            Grid(alignment: .leading, horizontalSpacing: 0, verticalSpacing: 0) {
                GridRow {
                    ForEach(0..<columnCount, id: \.self) { column in
                        tableCell(table.headers[safe: column] ?? "", isHeader: true)
                    }
                }
                ForEach(Array(table.rows.enumerated()), id: \.offset) { _, row in
                    GridRow {
                        ForEach(0..<columnCount, id: \.self) { column in
                            tableCell(row[safe: column] ?? "", isHeader: false)
                        }
                    }
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 7, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 7, style: .continuous)
                    .stroke(RemoteIMStyle.border, lineWidth: 1)
            )
        }
    }

    private var columnCount: Int {
        max(table.headers.count, table.rows.map(\.count).max() ?? 0)
    }

    private func tableCell(_ text: String, isHeader: Bool) -> some View {
        MarkdownInlineText(text: text)
            .font(.system(size: 12, weight: isHeader ? .semibold : .regular))
            .foregroundStyle(RemoteIMStyle.textPrimary)
            .lineLimit(nil)
            .padding(.horizontal, 9)
            .padding(.vertical, 7)
            .frame(minWidth: 78, maxWidth: 180, alignment: .leading)
            .background(isHeader ? Color(red: 0.93, green: 0.945, blue: 0.965) : Color.white.opacity(0.72))
            .overlay(
                Rectangle()
                    .stroke(RemoteIMStyle.border, lineWidth: 0.5)
            )
    }
}

private func parseMarkdownBlocks(_ source: String) -> [MarkdownBlock] {
    let displayText = cleanRemoteIMMessageDisplayText(source)
    let lines = displayText
        .replacingOccurrences(of: "\r\n", with: "\n")
        .replacingOccurrences(of: "\r", with: "\n")
        .components(separatedBy: "\n")
    var blocks: [MarkdownBlock] = []
    var markdownLines: [String] = []
    var index = 0

    func flushMarkdown() {
        let text = markdownLines
            .joined(separator: "\n")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        markdownLines.removeAll()
        if !text.isEmpty {
            blocks.append(MarkdownBlock(kind: .markdown(text)))
        }
    }

    while index < lines.count {
        let line = lines[index]
        let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)

        if trimmed.hasPrefix("```") {
            flushMarkdown()
            index += 1
            var codeLines: [String] = []
            while index < lines.count {
                let codeLine = lines[index]
                if codeLine.trimmingCharacters(in: .whitespacesAndNewlines).hasPrefix("```") {
                    index += 1
                    break
                }
                codeLines.append(codeLine)
                index += 1
            }
            blocks.append(MarkdownBlock(kind: .code(codeLines.joined(separator: "\n"))))
            continue
        }

        if let heading = parseMarkdownHeading(line) {
            flushMarkdown()
            blocks.append(MarkdownBlock(kind: .heading(level: heading.level, text: heading.text)))
            index += 1
            continue
        }

        if let parsed = parseMarkdownTable(lines: lines, startIndex: index) {
            flushMarkdown()
            blocks.append(MarkdownBlock(kind: .table(parsed.table)))
            index = parsed.endIndex
            continue
        }

        if let parsed = parseMarkdownList(lines: lines, startIndex: index) {
            flushMarkdown()
            blocks.append(MarkdownBlock(kind: .unorderedList(parsed.list)))
            index = parsed.endIndex
            continue
        }

        markdownLines.append(line)
        index += 1
    }

    flushMarkdown()
    return blocks.isEmpty ? [MarkdownBlock(kind: .markdown(displayText))] : blocks
}

private func cleanRemoteIMMessageDisplayText(_ source: String) -> String {
    var text = source.trimmingCharacters(in: .whitespacesAndNewlines)
    for prefix in ["【AICLI 输出】", "[AICLI 输出]", "【AICLI输出】", "[AICLI输出]"] {
        if text.hasPrefix(prefix) {
            text.removeFirst(prefix.count)
            return text.trimmingCharacters(in: .whitespacesAndNewlines)
        }
    }
    return text
}

private func parseMarkdownHeading(_ line: String) -> (level: Int, text: String)? {
    let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
    var level = 0
    for character in trimmed {
        if character == "#" {
            level += 1
        } else {
            break
        }
    }
    guard level > 0, level <= 6 else { return nil }
    let markerEnd = trimmed.index(trimmed.startIndex, offsetBy: level)
    guard markerEnd < trimmed.endIndex, trimmed[markerEnd] == " " else { return nil }
    let textStart = trimmed.index(after: markerEnd)
    let headingText = String(trimmed[textStart...]).trimmingCharacters(in: .whitespacesAndNewlines)
    guard !headingText.isEmpty else { return nil }
    return (level, headingText)
}

private func parseMarkdownList(lines: [String], startIndex: Int) -> (list: MarkdownList, endIndex: Int)? {
    guard parseMarkdownListItem(lines[startIndex]) != nil else { return nil }
    var items: [MarkdownListItem] = []
    var index = startIndex

    while index < lines.count {
        let line = lines[index]
        let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { break }

        if let itemText = parseMarkdownListItem(line) {
            items.append(MarkdownListItem(text: itemText))
            index += 1
            continue
        }

        if line.first?.isWhitespace == true, !items.isEmpty {
            items[items.count - 1].text += "\n" + trimmed
            index += 1
            continue
        }

        break
    }

    guard !items.isEmpty else { return nil }
    return (MarkdownList(items: items), index)
}

private func parseMarkdownListItem(_ line: String) -> String? {
    let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
    for marker in ["- ", "* ", "+ "] {
        if trimmed.hasPrefix(marker) {
            return String(trimmed.dropFirst(marker.count)).trimmingCharacters(in: .whitespacesAndNewlines)
        }
    }
    return nil
}

private func parseMarkdownTable(lines: [String], startIndex: Int) -> (table: MarkdownTable, endIndex: Int)? {
    guard startIndex + 1 < lines.count else { return nil }
    let headerLine = lines[startIndex]
    let separatorLine = lines[startIndex + 1]
    guard headerLine.contains("|"), isMarkdownTableSeparator(separatorLine) else { return nil }

    let headers = splitMarkdownTableRow(headerLine)
    guard !headers.isEmpty else { return nil }

    var rows: [[String]] = []
    var index = startIndex + 2
    while index < lines.count {
        let line = lines[index]
        let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, line.contains("|") else { break }
        let cells = splitMarkdownTableRow(line)
        guard !cells.isEmpty else { break }
        rows.append(cells)
        index += 1
    }

    return (MarkdownTable(headers: headers, rows: rows), index)
}

private func splitMarkdownTableRow(_ line: String) -> [String] {
    var trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
    if trimmed.hasPrefix("|") {
        trimmed.removeFirst()
    }
    if trimmed.hasSuffix("|") {
        trimmed.removeLast()
    }
    return trimmed
        .split(separator: "|", omittingEmptySubsequences: false)
        .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
}

private func isMarkdownTableSeparator(_ line: String) -> Bool {
    let cells = splitMarkdownTableRow(line)
    guard !cells.isEmpty else { return false }
    return cells.allSatisfy { cell in
        let compact = cell.replacingOccurrences(of: " ", with: "")
        let hyphenCount = compact.filter { $0 == "-" }.count
        return hyphenCount >= 3 && compact.allSatisfy { character in
            character == "-" || character == ":"
        }
    }
}

private extension Array {
    subscript(safe index: Int) -> Element? {
        indices.contains(index) ? self[index] : nil
    }
}

@MainActor
private final class VoiceMessageRecorder: NSObject, ObservableObject {
    @Published var isRecording = false

    private var recorder: AVAudioRecorder?
    private var startedAt: Date?
    private var recordingURL: URL?

    func start() async throws {
        guard !isRecording else { return }
        let granted = await requestRecordPermission()
        guard granted else {
            throw VoiceRecorderError.microphonePermissionDenied
        }

        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playAndRecord, mode: .spokenAudio, options: [.defaultToSpeaker])
        try session.setActive(true)

        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("remote-im-voice-\(UUID().uuidString)")
            .appendingPathExtension("m4a")
        let settings: [String: Any] = [
            AVFormatIDKey: Int(kAudioFormatMPEG4AAC),
            AVSampleRateKey: 16_000,
            AVNumberOfChannelsKey: 1,
            AVEncoderAudioQualityKey: AVAudioQuality.medium.rawValue
        ]
        let nextRecorder = try AVAudioRecorder(url: url, settings: settings)
        nextRecorder.prepareToRecord()
        guard nextRecorder.record() else {
            throw VoiceRecorderError.startFailed
        }

        recorder = nextRecorder
        recordingURL = url
        startedAt = Date()
        isRecording = true
    }

    func stop() -> RemoteIMVoiceRecording? {
        guard isRecording, let recorder, let recordingURL else { return nil }
        recorder.stop()
        self.recorder = nil
        self.recordingURL = nil
        isRecording = false
        let duration = max(1, Int(ceil(Date().timeIntervalSince(startedAt ?? Date()))))
        startedAt = nil
        return RemoteIMVoiceRecording(fileURL: recordingURL, durationSeconds: duration)
    }

    func cancel() {
        let url = recordingURL
        recorder?.stop()
        recorder = nil
        recordingURL = nil
        startedAt = nil
        isRecording = false
        if let url {
            try? FileManager.default.removeItem(at: url)
        }
    }

    private func requestRecordPermission() async -> Bool {
        await withCheckedContinuation { continuation in
            AVAudioSession.sharedInstance().requestRecordPermission { granted in
                continuation.resume(returning: granted)
            }
        }
    }
}

private enum VoiceRecorderError: LocalizedError {
    case microphonePermissionDenied
    case startFailed

    var errorDescription: String? {
        switch self {
        case .microphonePermissionDenied:
            return "没有麦克风权限"
        case .startFailed:
            return "录音启动失败"
        }
    }
}

private struct RemoteIMSlashCommand: Identifiable {
    let command: String
    let label: String

    var id: String { command }
}

private let remoteIMSlashCommands: [RemoteIMSlashCommand] = [
    .init(command: "/status", label: "查看状态"),
    .init(command: "/plan", label: "切换 Plan"),
    .init(command: "/build", label: "切换 Build"),
    .init(command: "/help", label: "命令帮助")
]

private struct RemoteIMSlashCommandBar: View {
    let commands: [RemoteIMSlashCommand]
    let onSelect: (RemoteIMSlashCommand) -> Void

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(commands) { command in
                    Button {
                        onSelect(command)
                    } label: {
                        HStack(spacing: 6) {
                            Text(command.command)
                                .font(.system(size: 12, weight: .semibold, design: .monospaced))
                                .foregroundStyle(RemoteIMStyle.blue)
                            Text(command.label)
                                .font(.system(size: 12, weight: .medium))
                                .foregroundStyle(RemoteIMStyle.textSecondary)
                        }
                        .padding(.horizontal, 11)
                        .frame(height: 32)
                        .background(RemoteIMStyle.blueSoft, in: Capsule())
                        .overlay(
                            Capsule()
                                .stroke(Color(red: 0.745, green: 0.87, blue: 1.0), lineWidth: 1)
                        )
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, 2)
        }
    }
}

private struct ComposerView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @StateObject private var voiceRecorder = VoiceMessageRecorder()
    @State private var isVoiceMode = false
    @State private var isPressingVoice = false
    @State private var isCancellingVoice = false
    @State private var isPhotoPickerPresented = false
    @State private var selectedPhotoItem: PhotosPickerItem?

    var body: some View {
        VStack(spacing: 8) {
            if voiceRecorder.isRecording {
                VoiceRecordingHint(isCancelling: isCancellingVoice)
            }

            if !isVoiceMode && !commandSuggestions.isEmpty {
                RemoteIMSlashCommandBar(commands: commandSuggestions) { command in
                    appState.draftText = command.command
                }
            }

            HStack(alignment: .bottom, spacing: 8) {
                Button {
                    isVoiceMode.toggle()
                    if !isVoiceMode {
                        voiceRecorder.cancel()
                    }
                } label: {
                    Image(systemName: isVoiceMode ? "keyboard" : "speaker.wave.2.fill")
                        .font(.system(size: 18, weight: .bold))
                        .frame(width: 44, height: 44)
                        .background(RemoteIMStyle.blueSoft, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                        .overlay(
                            RoundedRectangle(cornerRadius: 14, style: .continuous)
                                .stroke(RemoteIMStyle.border, lineWidth: 1)
                        )
                }
                .buttonStyle(.plain)
                .foregroundStyle(RemoteIMStyle.blue)

                if isVoiceMode {
                    PressToTalkButton(
                        isPressing: isPressingVoice,
                        isCancelling: isCancellingVoice,
                        isEnabled: appState.canSendVoice,
                        onChanged: { translation in
                            handleVoicePressChanged(translation: translation)
                        },
                        onEnded: { translation in
                            Task { await handleVoicePressEnded(translation: translation) }
                        }
                    )
                } else {
                    TextField("输入要发送给当前联系人的消息...", text: $appState.draftText, axis: .vertical)
                        .font(.system(size: 14))
                        .lineLimit(1...5)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .submitLabel(.send)
                        .onSubmit {
                            submitDraft()
                        }
                        .onChange(of: appState.draftText) { newValue in
                            submitWhenDraftEndsWithNewline(newValue)
                        }
                        .padding(.horizontal, 13)
                        .padding(.vertical, 11)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color.white, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                        .overlay(
                            RoundedRectangle(cornerRadius: 14, style: .continuous)
                                .stroke(appState.canSend ? RemoteIMStyle.blue : RemoteIMStyle.border, lineWidth: appState.canSend ? 1.5 : 1)
                        )
                }

                Button {
                    Task { await openPhotoPicker() }
                } label: {
                    Image(systemName: "plus")
                        .font(.system(size: 20, weight: .semibold))
                        .frame(width: 44, height: 44)
                        .background(Color.white, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                        .overlay(
                            RoundedRectangle(cornerRadius: 14, style: .continuous)
                                .stroke(RemoteIMStyle.border, lineWidth: 1)
                        )
                }
                .buttonStyle(.plain)
                .foregroundStyle(appState.canSendImage ? RemoteIMStyle.textPrimary : RemoteIMStyle.textSecondary)
                .disabled(!appState.canSendImage)
                .accessibilityLabel("选择图片")
            }
        }
        .padding(.horizontal, 16)
        .padding(.top, 12)
        .padding(.bottom, 10)
        .background(RemoteIMStyle.panelBackground)
        .overlay(alignment: .top) {
            Divider().background(RemoteIMStyle.border)
        }
        .photosPicker(
            isPresented: $isPhotoPickerPresented,
            selection: $selectedPhotoItem,
            matching: .images,
            photoLibrary: .shared()
        )
        .onChange(of: selectedPhotoItem) { item in
            guard let item else { return }
            Task { await sendSelectedPhoto(item) }
        }
    }

    private var commandSuggestions: [RemoteIMSlashCommand] {
        let query = appState.draftText.trimmingCharacters(in: .whitespaces)
        guard query.hasPrefix("/") else { return [] }
        return remoteIMSlashCommands.filter { $0.command.hasPrefix(query) }
    }

    private func openPhotoPicker() async {
        guard appState.canSendImage else { return }
        guard await requestPhotoLibraryPermission() else {
            appState.errorMessage = "没有相册权限，请在系统设置中允许访问照片"
            return
        }
        isPhotoPickerPresented = true
    }

    private func requestPhotoLibraryPermission() async -> Bool {
        switch PHPhotoLibrary.authorizationStatus(for: .readWrite) {
        case .authorized, .limited:
            return true
        case .notDetermined:
            let status = await withCheckedContinuation { continuation in
                PHPhotoLibrary.requestAuthorization(for: .readWrite) { status in
                    continuation.resume(returning: status)
                }
            }
            return status == .authorized || status == .limited
        case .denied, .restricted:
            return false
        @unknown default:
            return false
        }
    }

    private func submitDraft() {
        guard appState.canSend else { return }
        Task { await appState.sendDraft() }
    }

    private func submitWhenDraftEndsWithNewline(_ draft: String) {
        guard let draftWithoutReturn = RemoteIMDraftSubmitPolicy
            .textByConsumingTrailingReturn(from: draft)
        else { return }

        appState.draftText = draftWithoutReturn
        submitDraft()
    }

    private func handleVoicePressChanged(translation: CGSize) {
        guard appState.canSendVoice else { return }
        if !isPressingVoice {
            isPressingVoice = true
            Task { await startVoiceRecording() }
        }
        isCancellingVoice = translation.height < -70
    }

    private func handleVoicePressEnded(translation: CGSize) async {
        guard isPressingVoice else { return }
        let shouldCancel = translation.height < -70
        isPressingVoice = false
        isCancellingVoice = false
        if shouldCancel {
            voiceRecorder.cancel()
            return
        }

        guard let recording = voiceRecorder.stop() else { return }
        await appState.sendVoiceRecording(recording)
    }

    private func startVoiceRecording() async {
        do {
            try await voiceRecorder.start()
        } catch {
            isPressingVoice = false
            isCancellingVoice = false
            appState.errorMessage = error.localizedDescription
        }
    }

    private func sendSelectedPhoto(_ item: PhotosPickerItem) async {
        defer { selectedPhotoItem = nil }
        do {
            guard let data = try await item.loadTransferable(type: Data.self) else {
                appState.errorMessage = "图片读取失败"
                return
            }
            let imageFile = try savePickedImage(data: data, contentTypes: item.supportedContentTypes)
            await appState.sendImageFile(imageFile)
        } catch {
            appState.errorMessage = error.localizedDescription
        }
    }

    private func savePickedImage(
        data: Data,
        contentTypes: [UTType]
    ) throws -> RemoteIMImageFile {
        let contentType = contentTypes.first(where: { $0.conforms(to: .image) })
        let fileExtension = contentType?.preferredFilenameExtension ?? "jpg"
        let directory = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("RemoteIMPickedImage", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let fileURL = directory
            .appendingPathComponent("remote-im-image-\(UUID().uuidString)")
            .appendingPathExtension(fileExtension)
        try data.write(to: fileURL, options: .atomic)

        let image = UIImage(data: data)
        let width = image.map { Int($0.size.width * $0.scale) }
        let height = image.map { Int($0.size.height * $0.scale) }
        return RemoteIMImageFile(
            fileURL: fileURL,
            width: width,
            height: height,
            sizeBytes: data.count
        )
    }
}

private struct VoiceRecordingHint: View {
    let isCancelling: Bool

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: isCancelling ? "xmark.circle.fill" : "mic.fill")
            Text(isCancelling ? "松开取消" : "松开发送，上滑取消")
                .font(.system(size: 13, weight: .semibold))
        }
        .foregroundStyle(isCancelling ? .red : RemoteIMStyle.textPrimary)
        .padding(.horizontal, 14)
        .padding(.vertical, 9)
        .background(Color.white, in: Capsule())
        .overlay(Capsule().stroke(isCancelling ? Color.red.opacity(0.45) : RemoteIMStyle.border, lineWidth: 1))
    }
}

private struct PressToTalkButton: View {
    let isPressing: Bool
    let isCancelling: Bool
    let isEnabled: Bool
    let onChanged: (CGSize) -> Void
    let onEnded: (CGSize) -> Void

    var body: some View {
        Text(title)
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(isEnabled ? RemoteIMStyle.textPrimary : RemoteIMStyle.textSecondary)
            .frame(maxWidth: .infinity, minHeight: 44)
            .background(backgroundColor, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .stroke(borderColor, lineWidth: isPressing ? 1.5 : 1)
            )
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        onChanged(value.translation)
                    }
                    .onEnded { value in
                        onEnded(value.translation)
                    }
            )
            .allowsHitTesting(isEnabled)
    }

    private var title: String {
        if !isEnabled { return "选择联系人后可发送语音" }
        if isCancelling { return "松开取消" }
        return isPressing ? "松开发送" : "按住 说话"
    }

    private var backgroundColor: Color {
        if !isEnabled { return Color(.secondarySystemBackground) }
        if isCancelling { return Color.red.opacity(0.08) }
        return isPressing ? RemoteIMStyle.blueSoft : Color.white
    }

    private var borderColor: Color {
        if isCancelling { return .red }
        return isPressing ? RemoteIMStyle.blue : RemoteIMStyle.border
    }
}
