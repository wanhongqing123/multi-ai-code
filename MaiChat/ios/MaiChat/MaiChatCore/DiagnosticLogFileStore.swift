import Foundation

public actor DiagnosticLogFileStore {
    private let fileManager: FileManager
    private let logDirectoryURL: URL
    private let maximumFileBytes: UInt64
    private let maximumTotalBytes: UInt64
    private let retentionDays: Int
    private var currentDay = ""
    private var currentLogURL: URL?
    private var fileHandle: FileHandle?
    private var rotationIndex = 0

    private static let dayFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = .current
        formatter.dateFormat = "yyyyMMdd"
        return formatter
    }()

    private static let rotationFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = .current
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        return formatter
    }()

    public init(
        directoryURL: URL,
        maximumFileBytes: UInt64 = 5 * 1024 * 1024,
        maximumTotalBytes: UInt64 = 50 * 1024 * 1024,
        retentionDays: Int = 7,
        fileManager: FileManager = .default
    ) {
        self.logDirectoryURL = directoryURL
        self.maximumFileBytes = max(maximumFileBytes, 1)
        self.maximumTotalBytes = max(maximumTotalBytes, 1)
        self.retentionDays = max(retentionDays, 1)
        self.fileManager = fileManager
    }

    public func prepare(at date: Date = Date()) throws {
        try prepareDirectory()
        try pruneFiles(at: date)
    }

    public func append(
        _ entries: [DiagnosticLogEntry],
        at date: Date = Date()
    ) throws {
        guard !entries.isEmpty else { return }
        try prepareDirectory()
        for entry in entries {
            let payload = Data(Self.line(for: entry).utf8)
            try prepareFile(for: date, incomingByteCount: UInt64(payload.count))
            try fileHandle?.write(contentsOf: payload)
        }
        try fileHandle?.synchronize()
        if let currentLogURL {
            try? fileManager.setAttributes(
                [.modificationDate: date],
                ofItemAtPath: currentLogURL.path
            )
        }
        try pruneFiles(at: date)
    }

    public func makeExportSnapshot(
        in exportDirectoryURL: URL,
        at date: Date = Date()
    ) throws -> URL {
        try fileHandle?.synchronize()
        try prepareDirectory()
        try pruneFiles(at: date)
        try fileManager.createDirectory(
            at: exportDirectoryURL,
            withIntermediateDirectories: true
        )
        removeExpiredExports(in: exportDirectoryURL, at: date)

        let exportURL = exportDirectoryURL.appendingPathComponent(
            "MaiChat-Diagnostics-\(UUID().uuidString.lowercased()).log",
            isDirectory: false
        )
        var output = Data()
        for fileURL in retainedLogFiles() {
            if !output.isEmpty {
                output.append(Data("\n".utf8))
            }
            let data = try Data(contentsOf: fileURL)
            output.append(data)
            if data.last != 0x0A {
                output.append(Data("\n".utf8))
            }
        }
        if output.isEmpty {
            output = Data("No diagnostic log entries are available.\n".utf8)
        }
        try output.write(to: exportURL, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
        return exportURL
    }

    public func logFileURLs() -> [URL] {
        retainedLogFiles()
    }

    private static func line(for entry: DiagnosticLogEntry) -> String {
        let levelMarker: String
        switch entry.level {
        case .debug: levelMarker = "D"
        case .info: levelMarker = "I"
        case .warning: levelMarker = "W"
        case .error: levelMarker = "E"
        }
        return "\(entry.createdAt) [\(levelMarker)] seq=\(entry.sequence) \(entry.summary)\n"
    }

    private func prepareDirectory() throws {
        try fileManager.createDirectory(
            at: logDirectoryURL,
            withIntermediateDirectories: true
        )
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        var directoryURL = logDirectoryURL
        try directoryURL.setResourceValues(values)
    }

    private func prepareFile(for date: Date, incomingByteCount: UInt64) throws {
        let day = Self.dayFormatter.string(from: date)
        if day != currentDay || fileHandle == nil {
            try closeCurrentFile()
            let changedDay = !currentDay.isEmpty && day != currentDay
            currentDay = day
            currentLogURL = logDirectoryURL.appendingPathComponent("maichat-ios-\(day).log")
            rotationIndex = 0
            try openCurrentFile()
            if changedDay {
                try pruneFiles(at: date)
            }
        }

        let currentSize = currentLogURL.flatMap { try? fileSize(at: $0) } ?? 0
        if currentSize + incomingByteCount > maximumFileBytes, currentSize > 0 {
            try rotateCurrentFile(at: date)
        }
    }

    private func openCurrentFile() throws {
        guard let currentLogURL else { return }
        if !fileManager.fileExists(atPath: currentLogURL.path) {
            guard fileManager.createFile(
                atPath: currentLogURL.path,
                contents: nil,
                attributes: [
                    .posixPermissions: 0o600,
                    .protectionKey: FileProtectionType.completeUntilFirstUserAuthentication,
                ]
            ) else {
                throw CocoaError(.fileWriteUnknown)
            }
        }
        let handle = try FileHandle(forWritingTo: currentLogURL)
        try handle.seekToEnd()
        fileHandle = handle
    }

    private func closeCurrentFile() throws {
        if let fileHandle {
            try fileHandle.synchronize()
            try fileHandle.close()
        }
        fileHandle = nil
    }

    private func rotateCurrentFile(at date: Date) throws {
        guard let currentLogURL else { return }
        try closeCurrentFile()
        let timestamp = Self.rotationFormatter.string(from: date)
        var rotatedURL: URL
        repeat {
            rotationIndex += 1
            rotatedURL = logDirectoryURL.appendingPathComponent(
                "maichat-ios-\(timestamp)-\(String(format: "%04d", rotationIndex)).log"
            )
        } while fileManager.fileExists(atPath: rotatedURL.path)
        try fileManager.moveItem(at: currentLogURL, to: rotatedURL)
        try openCurrentFile()
    }

    private func pruneFiles(at date: Date) throws {
        let expirationDate = Calendar.current.date(
            byAdding: .day,
            value: -retentionDays,
            to: date
        ) ?? .distantPast
        if let currentLogURL,
           let currentMetadata = metadata(for: currentLogURL),
           currentMetadata.modifiedAt < expirationDate
        {
            try closeCurrentFile()
            self.currentLogURL = nil
            currentDay = ""
            rotationIndex = 0
        }
        var files = retainedLogFilesWithMetadata()
        for file in files where file.url != currentLogURL && file.modifiedAt < expirationDate {
            try? fileManager.removeItem(at: file.url)
        }

        files = retainedLogFilesWithMetadata().sorted { $0.modifiedAt < $1.modifiedAt }
        var totalBytes = files.reduce(UInt64(0)) { $0 + $1.size }
        for file in files where totalBytes > maximumTotalBytes && file.url != currentLogURL {
            do {
                try fileManager.removeItem(at: file.url)
                totalBytes = totalBytes >= file.size ? totalBytes - file.size : 0
            } catch {
                continue
            }
        }
    }

    private func retainedLogFiles() -> [URL] {
        retainedLogFilesWithMetadata()
            .map(\.url)
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    private func retainedLogFilesWithMetadata() -> [(url: URL, modifiedAt: Date, size: UInt64)] {
        let urls = (try? fileManager.contentsOfDirectory(
            at: logDirectoryURL,
            includingPropertiesForKeys: [.contentModificationDateKey, .fileSizeKey],
            options: [.skipsHiddenFiles]
        )) ?? []
        return urls.compactMap(metadata(for:))
    }

    private func metadata(for url: URL) -> (url: URL, modifiedAt: Date, size: UInt64)? {
        guard url.pathExtension == "log",
              url.lastPathComponent.hasPrefix("maichat-ios-")
        else {
            return nil
        }
        let values = try? url.resourceValues(
            forKeys: [.contentModificationDateKey, .fileSizeKey]
        )
        return (
            url,
            values?.contentModificationDate ?? .distantPast,
            UInt64(max(values?.fileSize ?? 0, 0))
        )
    }

    private func fileSize(at url: URL) throws -> UInt64 {
        let attributes = try fileManager.attributesOfItem(atPath: url.path)
        return (attributes[.size] as? NSNumber)?.uint64Value ?? 0
    }

    private func removeExpiredExports(in directoryURL: URL, at date: Date) {
        let expirationDate = date.addingTimeInterval(-24 * 60 * 60)
        let urls = (try? fileManager.contentsOfDirectory(
            at: directoryURL,
            includingPropertiesForKeys: [.contentModificationDateKey],
            options: [.skipsHiddenFiles]
        )) ?? []
        for url in urls where url.lastPathComponent.hasPrefix("MaiChat-Diagnostics-") {
            let modifiedAt = (try? url.resourceValues(
                forKeys: [.contentModificationDateKey]
            ).contentModificationDate) ?? .distantPast
            if modifiedAt < expirationDate {
                try? fileManager.removeItem(at: url)
            }
        }
    }
}
