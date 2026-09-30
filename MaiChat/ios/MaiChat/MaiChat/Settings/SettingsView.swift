import MaiChatCore
import SwiftUI
import UIKit

struct SettingsView: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        NavigationStack {
            List {
                Section {
                    NavigationLink {
                        PersonalProfileView()
                    } label: {
                        HStack(spacing: 18) {
                            SelfProfileAvatar(size: 64)
                            VStack(alignment: .leading, spacing: 6) {
                                Text(appState.profile(for: appState.masterUserID).displayName)
                                    .font(.title3.weight(.semibold))
                                    .foregroundStyle(RemoteIMStyle.textPrimary)
                                Text(appState.masterUserID)
                                    .font(.subheadline)
                                    .foregroundStyle(RemoteIMStyle.textSecondary)
                            }
                            .lineLimit(1)
                        }
                        .padding(.vertical, 16)
                    }
                }
                Section {
                    NavigationLink {
                        AccountSettingsView().navigationTitle("设置")
                    } label: {
                        Label("设置", systemImage: "gearshape")
                    }
                }
            }
            .listStyle(.insetGrouped)
            .navigationTitle("我")
            .navigationBarTitleDisplayMode(.inline)
        }
    }
}

private struct SelfProfileAvatar: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    let size: CGFloat

    var body: some View {
        let profile = appState.profile(for: appState.masterUserID)
        RemoteIMContactAvatar(
            contact: RemoteIMContact(
                userID: profile.userID, displayName: profile.displayName,
                avatarURL: profile.avatarURL
            ),
            isSelected: false, presenceStatus: .unknown, size: size
        )
    }
}

private struct PersonalProfileView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var isChoosingAvatar = false
    @State private var isSaving = false

    var body: some View {
        List {
            Section {
                Button {
                    isChoosingAvatar = true
                } label: {
                    HStack {
                        Text("头像").foregroundStyle(RemoteIMStyle.textPrimary)
                        Spacer()
                        if isSaving { ProgressView() }
                        SelfProfileAvatar(size: 48)
                        Image(systemName: "chevron.right")
                            .font(.footnote.weight(.semibold))
                            .foregroundStyle(RemoteIMStyle.textSecondary)
                    }
                    .padding(.vertical, 4)
                }
                .disabled(isSaving || appState.connectionState != .connected)
                LabeledContent("名字", value: appState.profile(for: appState.masterUserID).displayName)
                LabeledContent("账号", value: appState.masterUserID)
            }
        }
        .listStyle(.plain)
        .navigationTitle("个人资料")
        .navigationBarTitleDisplayMode(.inline)
        .sheet(isPresented: $isChoosingAvatar) {
            AvatarPhotoPicker { image in
                isChoosingAvatar = false
                guard let image else { return }
                saveAvatar(image)
            }
            .ignoresSafeArea()
        }
    }

    private func saveAvatar(_ image: UIImage) {
        guard !isSaving else { return }
        isSaving = true
        Task { @MainActor in
            defer { isSaving = false }
            let fileURL = FileManager.default.temporaryDirectory
                .appendingPathComponent("avatar-\(UUID().uuidString).jpg")
            defer { try? FileManager.default.removeItem(at: fileURL) }
            do {
                let format = UIGraphicsImageRendererFormat()
                format.scale = 1
                let renderer = UIGraphicsImageRenderer(
                    size: CGSize(width: 512, height: 512), format: format
                )
                let scale = max(512 / max(image.size.width, 1),
                                512 / max(image.size.height, 1))
                let width = image.size.width * scale
                let height = image.size.height * scale
                let avatar = renderer.image { _ in
                    image.draw(in: CGRect(x: (512 - width) / 2, y: (512 - height) / 2,
                                          width: width, height: height))
                }
                guard let data = avatar.jpegData(compressionQuality: 0.88) else {
                    appState.showTransientError("无法处理这张图片")
                    return
                }
                try data.write(to: fileURL, options: .atomic)
                _ = await appState.uploadSelfAvatar(fileURL: fileURL)
            } catch {
                appState.showTransientError(error.localizedDescription)
            }
        }
    }
}

private struct AvatarPhotoPicker: UIViewControllerRepresentable {
    let completion: (UIImage?) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(completion: completion) }

    func makeUIViewController(context: Context) -> UIImagePickerController {
        let picker = UIImagePickerController()
        picker.sourceType = .photoLibrary
        picker.allowsEditing = true
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ controller: UIImagePickerController, context: Context) {}

    final class Coordinator: NSObject, UIImagePickerControllerDelegate, UINavigationControllerDelegate {
        let completion: (UIImage?) -> Void
        init(completion: @escaping (UIImage?) -> Void) { self.completion = completion }

        func imagePickerController(
            _ picker: UIImagePickerController,
            didFinishPickingMediaWithInfo info: [UIImagePickerController.InfoKey: Any]
        ) {
            completion(info[.editedImage] as? UIImage ?? info[.originalImage] as? UIImage)
        }

        func imagePickerControllerDidCancel(_ picker: UIImagePickerController) { completion(nil) }
    }
}

private struct AccountSettingsView: View {
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
                if appState.connectionState != .connected {
                    Button {
                        Task { await appState.requestConnection() }
                    } label: {
                        Label(
                            appState.connectionState == .failed ? "重新连接" : "连接 IM",
                            systemImage: "arrow.clockwise"
                        )
                    }
                    .disabled(
                        appState.connectionState == .connecting ||
                            appState.masterUserID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
                    )
                }
                Text("登录后会自动连接 IM；连接失败时可在这里重新连接。")
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
