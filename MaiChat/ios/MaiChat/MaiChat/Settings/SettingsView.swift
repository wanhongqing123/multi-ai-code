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
    @State private var isSwitchingPhone = false

    var body: some View {
        Form {
            Section("账号") {
                LabeledContent("登录账号") {
                    Text(displayUserID)
                        .foregroundStyle(appState.masterUserID.isEmpty ? .secondary : .primary)
                }
                Button("切换手机号") {
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

        }
    }

    private var displayUserID: String {
        let userID = appState.masterUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        return userID.isEmpty ? "未登录" : userID
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
