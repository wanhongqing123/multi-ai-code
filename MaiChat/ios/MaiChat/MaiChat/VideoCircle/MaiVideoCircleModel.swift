// Video Circle UI state is shared by its toolbar entry and generated-video cards.
// The service is feature-gated: a 404 feed probe hides publishing and browsing.
import Foundation
import SwiftUI

struct MaiVideoCirclePublishSource: Identifiable, Equatable {
    let id: String
    let fileURL: URL
    let outputJSON: String
    let caption: String
    let sourceTaskID: String
    let sourceTool: String
}

enum MaiVideoCirclePresentation: Identifiable {
    case circle
    case publish(MaiVideoCirclePublishSource)

    var id: String {
        switch self {
        case .circle: "circle"
        case let .publish(source): "publish-\(source.id)"
        }
    }
}

@MainActor final class MaiVideoCircleModel: ObservableObject {
    static let shared = MaiVideoCircleModel()

    enum Availability: Equatable { case unknown, available, disabled }
    @Published private(set) var availability: Availability = .unknown
    @Published var presentation: MaiVideoCirclePresentation?
    private var probeInFlight = false

    var canPublishFromThisBuild: Bool {
        // A shared AI mark writer is required before production video bytes may
        // be uploaded. Local TLS fixture exercises the flow without public media.
        #if DEBUG && targetEnvironment(simulator)
        return ProcessInfo.processInfo.environment["MAICHAT_LOCAL_VIDEO_CIRCLE_URL"] != nil
        #else
        return false
        #endif
    }

    func probe() async {
        // Public rollout is blocked until the shared MP4 marking pipeline is
        // integrated and reviewed. Only an explicit local simulator fixture
        // may exercise this in-progress UI; installed builds expose nothing.
        guard canPublishFromThisBuild else {
            availability = .disabled
            presentation = nil
            return
        }
        guard !probeInFlight else { return }
        probeInFlight = true
        defer { probeInFlight = false }
        do {
            _ = try await MaiVideoCircleClient.shared.feed()
            availability = .available
        } catch let error as MaiVideoCircleTransportError where error.isFeatureUnavailable {
            availability = .disabled
            presentation = nil
        } catch {
            // A transient network failure cannot establish that the feature is disabled.
            availability = .unknown
        }
    }

    func reset() {
        presentation = nil
        availability = .unknown
    }
}
