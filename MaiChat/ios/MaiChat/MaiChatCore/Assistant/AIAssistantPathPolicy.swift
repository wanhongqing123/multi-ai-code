// 文件作用：规范创作中心工作区路径和附件路径，阻止越界访问或错误引用。
// 校验创作中心工具可访问的工作区路径。
// 路径解析拒绝越界与不可信别名，文件操作由具体工具执行。
import Foundation

public enum AIAssistantPathPolicy {
    public static func appRoot(workspacePath: String) -> String {
        #if targetEnvironment(simulator)
        if ProcessInfo.processInfo.arguments.contains("--ai-ui-test") {
            return URL(fileURLWithPath: workspacePath, isDirectory: true)
                .deletingLastPathComponent().deletingLastPathComponent().path
        }
        #endif
        return NSHomeDirectory()
    }

    public static func resolve(_ path: String, workspacePath: String,
                        appRootPath: String? = nil) -> URL? {
        guard !path.isEmpty, !workspacePath.isEmpty else { return nil }
        let workspace = URL(fileURLWithPath: workspacePath, isDirectory: true)
            .standardizedFileURL.resolvingSymlinksInPath()
        let root = URL(fileURLWithPath: appRootPath ?? appRoot(workspacePath: workspacePath),
                       isDirectory: true).standardizedFileURL.resolvingSymlinksInPath()
        let unresolved = ((path as NSString).isAbsolutePath
            ? URL(fileURLWithPath: path) : workspace.appendingPathComponent(path))
            .standardizedFileURL
        var existing = unresolved
        var suffix: [String] = []
        while !FileManager.default.fileExists(atPath: existing.path) {
            let parent = existing.deletingLastPathComponent()
            guard parent.path != existing.path else { return nil }
            suffix.insert(existing.lastPathComponent, at: 0)
            existing = parent
        }
        var candidate = existing.resolvingSymlinksInPath().standardizedFileURL
        for component in suffix { candidate.appendPathComponent(component) }
        candidate = candidate.standardizedFileURL
        guard candidate.path == root.path || candidate.path.hasPrefix(root.path + "/") else {
            return nil
        }
        return candidate
    }
}

public struct AIAssistantMediaMention: Hashable, Identifiable {
    public enum Kind: Hashable { case video, audio }

    public let url: URL
    public let kind: Kind
    public var id: String { url.path }
    public var fileName: String { url.lastPathComponent }
}

public enum AIAssistantMediaMentionPolicy {
    public static let maximumCardsPerMessage = 4

    private static let codePath = try? NSRegularExpression(
        pattern: #"`([^`\r\n]+)`"#,
        options: [.caseInsensitive]
    )
    private static let barePath = try? NSRegularExpression(
        pattern: #"[^\s`()<>\[\]{}"'“”‘’,，。;；:：|]+?\.(?:mp4|mov|wav|mp3|m4a|aac)(?![A-Za-z0-9_.])"#,
        options: [.caseInsensitive]
    )

    // 按一条已完成消息计数。五个及以上保持原文字列表；不存在、越界或代码块内
    // 的名字不产生可点击气泡，避免把清理文件的长清单变成一屏播放器。
    public static func cards(in text: String, workspacePath: String,
                             appRootPath: String? = nil) -> [AIAssistantMediaMention] {
        guard text.count <= 100_000, !workspacePath.isEmpty else { return [] }
        var result: [AIAssistantMediaMention] = []
        var seen = Set<String>()
        var inFence = false
        for lineSlice in text.split(omittingEmptySubsequences: false,
                                    whereSeparator: \.isNewline) {
            let line = String(lineSlice)
            if line.trimmingCharacters(in: .whitespaces).hasPrefix("```") {
                inFence.toggle()
                continue
            }
            if inFence || line.hasPrefix("    ") { continue }
            let bounds = NSRange(line.startIndex..<line.endIndex, in: line)
            let quoted = codePath?.matches(in: line, range: bounds) ?? []
            for match in quoted {
                guard let range = Range(match.range(at: 1), in: line) else { continue }
                add(String(line[range]), workspacePath: workspacePath,
                    appRootPath: appRootPath, seen: &seen, result: &result)
                if result.count > maximumCardsPerMessage { return [] }
            }
            for match in barePath?.matches(in: line, range: bounds) ?? [] {
                guard !quoted.contains(where: {
                    NSIntersectionRange($0.range, match.range).length > 0
                }), let range = Range(match.range, in: line) else { continue }
                add(String(line[range]), workspacePath: workspacePath,
                    appRootPath: appRootPath, seen: &seen, result: &result)
                if result.count > maximumCardsPerMessage { return [] }
            }
        }
        return result
    }

    private static func add(_ raw: String, workspacePath: String, appRootPath: String?,
                            seen: inout Set<String>,
                            result: inout [AIAssistantMediaMention]) {
        let path = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        let suffix = (path as NSString).pathExtension.lowercased()
        let kind: AIAssistantMediaMention.Kind
        switch suffix {
        case "mp4", "mov": kind = .video
        case "wav", "mp3", "m4a", "aac": kind = .audio
        default: return
        }
        guard let url = AIAssistantPathPolicy.resolve(path, workspacePath: workspacePath,
                                                      appRootPath: appRootPath),
              let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]),
              values.isRegularFile == true, (values.fileSize ?? 0) > 0,
              seen.insert(url.path).inserted else { return }
        result.append(AIAssistantMediaMention(url: url, kind: kind))
    }
}
