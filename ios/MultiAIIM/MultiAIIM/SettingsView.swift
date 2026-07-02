import MultiAIIMCore
import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        NavigationStack {
            Form {
                Section("账号") {
                    LabeledContent("登录账号") {
                        Text(displayUserID)
                            .foregroundStyle(appState.masterUserID.isEmpty ? .secondary : .primary)
                    }
                }

                Section("IM 配置") {
                    LabeledContent("通信配置") {
                        Text("内置")
                            .foregroundStyle(.secondary)
                    }
                    LabeledContent("连接凭证") {
                        Text("使用内置凭证")
                            .foregroundStyle(.secondary)
                    }
                    Text("基础 IM 配置由应用内置，设置页不再修改。")
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
                    Text("登录后会自动连接 IM；需要切换账号时重新进入登录页。")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("远程 IM 设置")
        }
    }

    private var displayUserID: String {
        let userID = appState.masterUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        return userID.isEmpty ? "未登录" : userID
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
