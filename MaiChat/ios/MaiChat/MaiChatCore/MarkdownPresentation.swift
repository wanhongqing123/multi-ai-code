import Foundation

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
