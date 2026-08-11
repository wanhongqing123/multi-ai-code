import CoreTransferable
import Foundation
import MaiChatCore
import OSLog
import UIKit
import UniformTypeIdentifiers

struct DiagnosticLogExport: Transferable, Sendable {
    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(exportedContentType: .plainText) { _ in
            let url = try await AppDiagnosticLog.shared.makeExportSnapshot()
            return SentTransferredFile(url)
        }
    }
}

@MainActor
final class AppDiagnosticLog: DiagnosticLogSink {
    static let shared = AppDiagnosticLog()

    private let logger = Logger(
        subsystem: Bundle.main.bundleIdentifier ?? "com.kongshang.maichat",
        category: "diagnostics"
    )
    private let fileStore: DiagnosticLogFileStore
    private let exportDirectoryURL: URL
    private let launchID = String(UUID().uuidString.prefix(8)).lowercased()
    private var sequence: UInt64 = 0
    private var pendingEntries: [DiagnosticLogEntry] = []
    private var writeTask: Task<Void, Never>?
    private var didRecordLaunch = false

    private static let timestampFormatter: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter
    }()

    private init(fileManager: FileManager = .default) {
        let applicationSupport = fileManager.urls(
            for: .applicationSupportDirectory,
            in: .userDomainMask
        ).first ?? fileManager.temporaryDirectory
        let logDirectoryURL = applicationSupport
            .appendingPathComponent("MaiChat", isDirectory: true)
            .appendingPathComponent("Logs", isDirectory: true)
        self.fileStore = DiagnosticLogFileStore(directoryURL: logDirectoryURL)
        self.exportDirectoryURL = fileManager.temporaryDirectory
    }

    func install() {
        guard !didRecordLaunch else { return }
        didRecordLaunch = true

        Task { [fileStore] in
            do {
                try await fileStore.prepare()
            } catch {
                Self.emitStorageFailure(error, event: "prepare-failed")
            }
        }

        let info = Bundle.main.infoDictionary
        record(
            level: .info,
            category: "app",
            event: "launch",
            fields: [
                "app_version": info?["CFBundleShortVersionString"] as? String ?? "unknown",
                "build": info?["CFBundleVersion"] as? String ?? "unknown",
                "ios": UIDevice.current.systemVersion,
                "device": UIDevice.current.model,
                "pid": String(ProcessInfo.processInfo.processIdentifier),
            ]
        )
    }

    func record(
        level: DiagnosticLogLevel,
        category: String,
        event: String,
        fields: [String: String] = [:]
    ) {
        sequence &+= 1
        var context = fields
        context["launch"] = launchID
        let entry = DiagnosticLogEntry(
            sequence: sequence,
            createdAt: Self.timestampFormatter.string(from: Date()),
            level: level,
            category: category,
            event: event,
            fields: context
        )
        emitToUnifiedLog(entry)
        pendingEntries.append(entry)
        scheduleWrite(immediately: level == .error)
    }

    func makeExportSnapshot() async throws -> URL {
        await flush()
        return try await fileStore.makeExportSnapshot(in: exportDirectoryURL)
    }

    func flush() async {
        install()
        if writeTask == nil, !pendingEntries.isEmpty {
            scheduleWrite(immediately: true)
        }
        while let writeTask {
            await writeTask.value
        }
    }

    private func scheduleWrite(immediately: Bool) {
        guard writeTask == nil else { return }
        writeTask = Task { [weak self] in
            if !immediately {
                try? await Task.sleep(for: .milliseconds(30))
            }
            guard let self else { return }
            await self.drainPendingEntries()
        }
    }

    private func drainPendingEntries() async {
        while !pendingEntries.isEmpty {
            let entries = pendingEntries
            pendingEntries.removeAll(keepingCapacity: true)
            var didWrite = false
            for attempt in 1 ... 3 {
                do {
                    try await fileStore.append(entries)
                    didWrite = true
                    break
                } catch {
                    if attempt == 3 {
                        Self.emitStorageFailure(error, event: "write-failed")
                    } else {
                        try? await Task.sleep(for: .milliseconds(100 * attempt))
                    }
                }
            }
            if !didWrite {
                // The bounded retry prevents a permanent storage failure from
                // blocking all later diagnostics while still retaining the
                // batch across transient file-protection or disk errors.
                continue
            }
        }
        writeTask = nil
        if !pendingEntries.isEmpty {
            scheduleWrite(immediately: true)
        }
    }

    private func emitToUnifiedLog(_ entry: DiagnosticLogEntry) {
        let category = entry.category
        let event = entry.event
        let fields = entry.fieldsSummary
        switch entry.level {
        case .debug:
            logger.debug(
                "[\(category, privacy: .public)] event=\(event, privacy: .public) seq=\(entry.sequence, privacy: .public) fields=\(fields, privacy: .private(mask: .hash))"
            )
        case .info:
            logger.info(
                "[\(category, privacy: .public)] event=\(event, privacy: .public) seq=\(entry.sequence, privacy: .public) fields=\(fields, privacy: .private(mask: .hash))"
            )
        case .warning:
            logger.warning(
                "[\(category, privacy: .public)] event=\(event, privacy: .public) seq=\(entry.sequence, privacy: .public) fields=\(fields, privacy: .private(mask: .hash))"
            )
        case .error:
            logger.error(
                "[\(category, privacy: .public)] event=\(event, privacy: .public) seq=\(entry.sequence, privacy: .public) fields=\(fields, privacy: .private(mask: .hash))"
            )
        }
    }

    nonisolated private static func emitStorageFailure(_ error: Error, event: String) {
        let value = error as NSError
        let logger = Logger(
            subsystem: Bundle.main.bundleIdentifier ?? "com.kongshang.maichat",
            category: "diagnostics"
        )
        logger.error(
            "[diagnostics] event=\(event, privacy: .public) domain=\(value.domain, privacy: .private(mask: .hash)) code=\(value.code, privacy: .public)"
        )
    }
}
