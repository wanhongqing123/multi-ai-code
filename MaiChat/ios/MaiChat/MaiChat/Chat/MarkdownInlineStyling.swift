// 文件作用：为聊天中的 Markdown 行内元素提供一致的 SwiftUI 字体和颜色样式。
// 为消息正文中的代码、链接和强调片段指定稳定配色。
// 属性修改前先固定文本区间，避免遍历中改变富文本 run 边界。
import SwiftUI

enum MarkdownInlineStyling {
    // 这些颜色用于固定的浅色消息底板，不跟随系统外观变化。
    // 若加入深色消息底板，须把配色参数纳入 MarkdownRenderCache 的键，或改用动态颜色。
    static let codeForeground = Color(red: 0.42, green: 0.23, blue: 0.59)
    static let codeFont = Font.system(size: 13, design: .monospaced)

    static func apply(to source: AttributedString) -> AttributedString {
        var styled = source
        // 写入富文本属性会改变片段边界；修改前先保存字符范围，不能遍历正在变化的片段视图。
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
