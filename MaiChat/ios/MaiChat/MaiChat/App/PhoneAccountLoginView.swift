// 文件作用：展示手机号登录界面，处理验证码、密码登录和协议确认的用户交互。
// 阅读边界：网络鉴权由 MaiChatAuthClient 执行，本文件负责输入状态和错误提示。
// 手机号、验证码、密码和协议勾选在此页呈现；提交动作交给账号服务。
import SwiftUI
import UIKit

struct PhoneAccountLoginView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.dismiss) private var dismiss
    @State private var phone = ""
    @State private var code = ""
    @State private var password = ""
    @State private var challengeId: String?
    @State private var usesPassword = false
    @State private var acceptedAgreement = false
    @State private var confirmedMinimumAge = false
    @State private var isBusy = false
    @State private var errorMessage: String?
    @State private var information: String?
    @State private var socialBinding: MaiChatSocialBindingIntent?
    @State private var didInjectSocialCredential = false
    @State private var isSocialLoggingIn = false

    private let accent = Color(red: 42 / 255, green: 211 / 255, blue: 142 / 255)
    private let termsURL = URL(string: "https://ichat.life/legal/terms.html")!
    private let privacyURL = URL(string: "https://ichat.life/legal/privacy.html")!
    private let childrenURL = URL(string: "https://ichat.life/legal/children.html")!

    var body: some View {
        Group {
            if let socialBinding {
                SocialPhoneBindingView(intent: socialBinding) {
                    self.socialBinding = nil
                    information = nil
                }
            } else {
                phoneLoginContent
            }
        }
        .task { await injectLocalSocialCredentialIfNeeded() }
    }

    private var phoneLoginContent: some View {
        GeometryReader { geometry in
            ScrollView {
                VStack(spacing: 0) {
                    HStack {
                        if challengeId != nil || usesPassword {
                            Button {
                                challengeId = nil
                                usesPassword = false
                                errorMessage = nil
                            } label: {
                                Image(systemName: "chevron.left")
                                    .font(.system(size: 22, weight: .medium))
                                    .frame(width: 44, height: 44)
                            }
                            .accessibilityLabel("返回手机号登录")
                        }
                        Spacer()
                    }
                    .foregroundStyle(.primary)
                    Color.clear.frame(height: max(60, geometry.size.height * 0.16))
                    Image(uiImage: Self.applicationIcon)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(width: 92, height: 92)
                        .clipShape(RoundedRectangle(cornerRadius: 24))
                        .accessibilityHidden(true)
                    Color.clear.frame(height: max(70, geometry.size.height * 0.1))
                    VStack(alignment: .leading, spacing: 20) {
                        HStack(spacing: 14) {
                            Text("+86")
                                .font(.system(size: 18, weight: .semibold))
                                .foregroundStyle(.secondary)
                            Rectangle().fill(Color.black.opacity(0.08))
                                .frame(width: 1, height: 24)
                            TextField("输入手机号", text: $phone)
                                .keyboardType(.phonePad)
                                .textContentType(.telephoneNumber)
                                .accessibilityIdentifier("account-phone")
                        }
                        .padding(.horizontal, 18)
                        .frame(height: 58)
                        .background(Color(.systemGray6), in: Capsule())
                        if challengeId != nil && !usesPassword {
                            TextField("输入验证码", text: $code)
                                .keyboardType(.numberPad)
                                .textContentType(.oneTimeCode)
                                .accessibilityIdentifier("account-code")
                                .padding(.horizontal, 18)
                                .frame(height: 58)
                                .background(Color(.systemGray6), in: Capsule())
                        }
                        if usesPassword {
                            SecureField("输入登录密码", text: $password)
                                .textContentType(.password)
                                .padding(.horizontal, 18)
                                .frame(height: 58)
                                .background(Color(.systemGray6), in: Capsule())
                        }
                        HStack {
                            Text("目前仅支持中国大陆地区号码")
                                .foregroundStyle(.secondary)
                            Spacer()
                            if challengeId != nil && !usesPassword {
                                Button("重新获取") { Task { await sendCode() } }
                                    .disabled(isBusy || !acceptedAgreement || !confirmedMinimumAge)
                                    .foregroundStyle(accent)
                            } else {
                                Button(usesPassword ? "验证码登录" : "密码登录") {
                                    usesPassword.toggle()
                                    challengeId = nil
                                    errorMessage = nil
                                }
                                .foregroundStyle(accent)
                            }
                        }
                        .font(.system(size: 13))
                        if let information {
                            Text(information).font(.footnote).foregroundStyle(.secondary)
                        }
                        if let accountError = appState.errorMessage,
                           appState.shouldShowInitialLogin {
                            Text(accountError).font(.footnote).foregroundStyle(.red)
                        }
                        if let errorMessage {
                            Text(errorMessage).font(.footnote).foregroundStyle(.red)
                        }
                        Button { Task { await submit() } } label: {
                            HStack(spacing: 8) {
                                if isBusy { ProgressView().tint(.white) }
                                Text(buttonTitle).font(.system(size: 18, weight: .semibold))
                            }
                            .frame(maxWidth: .infinity)
                            .frame(height: 56)
                            .background(accent.opacity(canSubmit ? 1 : 0.48), in: Capsule())
                            .contentShape(Capsule())
                        }
                        .buttonStyle(.plain)
                        .foregroundStyle(.white)
                        .disabled(!canSubmit)
                        .accessibilityIdentifier("account-submit")
                        Button { confirmedMinimumAge.toggle() } label: {
                            HStack(spacing: 8) {
                                Image(systemName: confirmedMinimumAge ? "checkmark.circle.fill" : "circle")
                                    .foregroundStyle(confirmedMinimumAge ? accent : .secondary)
                                    .frame(width: 24, height: 24)
                                Text("我已年满 14 周岁")
                                    .foregroundStyle(Color.secondary)
                            }
                            .font(.system(size: 12))
                        }
                        .buttonStyle(.plain)
                        .accessibilityIdentifier("account-minimum-age")
                        HStack(alignment: .top, spacing: 3) {
                            Button { acceptedAgreement.toggle() } label: {
                                Image(systemName: acceptedAgreement ? "checkmark.circle.fill" : "circle")
                                    .foregroundStyle(acceptedAgreement ? accent : .secondary)
                                    .frame(width: 24, height: 24)
                            }
                            .buttonStyle(.plain)
                            .accessibilityLabel(acceptedAgreement ? "取消同意协议" : "同意协议")
                            .accessibilityIdentifier("account-agreement")
                            VStack(alignment: .leading, spacing: 3) {
                                HStack(spacing: 0) {
                                    Text("已阅读并同意")
                                        .foregroundStyle(Color.secondary)
                                    Link("《用户协议》", destination: termsURL)
                                        .accessibilityIdentifier("account-terms")
                                    Link("《隐私政策》", destination: privacyURL)
                                        .accessibilityIdentifier("account-privacy")
                                }
                                Link("《儿童隐私保护声明》", destination: childrenURL)
                                    .accessibilityIdentifier("account-children-privacy")
                            }
                        }
                        .font(.system(size: 12))
                        .foregroundStyle(accent)
                    }
                    .padding(.horizontal, 28)
                    .frame(maxWidth: 470)
                    Spacer(minLength: max(100, geometry.size.height * 0.19))
                }
                .frame(maxWidth: .infinity)
                .frame(minHeight: geometry.size.height)
            }
            .scrollDismissesKeyboard(.interactively)
        }
        .background(Color.white.ignoresSafeArea())
    }

    private var canSubmit: Bool {
        phone.filter(\.isNumber).count == 11 && acceptedAgreement && confirmedMinimumAge && !isBusy &&
            (!usesPassword || !password.isEmpty) &&
            (challengeId == nil || usesPassword || code.count == 6)
    }

    private var buttonTitle: String {
        if isBusy { return "请稍候…" }
        return challengeId == nil && !usesPassword ? "获取验证码" : "登录"
    }

    private func submit() async {
        if challengeId == nil && !usesPassword { await sendCode() }
        else { await login() }
    }

    private func sendCode() async {
        guard confirmedMinimumAge, acceptedAgreement else { return }
        isBusy = true
        defer { isBusy = false }
        appState.errorMessage = nil
        errorMessage = nil
        information = nil
        do {
            let challenge = try await appState.requestAccountCode(
                phone: phone, ageConfirmed: confirmedMinimumAge
            )
            challengeId = challenge.challengeId
            information = "验证码已发送，5 分钟内有效"
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func login() async {
        guard confirmedMinimumAge, acceptedAgreement else { return }
        isBusy = true
        defer { isBusy = false }
        appState.errorMessage = nil
        errorMessage = nil
        do {
            if usesPassword {
                guard !password.isEmpty else { errorMessage = "请输入密码"; return }
                try await appState.loginWithAccountPassword(
                    phone: phone, password: password, ageConfirmed: confirmedMinimumAge
                )
            } else {
                guard let challengeId, code.count == 6 else {
                    errorMessage = "请输入 6 位验证码"
                    return
                }
                try await appState.loginWithAccountCode(
                    phone: phone, challengeId: challengeId, code: code,
                    ageConfirmed: confirmedMinimumAge
                )
            }
            dismiss()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    /// 将正式 SDK 交来的凭据交给账号服务；绑手机时在当前登录容器切换页面。
    private func startSocialLogin(
        _ credential: MaiChatSocialCredential, ageConfirmed: Bool
    ) async {
        guard !isBusy, !isSocialLoggingIn else { return }
        isSocialLoggingIn = true
        defer { isSocialLoggingIn = false }
        errorMessage = nil
        information = "正在验证\(credential.provider.displayName)账号…"
        do {
            let outcome = try await appState.loginWithSocialCredential(
                credential, ageConfirmed: ageConfirmed
            )
            switch outcome {
            case .signedIn:
                dismiss()
            case let .phoneBindingRequired(intent):
                information = nil
                socialBinding = intent
            }
        } catch {
            information = nil
            errorMessage = error.localizedDescription
        }
    }

    /// 只在 Debug 模拟器注入本机假 SDK 凭据；正式包不展示无配置的第三方入口。
    private func injectLocalSocialCredentialIfNeeded() async {
        #if DEBUG && targetEnvironment(simulator)
        let environment = ProcessInfo.processInfo.environment
        guard !didInjectSocialCredential,
              environment["MAICHAT_LOCAL_UI_SOCIAL_TEST"] == "1",
              environment["MAICHAT_LOCAL_UI_PHONE"] != nil,
              let provider = environment["MAICHAT_LOCAL_UI_SOCIAL_PROVIDER"],
              let value = environment["MAICHAT_LOCAL_UI_SOCIAL_CREDENTIAL"],
              !value.isEmpty else { return }
        let credential: MaiChatSocialCredential
        switch provider {
        case "wechat": credential = .wechatAuthorizationCode(value)
        case "qq": credential = .qqSDKAccessToken(value)
        default: return
        }
        didInjectSocialCredential = true
        await startSocialLogin(credential, ageConfirmed: true)
        #endif
    }

    private static var applicationIcon: UIImage {
        guard let icons = Bundle.main.object(forInfoDictionaryKey: "CFBundleIcons") as? [String: Any],
              let primary = icons["CFBundlePrimaryIcon"] as? [String: Any],
              let names = primary["CFBundleIconFiles"] as? [String],
              let name = names.last,
              let image = UIImage(named: name) else { return UIImage() }
        return image
    }
}
