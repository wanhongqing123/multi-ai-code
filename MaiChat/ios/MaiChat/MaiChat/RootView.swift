import MaiChatCore
import SwiftUI
import UIKit

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
    @FocusState private var isAccountFocused: Bool

    private let loginBlue = Color(
        red: 47.0 / 255.0,
        green: 129.0 / 255.0,
        blue: 247.0 / 255.0
    )

    private var isConnecting: Bool {
        appState.connectionState == .connecting
    }

    private var canSubmit: Bool {
        !isConnecting &&
            !appState.masterUserID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var body: some View {
        VStack(spacing: 0) {
            Spacer(minLength: 32)

            VStack(spacing: 12) {
                Image(uiImage: Self.applicationIcon)
                    .resizable()
                    .interpolation(.high)
                    .scaledToFit()
                    .frame(width: 64, height: 64)
                    .clipShape(RoundedRectangle(cornerRadius: 15, style: .continuous))

                Text("欢迎使用 MaiChat")
                    .font(.system(size: 20, weight: .heavy))
                    .foregroundStyle(Color(red: 15.0 / 255.0, green: 23.0 / 255.0, blue: 42.0 / 255.0))
            }
            .multilineTextAlignment(.center)
            .padding(.bottom, 28)

            TextField("请输入登录账号", text: $appState.masterUserID)
                .font(.system(size: 15))
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .submitLabel(.go)
                .focused($isAccountFocused)
                .padding(.horizontal, 14)
                .frame(height: 46)
                .background(Color.white, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .stroke(
                            isAccountFocused
                                ? loginBlue
                                : Color(red: 217.0 / 255.0, green: 225.0 / 255.0, blue: 236.0 / 255.0),
                            lineWidth: isAccountFocused ? 1.5 : 1
                        )
                )
                .onSubmit {
                    guard canSubmit else { return }
                    Task { await appState.submitInitialLogin() }
                }

            if let errorMessage = appState.errorMessage {
                Text(errorMessage)
                    .font(.system(size: 13, weight: .bold))
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: .infinity)
                    .padding(.top, 12)
            }

            Button {
                Task { await appState.submitInitialLogin() }
            } label: {
                HStack(spacing: 8) {
                    if appState.connectionState == .connecting {
                        ProgressView()
                            .tint(.white)
                    }
                    Text(appState.connectionState == .connecting ? "登录中..." : "登录")
                        .font(.system(size: 15, weight: .heavy))
                }
                .frame(maxWidth: .infinity)
                .frame(height: 46)
                .contentShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
            }
            .buttonStyle(.plain)
            .foregroundStyle(canSubmit ? Color.white : Color(red: 170.0 / 255.0, green: 180.0 / 255.0, blue: 195.0 / 255.0))
            .background(
                canSubmit
                    ? loginBlue
                    : Color(red: 238.0 / 255.0, green: 241.0 / 255.0, blue: 245.0 / 255.0),
                in: RoundedRectangle(cornerRadius: 10, style: .continuous)
            )
            .disabled(!canSubmit)
            .padding(.top, 16)

            Spacer(minLength: 32)
        }
        .padding(.horizontal, 28)
        .frame(maxWidth: 420)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.white.ignoresSafeArea())
    }

    private static var applicationIcon: UIImage {
        guard let icons = Bundle.main.object(forInfoDictionaryKey: "CFBundleIcons") as? [String: Any],
              let primaryIcon = icons["CFBundlePrimaryIcon"] as? [String: Any],
              let iconFiles = primaryIcon["CFBundleIconFiles"] as? [String],
              let iconFile = iconFiles.last,
              let image = UIImage(named: iconFile)
        else {
            return UIImage()
        }
        return image
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
