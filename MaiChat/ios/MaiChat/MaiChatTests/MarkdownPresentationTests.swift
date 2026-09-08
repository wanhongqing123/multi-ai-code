import XCTest
import SwiftUI
@testable import MaiChatCore

final class MarkdownPresentationTests: XCTestCase {
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
