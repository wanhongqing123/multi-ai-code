// 文件作用：第三方身份已核验但未绑定手机号时，完成短信验证并进入同一个账号。
// binding token 只在当前页面和账号客户端内存中使用，不持久化、不显示给用户。
import SwiftUI

struct SocialPhoneBindingView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.scenePhase) private var scenePhase
    let intent: MaiChatSocialBindingIntent
    let onCancel: () -> Void

    @State private var phone = ""
    @State private var code = ""
    @State private var challengeId: String?
    @State private var acceptedAgreement = false
    @State private var confirmedMinimumAge = false
    @State private var isBusy = false
    @State private var errorMessage: String?
    @State private var information: String?
    @State private var now = Date()
    @State private var expiresAt: Date
    @State private var resendAvailableAt = Date.distantPast

    private let accent = Color(red: 42 / 255, green: 211 / 255, blue: 142 / 255)
    private let termsURL = URL(string: "https://ichat.life/legal/terms.html")!
    private let privacyURL = URL(string: "https://ichat.life/legal/privacy.html")!
    private let childrenURL = URL(string: "https://ichat.life/legal/children.html")!

    init(intent: MaiChatSocialBindingIntent, onCancel: @escaping () -> Void) {
        self.intent = intent
        self.onCancel = onCancel
        _expiresAt = State(initialValue: Date().addingTimeInterval(TimeInterval(intent.expiresIn)))
    }

    /// 一页内完成手机号、验证码和协议确认；失败时保留输入以便重试。
    var body: some View {
        GeometryReader { geometry in
            let landscape = geometry.size.width > geometry.size.height
            ScrollView {
                VStack(spacing: 0) {
                    HStack {
                        Button(action: onCancel) {
                            Image(systemName: "chevron.left")
                                .font(.system(size: 22, weight: .medium))
                                .frame(width: 44, height: 44)
                        }
                        .accessibilityLabel("返回手机号登录")
                        Spacer()
                    }
                    .foregroundStyle(.primary)

                    Color.clear.frame(height: landscape ? 4 : max(40, geometry.size.height * 0.1))
                    bindingHeading(landscape: landscape)
                    .padding(.bottom, landscape ? 8 : 38)

                    VStack(alignment: .leading, spacing: landscape ? 8 : 18) {
                        HStack(spacing: 14) {
                            Text("+86")
                                .font(.system(size: 18, weight: .semibold))
                                .foregroundStyle(.secondary)
                            Rectangle().fill(Color.black.opacity(0.08))
                                .frame(width: 1, height: 24)
                            TextField("输入手机号", text: $phone)
                                .keyboardType(.phonePad)
                                .textContentType(.telephoneNumber)
                                .accessibilityIdentifier("social-bind-phone")
                        }
                        .padding(.horizontal, 18)
                        .frame(height: landscape ? 48 : 58)
                        .background(Color(.systemGray6), in: Capsule())

                        if challengeId != nil {
                            TextField("输入验证码", text: $code)
                                .keyboardType(.numberPad)
                                .textContentType(.oneTimeCode)
                                .accessibilityIdentifier("social-bind-code")
                                .padding(.horizontal, 18)
                                .frame(height: landscape ? 48 : 58)
                                .background(Color(.systemGray6), in: Capsule())
                        }

                        HStack {
                            Text("目前仅支持中国大陆地区号码")
                                .foregroundStyle(.secondary)
                            Spacer()
                            if challengeId != nil {
                                Button(resendTitle) { Task { await sendCode() } }
                                    .disabled(isBusy || isExpired || now < resendAvailableAt)
                                    .foregroundStyle(accent)
                            }
                        }
                        .font(.system(size: 13))

                        if isExpired {
                            Text("授权已过期，请返回并重新使用\(intent.provider.displayName)登录。")
                                .font(.footnote)
                                .foregroundStyle(.red)
                        } else if let information {
                            Text(information).font(.footnote).foregroundStyle(.secondary)
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
                            .frame(height: landscape ? 46 : 56)
                            .background(accent.opacity(canSubmit ? 1 : 0.48), in: Capsule())
                            .contentShape(Capsule())
                        }
                        .buttonStyle(.plain)
                        .foregroundStyle(.white)
                        .disabled(!canSubmit)
                        .accessibilityIdentifier("social-bind-submit")

                        Button { confirmedMinimumAge.toggle() } label: {
                            HStack(spacing: 8) {
                                Image(systemName: confirmedMinimumAge ? "checkmark.circle.fill" : "circle")
                                    .foregroundStyle(confirmedMinimumAge ? accent : .secondary)
                                    .frame(width: 24, height: 24)
                                Text("我已年满 14 周岁").foregroundStyle(.secondary)
                            }
                            .font(.system(size: 12))
                        }
                        .buttonStyle(.plain)
                        .accessibilityIdentifier("social-bind-minimum-age")

                        HStack(alignment: .top, spacing: 3) {
                            Button { acceptedAgreement.toggle() } label: {
                                Image(systemName: acceptedAgreement ? "checkmark.circle.fill" : "circle")
                                    .foregroundStyle(acceptedAgreement ? accent : .secondary)
                                    .frame(width: 24, height: 24)
                            }
                            .buttonStyle(.plain)
                            .accessibilityLabel(acceptedAgreement ? "取消同意协议" : "同意协议")
                            .accessibilityIdentifier("social-bind-agreement")
                            VStack(alignment: .leading, spacing: 3) {
                                HStack(spacing: 0) {
                                    Text("已阅读并同意").foregroundStyle(.secondary)
                                    Link("《用户协议》", destination: termsURL)
                                    Link("《隐私政策》", destination: privacyURL)
                                }
                                Link("《儿童隐私保护声明》", destination: childrenURL)
                            }
                        }
                        .font(.system(size: 12))
                        .foregroundStyle(accent)
                    }
                    .padding(.horizontal, 28)
                    .frame(maxWidth: 470)
                    Spacer(minLength: landscape ? 24 : max(60, geometry.size.height * 0.1))
                }
                .frame(maxWidth: .infinity)
                .frame(minHeight: geometry.size.height)
                // 额外内容高度属于滚动区，可把验证码和协议完整滚到系统安全区上方。
                .padding(.bottom, max(geometry.safeAreaInsets.bottom, 16))
            }
            .scrollDismissesKeyboard(.interactively)
        }
        .background(Color.white.ignoresSafeArea())
        .onReceive(Timer.publish(every: 1, on: .main, in: .common).autoconnect()) { now = $0 }
        .onChange(of: scenePhase) { phase in
            if phase == .active { now = Date() }
        }
    }

    /// 横屏把说明与图标并排，按窗口宽高收紧首屏而非挪动固定纵坐标。
    @ViewBuilder
    private func bindingHeading(landscape: Bool) -> some View {
        if landscape {
            HStack(spacing: 16) {
                Image(systemName: "person.crop.circle.badge.checkmark")
                    .font(.system(size: 48, weight: .light))
                    .foregroundStyle(accent)
                    .frame(width: 58, height: 58)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 4) {
                    Text("绑定手机号").font(.title2.weight(.semibold))
                    Text("验证手机号后，即可使用\(intent.provider.displayName)登录同一个 MaiChat 账号。")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal, 28)
            .frame(maxWidth: 470, alignment: .leading)
        } else {
            Image(systemName: "person.crop.circle.badge.checkmark")
                .font(.system(size: 65, weight: .light))
                .foregroundStyle(accent)
                .frame(height: 76)
                .accessibilityHidden(true)
            VStack(spacing: 8) {
                Text("绑定手机号").font(.title2.weight(.semibold))
                Text("验证手机号后，即可使用\(intent.provider.displayName)登录同一个 MaiChat 账号。")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            .padding(.top, 22)
        }
    }

    private var isExpired: Bool { now >= expiresAt }

    private var canSubmit: Bool {
        phone.filter(\.isNumber).count == 11 && acceptedAgreement &&
            confirmedMinimumAge && !isBusy && !isExpired &&
            (challengeId == nil || code.count == 6)
    }

    private var buttonTitle: String {
        if isBusy { return "请稍候…" }
        return challengeId == nil ? "获取验证码" : "绑定并登录"
    }

    private var resendTitle: String {
        let seconds = max(0, Int(ceil(resendAvailableAt.timeIntervalSince(now))))
        return seconds == 0 ? "重新获取" : "\(seconds) 秒后重试"
    }

    private func submit() async {
        if challengeId == nil { await sendCode() }
        else { await bind() }
    }

    private func sendCode() async {
        guard phone.filter(\.isNumber).count == 11, acceptedAgreement,
              confirmedMinimumAge, !isBusy, !isExpired,
              challengeId == nil || now >= resendAvailableAt else { return }
        isBusy = true
        defer { isBusy = false }
        errorMessage = nil
        information = nil
        do {
            let challenge = try await appState.requestSocialBindingCode(
                phone: phone, ageConfirmed: confirmedMinimumAge
            )
            challengeId = challenge.challengeId
            code = ""
            resendAvailableAt = Date().addingTimeInterval(TimeInterval(challenge.resendAfter))
            information = "验证码已发送，5 分钟内有效"
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func bind() async {
        guard let challengeId, canSubmit else { return }
        isBusy = true
        defer { isBusy = false }
        errorMessage = nil
        do {
            try await appState.completeSocialBinding(
                intent: intent, phone: phone, challengeId: challengeId,
                code: code, ageConfirmed: confirmedMinimumAge
            )
        } catch {
            if case MaiChatAuthError.requestFailed("Social binding expired") = error {
                expiresAt = .distantPast
            }
            errorMessage = error.localizedDescription
        }
    }
}
