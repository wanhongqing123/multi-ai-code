// 文件作用：为聊天中的 Markdown 行内元素提供一致的 SwiftUI 字体和颜色样式。
// 为消息正文中的代码、链接和强调片段指定稳定配色。
// 属性修改前先固定文本区间，避免遍历中改变富文本 run 边界。
import SwiftUI

enum MarkdownInlineStyling {
    // These colors belong to the fixed pale message surfaces, not the system
    // appearance. If dark message surfaces are added, parameterize the palette
    // and include it in MarkdownRenderCache's key (or use dynamic colors).
    static let codeForeground = Color(red: 0.42, green: 0.23, blue: 0.59)
    static let codeFont = Font.system(size: 13, design: .monospaced)

    static func apply(to source: AttributedString) -> AttributedString {
        var styled = source
        // Attribute writes can change run boundaries. Snapshot the character
        // ranges before changing any attributes; never iterate a live run view.
        let codeRanges = styled.runs
            .filter { $0.inlinePresentationIntent?.contains(.code) == true }
            .map(\.range)
        for range in codeRanges {
            styled[range].font = codeFont
            styled[range].foregroundColor = codeForeground
        }
        return styled
    }
}
