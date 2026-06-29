import MultiAIIMCore
import SwiftUI

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

private struct HeaderView: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                Text("远程 IM")
                    .font(.system(size: 21, weight: .bold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                Text(appState.masterUserID.isEmpty ? "未设置 UserID" : appState.masterUserID)
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
        ScrollView {
            LazyVStack(spacing: 0) {
                if appState.chatState.contacts.isEmpty {
                    EmptyConversationListView()
                        .padding(.top, 96)
                } else {
                    ForEach(appState.chatState.contacts) { contact in
                        VStack(spacing: 0) {
                            Button {
                                appState.selectContact(contact)
                                activeContact = contact
                            } label: {
                                ConversationRow(
                                    contact: contact,
                                    latestMessage: appState.chatState.latestMessage(with: contact.userID),
                                    selected: contact.userID == appState.chatState.selectedPeerID
                                )
                            }
                            .buttonStyle(.plain)

                            Divider()
                                .padding(.leading, 70)
                        }
                    }
                }
            }
        }
        .background(RemoteIMStyle.panelBackground)
    }
}

private struct ConversationRow: View {
    let contact: RemoteIMContact
    let latestMessage: RemoteIMMessage?
    let selected: Bool

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "desktopcomputer")
                .font(.system(size: 18, weight: .semibold))
                .foregroundStyle(selected ? Color(red: 0.035, green: 0.376, blue: 0.667) : RemoteIMStyle.textSecondary)
                .frame(width: 42, height: 42)
                .background(
                    selected ? RemoteIMStyle.blueSoft : Color(red: 0.953, green: 0.961, blue: 0.973),
                    in: RoundedRectangle(cornerRadius: 10, style: .continuous)
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
                    Text(latestMessage.createdAt.formatted(date: .omitted, time: .shortened))
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                }
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
            Text("到通讯录添加好友或奴隶 UserID 后即可开始聊天。")
                .font(.system(size: 13))
                .foregroundStyle(RemoteIMStyle.textSecondary)
        }
        .frame(maxWidth: .infinity)
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
        .onAppear {
            appState.selectContact(contact)
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
                                incomingRelation: peerRelation
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
            .onChange(of: messages.count) { _ in
                if let last = messages.last {
                    proxy.scrollTo(last.id, anchor: .bottom)
                }
            }
        }
    }
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
                    Text(message.createdAt.formatted(date: .omitted, time: .shortened))
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                    Spacer(minLength: 0)
                }

                HStack(alignment: .bottom, spacing: 10) {
                    MarkdownLikeText(message.text)
                        .font(
                            .system(
                                size: 13,
                                weight: .regular,
                                design: usesMonospace ? .monospaced : .default
                            )
                        )
                        .lineSpacing(3)
                        .foregroundStyle(RemoteIMStyle.textPrimary)

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

    private var usesMonospace: Bool {
        message.text.contains("|") || message.text.contains("```") || message.text.count > 400
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
    let text: String

    init(_ text: String) {
        self.text = text
    }

    var body: some View {
        if text.contains("|") || text.contains("```") {
            Text(text)
                .textSelection(.enabled)
        } else if let attributed = try? AttributedString(markdown: text) {
            Text(attributed)
                .textSelection(.enabled)
        } else {
            Text(text)
                .textSelection(.enabled)
        }
    }
}

private struct ComposerView: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        HStack(alignment: .bottom, spacing: 0) {
            TextField("输入要发送给当前 UserID 的消息...", text: $appState.draftText, axis: .vertical)
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
        .padding(.horizontal, 16)
        .padding(.top, 12)
        .padding(.bottom, 10)
        .background(RemoteIMStyle.panelBackground)
        .overlay(alignment: .top) {
            Divider().background(RemoteIMStyle.border)
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
}
