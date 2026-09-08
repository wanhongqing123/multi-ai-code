import XCTest
import SwiftUI
@testable import MaiChatCore

final class MarkdownPresentationTests: XCTestCase {
    func testConversationPreviewUsesReadableMarkdownText() {
        let cases: [(String, String)] = [
            ("## 标题\n\n**重点** 与 `code`", "标题 重点 与 code"),
            ("[文档](https://example.com) 与 ~~旧版~~", "文档 与 旧版"),
            ("```cpp\na*b*c\npath_with_under_score\n```", "a*b*c path_with_under_score"),
            ("> [!TIP]\n> 使用 **独立目录**", "建议：使用 独立目录"),
            ("- [x] 完成\n- [ ] 待办", "☑ 完成 ☐ 待办"),
            ("| A | B |\n|---|---|\n|1|2|", "A B 1 2"),
            ("[文档][ref]\n\n[ref]: https://example.com", "文档"),
            ("![替代文字](https://example.com/a.png)", "替代文字"),
            ("`**literal**` 与 `a_b`", "**literal** 与 a_b"),
            ("```text\n> [!TIP]\n```", "> [!TIP]"),
            ("> `[!TIP]`", "[!TIP]"),
            ("## 标题\r\n\r\n下一段", "标题 下一段")
        ]
        for (source, expected) in cases {
            XCTAssertEqual(MarkdownConversationPreview.plainText(source), expected, source)
            // Repeated list refreshes return the same cached presentation.
            XCTAssertEqual(MarkdownConversationPreview.plainText(source), expected, source)
        }
    }

    func testConversationPreviewDoesNotModifyMessagesOrAttachments() {
        var message = RemoteIMMessage(fromUserID: "a", toUserID: "b", text: "## **原文**",
                                      direction: .incoming, status: .received, createdAt: Date())
        XCTAssertEqual(MarkdownConversationPreview.text(for: message), "原文")
        XCTAssertEqual(message.text, "## **原文**")
        message.text = "**更新**"
        XCTAssertEqual(MarkdownConversationPreview.text(for: message), "更新")
        XCTAssertEqual(MarkdownConversationPreview.text(for: nil), "暂无消息")
        let attachment = RemoteIMMessage(fromUserID: "a", toUserID: "b", text: "[文件消息] **report**.md",
            fileAttachment: RemoteIMFileAttachment(localFilePath: "/tmp/report.md", fileName: "report.md", mimeType: "text/markdown"),
            direction: .incoming, status: .received, createdAt: Date())
        XCTAssertEqual(MarkdownConversationPreview.text(for: attachment), attachment.text)
    }

    func testConversationPreviewClampsAtWholeEmojiBoundaries() {
        let emoji = "👨‍👩‍👧‍👦"
        XCTAssertEqual(MarkdownConversationPreview.plainText(String(repeating: emoji, count: 170)),
                       String(repeating: emoji, count: 160) + "…")
        XCTAssertLessThanOrEqual(MarkdownConversationPreview.plainText(String(repeating: "长", count: 100_000)).count, 161)
        XCTAssertEqual(MarkdownConversationPreview.plainText("---"), "")
    }

    func testInlineCodeStylesEveryRangeAndPreservesOtherTextAndLinks() throws {
        let source = try AttributedString(markdown: "`one` plain `two` **`three`** [link](https://example.com)")
        let styled = MarkdownInlineStyling.apply(to: source)
        XCTAssertEqual(String(styled.characters), String(source.characters))
        var codeTexts: [String] = []
        for run in styled.runs {
            if run.inlinePresentationIntent?.contains(.code) == true {
                codeTexts.append(String(styled[run.range].characters))
                XCTAssertEqual(run.font, MarkdownInlineStyling.codeFont)
                XCTAssertEqual(run.foregroundColor, MarkdownInlineStyling.codeForeground)
                XCTAssertEqual(run.backgroundColor, MarkdownInlineStyling.codeBackground)
            } else {
                XCTAssertNil(run.backgroundColor)
                XCTAssertNil(run.font)
            }
        }
        XCTAssertEqual(codeTexts, ["one", "two", "three"])
        XCTAssertTrue(styled.runs.contains { $0.link == URL(string: "https://example.com") })
        XCTAssertTrue(styled.runs.contains { $0.inlinePresentationIntent?.contains(.stronglyEmphasized) == true })
        XCTAssertTrue(source.runs.allSatisfy { $0.backgroundColor == nil })
    }

    func testAdjacentCodeRangesCanMergeWithoutSkippingLaterRanges() throws {
        var first = try AttributedString(markdown: "`one`")
        first.foregroundColor = .red
        var second = try AttributedString(markdown: "`two`")
        second.foregroundColor = .blue
        let source = first + second + AttributedString(" plain ") + (try AttributedString(markdown: "`three`"))
        XCTAssertEqual(String(source.characters), "onetwo plain three")
        let styled = MarkdownInlineStyling.apply(to: source)
        XCTAssertEqual(String(styled.characters), "onetwo plain three")
        let codeRuns = styled.runs.filter { $0.inlinePresentationIntent?.contains(.code) == true }
        XCTAssertEqual(codeRuns.map { String(styled[$0.range].characters) }.joined(), "onetwothree")
        for run in codeRuns {
            XCTAssertEqual(run.font, MarkdownInlineStyling.codeFont)
            XCTAssertEqual(run.foregroundColor, MarkdownInlineStyling.codeForeground)
            XCTAssertEqual(run.backgroundColor, MarkdownInlineStyling.codeBackground)
        }
    }

    func testCalloutMarkerCaseAndTrailingHorizontalWhitespace() {
        for marker in ["[!Note] ", "[!note]\t", " [!NOTE]  "] {
            let quote = MarkdownQuotePresentation(marker + "\r\n正文")
            XCTAssertEqual(quote.kind, .note)
            XCTAssertEqual(quote.text, "正文")
        }
        XCTAssertNil(MarkdownQuotePresentation("[!Note]  同行正文").kind)
    }

    func testFiveCalloutsRetainBodyFormatting() {
        for kind in MarkdownQuotePresentation.Kind.allCases {
            let quote = MarkdownQuotePresentation("[!\(kind.rawValue)]\n**重点** 与 `code`\n下一行")
            XCTAssertEqual(quote.kind, kind)
            XCTAssertEqual(quote.text, "**重点** 与 `code`\n下一行")
            XCTAssertFalse(kind.title.isEmpty)
        }
    }

    func testUnknownInlineAndOrdinaryQuotesRemainLiteral() {
        for text in ["[!UNKNOWN]\n正文", "文字 [!NOTE]", "`[!WARNING]`", "[!TIP] 不独占一行", "普通引用"] {
            let quote = MarkdownQuotePresentation(text)
            XCTAssertNil(quote.kind)
            XCTAssertEqual(quote.text, text)
        }
    }

    func testEmptyCalloutAndCRLFMarker() {
        XCTAssertEqual(MarkdownQuotePresentation("[!NOTE]").text, "")
        XCTAssertEqual(MarkdownQuotePresentation("[!NOTE]\r\n正文").kind, .note)
        XCTAssertEqual(MarkdownQuotePresentation("[!TIP]\r\n第一行\n第二行\r\n第三行").text,
                       "第一行\n第二行\n第三行")
    }

    func testTaskMarkersDoNotConsumeBodyMarkdown() {
        XCTAssertEqual(MarkdownTaskPresentation.parse("[ ] **待办**")?.checked, false)
        XCTAssertEqual(MarkdownTaskPresentation.parse("[x] `done`")?.text, "`done`")
        XCTAssertEqual(MarkdownTaskPresentation.parse("[X]\t完成")?.checked, true)
        XCTAssertEqual(MarkdownTaskPresentation.parse("[x]")?.text, "")
        XCTAssertNil(MarkdownTaskPresentation.parse("[x](https://example.com)"))
        XCTAssertNil(MarkdownTaskPresentation.parse("普通 [x] 文字"))
    }
}
