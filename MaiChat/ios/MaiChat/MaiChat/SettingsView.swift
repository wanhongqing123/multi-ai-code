import MaiChatCore
import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
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

            Section("排障") {
                ShareLink(
                    item: DiagnosticLogExport(),
                    preview: SharePreview("MaiChat 排障日志")
                ) {
                    Label("导出排障日志", systemImage: "square.and.arrow.up")
                }
                Text("日志保留 7 天，记录状态、错误码、耗时、尺寸、事件计数和脱敏会话标识；不包含聊天正文、远程键盘内容或连接凭证。")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
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
