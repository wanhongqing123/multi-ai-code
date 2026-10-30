// 文件作用：通过共享 C++ 核心保存、浏览和清理屏幕截图批次；草稿与历史按引用持有同一张原图。
import CoreImage
import CoreVideo
import Foundation
import ImageIO
import MaiChatCore
import UniformTypeIdentifiers

struct AIMobileScreenArchiveContext: Identifiable, Equatable, Sendable {
    let workspace: URL
    let ownerSessionID: String
    let batchID: String
    var id: String { batchID }

    func belongs(to workspace: URL, sessionID: String) -> Bool {
        ownerSessionID == sessionID && self.workspace.standardizedFileURL.resolvingSymlinksInPath()
            == workspace.standardizedFileURL.resolvingSymlinksInPath()
    }
}

struct AIMobileScreenArchiveBatch: Decodable, Identifiable, Sendable {
    let id: String
    let status: String
    let frameCount: Int
    let totalBytes: Int64
    let startedAtMS: Int64
    let finishedAtMS: Int64?
    let finishReason: String?
    enum CodingKeys: String, CodingKey {
        case id, status
        case frameCount = "frame_count", totalBytes = "total_bytes"
        case startedAtMS = "started_at_ms", finishedAtMS = "finished_at_ms"
        case finishReason = "finish_reason"
    }
}

struct AIMobileScreenArchiveFrame: Decodable, Identifiable, Equatable, Sendable {
    let id: String
    let batchID: String
    let capturedAtMS: Int64
    let elapsedMS: Int64
    let sizeBytes: Int64
    let width: Int
    let height: Int
    let path: String
    enum CodingKeys: String, CodingKey {
        case id, width, height, path
        case batchID = "batch_id", capturedAtMS = "captured_at_ms", elapsedMS = "elapsed_ms"
        case sizeBytes = "size_bytes"
    }
}

struct AIMobileScreenArchiveReference: Equatable, Sendable {
    let context: AIMobileScreenArchiveContext
    let frame: AIMobileScreenArchiveFrame
    let referenceID: String
    let fileURL: URL
    var relativePath: String { frame.path }
}

private struct AIMobileScreenArchiveRetainReply: Decodable {
    let frame: AIMobileScreenArchiveFrame
    let referenceID: String
    enum CodingKeys: String, CodingKey { case referenceID = "reference_id" }
    init(from decoder: Decoder) throws {
        frame = try AIMobileScreenArchiveFrame(from: decoder)
        referenceID = try decoder.container(keyedBy: CodingKeys.self).decode(String.self, forKey: .referenceID)
    }
}

private struct AIMobileScreenArchiveAcknowledgement: Decodable, Sendable {}

private struct AIMobileScreenArchiveSessionRelease: Decodable, Sendable {
    let unavailableBatchCount: Int
    enum CodingKeys: String, CodingKey { case unavailableBatchCount = "unavailable_batch_count" }
}

struct AIMobileScreenArchiveListing: Decodable, Sendable {
    let batches: [AIMobileScreenArchiveBatch]
    let frames: [AIMobileScreenArchiveFrame]
    let unavailableBatches: [String]?
    enum CodingKeys: String, CodingKey {
        case batches, frames
        case unavailableBatches = "unavailable_batches"
    }
}

struct AIMobileScreenArchiveError: LocalizedError, Sendable {
    let code: String
    let message: String
    let terminal: Bool
    let limitReason: String?
    var errorDescription: String? { message }
}

private struct AIMobileScreenArchiveBatchReply: Decodable, Sendable {
    let batch: AIMobileScreenArchiveBatch
}

private struct AIMobileScreenArchiveAppendReply: Decodable {
    let batch: AIMobileScreenArchiveBatch
    let limitReached: Bool
    let limitReason: String?
    enum CodingKeys: String, CodingKey {
        case batch, limitReached = "limit_reached", limitReason = "limit_reason"
    }
}

final class AIMobileScreenFrameArchive: @unchecked Sendable {
    static let shared = AIMobileScreenFrameArchive()

    func begin(workspace: URL, sessionID: String) async throws
        -> (AIMobileScreenArchiveContext, AIMobileScreenArchiveBatch) {
        guard workspace.isFileURL else { throw AIMobileScreenCaptureError.invalidOutput }
        let workspace = workspace.standardizedFileURL.resolvingSymlinksInPath()
        let reply: AIMobileScreenArchiveBatchReply = try await request(
            workspace: workspace, sessionID: sessionID, payload: ["action": "begin"])
        let context = AIMobileScreenArchiveContext(workspace: workspace,
            ownerSessionID: sessionID, batchID: reply.batch.id)
        do {
            var root = workspace.appendingPathComponent(".maichat/screen-frame-archive", isDirectory: true)
            guard root.resolvingSymlinksInPath().path == root.path else {
                throw AIMobileScreenCaptureError.invalidOutput
            }
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try root.setResourceValues(values)
        } catch {
            _ = try? await finish(context: context, reason: "backup_exclusion_failed")
            throw error
        }
        return (context, reply.batch)
    }

    func finish(context: AIMobileScreenArchiveContext, reason: String) async throws
        -> AIMobileScreenArchiveBatch {
        let reply: AIMobileScreenArchiveBatchReply = try await request(
            workspace: context.workspace, sessionID: context.ownerSessionID,
            payload: ["action": "finish", "batch_id": context.batchID, "reason": reason])
        return reply.batch
    }

    func list(workspace: URL, sessionID: String, batchID: String? = nil) async throws
        -> AIMobileScreenArchiveListing {
        var payload = ["action": "list"]
        if let batchID { payload["batch_id"] = batchID }
        return try await request(workspace: workspace, sessionID: sessionID, payload: payload)
    }

    func get(context: AIMobileScreenArchiveContext, frameID: String) async throws
        -> AIMobileScreenArchiveFrame {
        try await request(workspace: context.workspace, sessionID: context.ownerSessionID,
            payload: ["action": "get", "batch_id": context.batchID, "frame_id": frameID])
    }

    // Revalidate the opaque identity in the core instead of trusting a caller's
    // relative path. UI and non-Agent modules use this same ordinary entry.
    func fileURL(for frame: AIMobileScreenArchiveFrame,
                 context: AIMobileScreenArchiveContext) throws -> URL {
        guard frame.batchID == context.batchID else {
            throw AIMobileScreenArchiveError(code: "invalid_input", message: "截图不属于此批次",
                                             terminal: false, limitReason: nil)
        }
        let verified: AIMobileScreenArchiveFrame = try requestBlocking(
            workspace: context.workspace, sessionID: context.ownerSessionID,
            payload: ["action": "get", "batch_id": context.batchID, "frame_id": frame.id])
        return try Self.checkedFrameURL(verified, context: context)
    }

    func retainSelected(frameIDs: [String], context: AIMobileScreenArchiveContext,
                        usageSessionID: String? = nil) async throws -> [AIMobileScreenArchiveReference] {
        let worker = Task.detached(priority: .utility) {
            var retained: [AIMobileScreenArchiveReference] = []
            var attempted: [(frame: String, reference: String)] = []
            var visited = Set<String>()
            do {
                for id in frameIDs where visited.insert(id).inserted {
                    try Task.checkCancellation()
                    let reference = UUID().uuidString.lowercased()
                    attempted.append((id, reference))
                    var payload: [String: Any] = ["action": "retain", "batch_id": context.batchID,
                                                  "frame_id": id, "reference_id": reference]
                    if let usageSessionID { payload["usage_session_id"] = usageSessionID }
                    let reply: AIMobileScreenArchiveRetainReply = try self.requestBlocking(
                        workspace: context.workspace, sessionID: context.ownerSessionID,
                        payload: payload)
                    guard reply.referenceID == reference, reply.frame.id == id else {
                        throw AIMobileScreenArchiveError(code: "internal", message: "截图引用响应无效",
                                                         terminal: false, limitReason: nil)
                    }
                    let source = try Self.checkedFrameURL(reply.frame, context: context)
                    retained.append(AIMobileScreenArchiveReference(context: context, frame: reply.frame,
                        referenceID: reference, fileURL: source))
                }
                try Task.checkCancellation()
                return retained
            } catch {
                for attempt in attempted {
                    let _: AIMobileScreenArchiveAcknowledgement? = try? self.requestBlocking(
                        workspace: context.workspace, sessionID: context.ownerSessionID,
                        payload: ["action": "release", "batch_id": context.batchID,
                                  "frame_id": attempt.frame, "reference_id": attempt.reference])
                }
                throw error
            }
        }
        return try await withTaskCancellationHandler {
            try await worker.value
        } onCancel: { worker.cancel() }
    }

    func release(references: [AIMobileScreenArchiveReference]) async throws {
        // Cleanup must finish even if the importing task was canceled. Each
        // token is idempotent and belongs only to this acquisition.
        try await Task.detached(priority: .utility) {
            var failure: Error?
            for reference in references {
                do {
                    let _: AIMobileScreenArchiveAcknowledgement = try self.requestBlocking(
                        workspace: reference.context.workspace,
                        sessionID: reference.context.ownerSessionID,
                        payload: ["action": "release", "batch_id": reference.context.batchID,
                                  "frame_id": reference.frame.id, "reference_id": reference.referenceID])
                } catch { failure = error }
            }
            if let failure { throw failure }
        }.value
    }

    func assignSession(references: [AIMobileScreenArchiveReference], usageSessionID: String) async throws {
        for reference in references {
            let _: AIMobileScreenArchiveAcknowledgement = try await request(
                workspace: reference.context.workspace, sessionID: reference.context.ownerSessionID,
                payload: ["action": "assign_session", "batch_id": reference.context.batchID,
                          "frame_id": reference.frame.id, "reference_id": reference.referenceID,
                          "usage_session_id": usageSessionID])
        }
    }

    @discardableResult
    func releaseSession(workspace: URL, sessionID: String) async throws -> Int {
        let reply: AIMobileScreenArchiveSessionRelease = try await request(
            workspace: workspace, sessionID: sessionID, payload: ["action": "release_session"])
        return reply.unavailableBatchCount
    }

    func discard(context: AIMobileScreenArchiveContext) async throws {
        struct DiscardReply: Decodable, Sendable { let discarded: Bool }
        let _: DiscardReply = try await request(workspace: context.workspace,
            sessionID: context.ownerSessionID,
            payload: ["action": "discard", "batch_id": context.batchID])
    }

    static func checkedScreenshotURL(_ url: URL, workspace: URL) throws -> URL {
        let root = workspace.standardizedFileURL.resolvingSymlinksInPath()
        let directory = root.appendingPathComponent("screenshots", isDirectory: true)
        let output = url.standardizedFileURL
        guard workspace.isFileURL, output.isFileURL,
              directory.resolvingSymlinksInPath().path == directory.path,
              output.deletingLastPathComponent().resolvingSymlinksInPath().path == directory.path,
              output.pathExtension.lowercased() == "png",
              output.lastPathComponent.hasPrefix("mobile_screen_"),
              !FileManager.default.fileExists(atPath: output.path) else {
            throw AIMobileScreenCaptureError.invalidOutput
        }
        return output
    }

    static func writeScreenshot(_ png: Data, to url: URL, workspace: URL) throws {
        let output = try checkedScreenshotURL(url, workspace: workspace)
        guard !png.isEmpty, png.count <= 20_000_000,
              png.starts(with: [137, 80, 78, 71, 13, 10, 26, 10]) else {
            throw AIMobileScreenCaptureError.captureFailed
        }
        try FileManager.default.createDirectory(at: output.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        try png.write(to: output, options: .atomic)
        do {
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            var saved = output
            try saved.setResourceValues(values)
        } catch {
            try? FileManager.default.removeItem(at: output)
            throw error
        }
    }

    // Both capture sources use the same bounded archive writer and retain the original PNG.
    func appendPNG(_ png: Data, context: AIMobileScreenArchiveContext,
                               width: Int, height: Int, capturedAtMS: Int64, elapsedMS: Int64)
        throws -> (AIMobileScreenArchiveBatch, Bool, String?) {
        let reply: AIMobileScreenArchiveAppendReply = try requestBlocking(
            workspace: context.workspace, sessionID: context.ownerSessionID,
            payload: ["action": "append", "batch_id": context.batchID,
                      "width": width, "height": height,
                      "captured_at_ms": capturedAtMS, "elapsed_ms": elapsedMS], png: png)
        return (reply.batch, reply.limitReached, reply.limitReason)
    }

    func finishBlocking(context: AIMobileScreenArchiveContext, reason: String)
        throws -> AIMobileScreenArchiveBatch {
        let reply: AIMobileScreenArchiveBatchReply = try requestBlocking(
            workspace: context.workspace, sessionID: context.ownerSessionID,
            payload: ["action": "finish", "batch_id": context.batchID, "reason": reason])
        return reply.batch
    }

    private static func checkedFrameURL(_ frame: AIMobileScreenArchiveFrame,
                                         context: AIMobileScreenArchiveContext) throws -> URL {
        let root = context.workspace.standardizedFileURL.resolvingSymlinksInPath()
        let path = root.appendingPathComponent(frame.path).standardizedFileURL.resolvingSymlinksInPath()
        guard frame.batchID == context.batchID, !frame.path.hasPrefix("/"),
              frame.path.hasPrefix(".maichat/screen-frame-archive/"),
              path.path.hasPrefix(root.path + "/.maichat/screen-frame-archive/"),
              path.pathExtension.lowercased() == "png" else {
            throw AIMobileScreenArchiveError(code: "invalid_input", message: "截图档案路径无效",
                                             terminal: false, limitReason: nil)
        }
        return path
    }

    private func request<T: Decodable & Sendable>(workspace: URL, sessionID: String,
                                                   payload: [String: Any]) async throws -> T {
        let json = String(data: try JSONSerialization.data(withJSONObject: payload), encoding: .utf8)!
        return try await Task.detached(priority: .utility) {
            try self.requestEncoded(workspace: workspace, sessionID: sessionID, json: json, png: Data())
        }.value
    }

    private func requestBlocking<T: Decodable>(workspace: URL, sessionID: String,
                                               payload: [String: Any], png: Data = Data()) throws -> T {
        let json = String(data: try JSONSerialization.data(withJSONObject: payload), encoding: .utf8)!
        return try requestEncoded(workspace: workspace, sessionID: sessionID, json: json, png: png)
    }

    private func requestEncoded<T: Decodable>(workspace: URL, sessionID: String,
                                               json: String, png: Data) throws -> T {
        let response = workspace.path.withCString { root in
            sessionID.withCString { owner in
                json.withCString { request in
                    png.withUnsafeBytes { bytes in
                        mai_screen_frame_archive_request(root, owner, request,
                            png.isEmpty ? nil : bytes.baseAddress, bytes.count)
                    }
                }
            }
        }
        guard let response else {
            throw AIMobileScreenArchiveError(code: "internal", message: "无法读取截图档案",
                                             terminal: true, limitReason: nil)
        }
        defer { mai_screen_frame_archive_free(response) }
        let data = Data(String(cString: response).utf8)
        guard let envelope = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw AIMobileScreenArchiveError(code: "internal", message: "截图档案响应无效",
                                             terminal: true, limitReason: nil)
        }
        guard envelope["ok"] as? Bool == true, let output = envelope["output"] else {
            throw AIMobileScreenArchiveError(code: envelope["errorCode"] as? String ?? "internal",
                message: envelope["error"] as? String ?? "截图档案操作失败",
                terminal: envelope["terminal"] as? Bool ?? false,
                limitReason: envelope["limit_reason"] as? String)
        }
        return try JSONDecoder().decode(T.self, from: JSONSerialization.data(withJSONObject: output))
    }
}

// This sampler retains one valid pixel buffer, so a stationary screen is still
// saved every two seconds. ScreenCaptureKit delivery and this timer share a
// serial frame queue; inactive system-picker/app-switcher frames are excluded.
final class AIMobileScreenFrameSampler: @unchecked Sendable {
    enum Phase: Sendable { case inactive, active, background }
    private let queue: DispatchQueue
    private let lock = NSLock()
    private let renderer = CIContext()
    private var phase: Phase = .inactive
    private var phaseGeneration = 0
    private var pixels: CVPixelBuffer?
    private var externalPNG: Data?
    private var externalAt = 0.0
    private var lastExternalEncode = 0.0
    private var context: AIMobileScreenArchiveContext?
    private var timer: DispatchSourceTimer?
    private var startedAt = 0.0
    private let changed: @Sendable (AIMobileScreenArchiveBatch?, String?) -> Void
    private let terminal: @Sendable (String) -> Void

    init(queue: DispatchQueue,
         changed: @escaping @Sendable (AIMobileScreenArchiveBatch?, String?) -> Void,
         terminal: @escaping @Sendable (String) -> Void) {
        self.queue = queue
        self.changed = changed
        self.terminal = terminal
    }

    func setPhase(_ phase: Phase) {
        lock.lock()
        self.phase = phase
        phaseGeneration += 1
        // Never reuse another phase's buffer during app transitions.
        pixels = nil
        lock.unlock()
    }

    func invalidateFrame() {
        lock.lock()
        pixels = nil
        phaseGeneration += 1
        lock.unlock()
    }

    func bind(context: AIMobileScreenArchiveContext) {
        queue.async { [self] in self.context = context }
    }

    // Called only on queue, including the two-second timer.
    func accept(_ buffer: CVPixelBuffer) {
        let now = ProcessInfo.processInfo.systemUptime
        lock.lock()
        let current = phase
        let generation = phaseGeneration
        if current != .inactive { pixels = buffer }
        let external = current == .background && now - lastExternalEncode >= 1
        if external { lastExternalEncode = now }
        lock.unlock()
        if external, let encoded = encode(buffer) {
            lock.lock()
            if phase == .background && phaseGeneration == generation {
                externalPNG = encoded.data
                externalAt = now
            }
            lock.unlock()
        }
    }

    func latestExternal() throws -> (Data, Int) {
        lock.lock()
        let data = externalPNG
        let age = Int((ProcessInfo.processInfo.systemUptime - externalAt) * 1_000)
        lock.unlock()
        guard let data, age >= 0, age <= 300_000 else {
            throw AIMobileScreenCaptureError.noRecentFrame
        }
        return (data, age)
    }

    func start(context: AIMobileScreenArchiveContext) {
        queue.async { [self] in
            guard timer == nil else { return }
            self.context = context
            startedAt = ProcessInfo.processInfo.systemUptime
            let next = DispatchSource.makeTimerSource(queue: queue)
            timer = next
            next.setEventHandler { [weak self] in self?.tick() }
            next.schedule(deadline: .now() + .seconds(2), leeway: .milliseconds(10))
            next.resume()
        }
    }

    func finish(reason: String, completion: @escaping @Sendable () -> Void = {}) {
        queue.async { [self] in
            timer?.cancel()
            timer = nil
            let previous = context
            context = nil
            lock.lock()
            pixels = nil
            externalPNG = nil
            externalAt = 0
            lastExternalEncode = 0
            phase = .inactive
            lock.unlock()
            if let previous {
                do { changed(try AIMobileScreenFrameArchive.shared.finishBlocking(
                    context: previous, reason: reason), nil) }
                catch { changed(nil, error.localizedDescription) }
            }
            completion()
        }
    }

    private func tick() {
        guard timer != nil, let context else { return }
        defer {
            // The core also limits persisted wall-clock write intervals. Wait
            // after encoding/writing finishes so varying PNG costs cannot turn
            // a normal next sample into a rate-limited skipped cycle.
            timer?.schedule(deadline: .now() + .seconds(2), leeway: .milliseconds(10))
        }
        let now = ProcessInfo.processInfo.systemUptime
        lock.lock()
        let buffer = phase == .inactive ? nil : pixels
        let generation = phaseGeneration
        lock.unlock()
        guard let buffer else { return }
        guard let encoded = encode(buffer) else {
            changed(nil, "截图编码失败，已停止共享并保留此前截图")
            terminate(reason: "encode_failed")
            return
        }
        lock.lock()
        let valid = phase != .inactive && phaseGeneration == generation
        if valid && phase == .background {
            externalPNG = encoded.data
            externalAt = now
        }
        lock.unlock()
        guard valid else { return }
        do {
            let (batch, limit, reason) = try AIMobileScreenFrameArchive.shared.appendPNG(
                encoded.data, context: context, width: encoded.width, height: encoded.height,
                capturedAtMS: Int64(Date().timeIntervalSince1970 * 1_000),
                elapsedMS: Int64((now - startedAt) * 1_000))
            changed(batch, nil)
            if limit { terminate(reason: reason ?? "limit") }
        } catch let error as AIMobileScreenArchiveError where error.code == "rate_limited" {
            // The core remains authoritative about the minimum interval.
        } catch let error as AIMobileScreenArchiveError {
            changed(nil, error.localizedDescription)
            terminate(reason: error.limitReason ?? "archive_error")
        } catch {
            changed(nil, error.localizedDescription)
            terminate(reason: "archive_error")
        }
    }

    private func terminate(reason: String) {
        timer?.cancel()
        timer = nil
        terminal(reason)
    }

    private func encode(_ pixels: CVPixelBuffer) -> (data: Data, width: Int, height: Int)? {
        let image = CIImage(cvPixelBuffer: pixels)
        guard let bitmap = renderer.createCGImage(image, from: image.extent) else { return nil }
        let output = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(output,
            UTType.png.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(destination, bitmap, nil)
        guard CGImageDestinationFinalize(destination), output.length > 0,
              output.length <= 20_000_000 else { return nil }
        return (output as Data, bitmap.width, bitmap.height)
    }
}

#if DEBUG && targetEnvironment(simulator)
// The generated buffer is immutable after preparation and is read only on the
// sampler's serial queue.
private struct AIMobileScreenSamplerFixturePixels: @unchecked Sendable {
    let buffer: CVPixelBuffer
}

private final class AIMobileScreenSamplerFixtureProbe: @unchecked Sendable {
    private let lock = NSLock()
    private var savedCount = 0
    private var failure: String?
    var count: Int { lock.lock(); defer { lock.unlock() }; return savedCount }
    var error: String? { lock.lock(); defer { lock.unlock() }; return failure }
    func changed(_ batch: AIMobileScreenArchiveBatch?, _ error: String?) {
        lock.lock()
        if let batch { savedCount = batch.frameCount }
        if let error { failure = error }
        lock.unlock()
    }
}

enum AIMobileScreenArchiveSimulatorFixture {
    // Uses the production sampler and ordinary C-backed archive APIs. Only the
    // source pixel buffer is safe generated content; timing and files are real.
    static func prepare(workspace root: URL, sessionID: String,
                        pixelSize: CGSize = CGSize(width: 320, height: 180)) async throws
        -> AIMobileScreenArchiveContext {
        let archive = AIMobileScreenFrameArchive.shared
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let (context, _) = try await archive.begin(workspace: root, sessionID: sessionID)
        let queue = DispatchQueue(label: "maichat.screen-archive-fixture")
        let probe = AIMobileScreenSamplerFixtureProbe()
        let sampler = AIMobileScreenFrameSampler(queue: queue,
            changed: { probe.changed($0, $1) }, terminal: { probe.changed(nil, $0) })
        func require(_ condition: Bool, _ message: String) throws {
            if !condition { throw AIMobileScreenArchiveError(code: "fixture_failed", message: message,
                                                             terminal: false, limitReason: nil) }
        }
        func waitForFrames(_ count: Int) async throws {
            for _ in 0..<65 {
                try Task.checkCancellation()
                if probe.count >= count { return }
                if let error = probe.error { try require(false, error) }
                try await Task.sleep(for: .milliseconds(100))
            }
            try require(false, "two-second sampler did not save expected stationary frames")
        }
        func finish(_ reason: String) async {
            await withCheckedContinuation { continuation in
                sampler.finish(reason: reason) { continuation.resume() }
            }
        }
        do {
            func makePixels(width: Int, height: Int) throws -> AIMobileScreenSamplerFixturePixels {
                try require(width > 0 && height > 0 && width <= 4096 && height <= 4096,
                            "fixture pixel size is out of bounds")
                var allocated: CVPixelBuffer?
                let status = CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                    kCVPixelFormatType_32BGRA, [kCVPixelBufferCGImageCompatibilityKey: true,
                        kCVPixelBufferCGBitmapContextCompatibilityKey: true] as CFDictionary, &allocated)
                try require(status == kCVReturnSuccess && allocated != nil, "pixel buffer allocation failed")
                let pixels = allocated!
                CVPixelBufferLockBaseAddress(pixels, [])
                let memory = CVPixelBufferGetBaseAddress(pixels)!.assumingMemoryBound(to: UInt8.self)
                let stride = CVPixelBufferGetBytesPerRow(pixels)
                for y in 0..<height {
                    for x in 0..<width {
                        let offset = y * stride + x * 4
                        memory[offset] = x < width / 2 ? 35 : 240
                        memory[offset + 1] = 80
                        memory[offset + 2] = x < width / 2 ? 240 : 35
                        memory[offset + 3] = 255
                    }
                }
                CVPixelBufferUnlockBaseAddress(pixels, [])
                return AIMobileScreenSamplerFixturePixels(buffer: pixels)
            }
            let immutablePixels = try makePixels(width: Int(pixelSize.width), height: Int(pixelSize.height))
            sampler.bind(context: context)
            sampler.setPhase(.active)
            sampler.start(context: context)
            queue.async { sampler.accept(immutablePixels.buffer) }
            try await waitForFrames(2)
            let activeCount = probe.count
            sampler.setPhase(.inactive)
            try await Task.sleep(for: .milliseconds(2_200))
            try require(probe.count == activeCount, "inactive system/transition frame was archived")
            sampler.setPhase(.active)
            try await Task.sleep(for: .milliseconds(2_200))
            try require(probe.count == activeCount, "old phase buffer was reused")
            sampler.setPhase(.background)
            // Keep a landscape source as well: full-viewport fitting must not
            // stretch or crop media whose aspect ratio differs from the device.
            let landscapePixels = try makePixels(width: 320, height: 180)
            queue.async { sampler.accept(landscapePixels.buffer) }
            try await waitForFrames(activeCount + 2)
            let external = try sampler.latestExternal()
            try require(!external.0.isEmpty, "independent background frame cache was not updated")
            await finish("fixture_stop")
            let listing = try await archive.list(workspace: root, sessionID: context.ownerSessionID,
                                                 batchID: context.batchID)
            try require(listing.frames.count >= 4 && listing.batches.first?.status == "stopped",
                        "stopping did not retain the archived frames")
            for index in 1..<listing.frames.count {
                try require(listing.frames[index].elapsedMS - listing.frames[index - 1].elapsedMS >= 2_000,
                            "archive minimum interval was violated")
            }
            let first = listing.frames[0]
            let references = try await archive.retainSelected(frameIDs: [first.id], context: context)
            do {
                let expected = root.standardizedFileURL.resolvingSymlinksInPath()
                    .appendingPathComponent(first.path)
                try require(references.count == 1 && references[0].relativePath == first.path &&
                            references[0].fileURL == expected &&
                            FileManager.default.fileExists(atPath: expected.path),
                            "selected frame did not retain its original path")
                try await archive.release(references: references)
            } catch {
                try? await archive.release(references: references)
                throw error
            }
            let foreign = AIMobileScreenArchiveContext(workspace: root,
                ownerSessionID: sessionID + ":another-owner", batchID: context.batchID)
            do {
                _ = try await archive.get(context: foreign, frameID: first.id)
                try require(false, "another owner accessed the archived frame")
            } catch let error as AIMobileScreenArchiveError {
                try require(error.code != "fixture_failed", error.message)
            }
            return context
        } catch {
            await finish("fixture_failed")
            throw error
        }
    }
}

extension AIMobileScreenFrameArchive {
    func prepareScreenArchiveUITest(workspace: URL, sessionID: String,
                                   pixelSize: CGSize = CGSize(width: 320, height: 180)) async throws
        -> AIMobileScreenArchiveContext {
        try await AIMobileScreenArchiveSimulatorFixture.prepare(workspace: workspace,
            sessionID: sessionID, pixelSize: pixelSize)
    }
}
#endif
