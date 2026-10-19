// 文件作用：展示账号、隐私和应用设置，并提供用户可执行的设置操作。
// 展示账号、通知、远程桌面及应用设置入口。
// 切换账号先退出当前会话，再进入 MaiChat 登录页。
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
                                Text(appState.selfDisplayName())
                                    .font(.title3.weight(.semibold))
                                    .foregroundStyle(RemoteIMStyle.textPrimary)
                                Text(appState.maiChatAccountID)
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
                LabeledContent("名字", value: appState.selfDisplayName())
                LabeledContent("账号", value: appState.maiChatAccountID)
                NavigationLink {
                    FriendQRShareView(userID: appState.maiChatAccountID)
                } label: {
                    Label("我的好友二维码", systemImage: "qrcode")
                }
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
    @State private var isSwitchingPhone = false

    var body: some View {
        Form {
            Section("账号") {
                LabeledContent("登录账号") {
                    Text(displayUserID)
                        .foregroundStyle(appState.maiChatAccountID.isEmpty ? .secondary : .primary)
                }
                Button("切换账号") {
                    guard !isSwitchingPhone else { return }
                    isSwitchingPhone = true
                    Task { @MainActor in
                        _ = await appState.switchPhoneAccount()
                        isSwitchingPhone = false
                    }
                }
                .disabled(isSwitchingPhone)
                if appState.hasUnifiedAccount {
                    NavigationLink("设置或修改登录密码") {
                        AccountPasswordView()
                    }
                }
            }

            if appState.hasUnifiedAccount {
                Section {
                    NavigationLink {
                        DesktopLoginScanView()
                    } label: {
                        Label("扫码登录电脑", systemImage: "qrcode.viewfinder")
                    }
                    .accessibilityIdentifier("desktop-login-entry")
                } header: {
                    Text("设备登录")
                } footer: {
                    Text("扫描电脑上的 MaiChat 登录二维码，并比对六位核对码。")
                }
            }

        }
    }

    private var displayUserID: String {
        guard appState.hasUnifiedAccount else { return "未登录" }
        let userID = appState.maiChatAccountID.trimmingCharacters(in: .whitespacesAndNewlines)
        return userID.isEmpty ? "MaiChat ID 待同步" : userID
    }

}

private struct AccountPasswordView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var password = ""
    @State private var confirmation = ""
    @State private var isSaving = false
    @State private var message: String?

    var body: some View {
        Form {
            Section {
                SecureField("新密码", text: $password)
                    .textContentType(.newPassword)
                SecureField("再次输入新密码", text: $confirmation)
                    .textContentType(.newPassword)
            } footer: {
                Text("至少 15 个字符；修改密码前须在最近 10 分钟内登录。")
            }
            if let message {
                Text(message).foregroundStyle(.secondary)
            }
            Button(isSaving ? "保存中…" : "保存密码") {
                Task { await save() }
            }
            .disabled(isSaving || password.count < 15 || password != confirmation)
        }
        .navigationTitle("登录密码")
    }

    private func save() async {
        isSaving = true
        defer { isSaving = false }
        do {
            try await appState.setAccountPassword(password)
            password = ""
            confirmation = ""
            message = "密码已保存"
        } catch {
            message = error.localizedDescription
        }
    }
}
