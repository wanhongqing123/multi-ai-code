import AVFoundation
import Contacts
import CoreLocation
import EventKit
import Foundation
import Photos
import UIKit
import UserNotifications

@MainActor
extension AIMobileHostToolProvider {
    func requestSystemPermission(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
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
            switch CNContactStore.authorizationStatus(for: .contacts) {
            case .authorized: status = "granted"
            case .limited: status = "limited"
            case .denied: status = "denied"
            case .restricted: status = "restricted"
            case .notDetermined: status = "not_determined"
            @unknown default: status = "restricted"
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
        ])
    }
}

@MainActor
private final class MaiLocationPermissionRequest: NSObject, CLLocationManagerDelegate {
    private let manager = CLLocationManager()
    private var continuation: CheckedContinuation<CLAuthorizationStatus, Never>?

    var currentStatus: CLAuthorizationStatus { manager.authorizationStatus }

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
