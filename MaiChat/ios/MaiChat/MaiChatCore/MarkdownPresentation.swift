import Foundation

private final class ConversationPreviewCache: @unchecked Sendable {
    let values = NSCache<NSString, NSString>()
    init() {
        values.countLimit = 512
        values.totalCostLimit = 2 * 1024 * 1024
    }
}

public enum MarkdownConversationPreview {
    private static let cache = ConversationPreviewCache()

    public static func text(for message: RemoteIMMessage?) -> String {
        guard let message else { return "暂无消息" }
        // Keep existing attachment captions/placeholders; do not parse filenames as Markdown.
        if message.imageAttachment != nil || message.fileAttachment != nil
            || message.videoAttachment != nil || message.voiceAttachment != nil {
            return message.text
        }
        let preview = plainText(message.text)
        return preview.isEmpty ? "新消息" : preview
    }

    public static func plainText(_ markdown: String) -> String {
        let input = String(String.UnicodeScalarView(markdown.unicodeScalars.prefix(8192)))
        let key = input as NSString
        if let cached = cache.values.object(forKey: key) { return cached as String }
        var blocks: [String] = []
        if let parsed = try? AttributedString(markdown: input) {
            var blockID: Int?
            var text = ""
            var quote = false
            var list = false
            var literalStart = false
            func flush() {
                guard !text.isEmpty else { return }
                if !literalStart, list, let task = MarkdownTaskPresentation.parse(text) {
                    text = (task.checked ? "☑ " : "☐ ") + task.text
                } else if !literalStart, quote, text.hasPrefix("[!"), let close = text.firstIndex(of: "]") {
                    let marker = MarkdownQuotePresentation(String(text[...close]))
                    let rest = text[text.index(after: close)...]
                    if let kind = marker.kind, rest.isEmpty || rest.first?.isWhitespace == true {
                        text = kind.title + "：" + rest.trimmingCharacters(in: .whitespacesAndNewlines)
                    }
                }
                blocks.append(text)
                text = ""
            }
            // Full Markdown parsing removes syntax while retaining code contents.
            // Separate block identities so headings, paragraphs and table cells don't run together.
            for run in parsed.runs {
                let components = run.presentationIntent?.components ?? []
                if components.contains(where: { if case .thematicBreak = $0.kind { return true }; return false }) {
                    flush()
                    blockID = nil
                    continue
                }
                let id = components.first?.identity
                if id != blockID { flush(); blockID = id }
                if text.isEmpty {
                    quote = components.contains { if case .blockQuote = $0.kind { return true }; return false }
                    list = components.contains { if case .listItem = $0.kind { return true }; return false }
                    literalStart = run.inlinePresentationIntent?.contains(.code) == true || run.link != nil
                        || components.contains { if case .codeBlock = $0.kind { return true }; return false }
                }
                text += String(parsed[run.range].characters)
            }
            flush()
        } else {
            blocks = [input]
        }
        let compact = blocks.joined(separator: " ").split(whereSeparator: \.isWhitespace).joined(separator: " ")
        let result = compact.count > 160 ? String(compact.prefix(160)) + "…" : compact
        cache.values.setObject(result as NSString, forKey: key, cost: input.utf8.count + result.utf8.count)
        return result
    }
}

/// Presentation metadata only. Original message text is never rewritten.
public struct MarkdownQuotePresentation: Equatable, Sendable {
    public enum Kind: String, CaseIterable, Sendable {
        case note = "NOTE", tip = "TIP", important = "IMPORTANT", warning = "WARNING", caution = "CAUTION"

        public var title: String {
            switch self {
            case .note: "提示"
            case .tip: "建议"
            case .important: "重要"
            case .warning: "注意"
            case .caution: "警告"
            }
        }
    }

    public let kind: Kind?
    public let text: String

    public init(_ quote: String) {
        let normalized = quote.replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n")
        let lines = normalized.split(separator: "\n", maxSplits: 1, omittingEmptySubsequences: false)
        let first = String(lines.first ?? "").trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        if let kind = Kind.allCases.first(where: { first == "[!\($0.rawValue)]" }) {
            self.kind = kind
            self.text = lines.count > 1 ? String(lines[1]) : ""
        } else {
            self.kind = nil
            self.text = quote
        }
    }
}

public struct MarkdownTaskPresentation: Equatable, Sendable {
    public let checked: Bool
    public let text: String

    public static func parse(_ text: String) -> Self? {
        for (prefix, checked) in [("[ ]", false), ("[x]", true), ("[X]", true)] {
            guard text.hasPrefix(prefix) else { continue }
            let rest = text.dropFirst(prefix.count)
            guard rest.isEmpty || rest.first == " " || rest.first == "\t" else { return nil }
            return Self(checked: checked, text: String(rest.drop(while: { $0 == " " || $0 == "\t" })))
        }
        return nil
    }
}
