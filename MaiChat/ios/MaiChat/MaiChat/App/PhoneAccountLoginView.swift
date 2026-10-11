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

    private let accent = Color(red: 42 / 255, green: 211 / 255, blue: 142 / 255)
    private let termsURL = URL(string: "https://ichat.life/legal/terms.html")!
    private let privacyURL = URL(string: "https://ichat.life/legal/privacy.html")!
    private let childrenURL = URL(string: "https://ichat.life/legal/children.html")!

    var body: some View {
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
                        }
                        .buttonStyle(.plain)
                        .foregroundStyle(.white)
                        .background(accent.opacity(canSubmit ? 1 : 0.48), in: Capsule())
                        .disabled(!canSubmit)
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

    private static var applicationIcon: UIImage {
        guard let icons = Bundle.main.object(forInfoDictionaryKey: "CFBundleIcons") as? [String: Any],
              let primary = icons["CFBundlePrimaryIcon"] as? [String: Any],
              let names = primary["CFBundleIconFiles"] as? [String],
              let name = names.last,
              let image = UIImage(named: name) else { return UIImage() }
        return image
    }
}
