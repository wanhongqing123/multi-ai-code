import Foundation
import SwiftUI
import UIKit
import WebKit

// The Agent owns one visible, isolated browser page in the selected local Session.
// Fixed scripts inspect and act on the page; model input is never evaluated as JavaScript.
@MainActor
final class AIMobileBrowserTools: NSObject, ObservableObject, WKNavigationDelegate {
    static let shared = AIMobileBrowserTools()

    @Published private(set) var ownerSessionID = ""
    @Published private(set) var pageTitle = ""
    @Published private(set) var pageURL = ""
    @Published private(set) var state = ""
    @Published private(set) var generation = 0

    private(set) var webView: WKWebView?
    private var allowedHost = ""
    private var snapshotID = ""
    private var observedLabels: [String: String] = [:]
    private var pageVisible = false
    private var loaded = false
    #if DEBUG && targetEnvironment(simulator)
    private var fixturePage = false
    #endif

    var isOpen: Bool { webView != nil }

    // Ordinary App callers use these typed entry points; Agent calls share execute().
    func openPage(_ url: URL, sessionID: String) async -> AIMaiChatHostToolExecution {
        await execute(["action": "open", "session_id": sessionID,
                       "url": url.absoluteString])
    }

    func observePage(sessionID: String) async -> AIMaiChatHostToolExecution {
        await execute(["action": "observe", "session_id": sessionID])
    }

    func capturePage(sessionID: String, expectedURL: String,
                     outputPath: String) async -> AIMaiChatHostToolExecution {
        await execute(["action": "screenshot", "session_id": sessionID,
                       "expected_url": expectedURL, "output_path": outputPath])
    }

    func actOnPage(sessionID: String, action: String, expectedURL: String,
                   snapshotID: String, elementID: String, expectedLabel: String,
                   text: String = "") async -> AIMaiChatHostToolExecution {
        var arguments: [String: Any] = ["action": action, "session_id": sessionID,
                                        "expected_url": expectedURL,
                                        "snapshot_id": snapshotID,
                                        "element_id": elementID,
                                        "expected_label": expectedLabel]
        if action == "type" { arguments["text"] = text }
        return await execute(arguments)
    }

    func setPageVisible(_ visible: Bool) {
        pageVisible = visible
    }

    func close() {
        webView?.stopLoading()
        webView?.navigationDelegate = nil
        webView = nil
        allowedHost = ""
        snapshotID = ""
        observedLabels.removeAll()
        loaded = false
        ownerSessionID = ""
        pageTitle = ""
        pageURL = ""
        state = ""
        generation += 1
    }

    func execute(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        guard let action = arguments["action"] as? String,
              let sessionID = arguments["session_id"] as? String,
              !sessionID.isEmpty else {
            return .failure(code: "invalid_input", message: "浏览器操作缺少所属 Session")
        }
        if action == "close" {
            guard ownerSessionID == sessionID else {
                return .failure(code: "not_found", message: "此 Session 没有打开的网页")
            }
            close()
            return Self.jsonSuccess(["state": "closed"])
        }
        guard pageVisible,
              AIAssistantModel.shared.selected == sessionID,
              AIAssistantModel.shared.selectedRemote == nil else {
            return .failure(code: "not_configured",
                            message: "请先在前台打开此设备的创作中心 Session")
        }
        if action == "open" {
            guard let raw = arguments["url"] as? String,
                  let parts = URLComponents(string: raw),
                  let url = parts.url,
                  (parts.scheme == "http" || parts.scheme == "https"),
                  let host = parts.host, !host.isEmpty,
                  parts.user == nil, parts.password == nil else {
                return .failure(code: "invalid_input", message: "请提供不含账号密码的 HTTP(S) 网址")
            }
            close()
            let configuration = WKWebViewConfiguration()
            configuration.websiteDataStore = .nonPersistent()
            let page = WKWebView(frame: .zero, configuration: configuration)
            page.navigationDelegate = self
            webView = page
            allowedHost = host.lowercased()
            ownerSessionID = sessionID
            pageURL = url.absoluteString
            pageTitle = host
            state = "正在加载网页…"
            generation += 1
            page.load(URLRequest(url: url))
            return Self.jsonSuccess(["state": "loading", "url": url.absoluteString,
                                "next_step": "Call observe after the page finishes loading."])
        }
        guard ownerSessionID == sessionID, let page = webView else {
            return .failure(code: "not_found", message: "此 Session 没有打开的网页")
        }
        if action == "observe" {
            if !loaded || page.isLoading {
                return Self.jsonSuccess(["state": "loading", "url": pageURL])
            }
            let id = UUID().uuidString.replacingOccurrences(of: "-", with: "")
            let script = Self.snapshotScript(id)
            do {
                guard let raw = try await page.evaluateJavaScript(script) as? String,
                      let data = raw.data(using: .utf8),
                      var result = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                      let elements = result["elements"] as? [[String: Any]] else {
                    return .failure(code: "protocol", message: "无法读取网页结构")
                }
                var labels: [String: String] = [:]
                for element in elements {
                    if let elementID = element["id"] as? String,
                       let label = element["label"] as? String {
                        labels[elementID] = label
                    }
                }
                snapshotID = id
                observedLabels = labels
                result["snapshot_id"] = id
                result["state"] = "ready"
                pageTitle = result["title"] as? String ?? pageTitle
                pageURL = result["url"] as? String ?? pageURL
                state = ""
                return Self.jsonSuccess(result)
            } catch {
                return .failure(code: "internal", message: "读取网页失败：\(error.localizedDescription)")
            }
        }
        if action == "screenshot" {
            guard loaded, !page.isLoading,
                  let expectedURL = arguments["expected_url"] as? String,
                  page.url?.absoluteString == expectedURL,
                  let requestedPath = arguments["output_path"] as? String,
                  let destination = authorizedOutputPath(requestedPath) else {
                return .failure(code: "invalid_input", message: "网页未加载或截图路径无效")
            }
            do {
                let image = try await withCheckedThrowingContinuation {
                    (continuation: CheckedContinuation<UIImage, Error>) in
                    page.takeSnapshot(with: nil) { image, error in
                        if let image { continuation.resume(returning: image) }
                        else { continuation.resume(throwing: error ?? BrowserFailure.emptyScreenshot) }
                    }
                }
                guard let png = image.pngData(), !png.isEmpty,
                      png.count <= 20 * 1024 * 1024 else {
                    return .failure(code: "internal", message: "网页截图为空或超出大小限制")
                }
                try png.write(to: destination, options: .atomic)
                return Self.jsonSuccess(["state": "captured", "url": page.url?.absoluteString ?? pageURL,
                                    "width": Int(image.size.width * image.scale),
                                    "height": Int(image.size.height * image.scale)])
            } catch {
                return .failure(code: "internal", message: "网页截图失败：\(error.localizedDescription)")
            }
        }
        if action == "click" || action == "type" {
            guard loaded, !page.isLoading,
                  let expectedURL = arguments["expected_url"] as? String,
                  page.url?.absoluteString == expectedURL,
                  let requestedSnapshot = arguments["snapshot_id"] as? String,
                  requestedSnapshot == snapshotID,
                  let elementID = arguments["element_id"] as? String,
                  let expectedLabel = arguments["expected_label"] as? String,
                  observedLabels[elementID] == expectedLabel else {
                return .failure(code: "invalid_input", message: "网页已变化，请重新读取后选择目标")
            }
            let value = arguments["text"] as? String ?? ""
            do {
                let script = Self.actionScript(action, id: elementID,
                                               label: expectedLabel, text: value)
                guard let outcome = try await page.evaluateJavaScript(script) as? String,
                      outcome == "dispatched" else {
                    return .failure(code: "invalid_input",
                                    message: "目标已变化或是密码输入框，请重新读取网页")
                }
                snapshotID = ""
                observedLabels.removeAll()
                return Self.jsonSuccess(["state": "dispatched", "action": action,
                                    "verified": false,
                                    "next_step": "Call observe to verify the page state."])
            } catch {
                return .failure(code: "internal", message: "网页操作失败：\(error.localizedDescription)")
            }
        }
        return .failure(code: "invalid_input", message: "未知的网页操作")
    }

    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        #if DEBUG && targetEnvironment(simulator)
        if fixturePage && action.request.url?.scheme == "about" {
            decisionHandler(.allow)
            return
        }
        #endif
        guard action.targetFrame?.isMainFrame == true,
              let url = action.request.url,
              (url.scheme == "http" || url.scheme == "https"),
              url.host?.lowercased() == allowedHost else {
            state = "已阻止离开当前网站；需要打开新网址请重新授权"
            decisionHandler(.cancel)
            return
        }
        snapshotID = ""
        observedLabels.removeAll()
        decisionHandler(.allow)
    }

    func webView(_ webView: WKWebView, didStartProvisionalNavigation navigation: WKNavigation!) {
        loaded = false
        state = "正在加载网页…"
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        loaded = true
        pageURL = webView.url?.absoluteString ?? pageURL
        pageTitle = webView.title ?? pageTitle
        state = ""
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!,
                 withError error: Error) {
        loaded = false
        state = "网页加载失败：\(error.localizedDescription)"
    }

    private func authorizedOutputPath(_ path: String) -> URL? {
        guard !path.isEmpty, path.hasSuffix(".png"),
              !AIAssistantModel.shared.workspacePath.isEmpty else { return nil }
        let root = URL(fileURLWithPath: AIAssistantModel.shared.workspacePath)
            .standardizedFileURL.resolvingSymlinksInPath().path
        let destination = URL(fileURLWithPath: path).standardizedFileURL
        let parent = destination.deletingLastPathComponent()
            .resolvingSymlinksInPath().path
        guard parent.hasPrefix(root + "/") else { return nil }
        return destination
    }

    private static func jsonSuccess(_ object: [String: Any]) -> AIMaiChatHostToolExecution {
        guard JSONSerialization.isValidJSONObject(object),
              let data = try? JSONSerialization.data(withJSONObject: object),
              let text = String(data: data, encoding: .utf8) else {
            return .failure(code: "internal", message: "无法编码网页结果")
        }
        return .success(text)
    }

    private static func literal(_ value: String) -> String {
        guard let data = try? JSONEncoder().encode(value),
              let text = String(data: data, encoding: .utf8) else { return "\"\"" }
        return text
    }

    private static func snapshotScript(_ nonce: String) -> String {
        let token = literal(nonce)
        return #"""
        (() => {
          const visible = e => {
            const r = e.getBoundingClientRect();
            const s = getComputedStyle(e);
            return r.width > 0 && r.height > 0 && s.visibility !== 'hidden' && s.display !== 'none';
          };
          const label = e => (e.getAttribute('aria-label') || e.innerText ||
            e.getAttribute('placeholder') || e.getAttribute('name') || e.getAttribute('title') ||
            e.tagName.toLowerCase()).trim().slice(0, 160);
          const candidates = Array.from(document.querySelectorAll(
            'a[href],button,input,textarea,select,[role="button"]')).filter(visible).slice(0, 80);
          const nonce = \#(token);
          const elements = candidates.map((e, i) => {
            const id = 'mai_' + nonce + '_' + i;
            e.setAttribute('data-maichat-agent-id', id);
            return {id, label: label(e), tag: e.tagName.toLowerCase(),
              type: (e.getAttribute('type') || '').toLowerCase()};
          });
          return JSON.stringify({url: location.href, title: document.title,
            text: (document.body?.innerText || '').slice(0, 20000), elements});
        })()
        """#
    }

    private static func actionScript(_ action: String, id: String, label: String,
                                     text: String) -> String {
        let idValue = literal(id)
        let labelValue = literal(label)
        let textValue = literal(text)
        let actionValue = literal(action)
        return #"""
        (() => {
          const id = \#(idValue), expected = \#(labelValue), action = \#(actionValue);
          const e = Array.from(document.querySelectorAll('[data-maichat-agent-id]'))
            .find(item => item.getAttribute('data-maichat-agent-id') === id);
          if (!e) return 'stale';
          const actual = (e.getAttribute('aria-label') || e.innerText ||
            e.getAttribute('placeholder') || e.getAttribute('name') ||
            e.getAttribute('title') || e.tagName.toLowerCase()).trim().slice(0, 160);
          if (actual !== expected) return 'stale';
          if (action === 'click') {
            setTimeout(() => e.click(), 0);
            return 'dispatched';
          }
          const type = (e.getAttribute('type') || 'text').toLowerCase();
          if ((e.tagName !== 'INPUT' && e.tagName !== 'TEXTAREA') ||
              ['password','hidden','file','checkbox','radio','submit'].includes(type) ||
              ['current-password','new-password','one-time-code'].includes(
                (e.getAttribute('autocomplete') || '').toLowerCase())) return 'unsupported';
          e.value = \#(textValue);
          e.dispatchEvent(new Event('input', {bubbles: true}));
          e.dispatchEvent(new Event('change', {bubbles: true}));
          return 'dispatched';
        })()
        """#
    }

    private enum BrowserFailure: Error { case emptyScreenshot }

    #if DEBUG && targetEnvironment(simulator)
    func runUITestFixture() async {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("maichat-browser-ui-test", isDirectory: true)
        let images = root.appendingPathComponent("images", isDirectory: true)
        try? FileManager.default.createDirectory(at: images, withIntermediateDirectories: true)
        AIAssistantModel.shared.installVideoToolTestWorkspace(root.path)
        close()
        fixturePage = true
        pageVisible = true
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        let page = WKWebView(frame: .zero, configuration: config)
        page.navigationDelegate = self
        webView = page
        ownerSessionID = "switch-A"
        pageTitle = "浏览器验收"
        generation += 1
        page.loadHTMLString("""
        <!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
        <title>浏览器验收</title><style>body{font-family:-apple-system;padding:18px}
        button,input{font-size:20px;padding:8px;margin:8px 0}button{background:#dff2ff}</style></head>
        <body><h2>MaiChat 浏览器测试</h2><input aria-label="测试输入" id="input">
        <button aria-label="确认按钮" onclick="document.getElementById('result').textContent=
        '操作已完成：'+document.getElementById('input').value">确认按钮</button>
        <p id="result">等待操作</p><input type="password" aria-label="密码输入"></body></html>
        """, baseURL: URL(string: "https://fixture.maichat.local/page"))
        for _ in 0..<80 {
            if loaded && !page.isLoading { break }
            try? await Task.sleep(for: .milliseconds(100))
        }
        var failures: [String] = []
        let first = await execute(["action": "observe", "session_id": "switch-A"])
        guard let firstText = first.output,
              let firstData = firstText.data(using: .utf8),
              let firstJSON = (try? JSONSerialization.jsonObject(with: firstData)) as? [String: Any],
              let firstID = firstJSON["snapshot_id"] as? String,
              let firstURL = firstJSON["url"] as? String,
              let firstElements = firstJSON["elements"] as? [[String: Any]],
              let input = firstElements.first(where: { $0["label"] as? String == "测试输入" }),
              let inputID = input["id"] as? String else {
            try? "observe failed: \(first.errorMessage ?? "invalid result")"
                .write(to: root.appendingPathComponent("result.txt"), atomically: true, encoding: .utf8)
            return
        }
        let typed = await execute(["action": "type", "session_id": "switch-A",
                                   "expected_url": firstURL, "snapshot_id": firstID,
                                   "element_id": inputID, "expected_label": "测试输入",
                                   "text": "iOS WebKit OK"])
        if typed.errorCode != nil { failures.append("type: \(typed.errorMessage ?? "unknown")") }
        let stale = await execute(["action": "type", "session_id": "switch-A",
                                   "expected_url": firstURL, "snapshot_id": firstID,
                                   "element_id": inputID, "expected_label": "测试输入",
                                   "text": "wrong"])
        if stale.errorCode == nil { failures.append("stale snapshot was accepted") }
        let otherSession = await execute(["action": "observe", "session_id": "switch-B"])
        if otherSession.errorCode == nil { failures.append("other Session accessed the page") }
        let second = await execute(["action": "observe", "session_id": "switch-A"])
        if let text = second.output, let data = text.data(using: .utf8),
           let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
           let id = json["snapshot_id"] as? String,
           let elements = json["elements"] as? [[String: Any]],
           let button = elements.first(where: { $0["label"] as? String == "确认按钮" }),
           let buttonID = button["id"] as? String {
            let clicked = await execute(["action": "click", "session_id": "switch-A",
                                         "expected_url": firstURL, "snapshot_id": id,
                                         "element_id": buttonID, "expected_label": "确认按钮"])
            if clicked.errorCode != nil { failures.append("click: \(clicked.errorMessage ?? "unknown")") }
        } else { failures.append("second observe failed") }
        try? await Task.sleep(for: .milliseconds(250))
        let third = await execute(["action": "observe", "session_id": "switch-A"])
        if !(third.output ?? "").contains("操作已完成：iOS WebKit OK") {
            failures.append("result not observed")
        }
        if let text = third.output, let data = text.data(using: .utf8),
           let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
           let id = json["snapshot_id"] as? String,
           let elements = json["elements"] as? [[String: Any]],
           let password = elements.first(where: { $0["label"] as? String == "密码输入" }),
           let passwordID = password["id"] as? String {
            let denied = await execute(["action": "type", "session_id": "switch-A",
                                        "expected_url": firstURL, "snapshot_id": id,
                                        "element_id": passwordID, "expected_label": "密码输入",
                                        "text": "forbidden"])
            if denied.errorCode == nil { failures.append("password field was accepted") }
        } else { failures.append("password input not observed") }
        let wrongURL = await execute(["action": "screenshot", "session_id": "switch-A",
                                      "expected_url": "https://wrong.example/",
                                      "output_path": images.appendingPathComponent("wrong.png").path])
        if wrongURL.errorCode == nil { failures.append("wrong URL screenshot was accepted") }
        let screenshot = await execute(["action": "screenshot", "session_id": "switch-A",
                                        "expected_url": firstURL,
                                        "output_path": images.appendingPathComponent("page.png").path])
        if screenshot.errorCode != nil { failures.append("screenshot: \(screenshot.errorMessage ?? "unknown")") }
        let result = failures.isEmpty
            ? "PASS: observe, type, click, verify, screenshot"
            : failures.joined(separator: "\n")
        try? result.write(to: root.appendingPathComponent("result.txt"), atomically: true, encoding: .utf8)
    }
    #endif
}

struct AIAssistantBrowserPane: View {
    @ObservedObject var browser: AIMobileBrowserTools
    @State private var headerHeight: CGFloat = 44

    var body: some View {
        ZStack(alignment: .top) {
            if browser.isOpen {
                BrowserWebView(browser: browser, headerHeight: headerHeight)
                    .id(browser.generation)
            }
            header
        }
        .background(Color(uiColor: .systemBackground))
        .overlay(Rectangle().stroke(Color.black.opacity(0.08), lineWidth: 1))
        .onPreferenceChange(BrowserHeaderHeight.self) { height in
            if height > 0, abs(headerHeight - height) > 0.5 { headerHeight = height }
        }
    }

    private var header: some View {
        HStack(spacing: 8) {
            Image(systemName: "globe")
                .foregroundStyle(.blue)
            VStack(alignment: .leading, spacing: 1) {
                Text(browser.pageTitle.isEmpty ? "网页" : browser.pageTitle)
                    .font(.caption.weight(.semibold)).lineLimit(1)
                Text(browser.state.isEmpty ? browser.pageURL : browser.state)
                    .font(.caption2).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer(minLength: 4)
            Button { browser.close() } label: {
                Image(systemName: "xmark.circle.fill")
                    .foregroundStyle(.secondary)
            }
            .accessibilityLabel("关闭网页")
        }
        .padding(.horizontal, 12).padding(.vertical, 8)
        .remoteIMBarBackground()
        .overlay(alignment: .bottom) { RemoteIMBarSeparator() }
        .background {
            GeometryReader { geometry in
                Color.clear.preference(key: BrowserHeaderHeight.self, value: geometry.size.height)
            }
        }
    }

    private struct BrowserHeaderHeight: PreferenceKey {
        static var defaultValue: CGFloat { 0 }
        static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = nextValue() }
    }

    private struct BrowserWebView: UIViewControllerRepresentable {
        let browser: AIMobileBrowserTools
        let headerHeight: CGFloat

        func makeUIViewController(context: Context) -> BrowserPageController {
            BrowserPageController(page: browser.webView ?? WKWebView(frame: .zero),
                                  headerHeight: headerHeight)
        }
        func updateUIViewController(_ controller: BrowserPageController, context: Context) {
            controller.updateHeaderHeight(headerHeight)
        }
        static func dismantleUIViewController(_ controller: BrowserPageController, coordinator: ()) {
            controller.restoreScrollAdjustment()
        }
    }

    // The page fills the pane behind the SwiftUI material header. UIKit then
    // adjusts the real web scroll view for the measured header safe area, so
    // neither its first line nor its last line is covered or made unreachable.
    private final class BrowserPageController: UIViewController {
        private let page: WKWebView
        private let previousAdjustment: UIScrollView.ContentInsetAdjustmentBehavior

        init(page: WKWebView, headerHeight: CGFloat) {
            self.page = page
            previousAdjustment = page.scrollView.contentInsetAdjustmentBehavior
            super.init(nibName: nil, bundle: nil)
            updateHeaderHeight(headerHeight)
        }
        required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
        override func loadView() {
            view = page
            page.scrollView.contentInsetAdjustmentBehavior = .always
        }
        func updateHeaderHeight(_ height: CGFloat) {
            additionalSafeAreaInsets = UIEdgeInsets(top: max(0, height), left: 0, bottom: 0, right: 0)
        }
        func restoreScrollAdjustment() {
            page.scrollView.contentInsetAdjustmentBehavior = previousAdjustment
        }
    }
}
