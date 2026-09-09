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

    enum InteractionMetric: String, CaseIterable {
        case composerEdit = "composer-edit"
        case composerTextMutation = "composer-text-mutation"
        case composerUpdate = "composer-update"
        case composerLayout = "composer-layout"
        case composerHeightQueue = "composer-height-queue"
        case asrMainActorWait = "asr-main-actor-wait"
    }

    private let logger = Logger(
        subsystem: Bundle.main.bundleIdentifier ?? "com.kongshang.maichat",
        category: "diagnostics"
    )
    private let fileStore: DiagnosticLogFileStore
    private let exportDirectoryURL: URL
    private let launchID = String(UUID().uuidString.prefix(8)).lowercased()
    private var sequence: UInt64 = 0
    private var pendingEntries: [DiagnosticLogEntry] = []
    private var recentEntries: [DiagnosticLogEntry] = []
    private var writeTask: Task<Void, Never>?
    private var didRecordLaunch = false
    private var performanceAccountTag = ""
    private var droppedAccountEntries = 0
    private var interactionTimings: [InteractionMetric: DiagnosticTimingAccumulator] = [:]
    private var performanceTimer: Timer?
    private var performanceObservers: [NSObjectProtocol] = []
    private var lastHeartbeatUptime: TimeInterval?
    private var lastPerformanceFlushUptime: TimeInterval = 0
    private var lastDelayLogUptime: TimeInterval = 0

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
        installPerformanceSampling()

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
        recentEntries.append(entry)
        if recentEntries.count > 2048 {
            droppedAccountEntries += recentEntries.prefix(512).filter { $0.fields["account"] == performanceAccountTag }.count
            recentEntries.removeFirst(512)
        }
        pendingEntries.append(entry)
        scheduleWrite(immediately: level == .error)
    }

    func makeExportSnapshot() async throws -> URL {
        await flush()
        return try await fileStore.makeExportSnapshot(in: exportDirectoryURL)
    }

    func remoteMetadata(peerUserID: String, accountTag: String, since: Date) -> [RemoteDiagnosticsLogEntry] {
        flushInteractionTimings()
        let retained = RemoteDiagnosticsProtocol.scopedLogs(recentEntries, accountTag: accountTag, peerUserID: peerUserID, since: since, limit: 2048)
        var result = Array(retained.suffix(1000))
        result.append(RemoteDiagnosticsLogEntry(DiagnosticLogEntry(sequence: sequence,
            createdAt: Self.timestampFormatter.string(from: Date()), level: .info, category: "diagnostics",
            event: "diagnostic-log-coverage", fields: [
                "dropped_entries": String(droppedAccountEntries), "export_truncated": retained.count > 1000 ? "1" : "0",
                "retained_entries": String(retained.count), "capacity": "2048"
            ])))
        return result
    }

    func flush() async {
        install()
        flushInteractionTimings()
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

    func setPerformanceAccount(_ accountTag: String) {
        guard accountTag != performanceAccountTag else { return }
        flushInteractionTimings()
        interactionTimings.removeAll()
        performanceAccountTag = accountTag
        droppedAccountEntries = 0
        lastHeartbeatUptime = nil
    }

    func recordDuration(
        _ metric: InteractionMetric,
        since start: TimeInterval,
        until end: TimeInterval = ProcessInfo.processInfo.systemUptime
    ) {
        guard !performanceAccountTag.isEmpty else { return }
        interactionTimings[metric, default: DiagnosticTimingAccumulator()]
            .record(seconds: end - start)
    }

    private func installPerformanceSampling() {
        let center = NotificationCenter.default
        performanceObservers.append(center.addObserver(
            forName: UIApplication.didBecomeActiveNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.startPerformanceSampling() }
        })
        performanceObservers.append(center.addObserver(
            forName: UIApplication.willResignActiveNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.performanceTimer?.invalidate()
                self?.performanceTimer = nil
                self?.lastHeartbeatUptime = nil
                self?.flushInteractionTimings()
            }
        })
        if UIApplication.shared.applicationState == .active {
            startPerformanceSampling()
        }
    }

    private func startPerformanceSampling() {
        guard performanceTimer == nil else { return }
        lastHeartbeatUptime = ProcessInfo.processInfo.systemUptime
        lastPerformanceFlushUptime = lastHeartbeatUptime ?? 0
        let timer = Timer(timeInterval: 0.25, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.sampleMainRunLoop() }
        }
        timer.tolerance = 0.025
        performanceTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    private func sampleMainRunLoop() {
        guard UIApplication.shared.applicationState == .active else {
            lastHeartbeatUptime = nil
            return
        }
        let now = ProcessInfo.processInfo.systemUptime
        if let previous = lastHeartbeatUptime {
            let lateMilliseconds = max(0, now - previous - 0.25) * 1_000
            // This measures a late run-loop tick, not a stack trace or proof of
            // CPU work. Do not count background suspension as a foreground stall.
            if lateMilliseconds >= 150, now - lastDelayLogUptime >= 2 {
                lastDelayLogUptime = now
                record(level: .warning, category: "interaction-performance",
                       event: "main-runloop-delay", fields: [
                        "account": performanceAccountTag, "scope": "account-process",
                        "late_ms": String(Int(lateMilliseconds.rounded())),
                        "sample_interval_ms": "250",
                       ])
            }
        }
        lastHeartbeatUptime = now
        if now - lastPerformanceFlushUptime >= 2 {
            lastPerformanceFlushUptime = now
            flushInteractionTimings()
        }
    }

    private func flushInteractionTimings() {
        for metric in InteractionMetric.allCases {
            guard let snapshot = interactionTimings[metric]?.takeSnapshot() else { continue }
            record(level: .info, category: "interaction-performance",
                   event: metric.rawValue, fields: [
                    "account": performanceAccountTag, "scope": "account-process",
                    "samples": String(snapshot.sampleCount),
                    "slow_samples": String(snapshot.slowSampleCount),
                    "slow_threshold_ms": "16",
                    "max_ms": String(format: "%.2f", snapshot.maximumMilliseconds),
                    "average_ms": String(format: "%.2f", snapshot.averageMilliseconds),
                   ])
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
