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
                // CoreLocation does not disclose whether GPS, Wi-Fi, or cellular supplied a fix.
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
            "access": access,
        ])
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
