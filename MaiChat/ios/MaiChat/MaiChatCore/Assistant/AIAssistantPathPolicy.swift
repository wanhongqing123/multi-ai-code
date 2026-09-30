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
