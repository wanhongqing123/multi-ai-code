import SwiftUI

@main
struct MultiAIIMApp: App {
    @StateObject private var appState = RemoteIMAppState()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(appState)
        }
    }
}
