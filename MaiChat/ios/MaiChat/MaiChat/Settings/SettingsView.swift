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
                                SelfConnectionStatus(state: appState.connectionState)
                            }
                            .lineLimit(1)
                        }
                        .padding(.vertical, 16)
                    }
                    .accessibilityIdentifier("personal-profile-entry")
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
            .remoteIMNavigationBarMaterial()
        }
    }
}

private struct SelfConnectionStatus: View {
    let state: RemoteIMAppState.ConnectionState

    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(color).frame(width: 8, height: 8)
            Text("消息\(state.rawValue)")
                .font(.caption)
                .foregroundStyle(RemoteIMStyle.textSecondary)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("消息连接")
        .accessibilityValue(state.rawValue)
        .accessibilityIdentifier("self-im-connection-status")
    }

    private var color: Color {
        switch state {
        case .connected: return RemoteIMStyle.green
        case .connecting: return .orange
        case .failed: return .red
        case .disconnected: return RemoteIMStyle.textSecondary
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
                NavigationLink {
                    AccountNicknameView()
                } label: {
                    LabeledContent("昵称", value: appState.maiChatNickname.isEmpty
                                   ? "未设置" : appState.maiChatNickname)
                }
                .accessibilityIdentifier("account-nickname-entry")
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

private struct AccountNicknameView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.dismiss) private var dismiss
    @State private var nickname = ""
    @State private var isSaving = false
    @State private var errorMessage: String?

    var body: some View {
        Form {
            Section {
                TextField("昵称", text: $nickname)
                    .accessibilityIdentifier("account-nickname-input")
            } footer: {
                Text("最多 32 个字符；留空后显示 MaiChat ID。")
            }
            if let errorMessage {
                Text(errorMessage).foregroundStyle(.red)
            }
        }
        .navigationTitle("修改昵称")
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button("保存") { saveNickname() }
                    .disabled(isSaving || nickname.trimmingCharacters(
                        in: .whitespacesAndNewlines) == appState.maiChatNickname)
            }
        }
        .onAppear { nickname = appState.maiChatNickname }
    }

    private func saveNickname() {
        let value = nickname.trimmingCharacters(in: .whitespacesAndNewlines)
            .precomposedStringWithCanonicalMapping
        guard value.unicodeScalars.count <= 32, value.utf8.count <= 120 else {
            errorMessage = "昵称最多 32 个字符，且不能超过 120 字节"
            return
        }
        isSaving = true
        errorMessage = nil
        Task { @MainActor in
            defer { isSaving = false }
            do {
                try await appState.setMaiChatNickname(value)
                dismiss()
            } catch {
                errorMessage = error.localizedDescription
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
                    NavigationLink {
                        AccountDeletionView()
                    } label: {
                        Text("注销账号").foregroundStyle(.red)
                    }
                    .accessibilityIdentifier("account-deletion-entry")
                }
            }

            Section("隐私与支持") {
                Link("隐私政策", destination: URL(string: "https://ichat.life/legal/privacy.html")!)
                Link("用户协议", destination: URL(string: "https://ichat.life/legal/terms.html")!)
                Link("联系我们", destination: URL(string: "mailto:1925271463@kongshang.com")!)
                NavigationLink("我的举报") { SafetyReportsView() }
            }

        }
    }

    private var displayUserID: String {
        guard appState.hasUnifiedAccount else { return "未登录" }
        let userID = appState.maiChatAccountID.trimmingCharacters(in: .whitespacesAndNewlines)
        return userID.isEmpty ? "MaiChat ID 待同步" : userID
    }

}

private struct AccountDeletionView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var options: MaiChatAccountDeletionOptions?
    @State private var accountID = ""
    @State private var password = ""
    @State private var code = ""
    @State private var challengeID: String?
    @State private var usePassword = false
    @State private var isBusy = false
    @State private var showConfirmation = false
    @State private var errorMessage: String?

    var body: some View {
        Form {
            Section {
                Text("提交后立即停用 MaiChat 账号并撤销账号登录。好友关系、云端会话和可识别素材会进入清理流程；依法必须保留的记录按期限处理。")
                    .font(.footnote)
                if let options {
                    Text("预计在 \(options.processingDays) 天内完成。完成后不能恢复账号及资料。")
                        .font(.footnote)
                }
            }
            Section("确认身份") {
                if options == nil { ProgressView() }
                if let options {
                    if options.hasPassword && options.phoneMasked != nil {
                        Picker("确认方式", selection: $usePassword) {
                            Text("登录密码").tag(true)
                            Text("短信验证码").tag(false)
                        }
                        .pickerStyle(.segmented)
                    }
                    if usePassword {
                        SecureField("输入登录密码", text: $password)
                            .textContentType(.password)
                    } else if let phone = options.phoneMasked {
                        LabeledContent("已绑定手机号", value: phone)
                        HStack {
                            TextField("输入 6 位验证码", text: $code)
                                .keyboardType(.numberPad)
                                .textContentType(.oneTimeCode)
                            Button(challengeID == nil ? "获取验证码" : "重新获取") {
                                Task { await requestCode() }
                            }
                            .disabled(isBusy)
                        }
                    }
                }
            }
            Section("确认 MaiChat ID") {
                Text(appState.maiChatAccountID)
                    .textSelection(.enabled)
                    .accessibilityIdentifier("account-deletion-current-id")
                TextField("输入上方 MaiChat ID", text: $accountID)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .accessibilityIdentifier("account-deletion-confirm-id")
            }
            if let errorMessage { Text(errorMessage).foregroundStyle(.red) }
            Section {
                Button("提交注销申请", role: .destructive) {
                    showConfirmation = true
                }
                .disabled(!canSubmit)
                .accessibilityIdentifier("account-deletion-submit")
            }
        }
        .navigationTitle("注销账号")
        .task { await loadOptions() }
        .confirmationDialog("确认永久注销这个账号？", isPresented: $showConfirmation) {
            Button("确认提交", role: .destructive) { Task { await submit() } }
            Button("取消", role: .cancel) {}
        } message: {
            Text("提交后立即停止账号使用，并开始清理关联数据。")
        }
    }

    private var canSubmit: Bool {
        guard let options, !isBusy,
              accountID == appState.maiChatAccountID else { return false }
        return usePassword ? !password.isEmpty :
            options.phoneMasked != nil && challengeID != nil && code.count == 6
    }

    private func loadOptions() async {
        guard options == nil else { return }
        do {
            let loaded = try await appState.accountDeletionOptions()
            options = loaded
            usePassword = loaded.hasPassword
        } catch { errorMessage = error.localizedDescription }
    }

    private func requestCode() async {
        isBusy = true
        defer { isBusy = false }
        do {
            let challenge = try await appState.requestAccountDeletionCode()
            challengeID = challenge.challengeId
            code = ""
            errorMessage = nil
        } catch { errorMessage = error.localizedDescription }
    }

    private func submit() async {
        guard canSubmit else { return }
        isBusy = true
        defer { isBusy = false }
        do {
            _ = try await appState.submitAccountDeletion(
                password: usePassword ? password : nil,
                challengeID: usePassword ? nil : challengeID,
                code: usePassword ? nil : code
            )
            password = ""
            code = ""
            errorMessage = nil
        } catch { errorMessage = error.localizedDescription }
    }
}

private struct SafetyReportsView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var reports: [MaiChatSafetyReport] = []
    @State private var errorMessage: String?

    var body: some View {
        List {
            if reports.isEmpty && errorMessage == nil {
                Label("暂无举报记录", systemImage: "checkmark.shield")
                    .foregroundStyle(.secondary)
            }
            ForEach(reports) { report in
                VStack(alignment: .leading, spacing: 5) {
                    Text(report.status == "resolved" ? "已处理" : "处理中")
                        .font(.headline)
                    Text(report.resolution ?? "我们会尽快核查并处理。")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                    Text("编号：\(report.id)")
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                }
            }
            if let errorMessage { Text(errorMessage).foregroundStyle(.red) }
        }
        .navigationTitle("我的举报")
        .task { await refresh() }
        .refreshable { await refresh() }
    }

    private func refresh() async {
        do {
            reports = try await appState.safetyReports()
            errorMessage = nil
        } catch { errorMessage = error.localizedDescription }
    }
}

/// 只保存展示状态和账号身份，不保存密码。普通设置页与 Agent 入口共用下方安全表单，
/// 工具只打开/查询 request，避免把新密码写入工具参数、对话历史或诊断日志。
@MainActor
final class AIAccountPasswordRequests: ObservableObject {
    static let shared = AIAccountPasswordRequests()

    struct Request: Identifiable {
        let id: UUID
        let accountIdentity: String
    }
    private struct Record {
        let request: Request
        var status = "opening"
        var opened = false
        var errorCode: String?
    }

    @Published private(set) var presentation: Request?
    var presenterActive = false
    private var records: [UUID: Record] = [:]
    private var order: [UUID] = []

    func open(appState: RemoteIMAppState) throws -> [String: Any] {
        guard appState.hasUnifiedAccount else { throw MaiChatAuthError.sessionExpired }
        guard presenterActive, UIApplication.shared.applicationState == .active else {
            throw PasswordRequestError.unavailable
        }
        let identity = appState.remoteDiagnosticsIdentity
        if let pending = presentation, pending.accountIdentity == identity {
            return try status(id: pending.id, appState: appState)
        }
        cancelForAccountChange()
        let request = Request(id: UUID(), accountIdentity: identity)
        records[request.id] = Record(request: request)
        order.append(request.id)
        // 状态只在本进程短期查询，不落磁盘；限制数量也避免长期账号请求状态积累。
        while order.count > 20 { records.removeValue(forKey: order.removeFirst()) }
        presentation = request
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(5))
            guard records[request.id]?.status == "opening" else { return }
            update(request.id, status: "failed", errorCode: "unavailable")
            if presentation?.id == request.id { presentation = nil }
        }
        return try status(id: request.id, appState: appState)
    }

    func status(id: UUID, appState: RemoteIMAppState) throws -> [String: Any] {
        guard appState.hasUnifiedAccount, let record = records[id],
              record.request.accountIdentity == appState.remoteDiagnosticsIdentity else {
            throw PasswordRequestError.notFound
        }
        var output: [String: Any] = ["request_id": id.uuidString.lowercased(),
            "opened": record.opened, "status": record.status, "changed": record.status == "changed"]
        if let code = record.errorCode { output["error_code"] = code }
        return output
    }

    func didAppear(_ id: UUID, appState: RemoteIMAppState) {
        guard var record = records[id],
              appState.hasUnifiedAccount,
              record.request.accountIdentity == appState.remoteDiagnosticsIdentity else {
            cancelForAccountChange()
            return
        }
        // SwiftUI 的展示请求可能排队；必须页面真正出现后才报告正在等待用户输入。
        record.opened = true
        record.status = "awaiting_user"
        record.errorCode = nil
        records[id] = record
    }

    func update(_ id: UUID, status: String, errorCode: String? = nil) {
        guard var record = records[id], record.status != "changed" else { return }
        record.status = status
        record.errorCode = errorCode
        records[id] = record
    }

    func dismissed(_ id: UUID) {
        // 已发送的 HTTP 不能靠关闭页面撤回；保持在途状态，不能误报已取消。
        guard records[id]?.status != "submitting" else { return }
        if let record = records[id], record.status == "opening" || record.status == "awaiting_user" {
            update(id, status: "canceled", errorCode: "canceled")
        }
        if presentation?.id == id { presentation = nil }
    }

    func cancelForAccountChange() {
        for id in order where records[id]?.status == "submitting" {
            // 换号允许收起旧页，但旧请求结果未知，绝不能声称它已取消或未修改。
            update(id, status: "failed", errorCode: "identity_changed")
        }
        for id in order where records[id]?.status == "opening" || records[id]?.status == "awaiting_user" {
            update(id, status: "canceled", errorCode: "identity_changed")
        }
        presentation = nil
    }

    enum PasswordRequestError: Error { case unavailable, notFound }
}

/// 密码仅停留在本地 SecureField 与普通鉴权调用中；关闭/成功会清空，绝不回传给 Agent。
struct AccountPasswordView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.dismiss) private var dismiss
    var request: AIAccountPasswordRequests.Request? = nil
    @State private var password = ""
    @State private var confirmation = ""
    @State private var isSaving = false
    @State private var message: String?
    @State private var accountIdentity: String?

    private var isValid: Bool {
        let count = password.unicodeScalars.count
        return (15...128).contains(count) && password.utf8.count <= 512 && password == confirmation
    }

    var body: some View {
        Form {
            Section {
                SecureField("新密码", text: $password)
                    .textContentType(.newPassword)
                    .disabled(isSaving)
                    .accessibilityIdentifier("account-password-new")
                SecureField("再次输入新密码", text: $confirmation)
                    .textContentType(.newPassword)
                    .disabled(isSaving)
                    .accessibilityIdentifier("account-password-confirm")
            } footer: {
                Text("密码须为 15–128 个字符；修改前须在最近 10 分钟内登录。保存后其他设备需重新登录，当前设备保持登录。")
            }
            if let message {
                Text(message).foregroundStyle(.secondary)
            }
            Button(isSaving ? "保存中…" : "保存密码") {
                Task { await save() }
            }
            .disabled(isSaving || !isValid)
            .accessibilityIdentifier("account-password-save")
        }
        .navigationTitle("登录密码")
        .navigationBarBackButtonHidden(isSaving)
        .toolbar {
            if request != nil {
                ToolbarItem(placement: .cancellationAction) {
                    Button("关闭") { dismiss() }
                        .disabled(isSaving)
                        .accessibilityIdentifier("account-password-close")
                }
            }
        }
        .interactiveDismissDisabled(isSaving)
        .onAppear {
            accountIdentity = request?.accountIdentity ?? appState.remoteDiagnosticsIdentity
            if let request { AIAccountPasswordRequests.shared.didAppear(request.id, appState: appState) }
        }
        .onDisappear {
            password = ""
            confirmation = ""
            if let request { AIAccountPasswordRequests.shared.dismissed(request.id) }
        }
    }

    private func save() async {
        // 两次点击可能在 SwiftUI 更新 disabled 前入队；在普通入口再挡一次重复提交。
        guard !isSaving else { return }
        isSaving = true
        // 输入阶段可取消；提交阶段不是可撤回操作，明确标记且禁用关闭/重复提交。
        if let request { AIAccountPasswordRequests.shared.update(request.id, status: "submitting") }
        defer { isSaving = false }
        do {
            // 固定进入页面时的账号；await 中切号后不能把旧页成功状态写进新账号。
            guard let accountIdentity else { throw MaiChatAuthError.identityMismatch }
            try await appState.setAccountPassword(password, expectedIdentity: accountIdentity)
            password = ""
            confirmation = ""
            message = "密码已保存"
            if let request {
                AIAccountPasswordRequests.shared.update(request.id, status: "changed")
                // 一次 request 对应一次提交；成功收起工具表单，下次 open 创建新请求。
                dismiss()
            }
        } catch {
            // 不回传任意底层错误文本或请求体；工具仅得到固定分类，密码仍只在表单内。
            let reauth: Bool
            if case MaiChatAuthError.requestFailed("Sign in again before changing the password") = error {
                reauth = true
            } else if case MaiChatAuthError.sessionExpired = error { reauth = true }
            else { reauth = false }
            message = reauth ? "请重新登录后再修改密码" : "密码修改尚未确认，请检查账号和网络后重试"
            if let request {
                AIAccountPasswordRequests.shared.update(request.id,
                    status: reauth ? "reauth_required" : "failed",
                    errorCode: reauth ? "reauth_required" : "failed")
            }
        }
    }
}
