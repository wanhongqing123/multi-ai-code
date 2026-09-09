import SwiftUI
import MaiChatCore

struct RemoteDiagnosticsView: View {
    let peer: RemoteIMContact
    let contacts: [RemoteIMContact]
    let ownerUserID: String
    let connected: Bool
    @ObservedObject var coordinator: RemoteDiagnosticsCoordinator
    @Environment(\.dismiss) private var dismiss
    @State private var recipientID = ""

    private var recipients: [RemoteIMContact] {
        contacts.filter { $0.userID != ownerUserID }
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("故障会话") {
                    Text(peer.displayName)
                    Text("收集 iOS 本地现场，并请求该好友的 MultiAICode 回传近期诊断记录。不需要选择问题类型或操作远端电脑。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("发送给排查好友") {
                    Picker("接收报告", selection: $recipientID) {
                        Text("请选择好友").tag("")
                        ForEach(recipients, id: \.userID) { contact in
                            Text(contact.displayName).tag(contact.userID)
                        }
                    }
                    .disabled(coordinator.isRunning)
                    Text("确认后将自动合并并发送给所选好友。报告不含聊天正文、登录凭据或环境变量；远端未响应或记录缺失时会明确标注。采集请求会作为一条消息留在当前聊天。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section {
                    Button("确认收集并发送") {
                        guard let recipient = recipients.first(where: { $0.userID == recipientID }) else { return }
                        coordinator.start(peer: peer, recipient: recipient)
                    }
                    .disabled(recipientID.isEmpty || coordinator.isRunning || !connected)
                    .accessibilityIdentifier("confirmRemoteDiagnostics")
                    if !connected { Text("本机未连接 IM，连接后才能发送请求与报告。").foregroundStyle(.secondary) }
                    if coordinator.isRunning {
                        ProgressView()
                        Button("取消回传", role: .cancel) { coordinator.cancel() }
                            .disabled(coordinator.isSending)
                    }
                    Text(coordinator.status).font(.footnote)
                        .accessibilityIdentifier("remoteDiagnosticsStatus")
                }
            }
            .navigationTitle("远程排障")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("关闭") { dismiss() }.disabled(coordinator.isRunning)
                }
            }
        }
        .interactiveDismissDisabled(coordinator.isRunning)
        .onAppear {
            recipientID = recipients.first(where: { $0.userID == "mac-multi-ai-code" })?.userID
                ?? recipients.first(where: { $0.userID == "house-multi-ai-code" })?.userID ?? ""
        }
    }
}
