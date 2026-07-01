import Foundation
import MultiAIIMCore

final class LocalChatHistoryStore {
    private struct StoredChatHistory: Codable {
        let schemaVersion: Int
        let sdkAppID: Int?
        let ownerUserID: String
        let messages: [RemoteIMMessage]
    }

    private let baseDirectoryURL: URL
    private let fileManager: FileManager
    private let encoder: JSONEncoder
    private let decoder: JSONDecoder

    init(
        baseDirectoryURL: URL? = nil,
        fileManager: FileManager = .default,
        encoder: JSONEncoder = JSONEncoder(),
        decoder: JSONDecoder = JSONDecoder()
    ) {
        self.fileManager = fileManager
        self.encoder = encoder
        self.decoder = decoder
        self.baseDirectoryURL = baseDirectoryURL ?? Self.defaultBaseDirectoryURL(fileManager: fileManager)
    }

    func load(sdkAppID: Int?, ownerUserID: String) -> [RemoteIMMessage] {
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanOwnerUserID.isEmpty else { return [] }

        let historyURL = fileURL(sdkAppID: sdkAppID, ownerUserID: cleanOwnerUserID)
        guard let data = try? Data(contentsOf: historyURL),
              let history = try? decoder.decode(StoredChatHistory.self, from: data),
              history.ownerUserID == cleanOwnerUserID,
              history.sdkAppID == sdkAppID
        else {
            return []
        }
        return history.messages
    }

    func save(
        messages: [RemoteIMMessage],
        sdkAppID: Int?,
        ownerUserID: String
    ) throws {
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanOwnerUserID.isEmpty else { return }

        try fileManager.createDirectory(
            at: baseDirectoryURL,
            withIntermediateDirectories: true
        )
        let history = StoredChatHistory(
            schemaVersion: 1,
            sdkAppID: sdkAppID,
            ownerUserID: cleanOwnerUserID,
            messages: messages
        )
        let data = try encoder.encode(history)
        try data.write(
            to: fileURL(sdkAppID: sdkAppID, ownerUserID: cleanOwnerUserID),
            options: .atomic
        )
    }

    private func fileURL(sdkAppID: Int?, ownerUserID: String) -> URL {
        let rawFileName = "\(sdkAppID.map(String.init) ?? "default")__\(ownerUserID)"
        let allowedCharacters = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_."))
        let fileName = rawFileName
            .addingPercentEncoding(withAllowedCharacters: allowedCharacters) ?? "history"
        return baseDirectoryURL.appendingPathComponent("\(fileName).json", isDirectory: false)
    }

    private static func defaultBaseDirectoryURL(fileManager: FileManager) -> URL {
        if let applicationSupportURL = try? fileManager.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        ) {
            return applicationSupportURL
                .appendingPathComponent("MultiAIIM", isDirectory: true)
                .appendingPathComponent("ChatHistory", isDirectory: true)
        }
        return fileManager.temporaryDirectory
            .appendingPathComponent("MultiAIIM", isDirectory: true)
            .appendingPathComponent("ChatHistory", isDirectory: true)
    }
}
