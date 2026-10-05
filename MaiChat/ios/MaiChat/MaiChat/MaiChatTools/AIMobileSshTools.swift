import Foundation
import UIKit

@MainActor
extension AIMobileHostToolProvider {
    func requestSSHPassword(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let host = Self.string(arguments, key: "host")
        let username = Self.string(arguments, key: "username")
        guard !host.isEmpty, !username.isEmpty else {
            return .failure(code: "invalid_input", message: "SSH host and username are required")
        }
        guard let view = sshAlertPresenter() else {
            return .failure(code: "canceled", message: "open MaiChat to enter the SSH password")
        }
        let password: String? = await withCheckedContinuation { continuation in
            let alert = UIAlertController(title: "SSH 登录：\(username)@\(host)",
                message: "密码只用于本次连接，不会交给 AI 或写入聊天记录。",
                preferredStyle: .alert)
            alert.addTextField { field in
                field.isSecureTextEntry = true
                field.textContentType = .password
                field.placeholder = "SSH 密码"
            }
            alert.addAction(UIAlertAction(title: "取消", style: .cancel) { _ in
                continuation.resume(returning: nil)
            })
            alert.addAction(UIAlertAction(title: "连接", style: .default) { _ in
                continuation.resume(returning: alert.textFields?.first?.text)
            })
            view.present(alert, animated: true)
        }
        guard let password else {
            return .failure(code: "canceled", message: "SSH password entry was canceled")
        }
        return Self.jsonSuccess(["password": password])
    }

    func trustSSHHost(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let host = Self.string(arguments, key: "host")
        let port = arguments["port"] as? Int ?? 22
        let fingerprint = Self.string(arguments, key: "fingerprint")
        guard !host.isEmpty, (1...65535).contains(port),
              fingerprint.hasPrefix("SHA256:") else {
            return .failure(code: "invalid_input", message: "SSH host fingerprint is invalid")
        }
        let key = "ssh.host.sha256.\(host):\(port)"
        if let trusted = UserDefaults.standard.string(forKey: key) {
            return Self.jsonSuccess(["trusted": trusted == fingerprint])
        }
        guard let view = sshAlertPresenter() else {
            return .failure(code: "canceled", message: "open MaiChat to verify the SSH host")
        }
        let approved: Bool = await withCheckedContinuation { continuation in
            let alert = UIAlertController(title: "确认 SSH 服务器身份",
                message: "\(host):\(port)\n\(fingerprint)\n请与云主机控制台的指纹核对。",
                preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "取消", style: .cancel) { _ in
                continuation.resume(returning: false)
            })
            alert.addAction(UIAlertAction(title: "指纹一致", style: .default) { _ in
                continuation.resume(returning: true)
            })
            view.present(alert, animated: true)
        }
        if approved { UserDefaults.standard.set(fingerprint, forKey: key) }
        return Self.jsonSuccess(["trusted": approved])
    }

    private func sshAlertPresenter() -> UIViewController? {
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive }),
              let window = scene.windows.first(where: { $0.isKeyWindow }),
              var view = window.rootViewController else { return nil }
        while let presented = view.presentedViewController { view = presented }
        return view
    }
}
