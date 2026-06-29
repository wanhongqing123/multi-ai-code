import MultiAIIMCore
import SwiftUI

enum AppTab {
    case messages
    case contacts
    case me
}

struct RootView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var selectedTab: AppTab = .messages
    @State private var activeChatContact: RemoteIMContact?

    var body: some View {
        VStack(spacing: 0) {
            Group {
                switch selectedTab {
                case .messages:
                    ChatView(activeContact: $activeChatContact)
                case .contacts:
                    ContactsView(selectedTab: $selectedTab, activeContact: $activeChatContact)
                case .me:
                    SettingsView()
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)

            if activeChatContact == nil {
                CompactTabBar(selectedTab: $selectedTab)
            }
        }
        .background(Color(red: 0.966, green: 0.976, blue: 0.988).ignoresSafeArea())
        .task {
            await appState.connectIfRequestedByLaunchEnvironment()
        }
        .overlay(alignment: .top) {
            if let errorMessage = appState.errorMessage {
                Text(errorMessage)
                    .font(.footnote)
                    .foregroundStyle(.white)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 9)
                    .background(.red, in: Capsule())
                    .padding(.top, 8)
                    .padding(.horizontal, 16)
            }
        }
    }
}

private struct CompactTabBar: View {
    @Binding var selectedTab: AppTab

    var body: some View {
        HStack(spacing: 8) {
            TabButton(
                title: "消息",
                systemImage: selectedTab == .messages ? "bubble.left.fill" : "bubble.left",
                selected: selectedTab == .messages
            ) {
                selectedTab = .messages
            }
            TabButton(
                title: "通讯录",
                systemImage: selectedTab == .contacts ? "person.2.fill" : "person.2",
                selected: selectedTab == .contacts
            ) {
                selectedTab = .contacts
            }
            TabButton(
                title: "我",
                systemImage: selectedTab == .me ? "person.fill" : "person",
                selected: selectedTab == .me
            ) {
                selectedTab = .me
            }
        }
        .padding(6)
        .background(Color(red: 0.962, green: 0.97, blue: 0.98), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .stroke(Color(red: 0.855, green: 0.894, blue: 0.941), lineWidth: 1)
        )
        .padding(.horizontal, 16)
        .padding(.top, 9)
        .padding(.bottom, 10)
        .frame(maxWidth: .infinity)
        .background(Color.white)
        .overlay(alignment: .top) {
            Divider().background(Color(red: 0.855, green: 0.894, blue: 0.941))
        }
    }
}

private struct TabButton: View {
    let title: String
    let systemImage: String
    let selected: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.system(size: 14, weight: .semibold))
                .frame(maxWidth: .infinity)
                .frame(height: 38)
        }
        .buttonStyle(.plain)
        .foregroundStyle(selected ? Color(red: 0.035, green: 0.376, blue: 0.667) : Color(red: 0.392, green: 0.459, blue: 0.561))
        .background(
            selected
                ? Color(red: 0.882, green: 0.957, blue: 1.0)
                : Color.clear,
            in: RoundedRectangle(cornerRadius: 10, style: .continuous)
        )
    }
}
