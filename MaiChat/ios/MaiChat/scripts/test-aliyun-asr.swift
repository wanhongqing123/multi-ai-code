// 文件作用：验证正式阿里逐句状态和 SDK pull PCM FIFO；不初始化 SDK、不联网或访问麦克风。
// swiftc -swift-version 6 MaiChat/Speech/SpeechRecognizing.swift MaiChat/Speech/AliyunSpeechTranscript.swift scripts/test-aliyun-asr.swift -o /tmp/maichat-aliyun-asr-test
import Foundation

@main
private struct AliyunASRTests {
    private struct Failure: Error { let message: String }
    private static func require(_ value: Bool, _ message: String) throws {
        guard value else { throw Failure(message: message) }
    }
    static func main() throws {
        var state = AliyunSpeechTranscript()
        func update(_ index: Int, _ text: String, final: Bool, task: String = "task-a") throws {
            let payload: [String: Any] = ["header": ["task_id": task, "status": 20_000_000],
                                           "payload": ["index": index, "result": text]]
            _ = try state.apply(json: String(decoding: try JSONSerialization.data(withJSONObject: payload),
                                             as: UTF8.self), sentenceFinal: final)
        }
        try update(1, "你好世界", final: false)
        try update(1, "你好", final: false)
        try require(state.text == "你好", "Interim correction was appended")
        try update(1, "你好，", final: true)
        try update(2, "世界", final: false)
        try update(1, "过时结果", final: false)
        try require(state.text == "你好，世界", "Late partial overwrote a stable sentence")
        try update(2, "世界。", final: true)
        try update(2, "世界。", final: true)
        try update(3, "异流", final: true, task: "task-b")
        try require(state.complete() == "你好，世界。", "Duplicate/other task changed completed text")
        try update(3, "迟到", final: true)
        try require(state.text == "你好，世界。", "Late result changed final text")
        state = AliyunSpeechTranscript()
        try update(1, "取消", final: false)
        state.cancel()
        try update(1, "取消后完成", final: true)
        try require(state.text.isEmpty, "Cancelled session accepted a result")
        state = AliyunSpeechTranscript()
        try require(state.complete().isEmpty, "No-speech completion was not empty")
        state = AliyunSpeechTranscript()
        var failed = false
        do { _ = try state.apply(json: #"{"header":{"status":40000000}}"#, sentenceFinal: false) }
        catch { failed = true }
        try require(failed, "Error status was treated as recognized text")
        state = AliyunSpeechTranscript()
        _ = try state.apply(json: #"{"payload":{"index":1,"time":2000,"result":"新结果"}}"#,
                            sentenceFinal: false)
        _ = try state.apply(json: #"{"payload":{"index":1,"time":1000,"result":"过时结果"}}"#,
                            sentenceFinal: false)
        try require(state.text == "新结果", "Reentrant late partial moved the transcript backwards")

        let fifo = AliyunPCMBuffer()
        try require(fifo.append(Data([1, 2, 3, 4, 5, 6])), "PCM append failed")
        var bytes = [CChar](repeating: 0, count: 8)
        let first = bytes.withUnsafeMutableBufferPointer { fifo.read(into: $0.baseAddress!, capacity: 4) }
        try require(first == 4 && Array(bytes.prefix(4)) == [1, 2, 3, 4], "PCM pull lost order")
        try require(fifo.append(Data([7, 8])), "Second PCM append failed")
        fifo.stopInput()
        try require(fifo.append(Data([9, 10])), "Stopped tap write did not return safely")
        let last = bytes.withUnsafeMutableBufferPointer { fifo.read(into: $0.baseAddress!, capacity: 8) }
        try require(last == 4 && Array(bytes.prefix(4)) == [5, 6, 7, 8],
                    "Stop discarded queued PCM or accepted late tap bytes")
        fifo.beginInput()
        try require(!fifo.append(Data(repeating: 0, count: 128_001)), "PCM FIFO was unbounded")
        fifo.clear()
        let empty = bytes.withUnsafeMutableBufferPointer { fifo.read(into: $0.baseAddress!, capacity: 8) }
        try require(empty == 0, "Cancel retained microphone bytes")
        print("Aliyun transcript, final/error/cancel and bounded PCM pull FIFO checks passed")
    }
}
