// 文件作用：按手机号维护登录短信发送门禁，供账号状态和登录界面共用。
// 只在内存保存绝对截止时间；界面重建和后台暂停计时不会重置等待期限。
import Foundation

public struct AccountCodeCooldown {
    private var availableAtByPhone: [String: Date] = [:]
    private var requestsInFlight: Set<String> = []

    public init() {}

    public func retryAfter(phone: String, at date: Date = Date()) -> Int {
        guard let deadline = availableAtByPhone[Self.phoneKey(phone)] else { return 0 }
        return max(0, Int(ceil(deadline.timeIntervalSince(date))))
    }

    public func isRequesting(phone: String) -> Bool {
        requestsInFlight.contains(Self.phoneKey(phone))
    }

    /// 首次获取与重发使用同一个入口，避免连续点击或重建页面并发发送。
    public mutating func beginRequest(phone: String, at date: Date = Date()) -> Bool {
        availableAtByPhone = availableAtByPhone.filter { $0.value > date }
        let key = Self.phoneKey(phone)
        guard !requestsInFlight.contains(key), retryAfter(phone: phone, at: date) == 0 else {
            return false
        }
        requestsInFlight.insert(key)
        return true
    }

    /// resendAfter 只在成功发码时传入；失败传 nil，释放并发门禁以便立即重试。
    public mutating func finishRequest(
        phone: String, resendAfter: Int?, at date: Date = Date()
    ) {
        let key = Self.phoneKey(phone)
        guard requestsInFlight.remove(key) != nil else { return }
        availableAtByPhone = availableAtByPhone.filter { $0.value > date }
        if let resendAfter {
            // 服务端的较长时限优先，本机至少等待一分钟；登录提交不读取此状态。
            availableAtByPhone[key] = date.addingTimeInterval(TimeInterval(max(60, resendAfter)))
        }
    }

    private static func phoneKey(_ phone: String) -> String {
        // 与账号服务接受的大陆号码格式一致，避免空格、横线和 +86 绕开同号码门禁。
        let clean = phone.replacingOccurrences(of: " ", with: "")
            .replacingOccurrences(of: "-", with: "")
        return clean.hasPrefix("+86") ? String(clean.dropFirst(3)) : clean
    }
}
