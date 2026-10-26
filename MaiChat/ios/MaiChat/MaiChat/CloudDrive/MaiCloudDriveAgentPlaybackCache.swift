import Foundation

/// Owns only temporary files created for the Agent cloud-drive open tool.
/// Local media and the regular cloud-drive viewer's cache are never touched.
@MainActor final class MaiCloudDriveAgentPlaybackCache {
    static let shared = MaiCloudDriveAgentPlaybackCache()

    private let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("mai-cloud-drive-agent-playback", isDirectory: true)
    private var activeFile: URL?
    private var activeVod: MaiCloudVodSession?
    private var activeAccountID = ""

    private init() {}

    func adopt(_ file: URL, accountID: String) -> Bool {
        guard !accountID.isEmpty, owns(file) else { return false }
        releaseActive()
        activeFile = file
        activeAccountID = accountID
        return true
    }

    func adoptVod(_ vod: MaiCloudVodSession, accountID: String) -> Bool {
        guard !accountID.isEmpty else { return false }
        releaseActive()
        activeVod = vod
        activeAccountID = accountID
        return true
    }

    func presentationChanged(to path: String?) {
        if let activeVod, activeVod.displayPath != path {
            releaseActive()
            return
        }
        guard let activeFile, !isImage(activeFile) else { return }
        guard activeFile.path != path else { return }
        releaseActive()
    }

    func previewChanged(to path: String?) {
        guard let activeFile, isImage(activeFile), activeFile.path != path else { return }
        releaseActive()
    }

    func ownsAgentImage(path: String) -> Bool {
        let url = URL(fileURLWithPath: path)
        return owns(url) && isImage(url)
    }

    /// Returns true when switching accounts should also dismiss the player.
    func accountChanged(to accountID: String) -> Bool {
        guard (activeFile != nil || activeVod != nil),
              activeAccountID != accountID else { return false }
        releaseActive()
        return true
    }

    func cleanupOrphans() {
        guard let files = try? FileManager.default.contentsOfDirectory(
            at: directory, includingPropertiesForKeys: nil) else { return }
        for file in files where owns(file) || ownsDownloadTemporaryFile(file) {
            if file != activeFile { try? FileManager.default.removeItem(at: file) }
        }
    }

    private func releaseActive() {
        activeVod?.cancel()
        activeVod = nil
        if let activeFile, owns(activeFile) {
            try? FileManager.default.removeItem(at: activeFile)
        }
        activeFile = nil
        activeAccountID = ""
    }

    private func owns(_ file: URL) -> Bool {
        file.standardizedFileURL.deletingLastPathComponent() == directory.standardizedFileURL &&
            ["mp4", "mov", "jpg", "png", "webp", "heic"].contains(
                file.pathExtension.lowercased()) &&
            UUID(uuidString: file.deletingPathExtension().lastPathComponent) != nil
    }

    private func ownsDownloadTemporaryFile(_ file: URL) -> Bool {
        guard file.standardizedFileURL.deletingLastPathComponent() == directory.standardizedFileURL,
              let range = file.lastPathComponent.range(of: ".maitmp-") else { return false }
        let original = String(file.lastPathComponent[..<range.lowerBound])
        let source = URL(fileURLWithPath: original)
        return ["mp4", "mov", "jpg", "png", "webp", "heic"].contains(
            source.pathExtension.lowercased()) &&
            UUID(uuidString: source.deletingPathExtension().lastPathComponent) != nil
    }

    private func isImage(_ file: URL) -> Bool {
        ["jpg", "png", "webp", "heic"].contains(file.pathExtension.lowercased())
    }
}
