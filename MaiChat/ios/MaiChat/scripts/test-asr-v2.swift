// 文件作用：验证生产 V2 原始 JSON 解析、全文修正及取消边界，不连接腾讯或采集麦克风。
// 运行：swiftc -swift-version 6 MaiChat/Speech/SpeechRecognizing.swift scripts/test-asr-v2.swift -o /tmp/maichat-asr-v2-test
import Foundation

@main
private struct ASRV2Tests {
    private struct Failure: Error { let message: String }

    private static func require(_ value: Bool, _ message: String) throws {
        guard value else { throw Failure(message: message) }
    }

    @discardableResult
    private static func apply(_ json: String, to state: inout TencentASRV2Transcript) throws -> Bool {
        try state.apply(TencentASRV2Response.decode(json))
    }

    static func main() throws {
        var state = TencentASRV2Transcript()
        try apply(#"{"code":0,"voice_id":"voice-a"}"#, to: &state)
        try apply(#"{"code":0,"sentences":{"voice_text_str":"你好","sentence_list":[{"sentence_id":0,"sentence_type":0,"sentence":"错误的备用文本"}]}}"#, to: &state)
        try require(state.text == "你好" && !state.isStable && !state.isFinal,
                    "Authoritative full text or interim state was lost")
        try apply(#"{"code":0,"sentences":{"voice_text_str":"你好，世界","sentence_list":[{"sentence_id":0,"sentence_type":1,"sentence":"你好，世界"}]}}"#, to: &state)
        try apply(#"{"code":0,"sentences":{"voice_text_str":"您好，世界","sentence_list":[{"sentence_id":0,"sentence_type":1,"sentence":"您好，世界"}]}}"#, to: &state)
        try require(state.text == "您好，世界" && state.isStable && !state.isFinal,
                    "A stable sentence could not be corrected")

        // 全文列表每次替换，并按 ID 排序；重复回调、同 ID 修正都不能重复追加。
        let snapshot = #"{"code":0,"sentences":{"sentence_list":[{"sentence_id":1,"sentence_type":0,"sentence":"世"},{"sentence_id":0,"sentence_type":1,"sentence":"你好，"},{"sentence_id":1,"sentence_type":1,"sentence":"世界"}]}}"#
        try apply(snapshot, to: &state)
        try apply(snapshot, to: &state)
        try require(state.text == "你好，世界" && state.isStable,
                    "Sentence snapshots duplicated text or lost their order")
        try apply(#"{"code":0,"sentences":{"sentence_list":[{"sentence_id":0,"sentence_type":0,"sentence":"更短的修正"}]}}"#, to: &state)
        try require(state.text == "更短的修正" && !state.isStable,
                    "A shorter snapshot retained stale sentences")
        try apply(#"{"code":0,"sentences":{"voice_text_str":""}}"#, to: &state)
        try require(state.text.isEmpty, "Explicit empty full text did not clear the transcript")
        try apply(snapshot, to: &state)
        try apply(#"{"code":0,"sentences":{"sentence_list":[]}}"#, to: &state)
        try require(state.text.isEmpty, "Empty sentence snapshot did not clear the transcript")

        // WebSocket V2 的单句形态按 ID 更新，同一句从中间态变稳态时直接替换。
        try apply(#"{"code":0,"sentences":{"sentence_id":0,"sentence_type":0,"sentence":"中间"}}"#, to: &state)
        try apply(#"{"code":0,"sentences":{"sentence_id":0,"sentence_type":1,"sentence":"最终"}}"#, to: &state)
        try apply(#"{"code":0,"sentences":{"sentence_id":1,"sentence_type":0,"sentence":"文字"}}"#, to: &state)
        try require(state.text == "最终文字" && !state.isStable,
                    "Single sentence updates were appended instead of replaced")
        let acceptedOtherVoice = try apply(#"{"code":0,"voice_id":"voice-b","sentences":{"voice_text_str":"旧会话文字"},"final":1}"#, to: &state)
        try require(!acceptedOtherVoice && state.text == "最终文字" && !state.isFinal,
                    "A different voice stream changed the current transcript")

        try apply(#"{"code":0,"voice_id":"voice-a","final":1}"#, to: &state)
        try require(state.text == "最终文字" && state.isFinal && state.isStable,
                    "An empty final envelope erased the recognized text")
        let acceptedAfterFinal = try apply(#"{"code":0,"sentences":{"voice_text_str":"迟到"}}"#, to: &state)
        try require(!acceptedAfterFinal && state.text == "最终文字", "Late result changed final text")

        state = TencentASRV2Transcript()
        try apply(snapshot, to: &state)
        state.cancel()
        let acceptedAfterCancel = try apply(snapshot, to: &state)
        try require(!acceptedAfterCancel && state.text.isEmpty, "Cancelled session accepted a late result")
        state = TencentASRV2Transcript()
        try apply(#"{"code":0,"voice_id":"voice-b","sentences":{"voice_text_str":"新会话"}}"#, to: &state)
        try require(state.text == "新会话", "Restart retained the old voice stream")

        for invalid in [#"{"code":0,"final":3}"#,
                        #"{"code":0,"sentences":{"sentence_list":[{"sentence_id":0,"sentence_type":2,"sentence":"无效"}]}}"#,
                        #"{"sentences":{"voice_text_str":"缺少状态码"}}"#,
                        "invalid json"] {
            var rejected = false
            do { try apply(invalid, to: &state) }
            catch TencentASRV2Error.invalidResponse { rejected = true }
            try require(rejected, "Malformed V2 response was accepted")
        }
        var rejectedServerError = false
        do { try apply(#"{"code":4008,"message":"timeout"}"#, to: &state) }
        catch TencentASRV2Error.service(4008) { rejectedServerError = true }
        try require(rejectedServerError, "Server error was treated as recognized text")
        print("ASR V2 snapshots, corrections, final, error and cancellation checks passed")
    }
}
