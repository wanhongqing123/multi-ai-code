// 文件作用：合并阿里 NLS 实时识别的逐句结果，供录音会话与无网 fixture 共用。
// 中间态按句子 index 替换；完成句不被迟到的中间态覆盖，重复回包不追加重复文字。
import Foundation

struct AliyunSpeechTranscript {
    private struct Response: Decodable {
        struct Header: Decodable { let taskId: String?; let status: Int? }
        struct Payload: Decodable { let index: Int; let result: String; let time: Int? }
        let header: Header?
        let payload: Payload?
    }
    private struct Sentence { let text: String; let isFinal: Bool; let time: Int? }
    private var sentences: [Int: Sentence] = [:]
    private var taskID: String?
    private var isClosed = false
    private(set) var text = ""

    mutating func apply(json: String, sentenceFinal: Bool) throws -> Bool {
        guard !isClosed else { return false }
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        let response: Response
        do { response = try decoder.decode(Response.self, from: Data(json.utf8)) }
        catch { throw SpeechRecognitionError.service("阿里云语音识别返回的数据无效") }
        if let incoming = response.header?.taskId, !incoming.isEmpty {
            if let taskID, incoming != taskID { return false }
            taskID = incoming
        }
        if let status = response.header?.status, status != 20_000_000 {
            throw SpeechRecognitionError.service("阿里云语音识别失败（\(status)）")
        }
        guard let payload = response.payload else { return true }
        guard payload.index > 0 else {
            throw SpeechRecognitionError.service("阿里云语音识别句子编号无效")
        }
        if sentences[payload.index]?.isFinal == true && !sentenceFinal { return false }
        // SDK 回调可来自不同线程；已处理更晚音频时长后，迟到的旧中间态不能倒退文字。
        if !sentenceFinal, let oldTime = sentences[payload.index]?.time,
           let time = payload.time, time < oldTime { return false }
        sentences[payload.index] = Sentence(text: payload.result, isFinal: sentenceFinal, time: payload.time)
        text = sentences.keys.sorted().compactMap { sentences[$0]?.text }
            .joined().trimmingCharacters(in: .whitespacesAndNewlines)
        return true
    }

    mutating func complete() -> String {
        isClosed = true
        return text
    }

    mutating func cancel() {
        isClosed = true
        sentences = [:]
        text = ""
    }
}

/// NUI 在自己的线程拉取 PCM；只用短锁交换内存，不等待主线程或调用 SDK。
final class AliyunPCMBuffer: @unchecked Sendable {
    private let lock = NSLock()
    private var bytes = Data()
    private var accepting = true
    // 四秒的 16kHz Int16 单声道上限。供音跟不上时明确失败，不能无限保留麦克风内容。
    private let maximumBytes = 16_000 * 2 * 4

    func append(_ data: Data) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard accepting else { return true }
        guard bytes.count + data.count <= maximumBytes else { return false }
        bytes.append(data)
        return true
    }

    func read(into pointer: UnsafeMutablePointer<CChar>, capacity: Int) -> Int32 {
        lock.lock(); defer { lock.unlock() }
        let count = min(capacity, bytes.count)
        guard count > 0 else { return 0 }
        bytes.copyBytes(to: UnsafeMutableRawPointer(pointer).assumingMemoryBound(to: UInt8.self), count: count)
        bytes.removeFirst(count)
        return Int32(count)
    }

    func stopInput() { lock.lock(); accepting = false; lock.unlock() }
    func beginInput() { lock.lock(); bytes.removeAll(); accepting = true; lock.unlock() }
    func clear() { lock.lock(); accepting = false; bytes.removeAll(); lock.unlock() }
}
