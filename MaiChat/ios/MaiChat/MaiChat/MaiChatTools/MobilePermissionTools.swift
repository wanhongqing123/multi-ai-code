import AVFoundation
import Contacts
import CoreLocation
import EventKit
import Foundation
import Photos
import UIKit
import UserNotifications

private func hasLimitedContactAccess(_ status: CNAuthorizationStatus) -> Bool {
    if #available(iOS 18.0, *) { return status == .limited }
    return false
}

@MainActor
extension AIMobileHostToolProvider {
    func getCurrentLocation() async -> AIMaiChatHostToolExecution {
        guard UIApplication.shared.applicationState == .active else {
            return Self.jsonSuccess(["code": "unavailable", "message": "Open MaiChat to read the current location."])
        }
        let request = MaiCurrentLocationRequest()
        switch request.authorizationStatus {
        case .authorizedWhenInUse, .authorizedAlways: break
        case .notDetermined:
            return Self.jsonSuccess([
                "code": "permission_denied", "settings_required": false,
                "message": "Call mobile_request_permission with location first.",
            ])
        case .denied, .restricted:
            return Self.jsonSuccess([
                "code": "permission_denied", "settings_required": true,
                "message": "Enable MaiChat location access in Settings.",
            ])
        @unknown default:
            return Self.jsonSuccess(["code": "permission_denied", "settings_required": true])
        }
        switch await request.readOnce() {
        case .location(let location):
            guard location.horizontalAccuracy >= 0,
                  location.coordinate.latitude.isFinite,
                  location.coordinate.longitude.isFinite else {
                return Self.jsonSuccess(["code": "unavailable", "message": "Location accuracy is unavailable."])
            }
            var result: [String: Any] = [
                "latitude": location.coordinate.latitude,
                "longitude": location.coordinate.longitude,
                "accuracy_m": location.horizontalAccuracy,
                "timestamp_ms": Int64((location.timestamp.timeIntervalSince1970 * 1000).rounded()),
                // CoreLocation 不告知这次定位具体来自 GPS、Wi-Fi 还是蜂窝网络。
                "source": "unknown",
            ]
            if location.verticalAccuracy >= 0, location.altitude.isFinite {
                result["altitude_m"] = location.altitude
            }
            return Self.jsonSuccess(result)
        case .timeout:
            return Self.jsonSuccess(["code": "timeout", "message": "A location fix was not available within 10 seconds."])
        case .unavailable:
            return Self.jsonSuccess(["code": "unavailable", "message": "The device could not provide a location fix."])
        }
    }

    func listSystemContacts(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        // 系统通讯录不同于 MaiChat 好友；只在 App 前台且已获授权时分页读取。
        guard UIApplication.shared.applicationState == .active else {
            return .failure(code: "unavailable", message: "Open MaiChat to read system contacts")
        }
        let status = CNContactStore.authorizationStatus(for: .contacts)
        guard status == .authorized || hasLimitedContactAccess(status) else {
            return Self.jsonSuccess([
                "code": "permission_denied", "permission": "contacts",
                "settings_required": status == .denied || status == .restricted,
                "message": "Call mobile_request_permission with contacts first."
            ])
        }
        let query = Self.string(arguments, key: "query")
        let offset = arguments["offset"] as? Int ?? 0
        let limit = arguments["limit"] as? Int ?? 20
        guard query.utf8.count <= 256, offset >= 0, offset <= 1_000,
              limit >= 1, limit <= 50 else {
            return .failure(code: "invalid_input", message: "query, offset, or limit is invalid")
        }
        let access = hasLimitedContactAccess(status) ? "limited" : "full"
        return await Task.detached(priority: .userInitiated) {
            readSystemContacts(query: query, offset: offset, limit: limit, access: access)
        }.value
    }

    func getSystemContact(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        // 只接受列表返回的系统联系人 ID；有限授权下未共享的联系人不可读取。
        guard UIApplication.shared.applicationState == .active else {
            return .failure(code: "unavailable", message: "Open MaiChat to read system contacts")
        }
        let status = CNContactStore.authorizationStatus(for: .contacts)
        guard status == .authorized || hasLimitedContactAccess(status) else {
            return Self.jsonSuccess([
                "code": "permission_denied", "permission": "contacts",
                "settings_required": status == .denied || status == .restricted,
                "message": "Call mobile_request_permission with contacts first."
            ])
        }
        let identifier = Self.string(arguments, key: "id")
        guard !identifier.isEmpty, identifier.utf8.count <= 256 else {
            return .failure(code: "invalid_input", message: "a contact id is required")
        }
        return await Task.detached(priority: .userInitiated) {
            readSystemContact(identifier: identifier)
        }.value
    }

    func requestSystemPermission(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        // 按用户当前任务申请单项权限；不能一次性弹出所有系统授权。
        let permission = Self.string(arguments, key: "permission")
        guard ["photos", "camera", "microphone", "location", "contacts",
               "calendar", "notifications"].contains(permission) else {
            return .failure(code: "invalid_input",
                            message: "supported permissions: photos, camera, microphone, location, contacts, calendar, notifications")
        }
        guard UIApplication.shared.applicationState == .active else {
            return .failure(code: "not_configured",
                            message: "open MaiChat before requesting a system permission")
        }
        let status: String
        let prompted: Bool
        var access = "system"
        switch permission {
        case "photos":
            let initial = PHPhotoLibrary.authorizationStatus(for: .readWrite)
            let final = initial == .notDetermined
                ? await PHPhotoLibrary.requestAuthorization(for: .readWrite) : initial
            prompted = initial == .notDetermined
            switch final {
            case .authorized: status = "granted"
            case .limited: status = "limited"
            case .denied: status = "denied"
            case .restricted: status = "restricted"
            case .notDetermined: status = "not_determined"
            @unknown default: status = "restricted"
            }
            access = status == "granted" ? "full" : status == "limited" ? "limited" : "none"
        case "camera", "microphone":
            let media: AVMediaType = permission == "camera" ? .video : .audio
            let initial = AVCaptureDevice.authorizationStatus(for: media)
            prompted = initial == .notDetermined
            let final: AVAuthorizationStatus
            if prompted {
                let granted = await withCheckedContinuation { continuation in
                    AVCaptureDevice.requestAccess(for: media) { value in
                        continuation.resume(returning: value)
                    }
                }
                final = granted ? .authorized : AVCaptureDevice.authorizationStatus(for: media)
            } else { final = initial }
            switch final {
            case .authorized: status = "granted"
            case .denied: status = "denied"
            case .restricted: status = "restricted"
            case .notDetermined: status = "not_determined"
            @unknown default: status = "restricted"
            }
        case "location":
            let request = MaiLocationPermissionRequest()
            let initial = request.currentStatus
            prompted = initial == .notDetermined
            let final = prompted ? await request.request() : initial
            switch final {
            case .authorizedWhenInUse, .authorizedAlways: status = "granted"
            case .denied: status = "denied"
            case .restricted: status = "restricted"
            case .notDetermined: status = "not_determined"
            @unknown default: status = "restricted"
            }
            access = status == "granted" ? request.accuracyAccess : "none"
        case "contacts":
            let store = CNContactStore()
            let initial = CNContactStore.authorizationStatus(for: .contacts)
            prompted = initial == .notDetermined
            if prompted {
                _ = await withCheckedContinuation { continuation in
                    store.requestAccess(for: .contacts) { granted, _ in
                        continuation.resume(returning: granted)
                    }
                }
            }
            let final = CNContactStore.authorizationStatus(for: .contacts)
            if hasLimitedContactAccess(final) {
                status = "limited"
            } else {
                switch final {
                case .authorized: status = "granted"
                case .denied: status = "denied"
                case .restricted: status = "restricted"
                case .notDetermined: status = "not_determined"
                @unknown default: status = "restricted"
                }
            }
        case "calendar":
            let store = EKEventStore()
            let initial = EKEventStore.authorizationStatus(for: .event)
            prompted = initial == .notDetermined
            if prompted {
                if #available(iOS 17.0, *) {
                    _ = try? await store.requestFullAccessToEvents()
                } else {
                    _ = await withCheckedContinuation { continuation in
                        store.requestAccess(to: .event) { granted, _ in
                            continuation.resume(returning: granted)
                        }
                    }
                }
            }
            switch EKEventStore.authorizationStatus(for: .event) {
            case .authorized, .fullAccess: status = "granted"
            case .writeOnly: status = "limited"
            case .denied: status = "denied"
            case .restricted: status = "restricted"
            case .notDetermined: status = "not_determined"
            @unknown default: status = "restricted"
            }
        default:
            let center = UNUserNotificationCenter.current()
            let initial = await withCheckedContinuation { continuation in
                center.getNotificationSettings { settings in
                    continuation.resume(returning: settings.authorizationStatus)
                }
            }
            prompted = initial == .notDetermined
            if prompted {
                _ = try? await center.requestAuthorization(options: [.alert, .badge, .sound])
            }
            let final = await withCheckedContinuation { continuation in
                center.getNotificationSettings { settings in
                    continuation.resume(returning: settings.authorizationStatus)
                }
            }
            switch final {
            case .authorized: status = "granted"
            case .provisional, .ephemeral: status = "limited"
            case .denied: status = "denied"
            case .notDetermined: status = "not_determined"
            @unknown default: status = "restricted"
            }
        }
        return Self.jsonSuccess([
            "permission": permission, "status": status, "prompted": prompted,
            "settings_required": status == "denied" || status == "restricted",
            "access": access,
        ])
    }
}

private func systemContactKeys() -> [CNKeyDescriptor] {
    [CNContactIdentifierKey as CNKeyDescriptor,
     CNContactOrganizationNameKey as CNKeyDescriptor,
     CNContactPhoneNumbersKey as CNKeyDescriptor,
     CNContactEmailAddressesKey as CNKeyDescriptor,
     CNContactFormatter.descriptorForRequiredKeys(for: .fullName)]
}

private func systemContactSummary(_ contact: CNContact) -> [String: Any] {
    let formatted = CNContactFormatter.string(from: contact, style: .fullName) ?? ""
    let name = formatted.isEmpty ? contact.organizationName : formatted
    return [
        "id": contact.identifier,
        "name": String(name.prefix(256)),
        "phone_numbers": contact.phoneNumbers.prefix(20).map {
            String($0.value.stringValue.prefix(128))
        },
        "emails": contact.emailAddresses.prefix(20).map {
            String(($0.value as String).prefix(320))
        }
    ]
}

private func systemContactResult(_ value: [String: Any]) -> AIMaiChatHostToolExecution {
    guard JSONSerialization.isValidJSONObject(value),
          let data = try? JSONSerialization.data(withJSONObject: value),
          let text = String(data: data, encoding: .utf8) else {
        return .failure(code: "internal", message: "Could not encode system contacts")
    }
    return .success(text)
}

private func readSystemContacts(query: String, offset: Int, limit: Int,
                                access: String) -> AIMaiChatHostToolExecution {
    let store = CNContactStore()
    let fetch = CNContactFetchRequest(keysToFetch: systemContactKeys())
    fetch.sortOrder = .userDefault
    let search = query.folding(options: [.caseInsensitive, .diacriticInsensitive], locale: .current)
    let digits = query.filter(\.isNumber)
    var skipped = 0
    var items: [[String: Any]] = []
    var hasMore = false
    do {
        try store.enumerateContacts(with: fetch) { contact, stop in
            let summary = systemContactSummary(contact)
            let phones = summary["phone_numbers"] as? [String] ?? []
            let emails = summary["emails"] as? [String] ?? []
            let name = summary["name"] as? String ?? ""
            let matches = search.isEmpty || name.folding(
                options: [.caseInsensitive, .diacriticInsensitive], locale: .current
            ).contains(search) || phones.contains(where: { number in
                number.localizedCaseInsensitiveContains(query) ||
                    (digits.count >= 3 && number.filter(\.isNumber).contains(digits))
            }) || emails.contains(where: { $0.localizedCaseInsensitiveContains(query) })
            guard matches else { return }
            if skipped < offset { skipped += 1; return }
            if items.count == limit { hasMore = true; stop.pointee = true; return }
            items.append(summary)
        }
        return systemContactResult([
            "contacts": items, "offset": offset, "count": items.count,
            "has_more": hasMore, "access": access
        ])
    } catch {
        return .failure(code: "unavailable", message: "Could not read system contacts")
    }
}

private func readSystemContact(identifier: String) -> AIMaiChatHostToolExecution {
    do {
        let contact = try CNContactStore().unifiedContact(
            withIdentifier: identifier, keysToFetch: systemContactKeys()
        )
        return systemContactResult(systemContactSummary(contact))
    } catch {
        return .failure(code: "not_found", message: "Contact is unavailable or not shared")
    }
}

private enum MaiCurrentLocationResult {
    case location(CLLocation)
    case timeout
    case unavailable
}

@MainActor
private final class MaiCurrentLocationRequest: NSObject, CLLocationManagerDelegate {
    private let manager = CLLocationManager()
    private var continuation: CheckedContinuation<MaiCurrentLocationResult, Never>?
    private var timeoutTask: Task<Void, Never>?

    var authorizationStatus: CLAuthorizationStatus { manager.authorizationStatus }

    func readOnce() async -> MaiCurrentLocationResult {
        await withCheckedContinuation { continuation in
            self.continuation = continuation
            manager.delegate = self
            manager.desiredAccuracy = kCLLocationAccuracyBest
            manager.requestLocation()
            timeoutTask = Task { [weak self] in
                do { try await Task.sleep(for: .seconds(10)) }
                catch { return }
                self?.finish(.timeout)
            }
        }
    }

    private func finish(_ result: MaiCurrentLocationResult) {
        guard let continuation else { return }
        self.continuation = nil
        timeoutTask?.cancel()
        timeoutTask = nil
        manager.delegate = nil
        continuation.resume(returning: result)
    }

    nonisolated func locationManager(
        _ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]
    ) {
        Task { @MainActor in
            if let location = locations.last { self.finish(.location(location)) }
            else { self.finish(.unavailable) }
        }
    }

    nonisolated func locationManager(
        _ manager: CLLocationManager, didFailWithError error: Error
    ) {
        Task { @MainActor in self.finish(.unavailable) }
    }
}

@MainActor
private final class MaiLocationPermissionRequest: NSObject, CLLocationManagerDelegate {
    private let manager = CLLocationManager()
    private var continuation: CheckedContinuation<CLAuthorizationStatus, Never>?

    var currentStatus: CLAuthorizationStatus { manager.authorizationStatus }
    var accuracyAccess: String {
        manager.accuracyAuthorization == .fullAccuracy ? "precise" : "approximate"
    }

    func request() async -> CLAuthorizationStatus {
        await withCheckedContinuation { continuation in
            self.continuation = continuation
            manager.delegate = self
            manager.requestWhenInUseAuthorization()
        }
    }

    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        Task { @MainActor in
            guard self.manager.authorizationStatus != .notDetermined,
                  let continuation = self.continuation else { return }
            self.continuation = nil
            continuation.resume(returning: self.manager.authorizationStatus)
        }
    }
}
