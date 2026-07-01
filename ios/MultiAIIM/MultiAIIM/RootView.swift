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
            if appState.shouldShowInitialLogin {
                InitialLoginView()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
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
        }
        .background(Color(red: 0.966, green: 0.976, blue: 0.988).ignoresSafeArea())
        .task {
            if !appState.shouldShowInitialLogin {
                await appState.connectIfRequestedByLaunchEnvironment()
            }
        }
        .overlay(alignment: .top) {
            if !appState.shouldShowInitialLogin, let errorMessage = appState.errorMessage {
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

private struct InitialLoginView: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    private var isConnecting: Bool {
        appState.connectionState == .connecting
    }

    private var canSubmit: Bool {
        !isConnecting &&
            !appState.masterUserID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var body: some View {
        VStack(spacing: 22) {
            Spacer(minLength: 24)

            VStack(alignment: .leading, spacing: 6) {
                Text("远程 IM 登录")
                    .font(.system(size: 28, weight: .bold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                Text("登录后再进入消息、通讯录和设置。")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            VStack(spacing: 12) {
                LoginField(title: "UserID", systemImage: "person") {
                    TextField("输入 IM UserID", text: $appState.masterUserID)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                }
                FixedCredentialSummary(sdkAppIDText: appState.sdkAppIDText)
            }

            if let errorMessage = appState.errorMessage {
                Text(errorMessage)
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(.red)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }

            Button {
                Task { await appState.submitInitialLogin() }
            } label: {
                HStack(spacing: 8) {
                    if appState.connectionState == .connecting {
                        ProgressView()
                            .tint(.white)
                    }
                    Text(appState.connectionState == .connecting ? "连接中..." : "登录并进入")
                        .font(.system(size: 16, weight: .bold))
                }
                .frame(maxWidth: .infinity)
                .frame(height: 48)
                .contentShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
            }
            .buttonStyle(.plain)
            .foregroundStyle(.white)
            .background(
                canSubmit ? RemoteIMStyle.blue : Color(red: 0.69, green: 0.82, blue: 0.91),
                in: RoundedRectangle(cornerRadius: 12, style: .continuous)
            )
            .disabled(!canSubmit)

            Spacer(minLength: 16)
        }
        .padding(.horizontal, 28)
        .background(RemoteIMStyle.pageBackground.ignoresSafeArea())
    }
}

private struct FixedCredentialSummary: View {
    let sdkAppIDText: String

    private var displaySDKAppID: String {
        let cleanSDKAppID = sdkAppIDText.trimmingCharacters(in: .whitespacesAndNewlines)
        return cleanSDKAppID.isEmpty ? String(RemoteIMCredentialDefaults.sdkAppID) : cleanSDKAppID
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("基础 IM 配置", systemImage: "lock.shield")
                .font(.system(size: 13, weight: .bold))
                .foregroundStyle(RemoteIMStyle.textSecondary)
            HStack {
                Text("SDKAppID")
                    .foregroundStyle(RemoteIMStyle.textSecondary)
                Spacer()
                Text(displaySDKAppID)
                    .monospacedDigit()
                    .fontWeight(.bold)
            }
            HStack {
                Text("UserSig 凭证")
                    .foregroundStyle(RemoteIMStyle.textSecondary)
                Spacer()
                Text("内置")
                    .fontWeight(.bold)
            }
        }
        .font(.system(size: 15))
        .padding(12)
        .background(Color.white, in: RoundedRectangle(cornerRadius: 9, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 9, style: .continuous)
                .stroke(RemoteIMStyle.border, lineWidth: 1)
        )
    }
}

private struct LoginField<Content: View>: View {
    let title: String
    let systemImage: String
    private let content: Content

    init(title: String, systemImage: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.systemImage = systemImage
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            Label(title, systemImage: systemImage)
                .font(.system(size: 13, weight: .bold))
                .foregroundStyle(RemoteIMStyle.textSecondary)
            content
                .font(.system(size: 15))
                .padding(.horizontal, 12)
                .frame(height: 44)
                .background(Color.white, in: RoundedRectangle(cornerRadius: 9, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: 9, style: .continuous)
                        .stroke(RemoteIMStyle.border, lineWidth: 1)
                )
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
