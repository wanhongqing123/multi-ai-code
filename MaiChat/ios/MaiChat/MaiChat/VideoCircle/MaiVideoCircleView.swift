// Video Circle browsing and publish confirmation. The server remains the source
// of truth for review status, visibility, reports and blocks.
import AVFoundation
import SwiftUI
import UIKit

private struct MaiVideoCirclePlayerItem: Identifiable {
    let url: URL
    var id: String { url.path }
}

struct MaiVideoCircleView: View {
    let close: () -> Void
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var selectedPage = 0
    @State private var feed: [MaiVideoCircleFeedPost] = []
    @State private var mine: [MaiVideoCircleMinePost] = []
    @State private var blocked: [MaiVideoCircleBlockedAuthor] = []
    @State private var nextCursor: String?
    @State private var mineCursor: String?
    @State private var isLoading = false
    @State private var isLoadingMore = false
    @State private var isLoadingMoreMine = false
    @State private var isPreparingVideo = false
    @State private var errorText = ""
    @State private var statusText = ""
    @State private var player: MaiVideoCirclePlayerItem?
    @State private var pendingAction: PendingAction?
    @State private var playbackTask: Task<Void, Never>?

    private enum PendingAction {
        case withdraw(String)
        case report(String)
        case block(String)
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                HStack(spacing: 0) {
                    pageButton("发现", page: 0)
                    pageButton("我的作品", page: 1)
                    pageButton("已屏蔽", page: 2)
                }
                .padding(4)
                .background(Color(uiColor: .secondarySystemGroupedBackground),
                            in: RoundedRectangle(cornerRadius: 13))
                .padding(.horizontal, 18).padding(.top, 12)

                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 16) {
                        if !errorText.isEmpty {
                            VStack(alignment: .leading, spacing: 8) {
                                Text(errorText).foregroundStyle(.red)
                                Button("重试") { Task { await reload() } }
                            }
                            .font(.subheadline)
                            .padding(16)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color.red.opacity(0.07),
                                        in: RoundedRectangle(cornerRadius: 14))
                        }
                        if !statusText.isEmpty {
                            Text(statusText).font(.subheadline)
                                .foregroundStyle(RemoteIMStyle.textPrimary)
                                .padding(12).frame(maxWidth: .infinity, alignment: .leading)
                                .background(Color.blue.opacity(0.07),
                                            in: RoundedRectangle(cornerRadius: 12))
                        }
                        if isLoading && feed.isEmpty && mine.isEmpty && blocked.isEmpty {
                            ProgressView("正在加载视频圈")
                                .frame(maxWidth: .infinity, minHeight: 200)
                        } else if selectedPage == 0 {
                            if feed.isEmpty && errorText.isEmpty {
                                emptyState("还没有公开视频", symbol: "play.rectangle.on.rectangle")
                            }
                            ForEach(feed) { post in feedCard(post) }
                            if nextCursor != nil {
                                Button(isLoadingMore ? "加载中…" : "加载更多") {
                                    Task { await loadMore() }
                                }
                                .disabled(isLoadingMore)
                                .frame(maxWidth: .infinity)
                                .padding(.vertical, 14)
                                .accessibilityIdentifier("video-circle-load-more")
                            }
                        } else if selectedPage == 1 {
                            if mine.isEmpty && errorText.isEmpty {
                                emptyState("还没有投稿", symbol: "film.stack")
                            }
                            ForEach(mine) { post in mineCard(post) }
                            if mineCursor != nil {
                                Button(isLoadingMoreMine ? "加载中…" : "加载更早作品") {
                                    Task { await loadMoreMine() }
                                }
                                .disabled(isLoadingMoreMine)
                                .frame(maxWidth: .infinity)
                                .padding(.vertical, 14)
                                .accessibilityIdentifier("video-circle-mine-load-more")
                            }
                        } else {
                            if blocked.isEmpty && errorText.isEmpty {
                                emptyState("没有屏蔽的作者", symbol: "person.crop.circle")
                            }
                            ForEach(blocked) { author in
                                HStack {
                                    Text(author.authorName)
                                        .font(.system(size: 15, weight: .medium))
                                    Spacer()
                                    Button("取消屏蔽") {
                                        Task { await unblock(authorID: author.authorId) }
                                    }
                                    .font(.subheadline)
                                }
                                .padding(15)
                                .background(Color(uiColor: .secondarySystemGroupedBackground),
                                            in: RoundedRectangle(cornerRadius: 13))
                                .accessibilityIdentifier("video-circle-blocked-\(author.authorId)")
                            }
                        }
                    }
                    .padding(18)
                }
                .id(selectedPage)
                .refreshable { await reload() }
            }
            .background(Color(uiColor: .systemGroupedBackground))
            .navigationTitle("视频圈")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("完成", action: close)
                        .accessibilityIdentifier("video-circle-close")
                }
            }
            .task { await reload() }
            .onDisappear { playbackTask?.cancel() }
            .fullScreenCover(item: $player) { item in
                MaiFfplayVideoScreen(path: item.url.path) {
                    player = nil
                    try? FileManager.default.removeItem(at: item.url)
                }
                .ignoresSafeArea()
                .statusBarHidden()
            }
            .confirmationDialog(actionTitle, isPresented: Binding(
                get: { pendingAction != nil },
                set: { if !$0 { pendingAction = nil } }), titleVisibility: .visible) {
                switch pendingAction {
                case let .withdraw(postID):
                    Button("撤回作品", role: .destructive) {
                        Task { await withdraw(postID: postID) }
                    }
                case let .block(authorID):
                    Button("屏蔽这位作者", role: .destructive) {
                        Task { await block(authorID: authorID) }
                    }
                case let .report(postID):
                    Button("垃圾或广告") { Task { await report(postID: postID, reason: "spam") } }
                    Button("骚扰") { Task { await report(postID: postID, reason: "harassment") } }
                    Button("色情内容") { Task { await report(postID: postID, reason: "sexual") } }
                    Button("暴力内容") { Task { await report(postID: postID, reason: "violence") } }
                    Button("侵权") { Task { await report(postID: postID, reason: "copyright") } }
                    Button("其他") { Task { await report(postID: postID, reason: "other") } }
                case nil: EmptyView()
                }
                Button("取消", role: .cancel) { pendingAction = nil }
            }
        }
        .accessibilityIdentifier("video-circle-screen")
    }

    private var actionTitle: String {
        switch pendingAction {
        case .withdraw: "撤回后其他人将无法再打开这段视频"
        case .block: "屏蔽后将不再看到该作者的作品"
        case .report: "举报原因"
        case nil: ""
        }
    }

    private func pageButton(_ title: String, page: Int) -> some View {
        Button {
            selectedPage = page
            statusText = ""
            Task { await reload() }
        } label: {
            Text(title).font(.system(size: 15, weight: .semibold))
                .frame(maxWidth: .infinity).padding(.vertical, 10)
                .foregroundStyle(selectedPage == page ? RemoteIMStyle.textPrimary : .secondary)
                .background(selectedPage == page ? Color(uiColor: .systemBackground) : .clear,
                            in: RoundedRectangle(cornerRadius: 10))
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier(page == 0 ? "video-circle-feed-tab" :
                                 page == 1 ? "video-circle-mine-tab" :
                                    "video-circle-blocked-tab")
    }

    private func emptyState(_ title: String, symbol: String) -> some View {
        VStack(spacing: 13) {
            Image(systemName: symbol).font(.system(size: 34, weight: .light))
                .foregroundStyle(RemoteIMStyle.blue)
            Text(title).font(.headline).foregroundStyle(RemoteIMStyle.textPrimary)
            Text(selectedPage == 0 ? "审核通过的 AI 视频会出现在这里" :
                 selectedPage == 1 ? "从创作中心生成的视频卡片提交作品" :
                    "可以在作品菜单中屏蔽作者")
                .font(.subheadline).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, minHeight: 250)
        .accessibilityIdentifier("video-circle-empty")
    }

    private func feedCard(_ post: MaiVideoCircleFeedPost) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Button { play(postID: post.id) } label: {
                ZStack {
                    LinearGradient(colors: [Color(red: 0.10, green: 0.20, blue: 0.36),
                                            Color(red: 0.19, green: 0.37, blue: 0.54)],
                                   startPoint: .topLeading, endPoint: .bottomTrailing)
                    Image(systemName: "play.fill")
                        .font(.system(size: 24, weight: .bold)).foregroundStyle(.white)
                        .frame(width: 58, height: 58)
                        .background(.black.opacity(0.38), in: Circle())
                    Text("AI 生成")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 9).padding(.vertical, 5)
                        .background(.black.opacity(0.46), in: Capsule())
                        .frame(maxWidth: .infinity, maxHeight: .infinity,
                               alignment: .topLeading).padding(12)
                    Text(durationText(post.durationMs))
                        .font(.caption.monospacedDigit()).foregroundStyle(.white)
                        .padding(.horizontal, 8).padding(.vertical, 5)
                        .background(.black.opacity(0.46), in: Capsule())
                        .frame(maxWidth: .infinity, maxHeight: .infinity,
                               alignment: .bottomTrailing).padding(12)
                }
                .aspectRatio(16 / 9, contentMode: .fit)
            }
            .buttonStyle(.plain)
            .disabled(isPreparingVideo)
            .accessibilityIdentifier("video-circle-play-\(post.id)")
            HStack(alignment: .top, spacing: 12) {
                VStack(alignment: .leading, spacing: 6) {
                    Text(post.title).font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                    Text(post.authorName).font(.subheadline).foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
                if post.authorId != appState.maiChatAccountID {
                    Menu {
                        Button("举报视频", systemImage: "flag") {
                            pendingAction = .report(post.id)
                        }
                        Button("屏蔽作者", systemImage: "person.crop.circle.badge.xmark") {
                            pendingAction = .block(post.authorId)
                        }
                    } label: {
                        Image(systemName: "ellipsis").frame(width: 44, height: 44)
                            .foregroundStyle(RemoteIMStyle.textSecondary)
                    }
                    .accessibilityLabel("作品操作")
                }
            }
            .padding(14)
        }
        .background(Color(uiColor: .secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 17))
        .clipShape(RoundedRectangle(cornerRadius: 17))
    }

    private func mineCard(_ post: MaiVideoCircleMinePost) -> some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "film")
                .font(.system(size: 19, weight: .medium))
                .foregroundStyle(RemoteIMStyle.blue)
                .frame(width: 44, height: 44)
                .background(Color.blue.opacity(0.08),
                            in: RoundedRectangle(cornerRadius: 11))
            VStack(alignment: .leading, spacing: 7) {
                Text(post.title).font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                Text(statusTitle(post.status)).font(.subheadline)
                    .foregroundStyle(post.status == "rejected" ? .red : .secondary)
                if let reason = post.reviewReason, !reason.isEmpty {
                    Text(reason).font(.footnote).foregroundStyle(.secondary)
                }
            }
            Spacer(minLength: 0)
            if post.status == "pending" || post.status == "approved" {
                Menu {
                    if post.status == "approved" {
                        Button("播放视频") { play(postID: post.id) }
                    }
                    Button("撤回作品", role: .destructive) {
                        pendingAction = .withdraw(post.id)
                    }
                } label: {
                    Image(systemName: "ellipsis").frame(width: 44, height: 44)
                }
                .accessibilityLabel("管理作品")
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(uiColor: .secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 15))
    }

    private func statusTitle(_ status: String) -> String {
        switch status {
        case "pending": "待审核"
        case "approved": "已公开"
        case "rejected": "未通过"
        case "withdrawn": "已撤回"
        case "hidden": "已下架"
        default: "状态待同步"
        }
    }

    private func durationText(_ milliseconds: Int) -> String {
        let seconds = max(0, milliseconds / 1000)
        return String(format: "%d:%02d", seconds / 60, seconds % 60)
    }

    private func reload() async {
        let requestedPage = selectedPage
        isLoading = true
        defer { if requestedPage == selectedPage { isLoading = false } }
        errorText = ""
        do {
            if requestedPage == 0 {
                let page = try await MaiVideoCircleClient.shared.feed()
                feed = page.posts
                nextCursor = page.nextCursor
            } else if requestedPage == 1 {
                let page = try await MaiVideoCircleClient.shared.mine()
                mine = page.posts
                mineCursor = page.nextCursor
            } else {
                blocked = try await MaiVideoCircleClient.shared.blocks().authors
            }
        } catch {
            if requestedPage == selectedPage { errorText = error.localizedDescription }
        }
    }

    private func loadMore() async {
        guard let cursor = nextCursor, !isLoadingMore else { return }
        isLoadingMore = true
        defer { isLoadingMore = false }
        do {
            let page = try await MaiVideoCircleClient.shared.feed(before: cursor)
            feed += page.posts.filter { incoming in !feed.contains { $0.id == incoming.id } }
            nextCursor = page.nextCursor
        } catch { errorText = error.localizedDescription }
    }

    private func loadMoreMine() async {
        guard let cursor = mineCursor, !isLoadingMoreMine else { return }
        isLoadingMoreMine = true
        defer { isLoadingMoreMine = false }
        do {
            let page = try await MaiVideoCircleClient.shared.mine(before: cursor)
            mine += page.posts.filter { incoming in !mine.contains { $0.id == incoming.id } }
            mineCursor = page.nextCursor
        } catch { errorText = error.localizedDescription }
    }

    private func play(postID: String) {
        guard !isPreparingVideo else { return }
        isPreparingVideo = true
        statusText = "正在准备视频…"
        playbackTask = Task {
            defer { isPreparingVideo = false }
            do {
                let url = try await MaiVideoCircleClient.shared.downloadForPlayback(postID: postID)
                guard !Task.isCancelled else { try? FileManager.default.removeItem(at: url); return }
                statusText = ""
                player = MaiVideoCirclePlayerItem(url: url)
            } catch { statusText = ""; errorText = error.localizedDescription }
        }
    }

    private func withdraw(postID: String) async {
        do {
            let receipt = try await MaiVideoCircleClient.shared.withdraw(postID: postID)
            statusText = receipt.cleanupPending
                ? "已从列表移除，文件清理中" : "作品已撤回"
            await reload()
        } catch { errorText = error.localizedDescription }
    }

    private func report(postID: String, reason: String) async {
        do {
            try await MaiVideoCircleClient.shared.report(postID: postID, reason: reason)
            statusText = "举报已提交"
        } catch { errorText = error.localizedDescription }
    }

    private func block(authorID: String) async {
        do {
            try await MaiVideoCircleClient.shared.block(authorID: authorID)
            statusText = "已屏蔽该作者"
            await reload()
        } catch { errorText = error.localizedDescription }
    }

    private func unblock(authorID: String) async {
        do {
            try await MaiVideoCircleClient.shared.unblock(authorID: authorID)
            statusText = "已取消屏蔽"
            await reload()
        } catch { errorText = error.localizedDescription }
    }
}

struct MaiVideoCirclePublishView: View {
    let source: MaiVideoCirclePublishSource
    let close: () -> Void
    let published: () -> Void
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var title = ""
    @State private var durationMs = 0
    @State private var sizeBytes: UInt64 = 0
    @State private var cover: UIImage?
    @State private var isPreparing = true
    @State private var isPublishing = false
    @State private var errorText = ""
    @State private var preview: MaiVideoCirclePlayerItem?

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    Button {
                        preview = MaiVideoCirclePlayerItem(url: source.fileURL)
                    } label: {
                        ZStack {
                            Color(red: 0.11, green: 0.19, blue: 0.30)
                            if let cover {
                                Image(uiImage: cover).resizable().scaledToFit()
                            }
                            Image(systemName: "play.fill").foregroundStyle(.white)
                                .font(.system(size: 21, weight: .bold))
                                .frame(width: 54, height: 54)
                                .background(.black.opacity(0.45), in: Circle())
                            Text("AI 生成")
                                .font(.caption.weight(.semibold)).foregroundStyle(.white)
                                .padding(8).background(.black.opacity(0.55), in: Capsule())
                                .frame(maxWidth: .infinity, maxHeight: .infinity,
                                       alignment: .topLeading).padding(12)
                        }
                        .aspectRatio(16 / 9, contentMode: .fit)
                        .clipShape(RoundedRectangle(cornerRadius: 16))
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("video-circle-publish-preview")

                    VStack(alignment: .leading, spacing: 8) {
                        Text("作品标题").font(.subheadline.weight(.semibold))
                        TextField("给视频起个名字", text: $title, axis: .vertical)
                            .lineLimit(1...2).textInputAutocapitalization(.never)
                            .padding(13)
                            .background(Color(uiColor: .secondarySystemGroupedBackground),
                                        in: RoundedRectangle(cornerRadius: 12))
                            .accessibilityIdentifier("video-circle-publish-title")
                    }
                    VStack(alignment: .leading, spacing: 7) {
                        Label("发布者：\(authorName)", systemImage: "person.crop.circle")
                        Label("审核通过后，其他已登录的 MaiChat 用户可以看到",
                              systemImage: "person.2")
                        Label("仅提交这段视频；原会话和私聊内容不会公开",
                              systemImage: "lock.shield")
                    }
                    .font(.subheadline).foregroundStyle(RemoteIMStyle.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)

                    if !errorText.isEmpty {
                        Text(errorText).font(.subheadline).foregroundStyle(.red)
                            .accessibilityIdentifier("video-circle-publish-error")
                    }
                    Button {
                        Task { await submit() }
                    } label: {
                        HStack(spacing: 8) {
                            if isPublishing { ProgressView().tint(.white) }
                            Text(isPublishing ? "正在提交…" : "确认提交审核")
                        }
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(.white)
                        .frame(maxWidth: .infinity).padding(.vertical, 15)
                        .background(RemoteIMStyle.blue,
                                    in: RoundedRectangle(cornerRadius: 13))
                    }
                    .disabled(isPreparing || isPublishing || title.trimmingCharacters(
                        in: .whitespacesAndNewlines).isEmpty || title.count > 80)
                    .accessibilityIdentifier("video-circle-submit")
                }
                .padding(18)
            }
            .background(Color(uiColor: .systemGroupedBackground))
            .navigationTitle("发布到视频圈")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("取消", action: close).disabled(isPublishing)
                }
            }
            .task { await prepare() }
            .fullScreenCover(item: $preview) { item in
                MaiFfplayVideoScreen(path: item.url.path) { preview = nil }
                    .ignoresSafeArea().statusBarHidden()
            }
        }
        .accessibilityIdentifier("video-circle-publish-screen")
    }

    private func prepare() async {
        title = String(source.caption.trimmingCharacters(in: .whitespacesAndNewlines).prefix(80))
        let url = source.fileURL
        guard url.pathExtension.lowercased() == "mp4",
              let values = try? url.resourceValues(forKeys: [.fileSizeKey]),
              let bytes = values.fileSize, bytes > 0, bytes <= 200_000_000 else {
            errorText = "视频文件不存在、格式不是 MP4，或超过 200 MB"
            isPreparing = false
            return
        }
        sizeBytes = UInt64(bytes)
        let asset = AVURLAsset(url: url)
        guard let duration = try? await asset.load(.duration), duration.seconds.isFinite,
              duration.seconds > 0, duration.seconds <= 60 else {
            errorText = "视频须可播放且不超过 60 秒"
            isPreparing = false
            return
        }
        durationMs = Int(duration.seconds * 1000)
        let generator = AVAssetImageGenerator(asset: asset)
        generator.appliesPreferredTrackTransform = true
        generator.maximumSize = CGSize(width: 900, height: 900)
        if let image = try? await generator.image(at: .zero) {
            cover = UIImage(cgImage: image.image)
        }
        isPreparing = false
    }

    private var authorName: String {
        appState.maiChatNickname.isEmpty ? appState.maiChatAccountID : appState.maiChatNickname
    }

    private func submit() async {
        let cleanTitle = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !isPreparing, !isPublishing, !cleanTitle.isEmpty,
              cleanTitle.count <= 80, sizeBytes > 0, durationMs > 0 else { return }
        isPublishing = true
        defer { isPublishing = false }
        errorText = ""
        do {
            let result = try await MaiVideoCircleClient.shared.publish(
                title: cleanTitle, source: source)
            guard result.status == "pending" else {
                throw MaiVideoCircleTransportError(
                    status: Int32(MAI_VIDEO_CIRCLE_INVALID_RESPONSE.rawValue),
                    httpStatus: 0, message: "提交状态异常，请在我的作品中核对")
            }
            published()
        } catch { errorText = error.localizedDescription }
    }
}
