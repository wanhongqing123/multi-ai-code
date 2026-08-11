import SwiftUI

@main
struct MaiChatApp: App {
    @StateObject private var appState = RemoteIMAppState()

    init() {
        AppDiagnosticLog.shared.install()
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(appState)
        }
    }
}
