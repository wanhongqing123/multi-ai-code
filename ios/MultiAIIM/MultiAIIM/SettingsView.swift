import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        NavigationStack {
            Form {
                Section("Tencent IM") {
                    TextField("SDKAppID", text: $appState.sdkAppIDText)
                        .keyboardType(.numberPad)
                    TextField("主人 UserID", text: $appState.masterUserID)
                        .textInputAutocapitalization(.never)
                    SecureField("UserSig SecretKey", text: $appState.secretKey)
                        .textInputAutocapitalization(.never)
                }

                Section("连接") {
                    HStack {
                        Text("状态")
                        Spacer()
                        Text(appState.connectionState.rawValue)
                            .foregroundStyle(statusColor)
                    }
                    Button("保存并连接") {
                        Task { await appState.connect() }
                    }
                    Button("断开连接", role: .destructive) {
                        Task { await appState.disconnect() }
                    }
                    .disabled(appState.connectionState != .connected)
                }
            }
            .navigationTitle("远程 IM 设置")
            .toolbar {
                Button("保存") {
                    appState.saveSettings()
                }
            }
        }
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
