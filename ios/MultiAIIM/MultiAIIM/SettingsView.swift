import MultiAIIMCore
import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        NavigationStack {
            Form {
                Section("账号") {
                    LabeledContent("UserID") {
                        Text(displayUserID)
                            .foregroundStyle(appState.masterUserID.isEmpty ? .secondary : .primary)
                    }
                }

                Section("Tencent IM") {
                    LabeledContent("SDKAppID") {
                        Text(displaySDKAppID)
                            .monospacedDigit()
                    }
                    LabeledContent("UserSig 凭证") {
                        Text("使用内置凭证")
                            .foregroundStyle(.secondary)
                    }
                    Text("基础 IM 配置由应用内置，设置页不再修改 SDKAppID 和凭证。")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }

                Section("连接") {
                    HStack {
                        Text("状态")
                        Spacer()
                        Text(appState.connectionState.rawValue)
                            .foregroundStyle(statusColor)
                    }
                    Button(appState.connectionState == .connected ? "重新连接" : "连接") {
                        Task { await appState.connect() }
                    }
                    .disabled(!canConnect)
                    Button("断开连接", role: .destructive) {
                        Task { await appState.disconnect() }
                    }
                    .disabled(appState.connectionState != .connected)
                }
            }
            .navigationTitle("远程 IM 设置")
        }
    }

    private var displayUserID: String {
        let userID = appState.masterUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        return userID.isEmpty ? "未登录" : userID
    }

    private var displaySDKAppID: String {
        let sdkAppIDText = appState.sdkAppIDText.trimmingCharacters(in: .whitespacesAndNewlines)
        return sdkAppIDText.isEmpty ? String(RemoteIMCredentialDefaults.sdkAppID) : sdkAppIDText
    }

    private var canConnect: Bool {
        appState.connectionState != .connecting &&
            !appState.masterUserID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private var statusColor: Color {
        switch appState.connectionState {
        case .connected:
            return .green
        case .connecting:
            return .orange
        case .failed:
            return .red
        case .disconnected:
            return .secondary
        }
    }
}
