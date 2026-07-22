import SwiftUI

@main
struct MaiChatApp: App {
    @StateObject private var appState = RemoteIMAppState()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(appState)
        }
    }
}
