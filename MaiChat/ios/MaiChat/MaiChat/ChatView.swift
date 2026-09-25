import AVFoundation
import AVKit
import CoreTransferable
import CryptoKit
import ImageIO
import MaiChatCore
import Photos
import PhotosUI
import QuickLook
import SwiftUI
import UIKit
import UniformTypeIdentifiers
import WebKit

enum RemoteIMStyle {
    static let pageBackground = Color(red: 0.966, green: 0.976, blue: 0.988)
    static let panelBackground = Color.white
    static let border = Color(red: 0.855, green: 0.894, blue: 0.941)
    static let textPrimary = Color(red: 0.055, green: 0.081, blue: 0.145)
    static let textSecondary = Color(red: 0.392, green: 0.459, blue: 0.561)
    static let blue = Color(red: 0.059, green: 0.553, blue: 0.867)
    static let blueSoft = Color(red: 0.882, green: 0.957, blue: 1.0)
    static let green = Color(red: 0.063, green: 0.596, blue: 0.325)
    static let greenSoft = Color(red: 0.848, green: 0.984, blue: 0.902)
    static let incomingBubbleBorder = Color(red: 235 / 255.0, green: 240 / 255.0, blue: 246 / 255.0)
    static let incomingBubbleBackground = Color(red: 250 / 255.0, green: 252 / 255.0, blue: 254 / 255.0)
    static let outgoingBubbleBackground = Color(red: 234 / 255.0, green: 244 / 255.0, blue: 255 / 255.0)
}

private struct RemoteIMImageRequest: Hashable, Sendable {
    let filePath: String
    let maximumPixelSize: Int
    let fileSize: Int
    let modificationMilliseconds: Int64

    init?(filePath: String?, maximumPixelSize: CGFloat) {
        guard let filePath,
              !filePath.isEmpty,
              maximumPixelSize.isFinite,
              maximumPixelSize > 0
        else { return nil }
        let fileURL = URL(fileURLWithPath: filePath).standardizedFileURL
        // Cover downloads publish the same final path first as metadata and then again after
        // the .part file is promoted. Including the lightweight fingerprint restarts the task
        // without allowing an older asynchronous result to overwrite the newer file.
        let values = try? fileURL.resourceValues(
            forKeys: [.fileSizeKey, .contentModificationDateKey]
        )
        self.filePath = fileURL.path
        self.maximumPixelSize = max(1, Int(maximumPixelSize.rounded(.up)))
        self.fileSize = values?.fileSize ?? -1
        self.modificationMilliseconds = values?.contentModificationDate.map {
            Int64(($0.timeIntervalSince1970 * 1_000).rounded())
        } ?? -1
    }
}

private final class RemoteIMDecodedImageBox: @unchecked Sendable {
    let image: UIImage
    let memoryCost: Int

    init(image: UIImage, memoryCost: Int) {
        self.image = image
        self.memoryCost = max(memoryCost, 1)
    }
}

private struct RemoteIMImageEncodingInput: @unchecked Sendable {
    let image: UIImage
}

private struct RemoteIMImageDecodeOutcome: @unchecked Sendable {
    let image: RemoteIMDecodedImageBox?
    let durationMilliseconds: Int
}

private actor RemoteIMImageDecodeLimiter {
    private let limit: Int
    private var activeCount = 0
    private var waiters: [CheckedContinuation<Void, Never>] = []

    init(limit: Int) {
        self.limit = max(limit, 1)
    }

    func acquire() async {
        if activeCount < limit {
            activeCount += 1
            return
        }
        await withCheckedContinuation { continuation in
            waiters.append(continuation)
        }
    }

    func release() {
        if waiters.isEmpty {
            activeCount = max(activeCount - 1, 0)
        } else {
            waiters.removeFirst().resume()
        }
    }
}

private actor RemoteIMImagePipeline {
    static let shared = RemoteIMImagePipeline()

    private let cache = NSCache<NSString, RemoteIMDecodedImageBox>()
    // A screenful of photos should not fan out into enough user-initiated work to starve UI.
    private let decodeLimiter = RemoteIMImageDecodeLimiter(limit: 2)
    private var requestsInFlight: [RemoteIMImageRequest: Task<RemoteIMImageDecodeOutcome, Never>] = [:]
    private var reportedFailures = Set<RemoteIMImageRequest>()
    private var reportedCacheHits = Set<RemoteIMImageRequest>()

    private init() {
        cache.name = "MaiChat.RemoteIMImagePipeline"
        cache.totalCostLimit = 32 * 1_024 * 1_024
    }

    func image(for request: RemoteIMImageRequest) async -> RemoteIMDecodedImageBox? {
        let cacheKey = Self.cacheKey(for: request)
        if let cached = cache.object(forKey: cacheKey) {
            if reportedCacheHits.insert(request).inserted {
                Task { @MainActor in
                    AppDiagnosticLog.shared.record(
                        level: .debug,
                        category: "media-performance",
                        event: "image-cache-hit",
                        fields: [
                            "file": DiagnosticLogPrivacy.stableTag(request.filePath, prefix: "f"),
                            "target_pixels": String(request.maximumPixelSize),
                        ]
                    )
                }
            }
            return cached
        }
        if let existing = requestsInFlight[request] {
            return await existing.value.image
        }

        let limiter = decodeLimiter
        let task = Task.detached(priority: .userInitiated) {
            await limiter.acquire()
            let outcome = Self.decode(request)
            await limiter.release()
            return outcome
        }
        requestsInFlight[request] = task
        let outcome = await task.value
        requestsInFlight[request] = nil

        if let image = outcome.image {
            cache.setObject(image, forKey: cacheKey, cost: image.memoryCost)
        }
        let shouldReportFailure = outcome.image == nil && reportedFailures.insert(request).inserted
        let shouldReportSlowDecode = outcome.image != nil && outcome.durationMilliseconds >= 50
        if shouldReportFailure || shouldReportSlowDecode {
            let event = outcome.image == nil ? "image-decode-failed" : "image-decode-slow"
            let level: DiagnosticLogLevel = outcome.image == nil ? .warning : .info
            Task { @MainActor in
                AppDiagnosticLog.shared.record(
                    level: level,
                    category: "media-performance",
                    event: event,
                    fields: [
                        "file": DiagnosticLogPrivacy.stableTag(request.filePath, prefix: "f"),
                        "target_pixels": String(request.maximumPixelSize),
                        "duration_ms": String(outcome.durationMilliseconds),
                    ]
                )
            }
        }
        return outcome.image
    }

    private static func cacheKey(for request: RemoteIMImageRequest) -> NSString {
        "\(request.filePath)#\(request.maximumPixelSize)#\(request.fileSize)#\(request.modificationMilliseconds)" as NSString
    }

    nonisolated private static func decode(
        _ request: RemoteIMImageRequest
    ) -> RemoteIMImageDecodeOutcome {
        let startedAt = ProcessInfo.processInfo.systemUptime
        let sourceOptions = [kCGImageSourceShouldCache: false] as CFDictionary
        guard let source = CGImageSourceCreateWithURL(
            URL(fileURLWithPath: request.filePath) as CFURL,
            sourceOptions
        ) else {
            return outcome(image: nil, startedAt: startedAt)
        }
        let thumbnailOptions = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: request.maximumPixelSize,
            kCGImageSourceShouldCacheImmediately: true,
        ] as CFDictionary
        guard let cgImage = CGImageSourceCreateThumbnailAtIndex(source, 0, thumbnailOptions) else {
            return outcome(image: nil, startedAt: startedAt)
        }
        let box = RemoteIMDecodedImageBox(
            image: UIImage(cgImage: cgImage),
            memoryCost: cgImage.bytesPerRow * cgImage.height
        )
        return outcome(image: box, startedAt: startedAt)
    }

    nonisolated private static func outcome(
        image: RemoteIMDecodedImageBox?,
        startedAt: TimeInterval
    ) -> RemoteIMImageDecodeOutcome {
        let elapsed = max(ProcessInfo.processInfo.systemUptime - startedAt, 0)
        return RemoteIMImageDecodeOutcome(
            image: image,
            durationMilliseconds: Int((elapsed * 1_000).rounded())
        )
    }
}

private struct RemoteIMImageLoadKey: Hashable, Sendable {
    let filePath: String?
    let pixels: Int
    let revision: UInt64
}

@MainActor
private final class RemoteIMAsyncImageState: ObservableObject {
    @Published private(set) var image: UIImage?
    @Published private(set) var hasFinished = false
    private var activeRequest: RemoteIMImageRequest?
    private var generation: UInt64 = 0

    func load(_ key: RemoteIMImageLoadKey) async {
        generation &+= 1
        let generation = generation
        do {
            let request = try await RemoteIMBackgroundWork.metadata {
                RemoteIMImageRequest(filePath: key.filePath, maximumPixelSize: CGFloat(key.pixels))
            }
            guard !Task.isCancelled, self.generation == generation else { return }
            if activeRequest == request, image != nil || hasFinished { return }
            activeRequest = request
            image = nil
            hasFinished = false
            guard let request else { hasFinished = true; return }
            let result = await RemoteIMImagePipeline.shared.image(for: request)
            guard !Task.isCancelled, self.generation == generation else { return }
            image = result?.image
            hasFinished = true
        } catch { }
    }
}

private struct RemoteIMAsyncImage<Content: View, Placeholder: View>: View {
    let filePath: String?
    let maximumPointSize: CGSize
    @ViewBuilder let content: (UIImage) -> Content
    @ViewBuilder let placeholder: (_ hasFailed: Bool) -> Placeholder
    @Environment(\.displayScale) private var displayScale
    @EnvironmentObject private var appState: RemoteIMAppState
    @StateObject private var state = RemoteIMAsyncImageState()

    var body: some View {
        let key = RemoteIMImageLoadKey(filePath: filePath,
            pixels: max(1, Int((max(maximumPointSize.width, maximumPointSize.height) * displayScale).rounded(.up))),
            revision: appState.mediaFileRevision)
        return Group {
            if let image = state.image { content(image) }
            else { placeholder(state.hasFinished) }
        }
        .task(id: key) { await state.load(key) }
    }
}

struct ChatView: View {
    @Binding var activeContact: RemoteIMContact?
    let showRemoteDesktop: () -> Void
    let bottomBar: AnyView
    @State private var searchTargetMessageID: UUID?
    @State private var preparedContactID: String?
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        ChatNavigationHost(
            root: AnyView(VStack(spacing: 0) {
                HeaderView()
                ConversationListView(
                    activeContact: $activeContact,
                    searchTargetMessageID: $searchTargetMessageID
                )
                // The bar belongs to the previous page, so UIKit reveals it
                // together with the conversation list during interactive pop.
                bottomBar
            }.background(RemoteIMStyle.pageBackground).environmentObject(appState)),
            detail: detail,
            selectionID: canPresentContact ? activeContact?.userID : nil,
            onPop: { activeContact = nil }
        )
        .background(RemoteIMStyle.pageBackground.ignoresSafeArea())
        .task(id: activeContact?.userID) {
            guard let contact = activeContact else {
                preparedContactID = nil
                return
            }
            // Read the local first page before constructing/pushing its view.
            // Otherwise one summary row flashes before the initial history page replaces it.
            await appState.loadInitialMessages(with: contact.userID)
            guard !Task.isCancelled, activeContact?.userID == contact.userID else { return }
            preparedContactID = contact.userID
        }
    }

    private var canPresentContact: Bool {
        guard let contact = activeContact else { return false }
        return preparedContactID == contact.userID &&
            !appState.isLoadingInitialMessages(with: contact.userID)
    }

    private var detail: AnyView? {
        guard let activeContact, canPresentContact else { return nil }
        return AnyView(ChatDetailView(
            contact: activeContact,
            activeContact: $activeContact,
            searchTargetMessageID: searchTargetMessageID,
            showRemoteDesktop: showRemoteDesktop
        ).environmentObject(appState))
    }
}

struct RemoteIMContactAvatar: View {
    let contact: RemoteIMContact
    let isSelected: Bool
    let presenceStatus: RemoteIMPresenceStatus
    let size: CGFloat

    var body: some View {
        RemoteIMUserAvatar(
            profile: RemoteIMUserProfile(
                userID: contact.userID,
                displayName: contact.displayName,
                avatarURL: contact.avatarURL
            ),
            outgoing: true,
            size: size,
            presenceStatus: presenceStatus,
            selected: isSelected
        )
    }
}

private struct RemoteIMUserAvatar: View {
    let profile: RemoteIMUserProfile
    let outgoing: Bool
    let size: CGFloat
    var presenceStatus: RemoteIMPresenceStatus = .unknown
    var selected = false

    var body: some View {
        ZStack(alignment: .bottomTrailing) {
            avatarContent
                .frame(width: size, height: size)
                .clipShape(RoundedRectangle(cornerRadius: size >= 40 ? 10 : 8, style: .continuous))
                .overlay(
                    RoundedRectangle(
                        cornerRadius: size >= 40 ? 10 : 8,
                        style: .continuous
                    )
                    .stroke(
                        presenceStatus.isOnline
                            ? Color(red: 0.118, green: 0.737, blue: 0.408)
                            : Color.clear,
                        lineWidth: presenceStatus.isOnline ? 2 : 0
                    )
                )
                .overlay(
                    RoundedRectangle(cornerRadius: size >= 40 ? 10 : 8, style: .continuous)
                        .stroke(selected ? RemoteIMStyle.blue : Color.clear, lineWidth: selected ? 1 : 0)
                )

            if presenceStatus.isOnline {
                Circle()
                    .fill(Color(red: 0.118, green: 0.737, blue: 0.408))
                    .frame(width: 11, height: 11)
                    .overlay(Circle().stroke(Color.white, lineWidth: 2))
                    .offset(x: 2, y: 2)
            }
        }
        .accessibilityLabel("\(profile.displayName) 的头像")
    }

    @ViewBuilder
    private var avatarContent: some View {
        if let avatarURL = profile.avatarURL,
           let url = URL(string: avatarURL),
           !avatarURL.isEmpty
        {
            AsyncImage(url: url) { phase in
                switch phase {
                case let .success(image):
                    image
                        .resizable()
                        .scaledToFill()
                default:
                    monogram
                }
            }
        } else {
            monogram
        }
    }

    private var monogram: some View {
        Text(RemoteIMAvatarMonogramPolicy.text(
            displayName: profile.displayName,
            userID: profile.userID
        ))
        .font(.system(size: max(11, size * 0.3), weight: .bold))
        .foregroundStyle(.white)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(
            LinearGradient(
                colors: outgoing
                    ? [Color(red: 0.357, green: 0.608, blue: 1.0), Color(red: 0.118, green: 0.251, blue: 0.686)]
                    : [Color(red: 0.176, green: 0.831, blue: 0.749), Color(red: 0.059, green: 0.463, blue: 0.431)],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
        )
    }
}

struct RemoteIMPresenceBadge: View {
    let status: RemoteIMPresenceStatus

    var body: some View {
        switch status {
        case .online:
            Text("在线")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(Color(red: 0.047, green: 0.518, blue: 0.29))
                .padding(.horizontal, 7)
                .padding(.vertical, 3)
                .background(RemoteIMStyle.greenSoft, in: Capsule())
        case .offline:
            Text("离线")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .padding(.horizontal, 7)
                .padding(.vertical, 3)
                .background(Color(red: 0.945, green: 0.953, blue: 0.965), in: Capsule())
        case .unknown:
            EmptyView()
        }
    }
}

private struct HeaderView: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            Spacer(minLength: 0)

            StatusPill(state: appState.connectionState)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(RemoteIMStyle.panelBackground)
        .overlay(alignment: .bottom) {
            Divider().background(RemoteIMStyle.border)
        }
    }
}

private struct StatusPill: View {
    let state: RemoteIMAppState.ConnectionState

    var body: some View {
        HStack(spacing: 7) {
            Circle()
                .fill(dotColor)
                .frame(width: 8, height: 8)
            if state != .connected {
                Text(state.rawValue)
                    .font(.system(size: 13, weight: .semibold))
            }
        }
        .foregroundStyle(textColor)
        .padding(.horizontal, state == .connected ? 0 : 12)
        .padding(.vertical, state == .connected ? 0 : 7)
        .background(
            state == .connected ? Color.clear : backgroundColor,
            in: Capsule()
        )
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("IM 连接状态")
        .accessibilityValue(state.rawValue)
    }

    private var dotColor: Color {
        switch state {
        case .connected:
            return RemoteIMStyle.green
        case .connecting:
            return .orange
        case .failed:
            return .red
        case .disconnected:
            return RemoteIMStyle.textSecondary
        }
    }

    private var textColor: Color {
        state == .connected ? RemoteIMStyle.green : RemoteIMStyle.textSecondary
    }

    private var backgroundColor: Color {
        state == .connected ? RemoteIMStyle.greenSoft : Color(.secondarySystemBackground)
    }
}

private struct ConversationListView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Binding var activeContact: RemoteIMContact?
    @Binding var searchTargetMessageID: UUID?
    @State private var searchText = ""
    @State private var pendingClearHistoryContact: RemoteIMContact?
    @State private var messageSearchHits: [LocalChatHistorySearchHit] = []
    @State private var isSearchingMessages = false

    var body: some View {
        ZStack {
            VStack(spacing: 0) {
                ConversationSearchField(text: $searchText)

                List {
                    if normalizedSearch.isEmpty {
                        if filteredContacts.isEmpty {
                            EmptyConversationListView()
                                .padding(.top, 96)
                                .listRowInsets(EdgeInsets())
                                .listRowSeparator(.hidden)
                                .listRowBackground(RemoteIMStyle.panelBackground)
                        } else {
                            ForEach(filteredContacts) { contact in
                                conversationButton(contact)
                            }
                        }
                    } else {
                        if !filteredContacts.isEmpty {
                            Section("联系人") {
                                ForEach(filteredContacts) { contact in
                                    conversationButton(contact)
                                }
                            }
                        }

                        if isSearchingMessages {
                            HStack(spacing: 10) {
                                ProgressView()
                                Text("正在搜索全部消息…")
                                    .foregroundStyle(RemoteIMStyle.textSecondary)
                            }
                            .listRowSeparator(.hidden)
                            .listRowBackground(RemoteIMStyle.panelBackground)
                        } else if !visibleMessageSearchHits.isEmpty {
                            Section("消息") {
                                ForEach(visibleMessageSearchHits) { hit in
                                    if let contact = contact(for: hit.peerUserID) {
                                        Button {
                                            guard let openedContact = appState.openMessageSearchHit(hit)
                                            else { return }
                                            searchTargetMessageID = hit.message.id
                                            activeContact = openedContact
                                        } label: {
                                            MessageSearchResultRow(contact: contact, hit: hit)
                                        }
                                        .buttonStyle(.plain)
                                        .listRowInsets(EdgeInsets())
                                        .listRowSeparator(.hidden)
                                        .listRowBackground(RemoteIMStyle.panelBackground)
                                    }
                                }
                            }
                        } else if filteredContacts.isEmpty {
                            EmptyMessageSearchView(query: searchText)
                                .padding(.top, 72)
                                .listRowInsets(EdgeInsets())
                                .listRowSeparator(.hidden)
                                .listRowBackground(RemoteIMStyle.panelBackground)
                        }
                    }
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
                .scrollDismissesKeyboard(.immediately)
                .background(RemoteIMStyle.panelBackground)
            }
            .background(RemoteIMStyle.panelBackground)

            if let pendingClearHistoryContact {
                ClearHistoryDialog(
                    contact: pendingClearHistoryContact,
                    cancel: { self.pendingClearHistoryContact = nil },
                    clear: {
                        self.pendingClearHistoryContact = nil
                        Task {
                            _ = await appState.clearHistory(
                                with: pendingClearHistoryContact.userID
                            )
                        }
                    }
                )
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(10)
            }
        }
        .animation(.easeOut(duration: 0.18), value: pendingClearHistoryContact?.userID)
        .task(id: normalizedSearch) {
            let query = normalizedSearch
            guard !query.isEmpty else {
                messageSearchHits = []
                isSearchingMessages = false
                return
            }
            isSearchingMessages = true
            defer {
                // A cancelled task must not clear the spinner owned by the replacement query.
                if normalizedSearch == query {
                    isSearchingMessages = false
                }
            }
            do {
                try await Task.sleep(nanoseconds: 250_000_000)
            } catch {
                return
            }
            let hits = await appState.searchMessages(query)
            guard !Task.isCancelled, normalizedSearch == query else { return }
            messageSearchHits = hits
        }
    }

    @ViewBuilder
    private func conversationButton(_ contact: RemoteIMContact) -> some View {
        Button {
            searchTargetMessageID = nil
            appState.selectContact(contact)
            activeContact = contact
        } label: {
            ConversationRow(
                contact: contact,
                latestMessage: appState.chatState.latestMessage(with: contact.userID),
                selected: contact.userID == appState.chatState.selectedPeerID,
                presenceStatus: appState.presenceStatus(for: contact),
                unreadCount: appState.unreadCount(for: contact.userID)
            )
        }
        .buttonStyle(.plain)
        .listRowInsets(EdgeInsets())
        .listRowSeparator(.hidden)
        .listRowBackground(RemoteIMStyle.panelBackground)
        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
            Button {
                pendingClearHistoryContact = contact
            } label: {
                Label("清空消息", systemImage: "eraser")
            }
            .tint(.orange)
        }
    }

    private var normalizedSearch: String {
        searchText.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    }

    private var visibleMessageSearchHits: [LocalChatHistorySearchHit] {
        messageSearchHits.filter { contact(for: $0.peerUserID) != nil }
    }

    private func contact(for userID: String) -> RemoteIMContact? {
        appState.chatState.contacts.first(where: { $0.userID == userID })
    }

    private var filteredContacts: [RemoteIMContact] {
        let query = normalizedSearch
        return appState.chatState.contacts
            .filter { contact in
                guard !query.isEmpty else { return true }
                let latestText = appState.chatState.latestMessage(with: contact.userID)?.text ?? ""
                return contact.userID.lowercased().contains(query) ||
                    contact.displayName.lowercased().contains(query) ||
                    latestText.lowercased().contains(query)
            }
            .sorted { left, right in
                let leftDate = appState.chatState.latestMessage(with: left.userID)?.createdAt ?? .distantPast
                let rightDate = appState.chatState.latestMessage(with: right.userID)?.createdAt ?? .distantPast
                if leftDate == rightDate {
                    return left.displayName.localizedStandardCompare(right.displayName) == .orderedAscending
                }
                return leftDate > rightDate
            }
    }

}

private struct ClearHistoryDialog: View {
    let contact: RemoteIMContact
    let cancel: () -> Void
    let clear: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.28)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture(perform: cancel)

            VStack(alignment: .leading, spacing: 16) {
                Text("清空聊天记录？")
                    .font(.system(size: 20, weight: .bold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                Text("将清空与 \(contact.displayName) 的消息，但保留该好友。")
                    .font(.system(size: 14))
                    .foregroundStyle(RemoteIMStyle.textSecondary)

                HStack(spacing: 12) {
                    dialogButton(title: "取消", color: RemoteIMStyle.textPrimary, action: cancel)
                    dialogButton(title: "清空", color: .red, action: clear)
                }
            }
            .padding(20)
            .frame(maxWidth: 360)
            .background(
                RemoteIMStyle.panelBackground,
                in: RoundedRectangle(cornerRadius: 18, style: .continuous)
            )
            .overlay {
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .stroke(RemoteIMStyle.border, lineWidth: 1)
            }
            .shadow(color: Color.black.opacity(0.16), radius: 24, y: 10)
            .padding(.horizontal, 24)
        }
        .accessibilityElement(children: .contain)
        .accessibilityAction(.escape, cancel)
    }

    private func dialogButton(
        title: String,
        color: Color,
        action: @escaping () -> Void
    ) -> some View {
        Button(title, action: action)
            .buttonStyle(.plain)
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(color)
            .frame(maxWidth: .infinity)
            .frame(height: 46)
            .background(
                RemoteIMStyle.pageBackground,
                in: RoundedRectangle(cornerRadius: 12, style: .continuous)
            )
    }
}

private struct ConversationSearchField: View {
    @Binding var text: String
    @FocusState private var isFocused: Bool

    var body: some View {
        HStack(spacing: 9) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(isFocused ? RemoteIMStyle.blue : RemoteIMStyle.textSecondary)
            TextField("搜索联系人或全部消息", text: $text)
                .font(.system(size: 15))
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .focused($isFocused)
                .submitLabel(.search)
                .onSubmit { isFocused = false }
                .accessibilityIdentifier("remote-im-message-search-field")
            if !text.isEmpty {
                Button {
                    text = ""
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 16))
                        .foregroundStyle(RemoteIMStyle.textSecondary.opacity(0.7))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("清空搜索")
            }
        }
        .padding(.horizontal, 13)
        .frame(height: 44)
        .background(
            RemoteIMStyle.pageBackground,
            in: RoundedRectangle(cornerRadius: 12, style: .continuous)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .stroke(isFocused ? RemoteIMStyle.blue : RemoteIMStyle.border, lineWidth: isFocused ? 1.5 : 1)
        )
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .background(RemoteIMStyle.panelBackground)
        .accessibilityElement(children: .contain)
    }
}

private struct MessageSearchResultRow: View {
    let contact: RemoteIMContact
    let hit: LocalChatHistorySearchHit

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            RemoteIMContactAvatar(
                contact: contact,
                isSelected: false,
                presenceStatus: .unknown,
                size: 38
            )
            VStack(alignment: .leading, spacing: 5) {
                HStack(spacing: 8) {
                    Text(contact.displayName)
                        .font(.system(size: 14, weight: .bold))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                        .lineLimit(1)
                    Spacer(minLength: 8)
                    Text(RemoteIMTimestampTextPolicy.displayText(for: hit.message.createdAt))
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                }
                Text(previewText)
                    .font(.system(size: 13))
                    .foregroundStyle(RemoteIMStyle.textSecondary)
                    .lineLimit(2)
                    .multilineTextAlignment(.leading)
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 11)
        .contentShape(Rectangle())
    }

    private var previewText: String {
        let compact = hit.message.text
            .replacingOccurrences(of: "\n", with: " ")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return hit.message.direction == .outgoing ? "我：\(compact)" : compact
    }
}

private struct EmptyMessageSearchView: View {
    let query: String

    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 28))
                .foregroundStyle(Color(red: 0.56, green: 0.59, blue: 0.64))
            Text("没有找到相关消息")
                .font(.system(size: 16, weight: .bold))
                .foregroundStyle(RemoteIMStyle.textPrimary)
            Text("没有包含“\(query.trimmingCharacters(in: .whitespacesAndNewlines))”的联系人或消息。")
                .font(.system(size: 13))
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
        .padding(.horizontal, 24)
    }
}

private struct ConversationPreviewText: View {
    let message: RemoteIMMessage?
    @State private var preparedText: String?
    @State private var preparedSource: String?
    private var fallback: String {
        guard let message else { return "暂无消息" }
        return String(message.text.prefix(160)).replacingOccurrences(of: "\n", with: " ")
    }
    var body: some View {
        Text(preparedSource == message?.text ? (preparedText ?? fallback) : fallback)
            .task(id: message?.text) {
                let message = message
                do {
                    let result = try await RemoteIMBackgroundWork.parse { MarkdownConversationPreview.text(for: message) }
                    guard !Task.isCancelled else { return }
                    preparedSource = message?.text
                    preparedText = result
                } catch { }
            }
    }
}

private struct ConversationRow: View {
    let contact: RemoteIMContact
    let latestMessage: RemoteIMMessage?
    let selected: Bool
    let presenceStatus: RemoteIMPresenceStatus
    let unreadCount: Int

    var body: some View {
        HStack(spacing: 12) {
            RemoteIMContactAvatar(
                contact: contact,
                isSelected: selected,
                presenceStatus: presenceStatus,
                size: 42
            )

            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 8) {
                    Text(contact.displayName)
                        .font(.system(size: 16, weight: .bold))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Spacer(minLength: 8)
                    if let latestMessage {
                        Text(RemoteIMTimestampTextPolicy.displayText(for: latestMessage.createdAt))
                            .font(.system(size: 11, weight: .medium))
                            .foregroundStyle(RemoteIMStyle.textSecondary)
                    }
                }

                HStack(spacing: 8) {
                    ConversationPreviewText(message: latestMessage)
                        .font(.system(size: 13))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                        .lineLimit(1)
                        .truncationMode(.tail)
                    Spacer(minLength: 8)
                    if unreadCount > 0 {
                        RemoteIMUnreadBadge(count: unreadCount)
                    }
                }
            }
        }
        .padding(.horizontal, 16)
        .frame(height: 72)
        .contentShape(Rectangle())
    }
}

struct RemoteIMUnreadBadge: View {
    let count: Int

    var body: some View {
        if count > 0 {
            Text(count > 99 ? "99+" : String(count))
                .font(.system(size: 11, weight: .bold))
                .foregroundStyle(.white)
                .padding(.horizontal, 5)
                .frame(minWidth: 18, minHeight: 18)
                .background(Color(red: 0.961, green: 0.247, blue: 0.247), in: Capsule())
                .accessibilityLabel("\(count) 条未读消息")
        }
    }
}

private struct EmptyConversationListView: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "bubble.left")
                .font(.system(size: 28))
                .foregroundStyle(Color(red: 0.56, green: 0.59, blue: 0.64))
            Text("暂无会话")
                .font(.system(size: 16, weight: .bold))
                .foregroundStyle(RemoteIMStyle.textPrimary)
            Text("到通讯录添加好友账号后即可开始聊天。")
                .font(.system(size: 13))
                .foregroundStyle(RemoteIMStyle.textSecondary)
        }
        .frame(maxWidth: .infinity)
    }
}

private struct AudioPlayerReference: @unchecked Sendable { let value: AVAudioPlayer }

private final class NativeVoicePlayer: NSObject, AVAudioPlayerDelegate, @unchecked Sendable {
    private var player: AVAudioPlayer?
    private var currentID: UUID?
    private var completed: (@MainActor @Sendable (UUID) -> Void)?
    deinit {
        guard let player else { return }
        let reference = AudioPlayerReference(value: player)
        RemoteIMBackgroundWork.audioQueue.async { reference.value.delegate = nil; reference.value.stop() }
    }
    func play(url: URL, id: UUID, cancellation: RemoteIMBackgroundWork.Cancellation,
              completed: @escaping @MainActor @Sendable (UUID) -> Void) {
        RemoteIMBackgroundWork.audioQueue.async { [self] in
            stopNative()
            do {
                try cancellation.check()
                let session = AVAudioSession.sharedInstance()
                try session.setCategory(.playback, mode: .spokenAudio)
                try session.setActive(true)
                try cancellation.check()
                let next = try AVAudioPlayer(contentsOf: url)
                next.prepareToPlay()
                try cancellation.check()
                player = next; currentID = id; self.completed = completed
                next.delegate = self
                guard next.play() else { throw VoiceRecorderError.startFailed }
            } catch {
                stopNative()
                Task { @MainActor in completed(id) }
            }
        }
    }
    func stop() { RemoteIMBackgroundWork.audioQueue.async { [self] in stopNative() } }
    private func stopNative() {
        player?.delegate = nil; player?.stop(); player = nil; currentID = nil; completed = nil
    }
    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) { finish(player) }
    func audioPlayerDecodeErrorDidOccur(_ player: AVAudioPlayer, error: Error?) { finish(player) }
    private func finish(_ player: AVAudioPlayer) {
        let reference = AudioPlayerReference(value: player)
        RemoteIMBackgroundWork.audioQueue.async { [self] in
            guard self.player === reference.value, let id = currentID, let completed else { return }
            stopNative()
            Task { @MainActor in completed(id) }
        }
    }
}

@MainActor
private final class VoiceMessagePlayer: ObservableObject {
    @Published private(set) var playingMessageID: UUID?
    private let native = NativeVoicePlayer()
    private var cancellation: RemoteIMBackgroundWork.Cancellation?
    func toggle(message: RemoteIMMessage) {
        guard let attachment = message.voiceAttachment else { return }
        if playingMessageID == message.id { stop(); return }
        stop()
        let cancellation = RemoteIMBackgroundWork.Cancellation()
        self.cancellation = cancellation
        playingMessageID = message.id
        native.play(url: URL(fileURLWithPath: attachment.localFilePath), id: message.id,
                    cancellation: cancellation) { [weak self] id in
            guard self?.playingMessageID == id else { return }
            self?.playingMessageID = nil
            self?.cancellation = nil
        }
    }
    func stop() {
        cancellation?.cancel(); cancellation = nil
        playingMessageID = nil
        native.stop()
    }
}

private enum RemoteIMImagePreviewLayout {
    static let coordinateSpaceName = "remote-im-image-preview-space"
}

private struct PresentedRemoteIMImage: Identifiable {
    let item: RemoteIMImagePreviewItem
    let image: UIImage
    let sourceFrame: CGRect

    var id: UUID { item.id }
}

private struct PresentedMessageActions: Identifiable {
    let message: RemoteIMMessage
    let sourceFrame: CGRect

    var id: UUID { message.id }
}

private struct ChatDetailView: View {
    let contact: RemoteIMContact
    @Binding var activeContact: RemoteIMContact?
    let searchTargetMessageID: UUID?
    let showRemoteDesktop: () -> Void
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var isAttachmentPanelPresented = false
    @State private var transcriptionPresentation = VoiceTranscriptionPresentation()
    @State private var imagePreviewPresentation: PresentedRemoteIMImage?
    @State private var isImagePreviewExpanded = false
    @State private var quoteTargetMessageID: UUID?
    @State private var messageActionTarget: PresentedMessageActions?
    @State private var selectingMessageID: UUID?
    @State private var forwardingMessage: RemoteIMMessage?

    var body: some View {
        ZStack {
            VStack(spacing: 0) {
                ChatDetailHeader(
                    contact: contact,
                    activeContact: $activeContact,
                    session: appState.remoteDesktop,
                    showRemoteDesktop: showRemoteDesktop
                )
                MessageListView(
                    peerUserID: contact.userID,
                    messages: appState.visibleMessages(with: contact.userID),
                    peerRelation: contact.relation,
                    searchTargetMessageID: quoteTargetMessageID ?? searchTargetMessageID,
                    hasEarlierMessages: appState.hasEarlierMessages(with: contact.userID),
                    selectingMessageID: selectingMessageID,
                    finishSelectingText: { selectingMessageID = nil },
                    dismissAttachmentPanel: { isAttachmentPanelPresented = false },
                    presentImagePreview: presentImagePreview,
                    showMessageActions: { message, sourceFrame in
                        dismissKeyboard()
                        messageActionTarget = PresentedMessageActions(
                            message: message,
                            sourceFrame: sourceFrame
                        )
                    },
                    replyToMessage: { message in
                        appState.beginReply(to: message)
                    },
                    openQuote: { quote in
                        Task {
                            quoteTargetMessageID = await appState.openQuotedMessage(
                                quote,
                                peerUserID: contact.userID
                            )
                        }
                    },
                    loadEarlierMessages: {
                        await appState.loadEarlierMessages(with: contact.userID)
                    }
                )
                .id(contact.userID)
                ComposerView(
                    peerUserID: contact.userID,
                    isAttachmentPanelPresented: $isAttachmentPanelPresented,
                    draft: appState.draft,
                    transcriptionPresentation: transcriptionPresentation
                )
            }
            // A transient voice panel must not participate in the chat's size
            // calculation. As a ZStack sibling, its minimum height enlarged the
            // underlying list and invalidated its lazy geometry on dismissal.
            .overlay {
                VoiceTranscriptionHighlightHost(presentation: transcriptionPresentation)
            }

            if let imagePreviewPresentation {
                FullScreenImagePreviewView(
                    presentation: imagePreviewPresentation,
                    isExpanded: isImagePreviewExpanded,
                    close: closeImagePreview
                )
                .zIndex(20)
            }

            if let messageActionTarget {
                MessageActionDialog(
                    sourceFrame: messageActionTarget.sourceFrame,
                    dismiss: { self.messageActionTarget = nil },
                    reply: {
                        self.messageActionTarget = nil
                        appState.beginReply(to: messageActionTarget.message)
                    },
                    selectCopy: {
                        selectingMessageID = messageActionTarget.message.id
                        self.messageActionTarget = nil
                    },
                    forward: {
                        forwardingMessage = messageActionTarget.message
                        self.messageActionTarget = nil
                        selectingMessageID = nil
                    },
                    copyAll: {
                        UIPasteboard.general.string = RemoteIMMessageCopyPolicy.fullText(
                            for: messageActionTarget.message
                        )
                        self.messageActionTarget = nil
                        selectingMessageID = nil
                    }
                )
                .transition(.opacity)
                .zIndex(30)
            }

            if let forwardingMessage {
                ForwardMessageDialog(
                    message: forwardingMessage,
                    contacts: appState.chatState.contacts,
                    cancel: { self.forwardingMessage = nil },
                    forward: { contact in
                        self.forwardingMessage = nil
                        Task {
                            _ = await appState.forwardMessage(forwardingMessage, to: contact)
                        }
                    }
                )
                .transition(.opacity.combined(with: .scale(scale: 0.97)))
                .zIndex(40)
            }
        }
        .coordinateSpace(name: RemoteIMImagePreviewLayout.coordinateSpaceName)
        .background(RemoteIMStyle.pageBackground.ignoresSafeArea())
        .toolbar(.hidden, for: .navigationBar)
        .animation(.easeOut(duration: 0.18), value: messageActionTarget?.id)
        .animation(.easeOut(duration: 0.18), value: forwardingMessage?.id)
        .onAppear {
            appState.selectContact(contact)
            appState.setConversationVisible(userID: contact.userID, visible: true)
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "remote-im-ui",
                event: "conversation-opened",
                fields: [
                    "peer": DiagnosticLogPrivacy.stableTag(contact.userID, prefix: "u"),
                    "account": appState.remoteDiagnosticsAccountTag,
                    "cached_messages": String(appState.visibleMessages(with: contact.userID).count),
                ]
            )
        }
        .onDisappear {
            appState.setConversationVisible(userID: contact.userID, visible: false)
            appState.cancelReply()
            messageActionTarget = nil
            selectingMessageID = nil
            forwardingMessage = nil
        }
        .task(id: contact.userID) {
            let startedAt = ProcessInfo.processInfo.systemUptime
            await appState.loadInitialMessages(with: contact.userID)
            let elapsed = max(ProcessInfo.processInfo.systemUptime - startedAt, 0)
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "remote-im-ui",
                event: "conversation-history-ready",
                fields: [
                    "peer": DiagnosticLogPrivacy.stableTag(contact.userID, prefix: "u"),
                    "messages": String(appState.visibleMessages(with: contact.userID).count),
                    "has_earlier": appState.hasEarlierMessages(with: contact.userID) ? "true" : "false",
                    "duration_ms": String(Int((elapsed * 1_000).rounded())),
                ]
            )
        }
    }

    private var imagePreviewAnimation: Animation {
        .interactiveSpring(response: 0.38, dampingFraction: 0.86, blendDuration: 0.08)
    }

    private func presentImagePreview(
        item: RemoteIMImagePreviewItem,
        image: UIImage,
        sourceFrame: CGRect
    ) {
        guard sourceFrame.width > 0, sourceFrame.height > 0 else { return }
        imagePreviewPresentation = PresentedRemoteIMImage(
            item: item,
            image: image,
            sourceFrame: sourceFrame
        )
        isImagePreviewExpanded = false
        DispatchQueue.main.async {
            guard imagePreviewPresentation?.id == item.id else { return }
            withAnimation(imagePreviewAnimation) {
                isImagePreviewExpanded = true
            }
        }
    }

    private func closeImagePreview() {
        guard let closingID = imagePreviewPresentation?.id else { return }
        withAnimation(imagePreviewAnimation) {
            isImagePreviewExpanded = false
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.42) {
            guard !isImagePreviewExpanded,
                  imagePreviewPresentation?.id == closingID
            else { return }
            imagePreviewPresentation = nil
        }
    }


}

struct VoiceActionFramesKey: PreferenceKey {
    static let defaultValue: [VoiceTranscriptionTarget: CGRect] = [:]
    static func reduce(value: inout [VoiceTranscriptionTarget: CGRect], nextValue: () -> [VoiceTranscriptionTarget: CGRect]) {
        value.merge(nextValue()) { _, new in new }
    }
}

@MainActor
final class VoiceTranscriptionPresentation: ObservableObject {
    private struct PresentationState: Equatable {
        var target: VoiceTranscriptionTarget?
        var liveText = ""
    }
    @Published private var state = PresentationState()
    var target: VoiceTranscriptionTarget? {
        get { state.target }
        set { if state.target != newValue { state.target = newValue } }
    }
    var liveText: String { state.liveText }
    private var firstLoggedSessionID: UInt64?

    var onCancel: (() -> Void)?
    var onEdit: (() -> Void)?
    var actionFrames: [VoiceTranscriptionTarget: CGRect] = [:]
    private(set) var diagnosticFields: [String: String] = [:]

    func prepareForNewSession(account: String, peer: String) {
        diagnosticFields = ["account": account,
                            "peer": DiagnosticLogPrivacy.stableTag(peer, prefix: "u"),
                            "asr_session": UUID().uuidString.lowercased()]
        state = PresentationState()
        firstLoggedSessionID = nil
    }

    func updateLiveText(_ text: String, sessionID: UInt64) {
        guard liveText != text else { return }
        if !text.isEmpty, firstLoggedSessionID != sessionID {
            firstLoggedSessionID = sessionID
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "asr",
                event: "ui-first-text-updated",
                fields: [
                    "session": String(sessionID),
                    "characters": String(text.count),
                ].merging(diagnosticFields) { _, context in context }
            )
        }
        state.liveText = text
    }

    func reset() {
        state = PresentationState()
        firstLoggedSessionID = nil
    }
}

struct VoiceTranscriptionHighlightHost: View {
    @ObservedObject var presentation: VoiceTranscriptionPresentation

    var body: some View {
        Group {
            if let target = presentation.target {
                VoiceTranscriptionHighlight(
                    target: target,
                    transcript: presentation.liveText,
                    onCancel: { presentation.onCancel?() },
                    onEdit: { presentation.onEdit?() }
                )
                .transition(.opacity)
                .onPreferenceChange(VoiceActionFramesKey.self) { presentation.actionFrames = $0 }
            }
        }
        .transaction { transaction in
            transaction.animation = nil
            transaction.disablesAnimations = true
        }
    }
}

private struct VoiceTranscriptionHighlight: View {
    let target: VoiceTranscriptionTarget
    let transcript: String
    let onCancel: () -> Void
    let onEdit: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.68)
                .ignoresSafeArea()

            VStack(spacing: 0) {
                Spacer(minLength: 120)

                if transcript.isEmpty {
                    listeningIndicator(size: 104)
                } else {
                    Text(transcript)
                        .font(.system(size: 24, weight: .medium))
                        .foregroundStyle(.white)
                        .multilineTextAlignment(.leading)
                        .lineLimit(8)
                        .minimumScaleFactor(0.78)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 24)
                        .padding(.vertical, 22)
                        .background(
                            RoundedRectangle(cornerRadius: 24, style: .continuous)
                                .fill(RemoteIMStyle.blue.opacity(0.94))
                        )
                        .padding(.horizontal, 30)
                        .transition(.opacity)

                    listeningIndicator(size: 58)
                        .padding(.top, 18)
                }

                Text(statusText)
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(.white)
                    .padding(.top, 16)

                Spacer()

                HStack(spacing: 72) {
                    targetBadge(
                        title: "取消",
                        systemImage: "xmark",
                        selected: target == .cancel,
                        selectedColor: .red
                    )
                    targetBadge(
                        title: "编辑",
                        systemImage: "pencil",
                        selected: target == .edit || target == .finishingEdit,
                        selectedColor: RemoteIMStyle.blue
                    )
                }
                .padding(.bottom, 76)
            }
        }

        .accessibilityElement(children: .ignore)
        .accessibilityLabel(statusText)
    }

    private func listeningIndicator(size: CGFloat) -> some View {
        ZStack {
            Circle()
                .fill(indicatorColor.opacity(0.2))
                .frame(width: size * 1.34, height: size * 1.34)
            Circle()
                .fill(indicatorColor)
                .frame(width: size, height: size)
            if target == .cancel {
                Image(systemName: "xmark")
                    .font(.system(size: size * 0.3, weight: .bold))
                    .foregroundStyle(.white)
            } else if target == .edit || target == .finishingEdit {
                Image(systemName: "pencil")
                    .font(.system(size: size * 0.28, weight: .bold))
                    .foregroundStyle(.white)
            }
        }
    }

    private func targetBadge(
        title: String,
        systemImage: String,
        selected: Bool,
        selectedColor: Color
    ) -> some View {
        Button(action: systemImage == "xmark" ? onCancel : onEdit) {
        VStack(spacing: 8) {
            Image(systemName: systemImage)
                .font(.system(size: 22, weight: .semibold))
                .frame(width: 64, height: 64)
                .background(
                    Circle().fill(selected ? selectedColor : Color.white.opacity(0.14))
                )
                .overlay(
                    Circle().stroke(Color.white.opacity(selected ? 0.9 : 0.3), lineWidth: 1.5)
                )
            Text(title)
                .font(.system(size: 15, weight: .semibold))
        }
        .foregroundStyle(.white)
        .background {
            GeometryReader { geometry in
                Color.clear.preference(key: VoiceActionFramesKey.self,
                    value: [systemImage == "xmark" ? .cancel : .edit: geometry.frame(in: .global)])
            }
        }
        .scaleEffect(selected ? 1.12 : 1)
        }
        .buttonStyle(.plain)
        .disabled(target == .finishingSend || target == .finishingEdit)
    }

    private var statusText: String {
        switch target {
        case .send:
            return transcript.isEmpty ? "正在听，松手发送" : "松手发送"
        case .cancel:
            return "松手取消"
        case .edit:
            return "松手编辑"
        case .finishingSend:
            return "正在完成识别…"
        case .finishingEdit:
            return "正在准备编辑…"
        }
    }

    private var indicatorColor: Color {
        switch target {
        case .cancel:
            return .red
        default:
            return RemoteIMStyle.blue
        }
    }
}

private struct ChatDetailHeader: View {
    let contact: RemoteIMContact
    @Binding var activeContact: RemoteIMContact?
    @ObservedObject var session: RemoteDesktopSession
    let showRemoteDesktop: () -> Void
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var showRemoteDiagnostics = false

    var body: some View {
        HStack(spacing: 10) {
            Button {
                activeContact = nil
            } label: {
                Image(systemName: "chevron.left")
                    .font(.system(size: 18, weight: .semibold))
                    .frame(width: 34, height: 34)
            }
            .buttonStyle(.plain)
            .foregroundStyle(RemoteIMStyle.textPrimary)

            Text(contact.displayName)
                .font(.system(size: 18, weight: .bold))
                .foregroundStyle(RemoteIMStyle.textPrimary)
                .lineLimit(1)
                .truncationMode(.middle)

            Spacer(minLength: 8)

            Button {
                if session.state.isActive {
                    showRemoteDesktop()
                } else {
                    showRemoteDesktop()
                    Task {
                        await appState.requestRemoteDesktopView(of: contact)
                    }
                }
            } label: {
                ZStack {
                    Image(systemName: "display")
                        .font(.system(size: 16, weight: .semibold))
                    if session.state == .inviting || session.state == .connecting {
                        ProgressView()
                            .tint(remoteButtonColor)
                            .scaleEffect(0.65)
                            .offset(x: 12, y: -11)
                    }
                }
                .frame(width: 36, height: 34)
            }
            .buttonStyle(.plain)
            .foregroundStyle(remoteButtonColor)
            .background(remoteButtonBackground, in: RoundedRectangle(cornerRadius: 8))
            .disabled(!canUseRemoteButton)
            .accessibilityLabel(remoteButtonAccessibilityLabel)

            Menu {
                Button { showRemoteDiagnostics = true } label: {
                    Label("远程排障", systemImage: "stethoscope")
                }
            } label: {
                Image(systemName: "ellipsis").frame(width: 34, height: 34)
            }
            .accessibilityLabel("更多")
            .accessibilityIdentifier("chatMoreMenu")

            StatusPill(state: appState.connectionState)
        }
        .padding(.horizontal, 10)
        .padding(.top, 8)
        .padding(.bottom, 10)
        .background(RemoteIMStyle.panelBackground)
        .overlay(alignment: .bottom) {
            Divider().background(RemoteIMStyle.border)
        }
        .sheet(isPresented: $showRemoteDiagnostics) {
            RemoteDiagnosticsView(peer: contact, contacts: appState.chatState.contacts,
                ownerUserID: appState.chatState.ownerUserID, connected: appState.connectionState == .connected,
                coordinator: appState.remoteDiagnostics)
        }
    }

    private var canUseRemoteButton: Bool {
        session.state.isActive ||
            (appState.connectionState == .connected && session.canStart)
    }

    private var remoteButtonColor: Color {
        switch session.state {
        case .inviting, .connecting:
            return .orange
        case .viewing:
            return RemoteIMStyle.blue
        case .idle, .failed:
            return canUseRemoteButton ? RemoteIMStyle.blue : RemoteIMStyle.textSecondary
        }
    }

    private var remoteButtonBackground: Color {
        switch session.state {
        case .inviting, .connecting:
            return Color.orange.opacity(0.12)
        case .viewing:
            return RemoteIMStyle.blueSoft
        case .idle, .failed:
            return Color(.secondarySystemBackground)
        }
    }

    private var remoteButtonAccessibilityLabel: String {
        switch session.state {
        case .inviting, .connecting:
            return "查看远程连接进度"
        case .viewing:
            return "进入远程桌面"
        case .idle, .failed:
            return "远程控制 \(contact.displayName)"
        }
    }
}

struct RelationBadge: View {
    let text: String

    var body: some View {
        Text(text)
            .font(.system(size: 11, weight: .bold))
            .foregroundStyle(Color(red: 0.706, green: 0.324, blue: 0.035))
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(Color(red: 1.0, green: 0.963, blue: 0.862), in: Capsule())
    }
}

private enum MessageTimelineItem: Identifiable {
    case message(RemoteIMMessage)
    case activity(RemoteIMActivitySignal)
    var id: String {
        switch self {
        case .message(let message): return message.id.uuidString
        case .activity(let signal): return "activity-" + signal.activityID
        }
    }
}

private struct MessageListView: View {
    let peerUserID: String
    let messages: [RemoteIMMessage]
    let peerRelation: RemoteIMContactRelation
    @StateObject private var scrollIntent = MessageScrollIntent()
    let searchTargetMessageID: UUID?
    let hasEarlierMessages: Bool
    let selectingMessageID: UUID?
    let finishSelectingText: () -> Void
    let dismissAttachmentPanel: () -> Void
    let presentImagePreview: (RemoteIMImagePreviewItem, UIImage, CGRect) -> Void
    let showMessageActions: (RemoteIMMessage, CGRect) -> Void
    let replyToMessage: (RemoteIMMessage) -> Void
    let openQuote: (RemoteIMQuote) -> Void
    let loadEarlierMessages: () async -> Void
    @EnvironmentObject private var appState: RemoteIMAppState
    @StateObject private var voicePlayer = VoiceMessagePlayer()
    @State private var videoPreviewItem: RemoteIMVideoPreviewItem?
    @State private var filePreviewItem: RemoteIMFilePreviewItem?
    @State private var isLoadingEarlierMessages = false
    @State private var isVisible = false
    @State private var keyboardIsVisible = false

    @EnvironmentObject private var navigationArrival: ChatNavigationArrival
    @State private var lastLoggedHistoryCount = -1


    private var approvalDecisionStates: [String: ApprovalDecisionDisplayState] {
        var states: [String: ApprovalDecisionDisplayState] = [:]
        for message in messages {
            guard let decision = message.approvalDecision else { continue }
            let token = decision.token
            if decision.action == .autoDeclined {
                states[token] = .autoDeclined
            } else if decision.action == .resolved, states[token] != .autoDeclined {
                states[token] = .resolved
            } else if states[token] == .resolved || states[token] == .autoDeclined {
                continue
            } else if message.status == .sent {
                states[token] = .sent
            } else if message.status == .pending, states[token] != .sent {
                states[token] = .sending
            }
        }
        return states
    }

    var body: some View {
        let decisionStates = approvalDecisionStates
        let activity = appState.activity(with: peerUserID)
        let timeline: [MessageTimelineItem] = messages.map { .message($0) }
            + (activity.map { [.activity($0)] } ?? [])
        ScrollViewReader { proxy in
            ScrollView {
                // One lazy list preserves message identity across local sends and
                // history changes without mounting all Markdown rows.
                VStack(alignment: .leading, spacing: 14) {
                    if hasEarlierMessages {
                        Button {
                            Task { await loadEarlierMessagesIfNeeded() }
                        } label: {
                            if isLoadingEarlierMessages { ProgressView() }
                            else { Text("加载更早消息").font(.caption) }
                        }
                        .disabled(isLoadingEarlierMessages)
                        .frame(maxWidth: .infinity)
                    }
                    if !timeline.isEmpty {
                        MessageHistoryStack(items: timeline) { item in
                            if case let .activity(signal) = item {
                                RemoteIMActivityBubble(signal: signal)
                            } else if case let .message(message) = item {
                            MessageBubbleView(
                                message: message,
                                approvalDecisionState: message.approvalRequest.map {
                                    decisionStates[$0.token] ?? .available
                                } ?? .available,
                                isVideoDownloading: appState.isVideoDownloading(
                                    remoteID: message.remoteID,
                                    localPath: message.videoAttachment?.localPath ?? ""
                                ),
                                isVoicePlaying: voicePlayer.playingMessageID == message.id,
                                playVoice: {
                                    voicePlayer.toggle(message: message)
                                },
                                previewImage: { image, sourceFrame in
                                    guard let nextItem = RemoteIMImagePreviewPolicy.previewItem(
                                        for: message
                                    ) else { return }
                                    presentImagePreview(nextItem, image, sourceFrame)
                                },
                                previewVideo: {
                                    videoPreviewItem = RemoteIMVideoPreviewPolicy.previewItem(for: message)
                                },
                                previewFile: {
                                    filePreviewItem = RemoteIMFilePreviewItem(message: message)
                                },
                                isSelectingText: selectingMessageID == message.id,
                                finishSelectingText: finishSelectingText,
                                showActions: { sourceFrame in
                                    showMessageActions(message, sourceFrame)
                                },
                                reply: { replyToMessage(message) },
                                openQuote: openQuote
                            )
                                .padding(searchTargetMessageID == message.id ? 4 : 0)
                                .overlay {
                                    RoundedRectangle(cornerRadius: 14, style: .continuous)
                                        .stroke(
                                            searchTargetMessageID == message.id
                                                ? Color.orange
                                                : Color.clear,
                                            lineWidth: 2
                                        )
                                }
                                // One view per message keeps the existing scroll anchor stable.
                                .padding(.top, message.id == messages.first?.id ? 0 : 14)
                                .overlay(alignment: .top) {
                                    if message.id != messages.first?.id {
                                        Rectangle()
                                            .fill(RemoteIMStyle.border.opacity(0.8))
                                            .frame(height: 0.5)
                                            .accessibilityHidden(true)
                                    }
                                }
                                .id(message.id)
                                .onAppear {
                                    if message.id == appState.chatState.messages(with: peerUserID).first?.id, scrollIntent.userBrowsedHistory {
                                        Task { await loadEarlierMessagesIfNeeded() }
                                    }
                                    let owner = appState.chatState.ownerUserID
                                    guard message.fromUserID == owner || message.toUserID == owner else { return }
                                    let peer = message.direction == .incoming ? message.fromUserID : message.toUserID
                                    AppDiagnosticLog.shared.record(level: .debug, category: "remote-im-ui", event: "row-presented", fields: [
                                        "account": appState.remoteDiagnosticsAccountTag,
                                        "peer": DiagnosticLogPrivacy.stableTag(peer, prefix: "u"),
                                        "message": DiagnosticLogPrivacy.stableTag(message.remoteID ?? message.id.uuidString, prefix: "m"),
                                        "text_bytes": String(message.text.utf8.count)
                                    ])
                                }
                            }
                        }
                    }
                    Color.clear.frame(height: 1).id("message-list-bottom")
                }
                .padding(.horizontal, MessageBubbleMetrics.horizontalInset)
                .padding(.vertical, 18)
                .background(MessageScrollPositionReader(
                    onViewportResizeNeedsBottom: {
                        guard !scrollIntent.userBrowsedHistory else { return }
                        proxy.scrollTo("message-list-bottom", anchor: .bottom)
                    },
                    onUserScroll: { scrollIntent.userDidScroll() }
                ) { nearBottom in
                    if nearBottom { scrollIntent.didReachLatest() }
                })
                .background(
                    ScrollViewKeyboardDismissInstaller { window in
                        dismissAttachmentPanel()
                        dismissKeyboard(in: window)
                        if selectingMessageID != nil {
                            finishSelectingText()
                        }
                    }
                )
            }
            .modifier(MessageInitialScrollAnchor())
            .scrollDismissesKeyboard(.interactively)
            .background(RemoteIMStyle.panelBackground)
            .overlay {
                if messages.isEmpty && activity == nil {
                    // Center in the visible message area, outside the timeline.
                    EmptyMessagesView()
                        .allowsHitTesting(false)
                }
            }
            .onAppear {
                isVisible = true
                navigationArrival.scrollIntent = scrollIntent
                recordHistoryLayout()
            }
            .onDisappear {
                isVisible = false
                voicePlayer.stop()
                scrollIntent.cancelPendingPositioning()
            }
            .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillShowNotification)) { notification in
                guard isVisible, !navigationArrival.isReturning, !keyboardIsVisible,
                      navigationArrival.composerView?.isFirstResponder == true else { return }
                keyboardIsVisible = true
                scrollIntent.positionWithKeyboard(
                    proxy: proxy,
                    id: "message-list-bottom",
                    notification: notification,
                    anchor: .bottom
                )
            }
            .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardDidHideNotification)) { _ in
                keyboardIsVisible = false
            }
            .onChange(of: appState.locallyQueuedMessageID) { id in
                // onChange can hold the previous body's messages value. Validate
                // against the current model, not that stale array snapshot.
                guard isVisible, !navigationArrival.isReturning, let id,
                      appState.chatState.message(id: id)?.toUserID == peerUserID else { return }
                scrollIntent.followLatest()
                scrollIntent.positionAtBottom(proxy: proxy, id: id, anchor: .bottom)
            }
            .onChange(of: messages.last?.id) { _ in
                guard !scrollIntent.userBrowsedHistory else { return }
                scrollIntent.positionAtBottom(
                    proxy: proxy, id: "message-list-bottom", anchor: .bottom)
            }
            .onChange(of: activity?.activityID) { _ in
                guard !scrollIntent.userBrowsedHistory else { return }
                scrollIntent.positionAtBottom(
                    proxy: proxy, id: "message-list-bottom", anchor: .bottom)
            }
            .onChange(of: messages.count) { _ in recordHistoryLayout() }
            .onChange(of: searchTargetMessageID) { targetID in
                _ = scrollToSearchTarget(proxy: proxy, targetID: targetID)
            }
            .onChange(of: navigationArrival.hasArrived) { arrived in
                guard arrived else { return }
                if !scrollToSearchTarget(proxy: proxy, targetID: searchTargetMessageID) {
                    scrollToLatestMessage(proxy: proxy)
                }
            }

        }
        .fullScreenCover(item: $videoPreviewItem) { item in
            FullScreenVideoPreviewView(item: item) {
                videoPreviewItem = nil
            }
        }
        .fullScreenCover(item: $filePreviewItem) { item in
            FullScreenFilePreviewView(item: item) {
                filePreviewItem = nil
            }
        }
    }

    private func recordHistoryLayout() {
        guard messages.count != lastLoggedHistoryCount, let message = messages.last else { return }
        let owner = appState.chatState.ownerUserID
        guard message.fromUserID == owner || message.toUserID == owner else { return }
        lastLoggedHistoryCount = messages.count
        let peer = message.direction == .incoming ? message.fromUserID : message.toUserID
        AppDiagnosticLog.shared.record(level: .info, category: "remote-im-ui", event: "history-layout", fields: [
            "account": appState.remoteDiagnosticsAccountTag,
            "peer": DiagnosticLogPrivacy.stableTag(peer, prefix: "u"),
            "layout": "normal-origin-lazy-stack", "loaded_messages": String(messages.count)
        ])
    }

    @discardableResult
    private func scrollToSearchTarget(proxy: ScrollViewProxy, targetID: UUID?) -> Bool {
        guard let targetID,
              appState.chatState.messages(with: peerUserID).contains(where: { $0.id == targetID })
        else { return false }
        let generation = scrollIntent.beginPositioning()
        DispatchQueue.main.async {
            guard scrollIntent.isCurrent(generation) else { return }
            withAnimation(.easeOut(duration: 0.2)) {
                proxy.scrollTo(targetID, anchor: .center)
            }
        }
        return true
    }

    private func scrollToLatestMessage(proxy: ScrollViewProxy) {
        guard appState.chatState.latestMessage(with: peerUserID) != nil else { return }
        scrollIntent.followLatest()
        scrollIntent.positionAtBottom(
            proxy: proxy, id: "message-list-bottom", anchor: .bottom)
    }

    @MainActor
    private func loadEarlierMessagesIfNeeded() async {
        guard hasEarlierMessages, !isLoadingEarlierMessages else { return }
        isLoadingEarlierMessages = true
        defer { isLoadingEarlierMessages = false }
        // Older rows append at the far end of the reversed timeline, so there
        // is no explicit positioning command after pagination.
        await loadEarlierMessages()
    }

}

private struct RemoteIMActivityBubble: View {
    let signal: RemoteIMActivitySignal

    var body: some View {
        HStack(spacing: signal.kind == .humanTyping ? 7 : 9) {
            if signal.kind == .humanTyping {
                TimelineView(.animation(minimumInterval: 0.32)) { context in
                    let phase = Int(context.date.timeIntervalSinceReferenceDate / 0.32) % 3
                    HStack(spacing: 5) {
                        ForEach(0..<3, id: \.self) { index in
                            Circle()
                                .fill(RemoteIMStyle.blue)
                                .frame(width: 7, height: 7)
                                .opacity(index == phase ? 1 : 0.38)
                                .scaleEffect(index == phase ? 1.12 : 0.92)
                        }
                    }
                }
            } else {
                TimelineView(.periodic(from: .now, by: 1)) { context in
                    HStack(spacing: 9) {
                        ProgressView()
                            .progressViewStyle(.circular)
                            .tint(RemoteIMStyle.blue)
                            .scaleEffect(0.72)
                        Text(machineStatusText(at: context.date))
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(RemoteIMStyle.blue)
                            .lineLimit(1)
                    }
                }
            }
        }
        .padding(.horizontal, signal.kind == .humanTyping ? 14 : 12)
        .frame(height: 42)
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityLabel(
            signal.kind == .humanTyping ? "对方正在输入" : machineStatusText(at: Date())
        )
    }

    private func machineStatusText(at date: Date) -> String {
        let startedAt = Date(
            timeIntervalSince1970: TimeInterval(signal.startedAtMilliseconds) / 1_000
        )
        let taskStartedAt = Date(
            timeIntervalSince1970: TimeInterval(signal.taskStartedAtMilliseconds) / 1_000
        )
        let elapsed = max(0, Int(date.timeIntervalSince(startedAt)))
        let taskElapsed = max(elapsed, Int(date.timeIntervalSince(taskStartedAt)))
        let title: String
        switch signal.kind {
        case .machineThinking: title = "思考中"
        case .machineTool: title = "正在使用工具"
        case .machineWaiting: title = "等待确认"
        case .machineWorking: title = "正在执行"
        case .humanTyping: return ""
        }
        return "\(title)\(elapsed)秒，任务总耗时\(taskElapsed)秒"
    }
}

private struct ScrollViewKeyboardDismissInstaller: UIViewRepresentable {
    let onTap: (UIWindow?) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(onTap: onTap)
    }

    func makeUIView(context: Context) -> UIView {
        let view = UIView(frame: .zero)
        view.isUserInteractionEnabled = false
        scheduleInstallation(from: view, coordinator: context.coordinator)
        return view
    }

    func updateUIView(_ view: UIView, context: Context) {
        context.coordinator.onTap = onTap
        scheduleInstallation(from: view, coordinator: context.coordinator)
    }

    static func dismantleUIView(_ view: UIView, coordinator: Coordinator) {
        coordinator.uninstall()
    }

    private func scheduleInstallation(from view: UIView, coordinator: Coordinator) {
        DispatchQueue.main.async { [weak view, weak coordinator] in
            guard let view, let coordinator else { return }
            coordinator.install(from: view)
        }
    }

    @MainActor
    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        var onTap: (UIWindow?) -> Void
        private weak var scrollView: UIScrollView?
        private lazy var tapGesture: UITapGestureRecognizer = {
            let gesture = UITapGestureRecognizer(
                target: self,
                action: #selector(handleTap)
            )
            gesture.cancelsTouchesInView = false
            gesture.delegate = self
            return gesture
        }()

        init(onTap: @escaping (UIWindow?) -> Void) {
            self.onTap = onTap
        }

        func install(from view: UIView) {
            var ancestor = view.superview
            while let current = ancestor, !(current is UIScrollView) {
                ancestor = current.superview
            }
            guard let nextScrollView = ancestor as? UIScrollView else { return }
            guard scrollView !== nextScrollView else { return }
            uninstall()
            nextScrollView.addGestureRecognizer(tapGesture)
            scrollView = nextScrollView
        }

        func uninstall() {
            scrollView?.removeGestureRecognizer(tapGesture)
            scrollView = nil
        }

        @objc private func handleTap() {
            let tappedWindow = scrollView?.window
            onTap(tappedWindow)
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer
        ) -> Bool {
            true
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldReceive touch: UITouch
        ) -> Bool {
            var touchedView = touch.view
            while let current = touchedView, current !== scrollView {
                if current is UITextView || current is UIControl {
                    return false
                }
                touchedView = current.superview
            }
            return true
        }
    }
}

@MainActor
private func dismissKeyboard(in window: UIWindow?) {
    let didEndEditing = window?.endEditing(true) ?? false
    if !didEndEditing {
        UIApplication.shared.sendAction(
            #selector(UIResponder.resignFirstResponder),
            to: nil,
            from: nil,
            for: nil
        )
    }
}

@MainActor
private func dismissKeyboard() {
    let activeWindow = UIApplication.shared.connectedScenes
        .compactMap { $0 as? UIWindowScene }
        .filter { $0.activationState == .foregroundActive }
        .flatMap(\.windows)
        .first(where: \.isKeyWindow)
    dismissKeyboard(in: activeWindow)
}

private struct EmptyMessagesView: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "message")
                .font(.system(size: 28))
                .foregroundStyle(Color(red: 0.56, green: 0.59, blue: 0.64))
            Text("暂无消息")
                .font(.system(size: 16, weight: .bold))
                .foregroundStyle(RemoteIMStyle.textPrimary)
            Text("发送一条消息开始远程任务。")
                .font(.system(size: 13))
                .foregroundStyle(RemoteIMStyle.textSecondary)
        }
        .frame(maxWidth: .infinity)
    }
}

private struct MessageBubbleView: View {
    let message: RemoteIMMessage
    let approvalDecisionState: ApprovalDecisionDisplayState
    let isVideoDownloading: Bool
    let isVoicePlaying: Bool
    let playVoice: () -> Void
    let previewImage: (UIImage, CGRect) -> Void
    let previewVideo: () -> Void
    let previewFile: () -> Void
    let isSelectingText: Bool
    let finishSelectingText: () -> Void
    let showActions: (CGRect) -> Void
    let reply: () -> Void
    let openQuote: (RemoteIMQuote) -> Void
    @State private var actionSourceFrame: CGRect = .zero
    @State private var isActionPressing = false
    @State private var requestsAccessibilityActions = false
    @State private var textSelectionController = MessageTextSelectionController()

    var body: some View {
        MessageBubbleLayout(isOutgoing: message.direction == .outgoing,
                            showsHeader: false) {
            EmptyView()
        } metadata: {
            EmptyView()
        } content: {
            if usesInlineDate {
                messageContent
            } else {
                MarkdownTrailingDate(timestamp: messageDate) { messageContent }
            }
        }
        #if targetEnvironment(simulator)
        .background {
            if ProcessInfo.processInfo.environment["MAICHAT_LAYOUT_PROBES"] == "1" {
                MessageGeometryProbe(identifier: "message-row-\(message.id.uuidString)")
            }
        }
        #endif
        // 不使用系统 contextMenu：它会把被长按的消息单独提亮/放大，且视觉风格
        // 与 MaiChat 完全不同。长按只打开根层自绘卡片，消息本身保持原样。
        .onLongPressGesture(
            minimumDuration: 0.42,
            perform: {
                guard !isSelectingText else { return }
                showActions(actionSourceFrame)
            },
            onPressingChanged: { isPressing in
                guard !isSelectingText, isActionPressing != isPressing else { return }
                isActionPressing = isPressing
            }
        )
        .accessibilityAction(named: "消息操作") {
            guard !isSelectingText else { return }
            if !requestsAccessibilityActions {
                requestsAccessibilityActions = true
            }
        }
        .accessibilityAction(named: "引用回复") {
            reply()
        }
    }

    private var messageDate: String {
        RemoteIMTimestampTextPolicy.displayText(for: message.createdAt)
    }

    private var usesInlineDate: Bool {
        message.imageAttachment == nil && message.fileAttachment == nil &&
        message.videoAttachment == nil && message.voiceAttachment == nil &&
        message.approvalRequest == nil
    }

    private var attachmentCaption: String? {
        guard message.imageAttachment != nil || message.fileAttachment != nil
            || message.videoAttachment != nil || message.voiceAttachment != nil else { return nil }
        let clean = message.text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !clean.isEmpty,
              !clean.hasPrefix("[图片消息]"),
              !clean.hasPrefix("[文件消息]"),
              !clean.hasPrefix("[视频消息"),
              !clean.hasPrefix("[语音消息") else { return nil }
        return clean
    }

    @ViewBuilder private var attachmentCaptionView: some View {
        if let attachmentCaption {
            MarkdownLikeText(attachmentCaption)
                .font(.system(size: 13, weight: .regular))
                .lineSpacing(3)
                .foregroundStyle(RemoteIMStyle.textPrimary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var messageContent: some View {
        HStack(alignment: .bottom, spacing: 10) {
            VStack(alignment: .leading, spacing: 8) {
                if let quote = message.quote {
                    Button {
                        guard !quote.messageID.isEmpty else { return }
                        openQuote(quote)
                    } label: {
                        HStack(spacing: 8) {
                            Capsule()
                                .fill(RemoteIMStyle.blue)
                                .frame(width: 3, height: 28)
                            Text(quote.senderID.isEmpty
                                ? quote.digest
                                : "\(quote.senderID)：\(quote.digest)")
                                .font(.system(size: 12))
                                .foregroundStyle(RemoteIMStyle.textSecondary)
                                .lineLimit(2)
                                .multilineTextAlignment(.leading)
                            Spacer(minLength: 0)
                        }
                        .padding(.horizontal, 9)
                        .padding(.vertical, 7)
                        .background(RemoteIMStyle.blueSoft, in: RoundedRectangle(cornerRadius: 7))
                    }
                    .buttonStyle(.plain)
                    .disabled(quote.messageID.isEmpty)
                }

                Group {
                    if let videoAttachment = message.videoAttachment {
                    VStack(alignment: .leading, spacing: 8) {
                        if message.captionAbove { attachmentCaptionView }
                        RemoteIMVideoButton(attachment: videoAttachment,
                            isIncoming: message.direction == .incoming,
                            isDownloadingHint: isVideoDownloading, action: previewVideo)
                        if !message.captionAbove { attachmentCaptionView }
                    }
                } else if let imageAttachment = message.imageAttachment {
                    VStack(alignment: .leading, spacing: 8) {
                        if message.captionAbove { attachmentCaptionView }
                        ImageBubbleContent(
                            attachment: imageAttachment,
                            previewImage: previewImage
                        )
                        if !message.captionAbove { attachmentCaptionView }
                    }
                } else if let fileAttachment = message.fileAttachment {
                    VStack(alignment: .leading, spacing: 8) {
                        if message.captionAbove { attachmentCaptionView }
                        Button(action: previewFile) {
                            FileBubbleContent(attachment: fileAttachment)
                        }
                        .buttonStyle(.plain)
                        if !message.captionAbove { attachmentCaptionView }
                    }
                } else if let voiceAttachment = message.voiceAttachment {
                    VStack(alignment: .leading, spacing: 8) {
                        if message.captionAbove { attachmentCaptionView }
                        Button(action: playVoice) {
                            VoiceBubbleContent(
                                attachment: voiceAttachment,
                                isPlaying: isVoicePlaying
                            )
                        }
                        .buttonStyle(.plain)
                        if !message.captionAbove { attachmentCaptionView }
                    }
                } else if let approvalRequest = message.approvalRequest {
                    VStack(alignment: .leading, spacing: 12) {
                        MarkdownLikeText(message.text)
                            .font(.system(size: 13, weight: .regular))
                            .lineSpacing(3)
                            .foregroundStyle(RemoteIMStyle.textPrimary)
                            .lineLimit(nil)
                            .multilineTextAlignment(.leading)
                            .fixedSize(horizontal: false, vertical: true)
                            .layoutPriority(1)

                        Divider()

                        RemoteIMApprovalActionsView(
                            request: approvalRequest,
                            messageID: message.id,
                            decisionState: approvalDecisionState
                        )
                    }
                } else {
                    MarkdownLikeText(message.text, trailingTimestamp: messageDate)
                        .font(.system(size: 13, weight: .regular))
                        .lineSpacing(3)
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                        .lineLimit(nil)
                        .multilineTextAlignment(.leading)
                        .fixedSize(horizontal: false, vertical: true)
                        .layoutPriority(1)
                    }
                }
            }
            .opacity(isSelectingText ? 0 : 1)
            .allowsHitTesting(!isSelectingText)
            .overlay(alignment: .topLeading) {
                if isSelectingText {
                    SelectableMessageTextView(
                        text: RemoteIMMessageCopyPolicy.selectionText(for: message),
                        selectionController: textSelectionController
                    )
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                    .clipped()
                }
            }
            .padding(.horizontal, 13)
            .padding(.vertical, 11)
            .background(bubbleBackground, in: RoundedRectangle(cornerRadius: 10, style: .continuous))

            if message.direction == .outgoing {
                StatusIcon(status: message.status)
                    .padding(.bottom, 4)
            }
        }
        .background {
            GeometryReader { geometry in
                Color.clear
                    .onAppear {
                        actionSourceFrame = geometry.frame(
                            in: .named(RemoteIMImagePreviewLayout.coordinateSpaceName)
                        )
                    }
                    .onChange(of: isActionPressing) { shouldRefresh in
                        guard shouldRefresh else { return }
                        let nextFrame = geometry.frame(
                            in: .named(RemoteIMImagePreviewLayout.coordinateSpaceName)
                        )
                        if actionSourceFrame != nextFrame {
                            actionSourceFrame = nextFrame
                        }
                    }
                    .onChange(of: requestsAccessibilityActions) { shouldShow in
                        guard shouldShow else { return }
                        let nextFrame = geometry.frame(
                            in: .named(RemoteIMImagePreviewLayout.coordinateSpaceName)
                        )
                        actionSourceFrame = nextFrame
                        requestsAccessibilityActions = false
                        showActions(nextFrame)
                    }
            }
        }
        .overlay(alignment: .topTrailing) {
            if isSelectingText {
                HStack(spacing: 2) {
                    Button {
                        UIPasteboard.general.string = textSelectionController.selectedText(
                            fallback: RemoteIMMessageCopyPolicy.selectionText(for: message)
                        )
                        finishSelectingText()
                    } label: {
                        Label("复制", systemImage: "doc.on.doc")
                            .font(.system(size: 12, weight: .semibold))
                            .padding(.horizontal, 9)
                            .frame(height: 30)
                    }
                    Button(action: finishSelectingText) {
                        Image(systemName: "checkmark")
                            .font(.system(size: 12, weight: .bold))
                            .frame(width: 30, height: 30)
                    }
                    .accessibilityLabel("完成选择")
                }
                .buttonStyle(.plain)
                .foregroundStyle(.white)
                .background(
                    Color(red: 0.25, green: 0.25, blue: 0.27),
                    in: Capsule()
                )
                .shadow(color: Color.black.opacity(0.18), radius: 8, y: 3)
                .padding(5)
            }
        }
    }

    private var bubbleBackground: Color {
        message.direction == .outgoing ? RemoteIMStyle.outgoingBubbleBackground : .clear
    }
}

private enum ApprovalDecisionDisplayState {
    case available
    case sending
    case sent
    case resolved
    case autoDeclined
}

private struct RemoteIMApprovalActionsView: View {
    let request: RemoteIMApprovalRequest
    let messageID: UUID
    let decisionState: ApprovalDecisionDisplayState
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var submittingAction: RemoteIMApprovalAction?
    @State private var submittedAction: RemoteIMApprovalAction?
    @State private var didLogRendered = false

    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            if decisionState == .autoDeclined {
                Label("审批已因新消息自动拒绝", systemImage: "xmark.circle.fill")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(Color.orange)
                    .accessibilityIdentifier("remote-im-approval-auto-declined")
            } else if decisionState == .resolved {
                Label("审批已处理", systemImage: "checkmark.circle.fill")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.textSecondary)
                    .accessibilityIdentifier("remote-im-approval-resolved")
            } else if decisionState == .sent ||
                        (decisionState == .available && submittedAction != nil) {
                Label("审批选择已发送", systemImage: "checkmark.circle.fill")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.green)
                    .accessibilityIdentifier("remote-im-approval-sent")
            } else if decisionState == .sending ||
                        (decisionState == .available && submittingAction != nil) {
                Label("审批选择正在发送…", systemImage: "clock.arrow.circlepath")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.blue)
                    .accessibilityIdentifier("remote-im-approval-sending")
            } else {
                ForEach(request.actions, id: \.self) { action in
                    Button {
                        submit(action)
                    } label: {
                        HStack(spacing: 8) {
                            if submittingAction == action {
                                ProgressView()
                                    .controlSize(.small)
                                    .tint(action == .reject ? .red : .white)
                            }
                            Text(action.title)
                                .font(.system(size: 14, weight: .semibold))
                            Spacer(minLength: 0)
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.horizontal, 12)
                        .frame(height: 40)
                        .foregroundStyle(action == .reject ? Color.red : Color.white)
                        .background(
                            action == .reject ? Color.clear : approvalButtonColor(action),
                            in: RoundedRectangle(cornerRadius: 9, style: .continuous)
                        )
                        .overlay(
                            RoundedRectangle(cornerRadius: 9, style: .continuous)
                                .stroke(
                                    action == .reject ? Color.red.opacity(0.72) : Color.clear,
                                    lineWidth: 1
                                )
                        )
                    }
                    .buttonStyle(.plain)
                    .disabled(submittingAction != nil)
                    .accessibilityIdentifier("remote-im-approval-\(action.rawValue)")
                }
            }
        }
        .onAppear {
            guard !didLogRendered else { return }
            didLogRendered = true
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "approval",
                event: "card-rendered",
                fields: [
                    "message": DiagnosticLogPrivacy.stableTag(messageID.uuidString, prefix: "m"),
                    "action_count": String(request.actions.count),
                    "state": decisionState.diagnosticName,
                ]
            )
        }
    }

    private func approvalButtonColor(_ action: RemoteIMApprovalAction) -> Color {
        action == .approvePrefix ? RemoteIMStyle.green : RemoteIMStyle.blue
    }

    private func submit(_ action: RemoteIMApprovalAction) {
        guard decisionState == .available,
              submittingAction == nil,
              submittedAction == nil
        else { return }
        submittingAction = action
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "approval",
            event: "button-tapped",
            fields: [
                "action": action.rawValue,
                "message_id": messageID.uuidString,
            ]
        )
        Task {
            let sent = await appState.sendApprovalDecision(action, for: request)
            if sent {
                submittedAction = action
            }
            submittingAction = nil
            AppDiagnosticLog.shared.record(
                level: sent ? .info : .error,
                category: "approval",
                event: sent ? "button-sent" : "button-send-failed",
                fields: [
                    "action": action.rawValue,
                    "message_id": messageID.uuidString,
                ]
            )
        }
    }
}

private extension ApprovalDecisionDisplayState {
    var diagnosticName: String {
        switch self {
        case .available: return "available"
        case .sending: return "sending"
        case .sent: return "sent"
        case .resolved: return "resolved"
        case .autoDeclined: return "auto-declined"
        }
    }
}

private struct MessageActionDialog: View {
    let sourceFrame: CGRect
    let dismiss: () -> Void
    let reply: () -> Void
    let selectCopy: () -> Void
    let forward: () -> Void
    let copyAll: () -> Void

    var body: some View {
        GeometryReader { geometry in
            let panelWidth = min(CGFloat(344), max(268, geometry.size.width - 48))
            let showAbove = sourceFrame.minY > 116
            let centerX = min(
                max(sourceFrame.midX, panelWidth / 2 + 16),
                geometry.size.width - panelWidth / 2 - 16
            )
            let centerY = showAbove
                ? max(58, sourceFrame.minY - 50)
                : min(geometry.size.height - 58, sourceFrame.maxY + 50)

            ZStack {
                Color.clear
                    .contentShape(Rectangle())
                    .onTapGesture(perform: dismiss)

                VStack(spacing: 0) {
                    if !showAbove {
                        MessageActionPointer()
                            .fill(Color(red: 0.25, green: 0.25, blue: 0.27))
                            .frame(width: 20, height: 9)
                            .rotationEffect(.degrees(180))
                    }

                    HStack(spacing: 0) {
                        actionButton(
                            title: "引用",
                            systemImage: "quote.opening",
                            action: reply
                        )
                        actionButton(
                            title: "选择",
                            systemImage: "text.cursor",
                            action: selectCopy
                        )
                        actionButton(
                            title: "转发",
                            systemImage: "arrowshape.turn.up.right",
                            action: forward
                        )
                        actionButton(
                            title: "复制",
                            systemImage: "doc.on.doc",
                            action: copyAll
                        )
                    }
                    .frame(width: panelWidth, height: 74)
                    .background(
                        Color(red: 0.25, green: 0.25, blue: 0.27),
                        in: RoundedRectangle(cornerRadius: 14, style: .continuous)
                    )

                    if showAbove {
                        MessageActionPointer()
                            .fill(Color(red: 0.25, green: 0.25, blue: 0.27))
                            .frame(width: 20, height: 9)
                    }
                }
                .shadow(color: Color.black.opacity(0.2), radius: 16, y: 7)
                .position(x: centerX, y: centerY)
            }
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("消息操作")
        .accessibilityAction(.escape, dismiss)
    }

    private func actionButton(
        title: String,
        systemImage: String,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            VStack(spacing: 6) {
                Image(systemName: systemImage)
                    .font(.system(size: 18, weight: .semibold))
                Text(title)
                    .font(.system(size: 13, weight: .medium))
            }
            .foregroundStyle(.white)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(title)
    }
}

private struct MessageActionPointer: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.minX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.midX, y: rect.maxY))
        path.closeSubpath()
        return path
    }
}

private struct ForwardMessageDialog: View {
    let message: RemoteIMMessage
    let contacts: [RemoteIMContact]
    let cancel: () -> Void
    let forward: (RemoteIMContact) -> Void
    @State private var searchText = ""

    private var filteredContacts: [RemoteIMContact] {
        let query = searchText.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard !query.isEmpty else { return contacts }
        return contacts.filter {
            $0.displayName.lowercased().contains(query)
                || $0.userID.lowercased().contains(query)
        }
    }

    var body: some View {
        ZStack {
            Color.black.opacity(0.28)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture(perform: cancel)

            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    VStack(alignment: .leading, spacing: 3) {
                        Text("转发给")
                            .font(.system(size: 20, weight: .bold))
                            .foregroundStyle(RemoteIMStyle.textPrimary)
                        Text(RemoteIMMessageCopyPolicy.selectionText(for: message))
                            .font(.system(size: 12))
                            .foregroundStyle(RemoteIMStyle.textSecondary)
                            .lineLimit(1)
                    }
                    Spacer()
                    Button("取消", action: cancel)
                        .buttonStyle(.plain)
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                }

                HStack(spacing: 10) {
                    Image(systemName: "magnifyingglass")
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                    TextField("搜索联系人", text: $searchText)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                }
                .padding(.horizontal, 13)
                .frame(height: 46)
                .background(
                    RemoteIMStyle.pageBackground,
                    in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                )
                .overlay {
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .stroke(RemoteIMStyle.border, lineWidth: 1)
                }

                ScrollView {
                    LazyVStack(spacing: 8) {
                        ForEach(filteredContacts) { contact in
                            Button {
                                forward(contact)
                            } label: {
                                HStack(spacing: 12) {
                                    Text(String(contact.displayName.prefix(1)).uppercased())
                                        .font(.system(size: 15, weight: .bold))
                                        .foregroundStyle(.white)
                                        .frame(width: 38, height: 38)
                                        .background(RemoteIMStyle.blue, in: Circle())
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(contact.displayName)
                                            .font(.system(size: 15, weight: .semibold))
                                            .foregroundStyle(RemoteIMStyle.textPrimary)
                                            .lineLimit(1)
                                        if contact.displayName != contact.userID {
                                            Text(contact.userID)
                                                .font(.system(size: 12, design: .monospaced))
                                                .foregroundStyle(RemoteIMStyle.textSecondary)
                                                .lineLimit(1)
                                                .truncationMode(.middle)
                                        }
                                    }
                                    Spacer(minLength: 0)
                                    Image(systemName: "paperplane.fill")
                                        .font(.system(size: 14, weight: .semibold))
                                        .foregroundStyle(RemoteIMStyle.blue)
                                }
                                .padding(.horizontal, 12)
                                .frame(height: 58)
                                .background(
                                    RemoteIMStyle.pageBackground,
                                    in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                                )
                            }
                            .buttonStyle(.plain)
                            .accessibilityLabel("转发给 \(contact.displayName)")
                        }

                        if filteredContacts.isEmpty {
                            Text("没有匹配的联系人")
                                .font(.system(size: 14))
                                .foregroundStyle(RemoteIMStyle.textSecondary)
                                .padding(.vertical, 28)
                        }
                    }
                }
                .frame(maxHeight: 340)
            }
            .padding(18)
            .frame(maxWidth: 380)
            .background(
                RemoteIMStyle.panelBackground,
                in: RoundedRectangle(cornerRadius: 18, style: .continuous)
            )
            .overlay {
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .stroke(RemoteIMStyle.border, lineWidth: 1)
            }
            .shadow(color: Color.black.opacity(0.16), radius: 24, y: 10)
            .padding(.horizontal, 24)
        }
        .accessibilityElement(children: .contain)
        .accessibilityAction(.escape, cancel)
    }
}

private final class MenuSuppressingTextView: UITextView {
    override func addInteraction(_ interaction: any UIInteraction) {
        guard !(interaction is UIEditMenuInteraction) else { return }
        super.addInteraction(interaction)
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        guard #available(iOS 16.0, *) else { return }
        for interaction in interactions where interaction is UIEditMenuInteraction {
            removeInteraction(interaction)
        }
    }

    override func canPerformAction(_ action: Selector, withSender sender: Any?) -> Bool {
        action == #selector(UIResponderStandardEditActions.copy(_:))
    }
}

@MainActor
private final class MessageTextSelectionController {
    weak var textView: UITextView?

    func selectedText(fallback: String) -> String {
        guard let textView else { return fallback }
        let fullText = textView.text as NSString
        let range = textView.selectedRange
        guard range.location != NSNotFound,
              range.length > 0,
              NSMaxRange(range) <= fullText.length
        else { return fallback }
        return fullText.substring(with: range)
    }
}

private struct SelectableMessageTextView: UIViewRepresentable {
    let text: String
    let selectionController: MessageTextSelectionController

    func makeCoordinator() -> Coordinator {
        Coordinator(selectionController: selectionController)
    }

    func makeUIView(context: Context) -> UITextView {
        let textView = MenuSuppressingTextView()
        textView.isEditable = false
        textView.isSelectable = true
        textView.isScrollEnabled = false
        textView.alwaysBounceVertical = false
        textView.backgroundColor = .clear
        textView.font = .systemFont(ofSize: 13)
        textView.adjustsFontForContentSizeCategory = true
        textView.textColor = .label
        textView.textContainerInset = .zero
        textView.textContainer.lineFragmentPadding = 0
        textView.textContainer.widthTracksTextView = true
        textView.textContainer.heightTracksTextView = false
        textView.textContainer.lineBreakMode = .byWordWrapping
        textView.clipsToBounds = true
        textView.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        textView.setContentHuggingPriority(.defaultLow, for: .horizontal)
        textView.delegate = context.coordinator
        textView.text = text
        textView.selectedRange = NSRange(location: 0, length: (text as NSString).length)
        selectionController.textView = textView
        DispatchQueue.main.async { [weak textView] in
            guard let textView, textView.window != nil else { return }
            textView.becomeFirstResponder()
            textView.selectedRange = NSRange(location: 0, length: (textView.text as NSString).length)
        }
        textView.accessibilityIdentifier = "selectable-message-copy-text"
        textView.accessibilityLabel = "可选择的消息正文"
        return textView
    }

    func updateUIView(_ textView: UITextView, context: Context) {
        selectionController.textView = textView
        guard textView.text != text else { return }
        textView.text = text
        // 选择模式只承载一条不可编辑的消息快照；它不会在展示期间被服务端改写。
        // 若视图因复用拿到另一条文本，重置为全选比保留上一条消息的 NSRange 更安全。
        textView.selectedRange = NSRange(location: 0, length: (text as NSString).length)
    }

    static func dismantleUIView(
        _ textView: UITextView,
        coordinator: Coordinator
    ) {
        textView.resignFirstResponder()
        textView.delegate = nil
        if coordinator.selectionController?.textView === textView {
            coordinator.selectionController?.textView = nil
        }
    }

    final class Coordinator: NSObject, UITextViewDelegate {
        weak var selectionController: MessageTextSelectionController?

        init(selectionController: MessageTextSelectionController) {
            self.selectionController = selectionController
        }

        @available(iOS 16.0, *)
        func textView(
            _ textView: UITextView,
            editMenuForTextIn range: NSRange,
            suggestedActions: [UIMenuElement]
        ) -> UIMenu? {
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "remote-im-ui",
                event: "message-system-edit-menu-suppressed",
                fields: ["api": "single-range"]
            )
            return UIMenu(children: [])
        }

        @available(iOS 26.0, *)
        func textView(
            _ textView: UITextView,
            editMenuForTextInRanges ranges: [NSValue],
            suggestedActions: [UIMenuElement]
        ) -> UIMenu? {
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "remote-im-ui",
                event: "message-system-edit-menu-suppressed",
                fields: ["api": "multi-range"]
            )
            return UIMenu(children: [])
        }
    }
}

private enum RemoteIMPhotoLibraryWriter {
    // Photos 会在自己的私有串行队列执行 change block。隔离保证写在真正执行
    // performChanges 的函数上，调用方无论来自哪个 actor，都不会把 MainActor
    // 隔离传进 change block。
    nonisolated static func saveImage(at fileURL: URL) async -> String? {
        do {
            try await PHPhotoLibrary.shared().performChanges {
                PHAssetChangeRequest.creationRequestForAssetFromImage(atFileURL: fileURL)
            }
            return nil
        } catch {
            return error.localizedDescription
        }
    }
}

private struct FullScreenImagePreviewView: View {
    let presentation: PresentedRemoteIMImage
    let isExpanded: Bool
    let close: () -> Void
    @State private var isSaving = false
    @State private var saveResultText: String?

    var body: some View {
        GeometryReader { geometry in
            let fittedSize = aspectFitSize(
                imageSize: presentation.image.size,
                containerSize: geometry.size
            )
            let destinationFrame = CGRect(
                x: (geometry.size.width - fittedSize.width) / 2,
                y: (geometry.size.height - fittedSize.height) / 2,
                width: fittedSize.width,
                height: fittedSize.height
            )
            let imageFrame = isExpanded ? destinationFrame : presentation.sourceFrame

            ZStack(alignment: .bottomTrailing) {
                Color.black
                    .opacity(isExpanded ? 1 : 0)
                    .ignoresSafeArea()
                    .onTapGesture(perform: close)

                Image(uiImage: presentation.image)
                        .resizable()
                        .scaledToFit()
                        .frame(
                            width: imageFrame.width,
                            height: imageFrame.height
                        )
                        .position(x: imageFrame.midX, y: imageFrame.midY)
                        .contentShape(Rectangle())
                        .onTapGesture(perform: close)
                        .accessibilityLabel("图片预览")
                        .accessibilityIdentifier("remote-im-image-preview")

                VStack(alignment: .trailing, spacing: 14) {
                    if let saveResultText {
                        Text(saveResultText)
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundStyle(.white)
                            .padding(.horizontal, 12)
                            .frame(height: 34)
                            .background(.black.opacity(0.62), in: Capsule())
                            .transition(.opacity.combined(with: .move(edge: .bottom)))
                    }

                    HStack(spacing: 14) {
                        ShareLink(item: URL(fileURLWithPath: presentation.item.localFilePath)) {
                            imagePreviewActionIcon("arrowshape.turn.up.right.fill")
                        }
                        .accessibilityLabel("分享图片")

                        Button(action: saveImageToPhotoLibrary) {
                            imagePreviewActionIcon(isSaving ? "hourglass" : "arrow.down.to.line")
                        }
                        .buttonStyle(.plain)
                        .disabled(isSaving)
                        .accessibilityLabel(isSaving ? "正在保存图片" : "保存图片")
                    }
                }
                .padding(.trailing, 22)
                .padding(.bottom, 24)
                .opacity(isExpanded ? 1 : 0)
                .allowsHitTesting(isExpanded)
            }
            .frame(width: geometry.size.width, height: geometry.size.height)
        }
        .accessibilityAction(.escape, close)
    }

    private func imagePreviewActionIcon(_ systemName: String) -> some View {
        Image(systemName: systemName)
            .font(.system(size: 18, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: 48, height: 48)
            .background(.black.opacity(0.56), in: Circle())
            .overlay {
                Circle().stroke(.white.opacity(0.12), lineWidth: 1)
            }
    }

    private func saveImageToPhotoLibrary() {
        guard !isSaving else { return }
        isSaving = true
        saveResultText = nil
        let fileURL = URL(fileURLWithPath: presentation.item.localFilePath)

        Task { @MainActor in
            guard await requestPhotoLibraryPermission() else {
                isSaving = false
                saveResultText = "请在系统设置中允许访问照片"
                return
            }
            let saveError = await RemoteIMPhotoLibraryWriter.saveImage(at: fileURL)
            if let saveError {
                isSaving = false
                withAnimation(.easeOut(duration: 0.2)) {
                    saveResultText = saveError
                }
            } else {
                isSaving = false
                withAnimation(.easeOut(duration: 0.2)) {
                    saveResultText = "已保存到相册"
                }
            }
        }
    }

    private func requestPhotoLibraryPermission() async -> Bool {
        switch PHPhotoLibrary.authorizationStatus(for: .readWrite) {
        case .authorized, .limited:
            return true
        case .notDetermined:
            let status = await withCheckedContinuation { continuation in
                PHPhotoLibrary.requestAuthorization(for: .readWrite) { status in
                    continuation.resume(returning: status)
                }
            }
            return status == .authorized || status == .limited
        case .denied, .restricted:
            return false
        @unknown default:
            return false
        }
    }

    private func aspectFitSize(imageSize: CGSize, containerSize: CGSize) -> CGSize {
        guard imageSize.width > 0,
              imageSize.height > 0,
              containerSize.width > 0,
              containerSize.height > 0
        else { return .zero }
        let scale = min(
            containerSize.width / imageSize.width,
            containerSize.height / imageSize.height
        )
        return CGSize(width: imageSize.width * scale, height: imageSize.height * scale)
    }
}

private struct FullScreenVideoPreviewView: View {
    let item: RemoteIMVideoPreviewItem
    let close: () -> Void

    var body: some View {
        ZStack(alignment: .topTrailing) {
            Color.black.ignoresSafeArea()
            LocalVideoPlayer(url: URL(fileURLWithPath: item.localFilePath))
                .ignoresSafeArea()
                .accessibilityIdentifier("remote-im-video-preview")

            Button(action: close) {
                Image(systemName: "xmark")
                    .font(.system(size: 16, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 44, height: 44)
                    .background(.black.opacity(0.52), in: Circle())
            }
            .buttonStyle(.plain)
            .padding(.top, 18)
            .padding(.trailing, 18)
            .accessibilityLabel("关闭视频预览")
        }
        .statusBarHidden(true)
    }
}

private struct LocalVideoPlayer: UIViewControllerRepresentable {
    let url: URL

    func makeUIViewController(context _: Context) -> AVPlayerViewController {
        let controller = AVPlayerViewController()
        controller.player = AVPlayer(url: url)
        controller.showsPlaybackControls = true
        controller.videoGravity = .resizeAspect
        controller.player?.play()
        return controller
    }

    func updateUIViewController(_ controller: AVPlayerViewController, context _: Context) {
        guard let currentAsset = controller.player?.currentItem?.asset as? AVURLAsset,
              currentAsset.url != url
        else { return }
        controller.player?.replaceCurrentItem(with: AVPlayerItem(url: url))
        controller.player?.play()
    }

    static func dismantleUIViewController(_ controller: AVPlayerViewController, coordinator _: Void) {
        controller.player?.pause()
        controller.player = nil
    }
}

private struct RemoteIMFilePreviewItem: Identifiable {
    let id: UUID
    let attachment: RemoteIMFileAttachment

    init?(message: RemoteIMMessage) {
        guard let attachment = message.fileAttachment else { return nil }
        self.id = message.id
        self.attachment = attachment
    }
}

private struct FullScreenFilePreviewView: View {
    let item: RemoteIMFilePreviewItem
    let close: () -> Void
    @State private var loaded = false
    @State private var integrityError: String?
    @State private var previewText = ""

    var body: some View {
        NavigationStack {
            Group {
                if !loaded {
                    ProgressView("正在读取文件")
                } else if let integrityError {
                    VStack(spacing: 12) {
                        Image(systemName: "exclamationmark.shield")
                            .font(.system(size: 34, weight: .semibold))
                            .foregroundStyle(.red)
                        Text("Diff 校验失败")
                            .font(.headline)
                        Text(integrityError)
                            .font(.footnote)
                            .foregroundStyle(RemoteIMStyle.textSecondary)
                    }
                    .padding(24)
                } else if item.attachment.mimeType == "text/html" {
                    RemoteIMHTMLPreview(filePath: item.attachment.localFilePath, html: previewText)
                } else if item.attachment.mimeType == "text/markdown" ||
                            item.attachment.fileName.lowercased().hasSuffix(".md")
                {
                    RemoteIMMarkdownFilePreview(text: previewText)
                } else {
                    RemoteIMQuickLookPreview(filePath: item.attachment.localFilePath)
                }
            }
            .navigationTitle(isGitDiff ? "代码 Diff" : item.attachment.fileName)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    ShareLink(item: URL(fileURLWithPath: item.attachment.localFilePath)) {
                        Image(systemName: "square.and.arrow.up")
                    }
                    .accessibilityLabel("分享文件")
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button("关闭", action: close)
                }
            }
        }
        .task(id: item.attachment.localFilePath) { await loadPreview() }
    }

    private var isGitDiff: Bool {
        RemoteIMGitDiffDisplayPolicy.isGitDiff(item.attachment)
    }

    private func loadPreview() async {
        loaded = false
        let path = item.attachment.localFilePath
        let expected = isGitDiff ? RemoteIMGitDiffDisplayPolicy.expectedSHA256(fileName: item.attachment.fileName) : nil
        let readsText = item.attachment.mimeType == "text/html" || item.attachment.mimeType == "text/markdown"
            || item.attachment.fileName.lowercased().hasSuffix(".md")
        do {
            let result = try await RemoteIMBackgroundWork.fileCancellable { cancellation -> (String?, String) in
                let url = URL(fileURLWithPath: path)
                if let expected {
                    let handle = try FileHandle(forReadingFrom: url)
                    defer { try? handle.close() }
                    var hash = SHA256()
                    while let chunk = try handle.read(upToCount: 64 * 1024), !chunk.isEmpty {
                        try cancellation.check()
                        hash.update(data: chunk)
                    }
                    let actual = hash.finalize().map { String(format: "%02x", $0) }.joined()
                    if actual != expected { return ("文件内容与发送方提供的 SHA256 不一致，已停止渲染。", "") }
                }
                return (nil, readsText ? try String(contentsOf: url, encoding: .utf8) : "")
            }
            guard !Task.isCancelled else { return }
            if result.0 == nil, readsText, item.attachment.mimeType != "text/html" {
                try await MarkdownPreparation.prepare([result.1])
            }
            guard !Task.isCancelled else { return }
            integrityError = result.0
            previewText = result.1
            loaded = true
        } catch {
            guard !Task.isCancelled else { return }
            integrityError = "无法读取文件：" + error.localizedDescription
            loaded = true
        }
    }

}

private struct RemoteIMQuickLookPreview: UIViewControllerRepresentable {
    let filePath: String

    func makeCoordinator() -> Coordinator {
        Coordinator(filePath: filePath)
    }

    func makeUIViewController(context: Context) -> QLPreviewController {
        let controller = QLPreviewController()
        controller.dataSource = context.coordinator
        return controller
    }

    func updateUIViewController(_ controller: QLPreviewController, context: Context) {
        guard context.coordinator.filePath != filePath else { return }
        context.coordinator.filePath = filePath
        controller.reloadData()
    }

    final class Coordinator: NSObject, QLPreviewControllerDataSource {
        var filePath: String

        init(filePath: String) {
            self.filePath = filePath
        }

        func numberOfPreviewItems(in _: QLPreviewController) -> Int {
            1
        }

        func previewController(_: QLPreviewController, previewItemAt _: Int) -> QLPreviewItem {
            URL(fileURLWithPath: filePath) as NSURL
        }
    }
}

private struct RemoteIMMarkdownFilePreview: View {
    let text: String

    var body: some View {
        ScrollView {
            MarkdownLikeText(text)
                .font(.system(size: 14))
                .padding(18)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .background(RemoteIMStyle.panelBackground)
    }


}

private struct RemoteIMHTMLPreview: UIViewRepresentable {
    let filePath: String
    let html: String
    final class Coordinator {
        var filePath: String?
        var html: String?
    }
    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context _: Context) -> WKWebView {
        let configuration = WKWebViewConfiguration()
        configuration.preferences.javaScriptEnabled = false
        let webView = WKWebView(frame: .zero, configuration: configuration)
        webView.backgroundColor = .white
        return webView
    }

    func updateUIView(_ webView: WKWebView, context: Context) {
        guard context.coordinator.filePath != filePath || context.coordinator.html != html else { return }
        context.coordinator.filePath = filePath
        context.coordinator.html = html
        webView.loadHTMLString(html, baseURL: URL(fileURLWithPath: filePath).deletingLastPathComponent())
    }
}

private struct RemoteIMVideoFileState: Equatable, Sendable {
    let isPlayable: Bool
    let isDownloading: Bool

    init(isPlayable: Bool, isDownloading: Bool) {
        self.isPlayable = isPlayable
        self.isDownloading = isDownloading
    }

    init(attachment: RemoteIMVideoAttachment, isDownloadingHint: Bool) {
        guard !attachment.localPath.isEmpty else {
            self.isPlayable = false
            self.isDownloading = false
            return
        }

        let localURL = URL(fileURLWithPath: attachment.localPath)
        let values = try? localURL.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
        let isPlayable = values?.isRegularFile == true && (values?.fileSize ?? 0) > 0
        self.isPlayable = isPlayable
        self.isDownloading = !isPlayable && (
            isDownloadingHint || FileManager.default.fileExists(
                atPath: RemoteIMMediaStorage.partialDownloadURL(for: localURL).path
            )
        )
    }
}

private struct RemoteIMVideoStatusKey: Equatable {
    let attachment: RemoteIMVideoAttachment
    let downloading: Bool
    let revision: UInt64
}

private struct RemoteIMVideoButton: View {
    let attachment: RemoteIMVideoAttachment
    let isIncoming: Bool
    let isDownloadingHint: Bool
    let action: () -> Void
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var fileState = RemoteIMVideoFileState(isPlayable: false, isDownloading: true)
    var body: some View {
        Button(action: action) {
            VideoBubbleContent(attachment: attachment, isIncoming: isIncoming, fileState: fileState)
        }
        .buttonStyle(.plain)
        .disabled(!fileState.isPlayable)
        .accessibilityIdentifier("remote-im-video-bubble")
        .task(id: RemoteIMVideoStatusKey(attachment: attachment, downloading: isDownloadingHint,
                                       revision: appState.mediaFileRevision)) {
            let attachment = attachment
            let hint = isDownloadingHint
            do {
                let result = try await RemoteIMBackgroundWork.metadata {
                    RemoteIMVideoFileState(attachment: attachment, isDownloadingHint: hint)
                }
                guard !Task.isCancelled else { return }
                fileState = result
            } catch { }
        }
    }
}

private struct VideoBubbleContent: View {
    let attachment: RemoteIMVideoAttachment
    let isIncoming: Bool
    let fileState: RemoteIMVideoFileState

    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            ZStack {
                RemoteIMAsyncImage(
                    filePath: attachment.coverPath,
                    maximumPointSize: CGSize(width: 220, height: previewHeight)
                ) { image in
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFill()
                        .frame(width: 220, height: previewHeight)
                        .clipped()
                        .accessibilityLabel("视频封面")
                        .accessibilityIdentifier("remote-im-video-cover")
                } placeholder: { _ in
                    ZStack {
                        LinearGradient(
                            colors: [Color(red: 0.10, green: 0.17, blue: 0.27), Color(red: 0.18, green: 0.32, blue: 0.47)],
                            startPoint: .topLeading,
                            endPoint: .bottomTrailing
                        )
                        Image(systemName: "video.fill")
                            .font(.system(size: 30, weight: .semibold))
                            .foregroundStyle(.white.opacity(0.72))
                    }
                    .frame(width: 220, height: previewHeight)
                }

                if fileState.isPlayable {
                    Image(systemName: "play.fill")
                        .font(.system(size: 20, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(width: 52, height: 52)
                        .background(.black.opacity(0.5), in: Circle())
                        .overlay(Circle().stroke(.white.opacity(0.85), lineWidth: 1.5))
                } else if fileState.isDownloading {
                    VStack(spacing: 8) {
                        ProgressView()
                            .tint(.white)
                        Text("视频下载中")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(.white)
                    }
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                    .background(.black.opacity(0.5), in: Capsule())
                } else {
                    VStack(spacing: 8) {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .font(.system(size: 20, weight: .bold))
                        Text(isIncoming ? "视频文件已丢失" : "本地视频已丢失")
                            .font(.system(size: 12, weight: .semibold))
                    }
                    .foregroundStyle(.white)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                    .background(.black.opacity(0.56), in: Capsule())
                }

                Text(durationText)
                    .font(.system(size: 11, weight: .semibold, design: .monospaced))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 7)
                    .frame(height: 22)
                    .background(.black.opacity(0.56), in: Capsule())
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                    .padding(8)
            }
            .frame(width: 220, height: previewHeight)
            .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
            .background(Color(red: 0.945, green: 0.957, blue: 0.973), in: RoundedRectangle(cornerRadius: 10, style: .continuous))

            Text(videoStatusText)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .accessibilityIdentifier("remote-im-video-status")
        }
        .contentShape(Rectangle())
    }

    private var videoStatusText: String {
        if fileState.isPlayable { return "点击播放" }
        if fileState.isDownloading { return "封面可先显示，视频正在后台下载" }
        return isIncoming ? "视频文件已丢失，暂时无法播放" : "本地视频文件已丢失"
    }

    private var previewHeight: CGFloat {
        guard attachment.width > 0, attachment.height > 0 else { return 142 }
        let ratio = CGFloat(attachment.width) / CGFloat(attachment.height)
        return min(180, max(118, 220 / max(0.35, ratio)))
    }

    private var durationText: String {
        let minutes = attachment.durationSeconds / 60
        let seconds = attachment.durationSeconds % 60
        return String(format: "%d:%02d", minutes, seconds)
    }
}

private struct ImageBubbleContent: View {
    let attachment: RemoteIMImageAttachment
    let previewImage: (UIImage, CGRect) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            RemoteIMAsyncImage(
                filePath: attachment.localFilePath,
                maximumPointSize: CGSize(width: 220, height: 180)
            ) { image in
                Image(uiImage: image)
                    .resizable()
                    .scaledToFit()
                    .frame(maxWidth: 220, maxHeight: 180)
                    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                    .background(Color(red: 0.945, green: 0.957, blue: 0.973), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
                    .overlay {
                        GeometryReader { geometry in
                            Button {
                                previewImage(
                                    image,
                                    geometry.frame(
                                        in: .named(
                                            RemoteIMImagePreviewLayout.coordinateSpaceName
                                        )
                                    )
                                )
                            } label: {
                                Color.clear
                                    .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                            .accessibilityLabel("消息图片")
                            .accessibilityIdentifier("remote-im-message-image")
                        }
                    }
            } placeholder: { hasFailed in
                Group {
                    if hasFailed {
                        HStack(spacing: 8) {
                            Image(systemName: "photo")
                            Text("图片文件已丢失，无法预览")
                                .font(.system(size: 13, weight: .semibold))
                        }
                    } else {
                        ProgressView()
                    }
                }
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .frame(width: 180, height: 120)
                .background(Color(red: 0.945, green: 0.957, blue: 0.973), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
            }
            Text(URL(fileURLWithPath: attachment.localFilePath).lastPathComponent)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .lineLimit(1)
                .truncationMode(.middle)
        }
    }
}

private struct FileBubbleContent: View {
    let attachment: RemoteIMFileAttachment

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: isGitDiff ? "arrow.left.arrow.right.square" : attachment.mimeType == "text/html" ? "safari" : "doc.text")
                .font(.system(size: 20, weight: .semibold))
                .foregroundStyle(RemoteIMStyle.blue)
                .frame(width: 38, height: 38)
                .background(RemoteIMStyle.blueSoft, in: RoundedRectangle(cornerRadius: 10, style: .continuous))

            VStack(alignment: .leading, spacing: 4) {
                Text(attachment.fileName)
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Text(
                    isGitDiff
                        ? "代码 Diff · 点击查看"
                        : attachment.mimeType == "text/html" ? "HTML 文件" : "Markdown 文件"
                )
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
        }
        .frame(minWidth: 190, alignment: .leading)
        .contentShape(Rectangle())
    }

    private var isGitDiff: Bool {
        RemoteIMGitDiffDisplayPolicy.isGitDiff(attachment)
    }
}

private struct VoiceBubbleContent: View {
    let attachment: RemoteIMVoiceAttachment
    let isPlaying: Bool

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                .font(.system(size: 13, weight: .bold))
                .frame(width: 18)
            HStack(spacing: 3) {
                ForEach(0..<7, id: \.self) { index in
                    Capsule()
                        .fill(RemoteIMStyle.textPrimary.opacity(isPlaying ? 0.85 : 0.55))
                        .frame(width: 3, height: CGFloat([8, 14, 10, 18, 12, 15, 9][index]))
                }
            }
            Text("\(attachment.durationSeconds)s")
                .font(.system(size: 13, weight: .semibold))
        }
        .foregroundStyle(RemoteIMStyle.textPrimary)
        .frame(minWidth: 116, alignment: .leading)
        .contentShape(Rectangle())
    }
}

private struct StatusIcon: View {
    let status: RemoteIMMessageStatus

    var body: some View {
        Image(systemName: systemName)
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(color)
            .accessibilityLabel(accessibilityText)
    }

    private var systemName: String {
        switch status {
        case .pending:
            return "clock.fill"
        case .sent, .received:
            return "checkmark.circle.fill"
        case .failed:
            return "exclamationmark.circle.fill"
        }
    }

    private var color: Color {
        switch status {
        case .pending:
            return .orange
        case .sent, .received:
            return RemoteIMStyle.green
        case .failed:
            return .red
        }
    }

    private var accessibilityText: String {
        switch status {
        case .pending:
            return "发送中"
        case .sent:
            return "已发送"
        case .received:
            return "已收到"
        case .failed:
            return "发送失败"
        }
    }
}

private struct PreparedMarkdown: Sendable {
    let source: String
    let blocks: [MarkdownBlock]
    let inline: [String: AttributedString]
}

private final class PreparedMarkdownBox {
    let value: PreparedMarkdown
    init(_ value: PreparedMarkdown) { self.value = value }
}

private struct PreparedMarkdownInlineKey: EnvironmentKey {
    static let defaultValue: [String: AttributedString] = [:]
}

private extension EnvironmentValues {
    var preparedMarkdownInline: [String: AttributedString] {
        get { self[PreparedMarkdownInlineKey.self] }
        set { self[PreparedMarkdownInlineKey.self] = newValue }
    }
}

private final class MarkdownRenderCache: @unchecked Sendable {
    static let shared = MarkdownRenderCache()
    private let values = NSCache<NSString, PreparedMarkdownBox>()
    private init() { values.countLimit = 256; values.totalCostLimit = 4 * 1024 * 1024 }
    func cached(_ text: String) -> PreparedMarkdown? { values.object(forKey: text as NSString)?.value }
    func prepare(_ text: String) -> PreparedMarkdown {
        dispatchPrecondition(condition: .notOnQueue(.main))
        if let cached = cached(text) { return cached }
        let blocks = parseMarkdownBlocks(text)
        var inline: [String: AttributedString] = [:]
        func append(_ value: String) {
            guard inline[value] == nil else { return }
            let parsed = (try? AttributedString(markdown: value,
                options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))) ?? AttributedString(value)
            inline[value] = MarkdownInlineStyling.apply(to: parsed)
        }
        for block in blocks {
            switch block.kind {
            case .markdown(let value), .sourceNote(let value), .heading(_, let value): append(value)
            case .list(let list): list.items.forEach { append($0.text) }
            case .quote(let quote): append(quote.text)
            case .table(let table):
                table.headers.forEach(append)
                table.rows.forEach { $0.forEach(append) }
            case .code, .divider: break
            }
        }
        let result = PreparedMarkdown(source: text, blocks: blocks, inline: inline)
        let cost = text.utf8.count * 4 + inline.values.reduce(0) { $0 + $1.characters.count * 8 }
        values.setObject(PreparedMarkdownBox(result), forKey: text as NSString, cost: cost)
        return result
    }
}

enum MarkdownPreparation {
    static func prepare(_ texts: [String]) async throws {
        try await RemoteIMBackgroundWork.parseCancellable { cancellation in
            for text in texts {
                try cancellation.check()
                _ = MarkdownRenderCache.shared.prepare(text)
            }
        }
    }
}

struct MarkdownLikeText: View {
    private let source: String
    @State private var prepared: PreparedMarkdown?
    private let trailingTimestamp: String?

    private let retainsPreviousWhilePreparing: Bool

    init(_ text: String, trailingTimestamp: String? = nil, retainsPreviousWhilePreparing: Bool = false) {
        self.retainsPreviousWhilePreparing = retainsPreviousWhilePreparing
        self.source = text
        _prepared = State(initialValue: MarkdownRenderCache.shared.cached(text))
        self.trailingTimestamp = trailingTimestamp
    }

    var body: some View {
        let document = (prepared?.source == source ? prepared : nil) ?? MarkdownRenderCache.shared.cached(source) ?? (retainsPreviousWhilePreparing ? prepared : nil)
        let blocks = document?.blocks ?? []
        return VStack(alignment: .leading, spacing: 12) {
            if document == nil { ProgressView() }
            if blocks.isEmpty, let trailingTimestamp {
                MarkdownInlineText(text: "", trailingTimestamp: trailingTimestamp)
            }
            ForEach(blocks) { block in
                let timestamp = block.id == blocks.last?.id ? trailingTimestamp : nil
                switch block.kind {
                case .markdown(let text):
                    MarkdownInlineText(text: text, trailingTimestamp: timestamp)
                case .sourceNote(let text):
                    MarkdownTrailingDate(timestamp: timestamp) {
                        MarkdownInlineText(text: text)
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(RemoteIMStyle.blue)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(RemoteIMStyle.blueSoft, in: RoundedRectangle(cornerRadius: 4))
                    }
                case .heading(let level, let text):
                    MarkdownHeadingView(level: level, text: text, trailingTimestamp: timestamp)
                case .list(let list):
                    MarkdownListView(list: list, trailingTimestamp: timestamp)
                case .code(let language, let code, let lineCount):
                    MarkdownTrailingDate(timestamp: timestamp) {
                        MarkdownCodeBlock(language: language, code: code, lineCount: lineCount)
                    }
                case .table(let table):
                    MarkdownTrailingDate(timestamp: timestamp) { MarkdownTableView(table: table) }
                case .quote(let quote):
                    MarkdownQuoteView(quote: quote, trailingTimestamp: timestamp)
                case .divider:
                    MarkdownTrailingDate(timestamp: timestamp) {
                        Rectangle().fill(RemoteIMStyle.border).frame(height: 1).padding(.vertical, 3)
                    }
                }
            }
        }
        .font(.system(size: 14))
        .lineSpacing(4)
        .tint(RemoteIMStyle.blue)
        .lineLimit(nil)
        .multilineTextAlignment(.leading)
        .environment(\.preparedMarkdownInline, document?.inline ?? [:])
        .task(id: source) {
            if prepared?.source == source { return }
            if let cached = MarkdownRenderCache.shared.cached(source) { prepared = cached; return }
            do {
                let source = source
                let result = try await RemoteIMBackgroundWork.parse { MarkdownRenderCache.shared.prepare(source) }
                guard !Task.isCancelled else { return }
                prepared = result
            } catch { }
        }
        .fixedSize(horizontal: false, vertical: true)
        // 普通态长按统一交给 MessageActionDialog。这里启用系统选择会优先弹出
        // Copy / Share…，绕过 MaiChat 的自绘菜单；需要选字时由“选择”动作切换到
        // MenuSuppressingTextView。
    }
}

private struct MarkdownBlock: Identifiable, Sendable {
    let id = UUID()
    let kind: MarkdownBlockKind
}

private enum MarkdownBlockKind: Sendable {
    case markdown(String)
    case sourceNote(String)
    case heading(level: Int, text: String)
    case list(MarkdownList)
    case code(language: String, text: String, lineCount: Int)
    case table(MarkdownTable)
    case quote(MarkdownQuotePresentation)
    case divider
}

private struct MarkdownList: Sendable {
    let items: [MarkdownListItem]
}

private struct MarkdownListItem: Identifiable, Sendable {
    let id = UUID()
    var text: String
    var marker: String = "•"
    var indent: Int = 0
    var isChecked: Bool? = nil
}

private struct MarkdownTable: Sendable {
    let headers: [String]
    let rows: [[String]]
    let columnWidths: [CGFloat]
    init(headers: [String], rows: [[String]]) {
        self.headers = headers
        self.rows = rows
        // Share column widths across rows; don't rescan the table for every cell.
        let count = max(headers.count, rows.map(\.count).max() ?? 0)
        self.columnWidths = (0..<count).map { column in
            let values = [headers[safe: column] ?? ""] + rows.map { $0[safe: column] ?? "" }
            let longest = values
                .flatMap { $0.components(separatedBy: .newlines) }
                .map(\.count).max() ?? 0
            return min(190, max(88, CGFloat(longest) * 7 + 24))
        }
    }
}

private struct MarkdownTrailingDate<Content: View>: View {
    let timestamp: String?
    @ViewBuilder let content: () -> Content
    var body: some View {
        if let timestamp {
            HStack(alignment: .bottom, spacing: 8) {
                content()
                Text("· " + timestamp)
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.blue)
                    .fixedSize()
            }
        } else { content() }
    }
}

private struct MarkdownInlineText: View {
    let text: String
    var trailingTimestamp: String? = nil
    @Environment(\.preparedMarkdownInline) private var inline
    var body: some View {
        var rendered = inline[text] ?? AttributedString(text)
        if let trailingTimestamp {
            var date = AttributedString("  · " + trailingTimestamp)
            date.font = .system(size: 11, weight: .semibold)
            date.foregroundColor = RemoteIMStyle.blue
            rendered.append(date)
        }
        return Text(rendered)
            .lineLimit(nil)
            .multilineTextAlignment(.leading)
            .fixedSize(horizontal: false, vertical: true)
    }
}

private struct MarkdownQuoteView: View {
    let quote: MarkdownQuotePresentation
    var trailingTimestamp: String? = nil

    private var accent: Color {
        switch quote.kind {
        case .note: Color(red: 0.12, green: 0.39, blue: 0.69)
        case .tip: Color(red: 0.09, green: 0.51, blue: 0.42)
        case .important: Color(red: 0.46, green: 0.28, blue: 0.66)
        case .warning: Color(red: 0.61, green: 0.39, blue: 0.06)
        case .caution: Color(red: 0.73, green: 0.24, blue: 0.25)
        case nil: RemoteIMStyle.blue.opacity(0.55)
        }
    }

    private var symbol: String {
        switch quote.kind {
        case .note: "info.circle.fill"
        case .tip: "lightbulb.fill"
        case .important: "exclamationmark.bubble.fill"
        case .warning: "exclamationmark.triangle.fill"
        case .caution: "exclamationmark.octagon.fill"
        case nil: "quote.opening"
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let kind = quote.kind {
                Label(kind.title, systemImage: symbol)
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(accent)
            }
            if !quote.text.isEmpty || trailingTimestamp != nil {
                MarkdownInlineText(text: quote.text, trailingTimestamp: trailingTimestamp)
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(12)
        .padding(.leading, 3)
        .background(quote.kind == nil ? Color.primary.opacity(0.035) : accent.opacity(0.065),
                    in: RoundedRectangle(cornerRadius: 8))
        .overlay(alignment: .leading) {
            RoundedRectangle(cornerRadius: 2).fill(accent).frame(width: 3)
        }
    }
}

private struct MarkdownHeadingView: View {
    let level: Int
    let text: String
    var trailingTimestamp: String? = nil

    var body: some View {
        MarkdownInlineText(text: text, trailingTimestamp: trailingTimestamp)
            .font(.system(size: fontSize, weight: .semibold))
            .foregroundStyle(level <= 2 ? Color(red: 0.11, green: 0.31, blue: 0.54) : RemoteIMStyle.textPrimary)
            .lineSpacing(3)
            .padding(.top, level <= 2 ? 4 : 2)
            .accessibilityAddTraits(.isHeader)
    }

    private var fontSize: CGFloat {
        switch level {
        case 1:
            return 22
        case 2:
            return 18
        default:
            return level == 3 ? 16 : 14
        }
    }
}

private struct MarkdownListView: View {
    let list: MarkdownList
    var trailingTimestamp: String? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(list.items) { item in
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Group {
                        if let checked = item.isChecked {
                            Image(systemName: checked ? "checkmark.square.fill" : "square")
                                .foregroundStyle(checked ? Color(red: 0.09, green: 0.51, blue: 0.42) : RemoteIMStyle.textSecondary)
                                .accessibilityLabel(checked ? "已完成" : "未完成")
                        } else {
                            Text(item.marker).foregroundStyle(RemoteIMStyle.blue)
                        }
                    }
                    .font(.system(size: 13, weight: .semibold))
                    .frame(minWidth: 16, alignment: .trailing)
                    MarkdownInlineText(text: item.text,
                        trailingTimestamp: item.id == list.items.last?.id ? trailingTimestamp : nil)
                        .font(.system(size: 14, weight: .regular))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                        .lineSpacing(4)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .padding(.leading, CGFloat(min(item.indent, 4)) * 12)
                .accessibilityElement(children: .combine)
            }
        }
    }
}

private struct MarkdownCodeBlock: View {
    let language: String
    let code: String
    let lineCount: Int

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 6) {
                Image(systemName: "chevron.left.forwardslash.chevron.right")
                    .accessibilityHidden(true)
                Text(language.isEmpty ? "代码" : language)
                    .lineLimit(1)
                Spacer(minLength: 8)
                Text("\(lineCount) 行")
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
            .font(.system(size: 11, weight: .medium))
            .foregroundStyle(Color(red: 0.11, green: 0.31, blue: 0.54))
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color(red: 0.91, green: 0.95, blue: 0.98))

            ScrollView(.horizontal, showsIndicators: true) {
                Text(code.isEmpty ? " " : code)
                    .font(.system(size: 12, weight: .regular, design: .monospaced))
                    .lineSpacing(4)
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .fixedSize(horizontal: true, vertical: true)
                    .padding(12)
            }
        }
        .background(Color(red: 0.955, green: 0.965, blue: 0.978))
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .stroke(RemoteIMStyle.border.opacity(0.7), lineWidth: 0.5)
        )
    }
}

private struct MarkdownTableView: View {
    let table: MarkdownTable
    private let columnWidths: [CGFloat]

    init(table: MarkdownTable) {
        self.table = table
        self.columnWidths = table.columnWidths
    }

    var body: some View {
        ScrollView(.horizontal, showsIndicators: true) {
            VStack(alignment: .leading, spacing: 0) {
                HStack(alignment: .top, spacing: 0) {
                    ForEach(0..<columnCount, id: \.self) { column in
                        tableCell(
                            table.headers[safe: column] ?? "",
                            isHeader: true,
                            width: columnWidths[column]
                        )
                    }
                }
                .background(Color(red: 0.91, green: 0.94, blue: 0.97))
                ForEach(Array(table.rows.enumerated()), id: \.offset) { rowIndex, row in
                    HStack(alignment: .top, spacing: 0) {
                        ForEach(0..<columnCount, id: \.self) { column in
                            tableCell(
                                row[safe: column] ?? "",
                                isHeader: false,
                                width: columnWidths[column]
                            )
                        }
                    }
                    .background(rowIndex.isMultiple(of: 2) ? Color.white.opacity(0.85) : Color(red: 0.96, green: 0.97, blue: 0.98))
                    .overlay(alignment: .bottom) {
                        Rectangle().fill(RemoteIMStyle.border.opacity(0.6)).frame(height: 0.5)
                    }
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .stroke(RemoteIMStyle.border, lineWidth: 0.5)
            )
        }
    }

    private var columnCount: Int {
        columnWidths.count
    }

    private func tableCell(_ text: String, isHeader: Bool, width: CGFloat) -> some View {
        MarkdownInlineText(text: text)
            .font(.system(size: 12, weight: isHeader ? .semibold : .regular))
            .foregroundStyle(RemoteIMStyle.textPrimary)
            .lineLimit(nil)
            .lineSpacing(3)
            .frame(width: max(1, width - 24), alignment: .topLeading)
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .frame(width: width, alignment: .topLeading)
    }
}

private func parseMarkdownBlocks(_ source: String) -> [MarkdownBlock] {
    let displayText = cleanRemoteIMMessageDisplayText(source)
    let lines = displayText
        .replacingOccurrences(of: "\r\n", with: "\n")
        .replacingOccurrences(of: "\r", with: "\n")
        .components(separatedBy: "\n")
    var blocks: [MarkdownBlock] = []
    var markdownLines: [String] = []
    var index = 0

    func flushMarkdown() {
        let text = markdownLines
            .joined(separator: "\n")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        markdownLines.removeAll()
        if !text.isEmpty {
            blocks.append(MarkdownBlock(kind: .markdown(text)))
        }
    }

    while index < lines.count {
        let line = lines[index]
        let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)

        if index == lines.count - 1, trimmed == "此消息来自 imcli" {
            flushMarkdown()
            blocks.append(MarkdownBlock(kind: .sourceNote(trimmed)))
            index += 1
            continue
        }

        if trimmed.hasPrefix("```") {
            flushMarkdown()
            let language = String(trimmed.dropFirst(3)).trimmingCharacters(in: .whitespaces)
            index += 1
            var codeLines: [String] = []
            while index < lines.count {
                let codeLine = lines[index]
                if codeLine.trimmingCharacters(in: .whitespacesAndNewlines).hasPrefix("```") {
                    index += 1
                    break
                }
                codeLines.append(codeLine)
                index += 1
            }
            blocks.append(MarkdownBlock(kind: .code(language: language, text: codeLines.joined(separator: "\n"), lineCount: max(1, codeLines.count))))
            continue
        }

        if trimmed.isEmpty {
            flushMarkdown()
            index += 1
            continue
        }

        if trimmed.hasPrefix(">") {
            flushMarkdown()
            var quoteLines: [String] = []
            while index < lines.count {
                let quoted = lines[index].trimmingCharacters(in: .whitespaces)
                guard quoted.hasPrefix(">") else { break }
                let content = quoted.dropFirst()
                quoteLines.append(String(content.first == " " ? content.dropFirst() : content))
                index += 1
            }
            blocks.append(MarkdownBlock(kind: .quote(MarkdownQuotePresentation(quoteLines.joined(separator: "\n")))))
            continue
        }

        let rule = trimmed.filter { !$0.isWhitespace }
        if rule.count >= 3, let marker = rule.first, ["-", "*", "_"].contains(String(marker)), rule.allSatisfy({ $0 == marker }) {
            flushMarkdown()
            blocks.append(MarkdownBlock(kind: .divider))
            index += 1
            continue
        }

        if let heading = parseMarkdownHeading(line) {
            flushMarkdown()
            blocks.append(MarkdownBlock(kind: .heading(level: heading.level, text: heading.text)))
            index += 1
            continue
        }

        if let parsed = parseMarkdownTable(lines: lines, startIndex: index) {
            flushMarkdown()
            blocks.append(MarkdownBlock(kind: .table(parsed.table)))
            index = parsed.endIndex
            continue
        }

        if let parsed = parseMarkdownList(lines: lines, startIndex: index) {
            flushMarkdown()
            blocks.append(MarkdownBlock(kind: .list(parsed.list)))
            index = parsed.endIndex
            continue
        }

        markdownLines.append(line)
        index += 1
    }

    flushMarkdown()
    return blocks.isEmpty ? [MarkdownBlock(kind: .markdown(displayText))] : blocks
}

private func cleanRemoteIMMessageDisplayText(_ source: String) -> String {
    var text = source.trimmingCharacters(in: .whitespacesAndNewlines)
    let invisibleAICLIPrefix = "\u{2063}\u{200B}\u{200C}\u{200D}\u{2063}"
    if text.hasPrefix(invisibleAICLIPrefix) {
        text.removeFirst(invisibleAICLIPrefix.count)
        return text.trimmingCharacters(in: .whitespacesAndNewlines)
    }
    for prefix in ["【AICLI 输出】", "[AICLI 输出]", "【AICLI输出】", "[AICLI输出]"] {
        if text.hasPrefix(prefix) {
            text.removeFirst(prefix.count)
            return text.trimmingCharacters(in: .whitespacesAndNewlines)
        }
    }
    return text
}

private func parseMarkdownHeading(_ line: String) -> (level: Int, text: String)? {
    let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
    var level = 0
    for character in trimmed {
        if character == "#" {
            level += 1
        } else {
            break
        }
    }
    guard level > 0, level <= 6 else { return nil }
    let markerEnd = trimmed.index(trimmed.startIndex, offsetBy: level)
    guard markerEnd < trimmed.endIndex, trimmed[markerEnd] == " " else { return nil }
    let textStart = trimmed.index(after: markerEnd)
    let headingText = String(trimmed[textStart...]).trimmingCharacters(in: .whitespacesAndNewlines)
    guard !headingText.isEmpty else { return nil }
    return (level, headingText)
}

private func parseMarkdownList(lines: [String], startIndex: Int) -> (list: MarkdownList, endIndex: Int)? {
    guard parseMarkdownListItem(lines[startIndex]) != nil else { return nil }
    var items: [MarkdownListItem] = []
    var index = startIndex

    while index < lines.count {
        let line = lines[index]
        let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { break }

        if let item = parseMarkdownListItem(line) {
            items.append(item)
            index += 1
            continue
        }

        if line.first?.isWhitespace == true, !items.isEmpty {
            items[items.count - 1].text += "\n" + trimmed
            index += 1
            continue
        }

        break
    }

    guard !items.isEmpty else { return nil }
    return (MarkdownList(items: items), index)
}

private func parseMarkdownListItem(_ line: String) -> MarkdownListItem? {
    let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
    let indent = line.prefix(while: { $0.isWhitespace }).reduce(0) { $0 + ($1 == "\t" ? 4 : 1) } / 2
    for marker in ["- ", "* ", "+ "] {
        if trimmed.hasPrefix(marker) {
            let text = String(trimmed.dropFirst(marker.count))
            if let task = MarkdownTaskPresentation.parse(text) {
                return MarkdownListItem(text: task.text, indent: indent, isChecked: task.checked)
            }
            return MarkdownListItem(text: text, indent: indent)
        }
    }
    let digits = trimmed.prefix(while: { $0.isASCII && $0.isNumber })
    if !digits.isEmpty, digits.count <= 9 {
        let suffix = trimmed.dropFirst(digits.count)
        if suffix.hasPrefix(". ") || suffix.hasPrefix(") ") {
            return MarkdownListItem(text: String(suffix.dropFirst(2)), marker: String(digits) + ".", indent: indent)
        }
    }
    return nil
}

private func parseMarkdownTable(lines: [String], startIndex: Int) -> (table: MarkdownTable, endIndex: Int)? {
    guard startIndex + 1 < lines.count else { return nil }
    let headerLine = lines[startIndex]
    let separatorLine = lines[startIndex + 1]
    guard headerLine.contains("|"), isMarkdownTableSeparator(separatorLine) else { return nil }

    let headers = splitMarkdownTableRow(headerLine)
    guard !headers.isEmpty else { return nil }

    var rows: [[String]] = []
    var index = startIndex + 2
    while index < lines.count {
        let line = lines[index]
        let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, line.contains("|") else { break }
        let cells = splitMarkdownTableRow(line)
        guard !cells.isEmpty else { break }
        rows.append(cells)
        index += 1
    }

    return (MarkdownTable(headers: headers, rows: rows), index)
}

private func splitMarkdownTableRow(_ line: String) -> [String] {
    var trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
    if trimmed.hasPrefix("|") {
        trimmed.removeFirst()
    }
    if trimmed.hasSuffix("|") {
        trimmed.removeLast()
    }
    return trimmed
        .split(separator: "|", omittingEmptySubsequences: false)
        .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
}

private func isMarkdownTableSeparator(_ line: String) -> Bool {
    let cells = splitMarkdownTableRow(line)
    guard !cells.isEmpty else { return false }
    return cells.allSatisfy { cell in
        let compact = cell.replacingOccurrences(of: " ", with: "")
        let hyphenCount = compact.filter { $0 == "-" }.count
        return hyphenCount >= 3 && compact.allSatisfy { character in
            character == "-" || character == ":"
        }
    }
}

private extension Array {
    subscript(safe index: Int) -> Element? {
        indices.contains(index) ? self[index] : nil
    }
}

private struct AudioRecorderReference: @unchecked Sendable {
    let value: AVAudioRecorder
    let url: URL?
}

private final class NativeVoiceRecorder: @unchecked Sendable {
    private var recorder: AVAudioRecorder?
    private var recordingURL: URL?
    private var startedAt: TimeInterval?
    private var sessionID: UUID?
    deinit {
        guard let recorder else { return }
        let reference = AudioRecorderReference(value: recorder, url: recordingURL)
        RemoteIMBackgroundWork.audioQueue.async {
            reference.value.stop()
            if let url = reference.url { try? FileManager.default.removeItem(at: url) }
        }
    }

    @MainActor
    func start(id: UUID, cancellation: RemoteIMBackgroundWork.Cancellation) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            RemoteIMBackgroundWork.audioQueue.async { [self] in
                var pendingURL: URL?
                var pendingRecorder: AVAudioRecorder?
                do {
                    try cancellation.check()
                    let session = AVAudioSession.sharedInstance()
                    try session.setCategory(.playAndRecord, mode: .spokenAudio, options: [.defaultToSpeaker])
                    try session.setActive(true)
                    try cancellation.check()
                    let url = RemoteIMMediaStorage.fileURL(category: .outgoingVoices,
                        stem: "remote-im-voice-\(UUID().uuidString)", pathExtension: "m4a")
                    pendingURL = url
                    let settings: [String: Any] = [AVFormatIDKey: Int(kAudioFormatMPEG4AAC),
                        AVSampleRateKey: 16_000, AVNumberOfChannelsKey: 1,
                        AVEncoderAudioQualityKey: AVAudioQuality.medium.rawValue]
                    let next = try AVAudioRecorder(url: url, settings: settings)
                    pendingRecorder = next
                    next.prepareToRecord()
                    guard next.record() else { throw VoiceRecorderError.startFailed }
                    try cancellation.check()
                    recorder = next; recordingURL = url
                    startedAt = ProcessInfo.processInfo.systemUptime; sessionID = id
                    continuation.resume()
                } catch {
                    pendingRecorder?.stop()
                    if let url = pendingURL { try? FileManager.default.removeItem(at: url) }
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    @MainActor
    func stop(id: UUID) async -> RemoteIMVoiceRecording? {
        await withCheckedContinuation { continuation in
            RemoteIMBackgroundWork.audioQueue.async { [self] in
                guard sessionID == id, let recorder, let url = recordingURL else {
                    continuation.resume(returning: nil); return
                }
                recorder.stop()
                let duration = max(1, Int(ceil(ProcessInfo.processInfo.systemUptime - (startedAt ?? ProcessInfo.processInfo.systemUptime))))
                self.recorder = nil; recordingURL = nil; startedAt = nil; sessionID = nil
                continuation.resume(returning: RemoteIMVoiceRecording(fileURL: url, durationSeconds: duration))
            }
        }
    }

    @MainActor
    func cancel(id: UUID) {
        RemoteIMBackgroundWork.audioQueue.async { [self] in
            guard sessionID == id else { return }
            recorder?.stop(); recorder = nil; sessionID = nil; startedAt = nil
            if let url = recordingURL { try? FileManager.default.removeItem(at: url) }
            recordingURL = nil
        }
    }
}

@MainActor
private final class VoiceMessageRecorder: ObservableObject {
    @Published private(set) var isRecording = false
    private let native = NativeVoiceRecorder()
    private var sessionID: UUID?
    private var cancellation: RemoteIMBackgroundWork.Cancellation?

    func start() async throws {
        guard sessionID == nil else { return }
        let id = UUID(), cancellation = RemoteIMBackgroundWork.Cancellation()
        sessionID = id; self.cancellation = cancellation
        do {
            let granted = await withCheckedContinuation { continuation in
                AVAudioSession.sharedInstance().requestRecordPermission { continuation.resume(returning: $0) }
            }
            guard granted else { throw VoiceRecorderError.microphonePermissionDenied }
            try cancellation.check(); try Task.checkCancellation()
            try await native.start(id: id, cancellation: cancellation)
            guard sessionID == id, !Task.isCancelled else { native.cancel(id: id); throw CancellationError() }
            isRecording = true
        } catch {
            native.cancel(id: id)
            if sessionID == id { sessionID = nil; self.cancellation = nil; isRecording = false }
            throw error
        }
    }

    func stop() async -> RemoteIMVoiceRecording? {
        guard let id = sessionID else { return nil }
        cancellation?.cancel(); cancellation = nil
        sessionID = nil; isRecording = false
        return await native.stop(id: id)
    }

    func cancel() {
        cancellation?.cancel(); cancellation = nil
        guard let id = sessionID else { return }
        sessionID = nil; isRecording = false
        native.cancel(id: id)
    }
}

private enum VoiceRecorderError: LocalizedError {
    case microphonePermissionDenied
    case startFailed

    var errorDescription: String? {
        switch self {
        case .microphonePermissionDenied:
            return "没有麦克风权限"
        case .startFailed:
            return "录音启动失败"
        }
    }
}

private struct RemoteIMSlashCommand: Identifiable {
    let command: String
    let label: String

    var id: String { command }
}

private let remoteIMSlashCommands: [RemoteIMSlashCommand] = [
    .init(command: "/status", label: "查看状态"),
    .init(command: "/plan", label: "切换 Plan"),
    .init(command: "/build", label: "切换 Build"),
    .init(command: "/models", label: "模型列表"),
    .init(command: "/model ", label: "模型/推理"),
    .init(command: "/goal ", label: "管理 Goal"),
    .init(command: "/btw ", label: "子任务"),
    .init(command: "/diff ", label: "仓库 Diff"),
    .init(command: "/interrupt", label: "中断任务"),
    .init(command: "/compact", label: "压缩上下文"),
    .init(command: "/clear", label: "清空上下文"),
    .init(command: "/help", label: "命令帮助")
]

private struct RemoteIMSlashCommandBar: View {
    let commands: [RemoteIMSlashCommand]
    let visibleHeight: CGFloat
    let onSelect: (RemoteIMSlashCommand) -> Void

    private let rowHeight: CGFloat = 44
    private let rowSpacing: CGFloat = 8
    private let verticalPadding: CGFloat = 16

    var body: some View {
        ScrollView(.vertical, showsIndicators: commands.count > 4) {
            VStack(spacing: 8) {
                ForEach(commands) { command in
                    Button {
                        onSelect(command)
                    } label: {
                        HStack(spacing: 6) {
                            Text(command.command)
                                .font(.system(size: 12, weight: .semibold, design: .monospaced))
                                .foregroundStyle(RemoteIMStyle.blue)
                            Text(command.label)
                                .font(.system(size: 12, weight: .medium))
                                .foregroundStyle(RemoteIMStyle.textSecondary)
                            Spacer(minLength: 0)
                        }
                        .padding(.horizontal, 11)
                        .frame(height: rowHeight)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(RemoteIMStyle.blueSoft, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                        .overlay(
                            RoundedRectangle(cornerRadius: 12, style: .continuous)
                                .stroke(Color(red: 0.745, green: 0.87, blue: 1.0), lineWidth: 1)
                        )
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, 2)
            .padding(.vertical, verticalPadding / 2)
        }
        .frame(height: panelHeight)
    }

    private var panelHeight: CGFloat {
        let spacingHeight = CGFloat(max(commands.count - 1, 0)) * rowSpacing
        let contentHeight = CGFloat(commands.count) * rowHeight + spacingHeight + verticalPadding
        let availableHeight = max(160, visibleHeight * 0.68)
        return min(contentHeight, availableHeight)
    }
}

private enum RemoteIMPickedMediaError: LocalizedError {
    case videoReadFailed
    case videoTrackMissing
    case coverGenerationFailed

    var errorDescription: String? {
        switch self {
        case .videoReadFailed:
            return "视频读取失败"
        case .videoTrackMissing:
            return "所选文件没有可用的视频轨道"
        case .coverGenerationFailed:
            return "视频封面生成失败"
        }
    }
}

private struct RemoteIMPickedVideoTransfer: Transferable, Sendable {
    let fileURL: URL

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(contentType: .movie) { video in
            SentTransferredFile(video.fileURL)
        } importing: { received in
            let sourceURL = received.file
            let targetURL = try await RemoteIMBackgroundWork.file {
                let sourceExtension = sourceURL.pathExtension.trimmingCharacters(in: .whitespacesAndNewlines)
                let targetURL = RemoteIMMediaStorage.fileURL(
                    category: .outgoingVideos,
                    stem: "remote-im-video-\(UUID().uuidString)",
                    pathExtension: sourceExtension.isEmpty ? "mov" : sourceExtension
                )
                try FileManager.default.copyItem(at: sourceURL, to: targetURL)
                return targetURL
            }
            return RemoteIMPickedVideoTransfer(fileURL: targetURL)
        }
    }
}

struct ComposerAttachmentPanel: View {
    let canSendImage: Bool
    let canSendVideo: Bool
    let canSendFile: Bool
    let canSendVoice: Bool
    let openLibrary: () -> Void
    let openCamera: () -> Void
    let openFile: () -> Void
    let openVoiceInput: () -> Void
    var showsVoiceInput = true

    private var columns: [GridItem] {
        Array(
            repeating: GridItem(.flexible(), spacing: 12, alignment: .top),
            count: showsVoiceInput ? 4 : 3
        )
    }

    var body: some View {
        LazyVGrid(columns: columns, alignment: .center, spacing: 14) {
            actionButton(
                title: "相册",
                systemImage: "photo",
                enabled: canSendImage || canSendVideo,
                action: openLibrary
            )
            actionButton(
                title: "拍摄",
                systemImage: "camera.fill",
                enabled: canSendImage,
                action: openCamera
            )
            actionButton(
                title: "文件",
                systemImage: "doc.fill",
                enabled: canSendFile,
                action: openFile
            )
            if showsVoiceInput {
                actionButton(
                    title: "语音输入",
                    systemImage: "mic.fill",
                    enabled: canSendVoice,
                    action: openVoiceInput
                )
            }
        }
        .padding(.horizontal, 18)
        .padding(.top, 18)
        .padding(.bottom, 20)
        .background(RemoteIMStyle.pageBackground)
    }

    private func actionButton(
        title: String,
        systemImage: String,
        enabled: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            VStack(spacing: 8) {
                Image(systemName: systemImage)
                    .font(.system(size: 25, weight: .medium))
                    .foregroundStyle(
                        enabled ? RemoteIMStyle.textPrimary : RemoteIMStyle.textSecondary.opacity(0.5)
                    )
                    .frame(maxWidth: .infinity)
                    .aspectRatio(1, contentMode: .fit)
                    .background(
                        RemoteIMStyle.panelBackground,
                        in: RoundedRectangle(cornerRadius: 13, style: .continuous)
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 13, style: .continuous)
                            .stroke(RemoteIMStyle.border, lineWidth: 1)
                    )
                Text(title)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(
                        enabled ? RemoteIMStyle.textSecondary : RemoteIMStyle.textSecondary.opacity(0.5)
                    )
                    .lineLimit(1)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .accessibilityLabel(title)
    }
}

private struct ComposerView: View {
    let peerUserID: String
    @Binding var isAttachmentPanelPresented: Bool
    @EnvironmentObject private var appState: RemoteIMAppState
    @ObservedObject var draft: RemoteIMDraftState
    let transcriptionPresentation: VoiceTranscriptionPresentation
    @StateObject private var voiceRecorder = VoiceMessageRecorder()
    @StateObject private var realtimeSpeechRecognizer = TencentRealtimeSpeechRecognizer(
        appId: TencentASRCredentials.appId,
        secretId: TencentASRCredentials.secretId,
        secretKey: TencentASRCredentials.secretKey
    )
    @State private var isVoiceMode = false
    @State private var isPressingVoice = false
    @State private var isCancellingVoice = false
    @State private var realtimeStartTask: Task<Bool, Never>?
    @State private var composerFocusRequestGeneration = 0
    @State private var isCameraPresented = false
    @State private var isPhotoPickerPresented = false
    @State private var isFileImporterPresented = false
    @State private var selectedMediaItems: [PhotosPickerItem] = []
    @State private var keyboardVisibleHeight = UIScreen.main.bounds.height
    @State private var composerEditingController = ComposerTextEditingController()
    @State private var composerEditMenuState: ComposerEditMenuState?

    var body: some View {
        VStack(spacing: 0) {
            VStack(spacing: 8) {
                if !isVoiceMode && !commandSuggestions.isEmpty {
                    RemoteIMSlashCommandBar(
                        commands: commandSuggestions,
                        visibleHeight: keyboardVisibleHeight
                    ) { command in
                        draft.text = command.command
                    }
                }

                if let quote = draft.quote {
                    HStack(spacing: 8) {
                        Capsule()
                            .fill(RemoteIMStyle.blue)
                            .frame(width: 3, height: 26)
                        Text(quote.senderID.isEmpty
                            ? quote.digest
                            : "回复 \(quote.senderID)：\(quote.digest)")
                            .font(.system(size: 12))
                            .foregroundStyle(RemoteIMStyle.textSecondary)
                            .lineLimit(1)
                        Spacer(minLength: 4)
                        Button {
                            appState.cancelReply()
                        } label: {
                            Image(systemName: "xmark")
                                .font(.system(size: 12, weight: .bold))
                                .frame(width: 30, height: 30)
                        }
                        .buttonStyle(.plain)
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                        .accessibilityLabel("取消引用回复")
                    }
                    .padding(.leading, 9)
                    .padding(.trailing, 4)
                    .frame(height: 40)
                    .background(RemoteIMStyle.blueSoft, in: RoundedRectangle(cornerRadius: 9))
                }

                HStack(alignment: .bottom, spacing: 8) {
                    Button {
                        composerEditMenuState = nil
                        setVoiceMode(!isVoiceMode)
                    } label: {
                        Image(systemName: isVoiceMode ? "keyboard" : "speaker.wave.2.fill")
                            .font(.system(size: 18, weight: .bold))
                            .frame(width: 44, height: 44)
                            .background(RemoteIMStyle.blueSoft, in: Circle())
                            .overlay(
                                Circle()
                                    .stroke(RemoteIMStyle.border, lineWidth: 1)
                            )
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(RemoteIMStyle.blue)

                if isVoiceMode {
                    PressToTalkButton(
                        isPressing: isPressingVoice,
                        isCancelling: isCancellingVoice,
                        isEnabled: appState.canSendVoice,
                        idleTitle: "按住 发语音",
                        onChanged: { translation in
                            handleVoicePressChanged(
                                translation: translation,
                                showsTranscriptionHighlight: false
                            )
                        },
                        onEnded: { translation in
                            Task {
                                await handleVoicePressEnded(
                                    translation: translation,
                                    sendsVoiceDirectly: true
                                )
                            }
                        }
                    )
                } else {
                    ZStack(alignment: .topLeading) {
                        ComposerTextView(
                            text: Binding(
                                get: { draft.text },
                                set: { draft.updateFromEditor($0) }
                            ),
                            onSubmit: submitDraft,
                            focusRequestGeneration: composerFocusRequestGeneration,
                            editingController: composerEditingController,
                            onEditMenuRequested: { state in
                                let nextState = state.hasActions ? state : nil
                                guard composerEditMenuState != nextState else { return }
                                withAnimation(.easeOut(duration: 0.1)) {
                                    composerEditMenuState = nextState
                                }
                            },
                            onEditMenuDismissed: {
                                guard composerEditMenuState != nil else { return }
                                withAnimation(.easeOut(duration: 0.08)) {
                                    composerEditMenuState = nil
                                }
                            },
                            onTypingActivityChanged: { active in
                                appState.updateHumanTyping(active, to: peerUserID)
                            },
                            voiceTranscriptionEnabled: appState.canSendVoice && draft.text.isEmpty,
                            onVoiceLongPressChanged: { translation, location in
                                handleVoicePressChanged(
                                    translation: translation,
                                    showsTranscriptionHighlight: true,
                                    location: location
                                )
                            },
                            onVoiceLongPressEnded: { translation, location in
                                Task {
                                    await handleVoicePressEnded(
                                        translation: translation,
                                        sendsVoiceDirectly: false,
                                        location: location
                                    )
                                }
                            },
                            onVoiceLongPressCancelled: cancelVoiceLongPress
                        )

                        if draft.text.isEmpty {
                            Text(textComposerPrompt)
                                .font(.system(size: 14, weight: isPressingVoice ? .semibold : .regular))
                                .foregroundStyle(textComposerPromptColor)
                                .padding(.horizontal, 13)
                                .padding(.vertical, 13)
                                .allowsHitTesting(false)
                                .accessibilityHidden(true)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color.white, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                    .overlay(
                        RoundedRectangle(cornerRadius: 14, style: .continuous)
                            .stroke(
                                appState.canSend ? RemoteIMStyle.blue : RemoteIMStyle.border,
                                lineWidth: appState.canSend ? 1.5 : 1
                            )
                    )
                    .overlay(alignment: .topLeading) {
                        if let state = composerEditMenuState {
                            ComposerEditActionBar(
                                state: state,
                                pasteTarget: composerEditingController.textView,
                                pasteCompleted: {
                                    withAnimation(.easeOut(duration: 0.08)) {
                                        composerEditMenuState = nil
                                    }
                                },
                                perform: performComposerEditAction
                            )
                            .offset(x: 4, y: -50)
                            .transition(.opacity)
                            .zIndex(20)
                        }
                    }
                    .zIndex(composerEditMenuState == nil ? 0 : 20)
                }

                Button {
                    composerEditMenuState = nil
                    if isAttachmentPanelPresented {
                        isAttachmentPanelPresented = false
                    } else {
                        dismissKeyboard()
                        isAttachmentPanelPresented = true
                    }
                } label: {
                    Image(systemName: "plus")
                        .font(.system(size: 20, weight: .semibold))
                        .frame(width: 44, height: 44)
                        .background(Color.white, in: Circle())
                        .overlay(
                            Circle()
                                .stroke(RemoteIMStyle.border, lineWidth: 1)
                        )
                }
                .buttonStyle(.plain)
                .foregroundStyle(
                    canOpenAttachmentPanel
                        ? RemoteIMStyle.textPrimary
                        : RemoteIMStyle.textSecondary
                )
                .disabled(!canOpenAttachmentPanel)
                .accessibilityLabel(isAttachmentPanelPresented ? "收起更多功能" : "展开更多功能")
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 12)
            .padding(.bottom, 10)

            if isAttachmentPanelPresented {
                Divider().background(RemoteIMStyle.border)
                ComposerAttachmentPanel(
                    canSendImage: appState.canSendImage,
                    canSendVideo: appState.canSendVideo,
                    canSendFile: appState.canSendFile,
                    canSendVoice: appState.canSendVoice,
                    openLibrary: {
                        isAttachmentPanelPresented = false
                        Task { await openPhotoPicker() }
                    },
                    openCamera: {
                        isAttachmentPanelPresented = false
                        Task { await openCamera() }
                    },
                    openFile: {
                        isAttachmentPanelPresented = false
                        isFileImporterPresented = true
                    },
                    openVoiceInput: {
                        setVoiceMode(true)
                    }
                )
                .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
        .animation(.easeOut(duration: 0.18), value: isAttachmentPanelPresented)
        .onChange(of: draft.quote) { quote in
            if quote != nil {
                composerFocusRequestGeneration &+= 1
            }
        }
        .background(RemoteIMStyle.panelBackground)
        .overlay(alignment: .top) {
            Divider().background(RemoteIMStyle.border)
        }
        .photosPicker(
            isPresented: $isPhotoPickerPresented,
            selection: $selectedMediaItems,
            maxSelectionCount: 20,
            selectionBehavior: .ordered,
            matching: .any(of: [.images, .videos]),
            photoLibrary: .shared()
        )
        .fullScreenCover(isPresented: $isCameraPresented) {
            RemoteIMCameraPicker(
                onCapture: { image in
                    isCameraPresented = false
                    Task { await sendCapturedPhoto(image) }
                },
                onCancel: {
                    isCameraPresented = false
                }
            )
            .ignoresSafeArea()
        }
        .fileImporter(
            isPresented: $isFileImporterPresented,
            allowedContentTypes: [.item],
            allowsMultipleSelection: false
        ) { result in
            Task { await sendSelectedFile(result) }
        }
        .onChange(of: selectedMediaItems) { items in
            guard !items.isEmpty else { return }
            Task { await sendSelectedMedia(items) }
        }
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillChangeFrameNotification)) { notification in
            updateKeyboardVisibleHeight(from: notification)
        }
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillHideNotification)) { _ in
            keyboardVisibleHeight = UIScreen.main.bounds.height
        }
        .onAppear {
            transcriptionPresentation.onCancel = cancelVoiceLongPress
            transcriptionPresentation.onEdit = {
                Task {
                    await handleVoicePressEnded(translation: .zero, sendsVoiceDirectly: false,
                                               explicitTarget: .edit)
                }
            }
            realtimeSpeechRecognizer.onLiveTextUpdate = {
                [weak transcriptionPresentation] text, sessionID in
                guard transcriptionPresentation?.target != nil else { return }
                transcriptionPresentation?.updateLiveText(text, sessionID: sessionID)
            }
        }
        .onDisappear {
            appState.updateHumanTyping(false, to: peerUserID)
            composerEditMenuState = nil
            realtimeSpeechRecognizer.onLiveTextUpdate = nil
            transcriptionPresentation.onCancel = nil
            transcriptionPresentation.onEdit = nil
            realtimeStartTask?.cancel()
            realtimeStartTask = nil
            realtimeSpeechRecognizer.cancel()
            voiceRecorder.cancel()
            transcriptionPresentation.reset()
            isAttachmentPanelPresented = false
        }
    }

    private var commandSuggestions: [RemoteIMSlashCommand] {
        let query = draft.text.trimmingCharacters(in: .whitespaces)
        guard query.hasPrefix("/") else { return [] }
        return remoteIMSlashCommands.filter { $0.command.hasPrefix(query) }
    }

    private var canOpenAttachmentPanel: Bool {
        appState.canSendImage || appState.canSendVideo || appState.canSendFile || appState.canSendVoice
    }

    private func performComposerEditAction(_ action: ComposerEditAction) {
        composerEditingController.perform(action)
        switch action {
        case .select, .selectAll:
            let nextState = composerEditingController.menuState()
            composerEditMenuState = nextState.hasActions ? nextState : nil
        case .paste, .cut, .copy, .readAloud, .newLine:
            composerEditMenuState = nil
        }
    }

    private func setVoiceMode(_ enabled: Bool) {
        isAttachmentPanelPresented = false
        guard RemoteIMVoiceModeTransitionPolicy.requiresCleanup(
            current: isVoiceMode,
            target: enabled
        ) else { return }
        transcriptionPresentation.reset()
        realtimeStartTask?.cancel()
        realtimeStartTask = nil
        realtimeSpeechRecognizer.cancel()
        voiceRecorder.cancel()
        isVoiceMode = enabled
    }

    private var textComposerPrompt: String {
        if isCancellingVoice { return "松开取消" }
        if transcriptionPresentation.target == .edit { return "松开编辑" }
        if isPressingVoice { return "松手发送" }
        return "可按住 转文字"
    }

    private var textComposerPromptColor: Color {
        if isCancellingVoice { return .red }
        return isPressingVoice ? RemoteIMStyle.blue : RemoteIMStyle.textSecondary
    }

    private func openPhotoPicker() async {
        guard appState.canSendImage || appState.canSendVideo else { return }
        guard await requestPhotoLibraryPermission() else {
            appState.errorMessage = "没有相册权限，请在系统设置中允许访问照片"
            return
        }
        isPhotoPickerPresented = true
    }

    private func openCamera() async {
        guard appState.canSendImage else { return }
        guard UIImagePickerController.isSourceTypeAvailable(.camera) else {
            appState.errorMessage = "当前设备不支持拍照"
            return
        }
        guard await requestCameraPermission() else {
            appState.errorMessage = "没有相机权限，请在系统设置中允许 MaiChat 使用相机"
            return
        }
        isCameraPresented = true
    }

    private func requestCameraPermission() async -> Bool {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            return true
        case .notDetermined:
            return await withCheckedContinuation { continuation in
                AVCaptureDevice.requestAccess(for: .video) { granted in
                    continuation.resume(returning: granted)
                }
            }
        case .denied, .restricted:
            return false
        @unknown default:
            return false
        }
    }

    private func requestPhotoLibraryPermission() async -> Bool {
        switch PHPhotoLibrary.authorizationStatus(for: .readWrite) {
        case .authorized, .limited:
            return true
        case .notDetermined:
            let status = await withCheckedContinuation { continuation in
                PHPhotoLibrary.requestAuthorization(for: .readWrite) { status in
                    continuation.resume(returning: status)
                }
            }
            return status == .authorized || status == .limited
        case .denied, .restricted:
            return false
        @unknown default:
            return false
        }
    }

    private func submitDraft() {
        guard appState.canSend else { return }
        appState.updateHumanTyping(false, to: peerUserID)
        Task { await appState.sendDraft(to: peerUserID) }
    }

    private func updateKeyboardVisibleHeight(from notification: Notification) {
        guard
            let endFrame = notification.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect
        else {
            keyboardVisibleHeight = UIScreen.main.bounds.height
            return
        }

        let screenHeight = UIScreen.main.bounds.height
        keyboardVisibleHeight = max(0, min(screenHeight, endFrame.minY))
        if endFrame.minY < screenHeight - 1 {
            isAttachmentPanelPresented = false
        }
    }

    private func handleVoicePressChanged(
        translation: CGSize,
        showsTranscriptionHighlight: Bool,
        location: CGPoint? = nil
    ) {
        guard appState.canSendVoice else { return }
        if !isPressingVoice {
            isPressingVoice = true
            if showsTranscriptionHighlight {
                transcriptionPresentation.prepareForNewSession(
                    account: appState.remoteDiagnosticsAccountTag,
                    peer: appState.chatState.selectedPeerID ?? ""
                )
                transcriptionPresentation.target = .send
                AppDiagnosticLog.shared.record(
                    level: .info,
                    category: "asr",
                    event: "gesture-started",
                    fields: ["mode": "transcription"].merging(transcriptionPresentation.diagnosticFields) { _, context in context }
                )
                realtimeStartTask = Task { await startRealtimeTranscription() }
            } else {
                Task { await startVoiceRecording() }
            }
        }
        if showsTranscriptionHighlight {
            let target = transcriptionTarget(for: translation, location: location)
            let nextIsCancelling = target == .cancel
            if isCancellingVoice != nextIsCancelling {
                isCancellingVoice = nextIsCancelling
            }
            if transcriptionPresentation.target != target {
                transcriptionPresentation.target = target
            }
        } else {
            let nextIsCancelling = translation.height < -70
            if isCancellingVoice != nextIsCancelling {
                isCancellingVoice = nextIsCancelling
            }
            if transcriptionPresentation.target != nil {
                transcriptionPresentation.target = nil
            }
        }
    }

    private func handleVoicePressEnded(
        translation: CGSize,
        sendsVoiceDirectly: Bool,
        location: CGPoint? = nil,
        explicitTarget: VoiceTranscriptionTarget? = nil
    ) async {
        guard isPressingVoice else { return }
        isPressingVoice = false

        if sendsVoiceDirectly {
            transcriptionPresentation.target = nil
            let shouldCancel = translation.height < -70
            isCancellingVoice = false
            if shouldCancel {
                voiceRecorder.cancel()
                return
            }
            guard let recording = await voiceRecorder.stop() else { return }
            await appState.sendVoiceRecording(recording, to: peerUserID)
            return
        }

        let diagnosticFields = transcriptionPresentation.diagnosticFields
        let releasedAt = ProcessInfo.processInfo.systemUptime
        let releaseTarget = explicitTarget ?? transcriptionTarget(for: translation, location: location)
        isCancellingVoice = false
        if releaseTarget == .cancel {
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "asr",
                event: "gesture-ended",
                fields: ["action": "cancel"].merging(diagnosticFields) { _, context in context }
            )
            realtimeStartTask?.cancel()
            realtimeStartTask = nil
            realtimeSpeechRecognizer.cancel()
            transcriptionPresentation.reset()
            return
        }

        let shouldEdit = releaseTarget == .edit
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "asr",
            event: "gesture-ended",
            fields: ["action": shouldEdit ? "edit" : "send"].merging(diagnosticFields) { _, context in context }
        )
        transcriptionPresentation.target = shouldEdit ? .finishingEdit : .finishingSend
        let didStart = await realtimeStartTask?.value ?? realtimeSpeechRecognizer.isRecognizing
        realtimeStartTask = nil
        guard didStart, realtimeSpeechRecognizer.isRecognizing else {
            transcriptionPresentation.reset()
            return
        }
        do {
            let text = try await realtimeSpeechRecognizer.stop()
                .trimmingCharacters(in: .whitespacesAndNewlines)
            transcriptionPresentation.reset()
            guard !text.isEmpty else {
                AppDiagnosticLog.shared.record(level: .info, category: "asr",
                    event: "transcription-empty", fields: diagnosticFields)
                appState.showTransientError("没有识别到文字，未发送")
                return
            }
            if shouldEdit {
                draft.text = text
                composerFocusRequestGeneration &+= 1
            } else {
                await appState.sendText(text, to: peerUserID)
            }
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "asr",
                event: "transcription-applied",
                fields: [
                    "duration_ms": String(Int((ProcessInfo.processInfo.systemUptime - releasedAt) * 1000)),
                    "action": shouldEdit ? "edit" : "send",
                    "characters": String(text.count),
                ].merging(diagnosticFields) { _, context in context }
            )
        } catch is CancellationError {
            transcriptionPresentation.reset()
        } catch {
            transcriptionPresentation.reset()
            AppDiagnosticLog.shared.record(level: .warning, category: "asr",
                event: "transcription-apply-failed",
                fields: ["code": String((error as NSError).code)]
                    .merging(diagnosticFields) { _, context in context })
            appState.errorMessage = "语音转文字失败，未发送：\(error.localizedDescription)"
        }
    }

    private func cancelVoiceLongPress() {
        transcriptionPresentation.reset()
        isPressingVoice = false
        isCancellingVoice = false
        realtimeStartTask?.cancel()
        realtimeStartTask = nil
        realtimeSpeechRecognizer.cancel()
    }

    private func transcriptionTarget(for translation: CGSize, location: CGPoint?) -> VoiceTranscriptionTarget {
        VoiceTranscriptionHitTest.target(translation: translation, location: location,
            cancelFrame: transcriptionPresentation.actionFrames[.cancel],
            editFrame: transcriptionPresentation.actionFrames[.edit])
    }

    private func startRealtimeTranscription() async -> Bool {
        let diagnosticFields = transcriptionPresentation.diagnosticFields
        guard realtimeSpeechRecognizer.isAvailable else {
            transcriptionPresentation.target = nil
            isPressingVoice = false
            appState.errorMessage = "语音转文字凭证未配置"
            return false
        }
        do {
            try await realtimeSpeechRecognizer.start(diagnosticFields: diagnosticFields)
            guard !Task.isCancelled else {
                realtimeSpeechRecognizer.cancel()
                return false
            }
            return true
        } catch is CancellationError {
            realtimeSpeechRecognizer.cancel()
            return false
        } catch {
            transcriptionPresentation.reset()
            isPressingVoice = false
            isCancellingVoice = false
            appState.errorMessage = error.localizedDescription
            AppDiagnosticLog.shared.record(
                level: .warning,
                category: "asr",
                event: "session-start-failed",
                fields: [
                    "error_domain": (error as NSError).domain,
                    "code": String((error as NSError).code),
                ].merging(diagnosticFields) { _, context in context }
            )
            return false
        }
    }

    private func startVoiceRecording() async {
        do {
            try await voiceRecorder.start()
        } catch {
            if error is CancellationError { return }
            transcriptionPresentation.target = nil
            isPressingVoice = false
            isCancellingVoice = false
            appState.errorMessage = error.localizedDescription
        }
    }

    private func sendSelectedMedia(_ items: [PhotosPickerItem]) async {
        let identity = appState.remoteDiagnosticsIdentity
        defer { selectedMediaItems = [] }
        var sentCount = 0
        var failedReadCount = 0

        for item in items {
            do {
                if item.supportedContentTypes.contains(where: { $0.conforms(to: .movie) }) {
                    let video = try await preparePickedVideo(item)
                    guard appState.remoteDiagnosticsIdentity == identity, !Task.isCancelled else { return }
                    await appState.sendVideoFile(video, to: peerUserID)
                } else {
                    guard let data = try await item.loadTransferable(type: Data.self) else {
                        failedReadCount += 1
                        continue
                    }
                    let contentTypes = item.supportedContentTypes
                    let imageFile = try await RemoteIMBackgroundWork.file {
                        try Self.savePickedImage(data: data, contentTypes: contentTypes)
                    }
                    guard appState.remoteDiagnosticsIdentity == identity, !Task.isCancelled else { return }
                    await appState.sendImageFile(imageFile, to: peerUserID)
                }
                sentCount += 1
            } catch {
                failedReadCount += 1
            }
        }

        if failedReadCount > 0 {
            appState.errorMessage = sentCount > 0
                ? "已发送 \(sentCount) 项，另有 \(failedReadCount) 项读取失败"
                : "所选照片或视频读取失败"
        }
    }

    private func preparePickedVideo(_ item: PhotosPickerItem) async throws -> RemoteIMVideoFile {
        guard let pickedVideo = try await item.loadTransferable(type: RemoteIMPickedVideoTransfer.self) else {
            throw RemoteIMPickedMediaError.videoReadFailed
        }
        let fileURL = pickedVideo.fileURL
        let asset = AVURLAsset(url: fileURL)
        let duration = try await asset.load(.duration)
        let durationValue = CMTimeGetSeconds(duration)
        let durationSeconds = durationValue.isFinite
            ? max(1, Int(ceil(durationValue)))
            : 1

        let videoTracks = try await asset.loadTracks(withMediaType: .video)
        guard let videoTrack = videoTracks.first else {
            throw RemoteIMPickedMediaError.videoTrackMissing
        }
        let naturalSize = try await videoTrack.load(.naturalSize)
        let preferredTransform = try await videoTrack.load(.preferredTransform)
        let transformedRect = CGRect(origin: .zero, size: naturalSize).applying(preferredTransform)
        let width = max(0, Int(abs(transformedRect.width).rounded()))
        let height = max(0, Int(abs(transformedRect.height).rounded()))

        let imageGenerator = AVAssetImageGenerator(asset: asset)
        imageGenerator.appliesPreferredTrackTransform = true
        imageGenerator.maximumSize = CGSize(width: 1_280, height: 1_280)
        let coverTime = CMTime(
            seconds: durationValue.isFinite && durationValue > 0.2 ? 0.1 : 0,
            preferredTimescale: 600
        )
        let generatedCover = try await imageGenerator.image(at: coverTime)
        let cover = RemoteIMImageEncodingInput(image: UIImage(cgImage: generatedCover.image))
        return try await RemoteIMBackgroundWork.file {
            guard let coverData = cover.image.jpegData(compressionQuality: 0.86) else {
                throw RemoteIMPickedMediaError.coverGenerationFailed
            }
            let coverFileURL = RemoteIMMediaStorage.fileURL(
                category: .outgoingVideoCovers,
                stem: fileURL.deletingPathExtension().lastPathComponent,
                pathExtension: "jpg"
            )
            try coverData.write(to: coverFileURL, options: .atomic)
            let resourceValues = try fileURL.resourceValues(forKeys: [.fileSizeKey])
            let fileType = fileURL.pathExtension.trimmingCharacters(in: .whitespacesAndNewlines)
            return RemoteIMVideoFile(fileURL: fileURL, coverFileURL: coverFileURL,
                fileType: fileType.isEmpty ? "mp4" : fileType.lowercased(),
                durationSeconds: durationSeconds, width: width, height: height,
                sizeBytes: Int64(max(0, resourceValues.fileSize ?? 0)))
        }
    }

    private func sendCapturedPhoto(_ image: UIImage) async {
        let identity = appState.remoteDiagnosticsIdentity
        do {
            let input = RemoteIMImageEncodingInput(image: image)
            let imageFile = try await RemoteIMBackgroundWork.file {
                guard let data = input.image.jpegData(compressionQuality: 0.9) else {
                    throw RemoteIMPickedMediaError.coverGenerationFailed
                }
                return try Self.savePickedImage(data: data, contentTypes: [.jpeg])
            }
            guard appState.remoteDiagnosticsIdentity == identity, !Task.isCancelled else { return }
                    await appState.sendImageFile(imageFile, to: peerUserID)
        } catch {
            appState.errorMessage = error.localizedDescription
        }
    }

    private func sendSelectedFile(_ result: Result<[URL], Error>) async {
        let identity = appState.remoteDiagnosticsIdentity
        do {
            guard let selectedURL = try result.get().first else { return }
            let file = try await RemoteIMBackgroundWork.file { try Self.copyImportedFileToCache(selectedURL) }
            guard appState.remoteDiagnosticsIdentity == identity, !Task.isCancelled else { return }
                    await appState.sendFile(file, to: peerUserID)
        } catch {
            appState.errorMessage = error.localizedDescription
        }
    }

    nonisolated private static func copyImportedFileToCache(_ sourceURL: URL) throws -> RemoteIMFile {
        let accessedSecurityScopedResource = sourceURL.startAccessingSecurityScopedResource()
        defer {
            if accessedSecurityScopedResource {
                sourceURL.stopAccessingSecurityScopedResource()
            }
        }

        let fileName = sourceURL.lastPathComponent.isEmpty ? "file" : sourceURL.lastPathComponent
        let targetURL = RemoteIMMediaStorage.fileURL(
            category: .outgoingFiles,
            stem: UUID().uuidString,
            pathExtension: sourceURL.pathExtension
        )
        try FileManager.default.copyItem(at: sourceURL, to: targetURL)

        let resourceValues = try? targetURL.resourceValues(forKeys: [.fileSizeKey])
        let mimeType = UTType(filenameExtension: sourceURL.pathExtension)?.preferredMIMEType ??
            "application/octet-stream"
        return RemoteIMFile(
            fileURL: targetURL,
            fileName: fileName,
            mimeType: mimeType,
            sizeBytes: resourceValues?.fileSize
        )
    }

    nonisolated private static func savePickedImage(
        data: Data,
        contentTypes: [UTType]
    ) throws -> RemoteIMImageFile {
        let contentType = contentTypes.first(where: { $0.conforms(to: .image) })
        let fileExtension = contentType?.preferredFilenameExtension ?? "jpg"
        let fileURL = RemoteIMMediaStorage.fileURL(
            category: .outgoingImages,
            stem: "remote-im-image-\(UUID().uuidString)",
            pathExtension: fileExtension
        )
        try data.write(to: fileURL, options: .atomic)

        let image = UIImage(data: data)
        let width = image.map { Int($0.size.width * $0.scale) }
        let height = image.map { Int($0.size.height * $0.scale) }
        return RemoteIMImageFile(
            fileURL: fileURL,
            width: width,
            height: height,
            sizeBytes: data.count
        )
    }
}

private enum ComposerEditAction: CaseIterable, Identifiable {
    case paste
    case select
    case selectAll
    case cut
    case copy
    case readAloud
    case newLine

    var id: Self { self }

    var title: String {
        switch self {
        case .paste: return "粘贴"
        case .select: return "选择"
        case .selectAll: return "全选"
        case .cut: return "剪切"
        case .copy: return "复制"
        case .readAloud: return "朗读"
        case .newLine: return "换行"
        }
    }

    var systemImage: String {
        switch self {
        case .paste: return "doc.on.clipboard"
        case .select: return "selection.pin.in.out"
        case .selectAll: return "text.badge.checkmark"
        case .cut: return "scissors"
        case .copy: return "doc.on.doc"
        case .readAloud: return "speaker.wave.2"
        case .newLine: return "return"
        }
    }
}

private struct ComposerEditMenuState: Equatable {
    let actions: [ComposerEditAction]
    let disabledActions: Set<ComposerEditAction>

    var hasActions: Bool { !actions.isEmpty }

    func isEnabled(_ action: ComposerEditAction) -> Bool {
        !disabledActions.contains(action)
    }
}

@MainActor
private final class ComposerTextEditingController {
    weak var textView: UITextView?
    private let speechSynthesizer = AVSpeechSynthesizer()

    func menuState() -> ComposerEditMenuState {
        guard let textView else {
            return ComposerEditMenuState(actions: [], disabledActions: [])
        }
        let textLength = (textView.text as NSString).length
        let selectedRange = textView.selectedRange
        let hasSelection = selectedRange.location != NSNotFound && selectedRange.length > 0
        let hasText = textLength > 0

        // UIPasteControl 会根据 target 的 pasteConfiguration 自己决定能否粘贴，
        // 这里不主动读取系统剪贴板，避免触发跨 App 粘贴授权提示。
        let actions: [ComposerEditAction] = hasSelection
            ? [.copy, .cut, .paste, .readAloud, .newLine]
            : [.paste, .select, .selectAll, .readAloud, .newLine]
        var disabledActions = Set<ComposerEditAction>()
        if !hasText {
            disabledActions.formUnion([.select, .selectAll, .readAloud])
        }
        return ComposerEditMenuState(actions: actions, disabledActions: disabledActions)
    }

    func perform(_ action: ComposerEditAction) {
        guard let textView else { return }
        switch action {
        case .paste:
            // 粘贴必须由 UIPasteControl 发起，才能保留 iOS 16+ 的受信任
            // 用户操作语义；自定义按钮直接读剪贴板会触发授权提示。
            break
        case .select:
            textView.select(nil)
        case .selectAll:
            textView.selectAll(nil)
        case .cut:
            textView.cut(nil)
        case .copy:
            textView.copy(nil)
        case .readAloud:
            let fullText = textView.text ?? ""
            let selectedRange = textView.selectedRange
            let spokenText: String
            if selectedRange.location != NSNotFound,
               selectedRange.length > 0,
               NSMaxRange(selectedRange) <= (fullText as NSString).length {
                spokenText = (fullText as NSString).substring(with: selectedRange)
            } else {
                spokenText = fullText
            }
            guard !spokenText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
                return
            }
            speechSynthesizer.stopSpeaking(at: .immediate)
            let utterance = AVSpeechUtterance(string: spokenText)
            utterance.voice = AVSpeechSynthesisVoice(
                language: Locale.preferredLanguages.first ?? "zh-CN"
            )
            speechSynthesizer.speak(utterance)
        case .newLine:
            let fullText = textView.text ?? ""
            let textLength = (fullText as NSString).length
            let selectedRange = textView.selectedRange
            let insertionRange = selectedRange.location != NSNotFound
                && NSMaxRange(selectedRange) <= textLength
                ? selectedRange
                : NSRange(location: textLength, length: 0)
            textView.text = (fullText as NSString).replacingCharacters(
                in: insertionRange,
                with: "\n"
            )
            textView.selectedRange = NSRange(
                location: insertionRange.location + 1,
                length: 0
            )
            textView.delegate?.textViewDidChange?(textView)
        }
    }
}

private struct ComposerEditActionBar: View {
    let state: ComposerEditMenuState
    weak var pasteTarget: UITextView?
    let pasteCompleted: () -> Void
    let perform: (ComposerEditAction) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 0) {
                ForEach(state.actions) { action in
                    if action == .paste {
                        ComposerPasteControl(
                            target: pasteTarget,
                            pasteCompleted: pasteCompleted
                        )
                        .frame(width: 58, height: 42)
                        .overlay {
                            ZStack {
                                Color.white
                                Text("粘贴")
                                    .font(.system(size: 14, weight: .medium))
                                    .foregroundStyle(RemoteIMStyle.textPrimary)
                            }
                            .allowsHitTesting(false)
                            .accessibilityHidden(true)
                        }
                        .accessibilityLabel("粘贴")
                        .accessibilityIdentifier("composer-custom-paste-control")
                    } else {
                        Button {
                            perform(action)
                        } label: {
                            Text(action.title)
                                .font(.system(size: 14, weight: .medium))
                                .foregroundStyle(RemoteIMStyle.textPrimary)
                                .frame(minWidth: 58, minHeight: 42)
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .disabled(!state.isEnabled(action))
                        .opacity(state.isEnabled(action) ? 1 : 0.35)
                        .accessibilityLabel(action.title)
                    }

                    if action.id != state.actions.last?.id {
                        Divider()
                            .frame(height: 42)
                            .overlay(RemoteIMStyle.border)
                    }
                }
            }
            .background(Color.white)
            .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .stroke(RemoteIMStyle.border.opacity(0.72), lineWidth: 0.5)
            }

            MessageActionPointer()
                .fill(Color.white)
                .frame(width: 16, height: 7)
                .padding(.leading, 20)
                .offset(y: -0.5)
        }
        .shadow(color: Color.black.opacity(0.14), radius: 11, y: 5)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("composer-custom-edit-action-bar")
    }
}

private struct ComposerPasteControl: UIViewRepresentable {
    weak var target: UITextView?
    let pasteCompleted: () -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(pasteCompleted: pasteCompleted)
    }

    func makeUIView(context: Context) -> UIPasteControl {
        let configuration = UIPasteControl.Configuration()
        configuration.displayMode = .iconOnly
        configuration.cornerRadius = 0
        configuration.baseForegroundColor = .white
        // 显式白底避免 `.clear` 在 iOS 26 被回退成独立灰/黑按钮；
        // 可见的“粘贴”文字由上层自绘，系统控件只保留受信任点击能力。
        configuration.baseBackgroundColor = .white
        let control = UIPasteControl(configuration: configuration)
        control.target = target
        control.accessibilityIdentifier = "composer-custom-paste-control"
        control.addTarget(
            context.coordinator,
            action: #selector(Coordinator.didPaste),
            for: .primaryActionTriggered
        )
        return control
    }

    func updateUIView(_ control: UIPasteControl, context: Context) {
        context.coordinator.pasteCompleted = pasteCompleted
        control.target = target
    }

    @MainActor
    final class Coordinator: NSObject {
        var pasteCompleted: () -> Void

        init(pasteCompleted: @escaping () -> Void) {
            self.pasteCompleted = pasteCompleted
        }

        @objc func didPaste() {
            DispatchQueue.main.async { [weak self] in
                self?.pasteCompleted()
            }
        }
    }
}

private final class GrowingComposerUITextView: UITextView {
    var onContentHeightChange: (() -> Void)?

    override func addInteraction(_ interaction: any UIInteraction) {
        guard !(interaction is UIEditMenuInteraction) else { return }
        super.addInteraction(interaction)
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        removeSystemEditMenuInteractions()
    }

    func removeSystemEditMenuInteractions() {
        guard #available(iOS 16.0, *) else { return }
        for interaction in interactions where interaction is UIEditMenuInteraction {
            removeInteraction(interaction)
        }
    }

    override func canPerformAction(_ action: Selector, withSender sender: Any?) -> Bool {
        switch action {
        case #selector(UIResponderStandardEditActions.paste(_:)),
             #selector(UIResponderStandardEditActions.select(_:)),
             #selector(UIResponderStandardEditActions.selectAll(_:)),
             #selector(UIResponderStandardEditActions.cut(_:)),
             #selector(UIResponderStandardEditActions.copy(_:)):
            return super.canPerformAction(action, withSender: sender)
        default:
            return false
        }
    }

    override var contentSize: CGSize {
        didSet {
            guard abs(oldValue.height - contentSize.height) > 0.5 else { return }
            onContentHeightChange?()
        }
    }
}

private struct ComposerTextView: UIViewRepresentable {
    @EnvironmentObject private var navigationArrival: ChatNavigationArrival
    @Binding var text: String
    let onSubmit: () -> Void
    let focusRequestGeneration: Int
    let editingController: ComposerTextEditingController
    let onEditMenuRequested: (ComposerEditMenuState) -> Void
    let onEditMenuDismissed: () -> Void
    let onTypingActivityChanged: (Bool) -> Void
    let voiceTranscriptionEnabled: Bool
    let onVoiceLongPressChanged: (CGSize, CGPoint) -> Void
    let onVoiceLongPressEnded: (CGSize, CGPoint) -> Void
    let onVoiceLongPressCancelled: () -> Void

    private let minimumHeight: CGFloat = 44
    private let maximumLineCount: CGFloat = 5
    private let verticalInset: CGFloat = 11

    func makeCoordinator() -> Coordinator {
        Coordinator(parent: self)
    }

    func makeUIView(context: Context) -> GrowingComposerUITextView {
        // The composer is plain text. Use TextKit 1 explicitly to avoid the
        // TextKit 2 viewport/caret layout work measured during continuous input.
        let textView = GrowingComposerUITextView(usingTextLayoutManager: false)
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "remote-im-ui",
            event: "composer-created",
            fields: ["layout_engine": "textkit1"]
        )
        textView.delegate = context.coordinator
        textView.backgroundColor = .clear
        textView.font = .systemFont(ofSize: 14)
        textView.textColor = .label
        textView.tintColor = UIColor(RemoteIMStyle.blue)
        textView.autocapitalizationType = .none
        textView.autocorrectionType = .no
        textView.spellCheckingType = .no
        textView.pasteConfiguration = UIPasteConfiguration(
            acceptableTypeIdentifiers: [
                UTType.utf8PlainText.identifier,
                UTType.plainText.identifier,
                UTType.url.identifier,
            ]
        )
        textView.returnKeyType = .send
        textView.enablesReturnKeyAutomatically = true
        textView.textContainerInset = UIEdgeInsets(
            top: verticalInset,
            left: 13,
            bottom: verticalInset,
            right: 13
        )
        textView.textContainer.lineFragmentPadding = 0
        textView.textContainer.widthTracksTextView = true
        textView.textContainer.heightTracksTextView = false
        // Keep TextKit's document container unbounded. The outer SwiftUI view
        // caps the visible height at five lines; the text view handles overflow.
        textView.isScrollEnabled = true
        textView.bounces = false
        textView.alwaysBounceVertical = false
        textView.showsVerticalScrollIndicator = false
        textView.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        textView.accessibilityIdentifier = "message-composer-text-view"
        textView.accessibilityLabel = "消息输入框，长按语音转文字"
        editingController.textView = textView
        navigationArrival.composerView = textView
        let voiceLongPress = UILongPressGestureRecognizer(
            target: context.coordinator,
            action: #selector(Coordinator.handleVoiceLongPress(_:))
        )
        voiceLongPress.minimumPressDuration = 0.35
        voiceLongPress.allowableMovement = 400
        voiceLongPress.cancelsTouchesInView = true
        voiceLongPress.delegate = context.coordinator
        textView.addGestureRecognizer(voiceLongPress)
        context.coordinator.voiceLongPressGesture = voiceLongPress

        let editTap = UITapGestureRecognizer(
            target: context.coordinator,
            action: #selector(Coordinator.handleEditTap(_:))
        )
        editTap.cancelsTouchesInView = false
        editTap.delegate = context.coordinator
        textView.addGestureRecognizer(editTap)
        context.coordinator.editTapGesture = editTap

        let editLongPress = UILongPressGestureRecognizer(
            target: context.coordinator,
            action: #selector(Coordinator.handleEditLongPress(_:))
        )
        editLongPress.minimumPressDuration = 0.5
        editLongPress.cancelsTouchesInView = false
        editLongPress.delegate = context.coordinator
        textView.addGestureRecognizer(editLongPress)
        context.coordinator.editLongPressGesture = editLongPress
        textView.removeSystemEditMenuInteractions()
        textView.onContentHeightChange = { [weak coordinator = context.coordinator, weak textView] in
            guard let coordinator, let textView else { return }
            coordinator.scheduleContentHeightRefresh(for: textView)
        }
        return textView
    }

    func updateUIView(_ textView: GrowingComposerUITextView, context: Context) {
        let started = ProcessInfo.processInfo.systemUptime
        defer { AppDiagnosticLog.shared.recordDuration(.composerUpdate, since: started) }
        context.coordinator.parent = self
        editingController.textView = textView
        navigationArrival.composerView = textView
        if textView.text != text {
            let nextText = text
            context.coordinator.applyExternalText(nextText, to: textView)
        }
        if context.coordinator.lastFocusRequestGeneration != focusRequestGeneration {
            context.coordinator.lastFocusRequestGeneration = focusRequestGeneration
            DispatchQueue.main.async { [weak textView] in
                textView?.becomeFirstResponder()
            }
        }
    }

    static func dismantleUIView(
        _ textView: GrowingComposerUITextView,
        coordinator: Coordinator
    ) {
        textView.delegate = nil
        textView.onContentHeightChange = nil
        if coordinator.parent.editingController.textView === textView {
            coordinator.parent.editingController.textView = nil
        }
    }

    func sizeThatFits(
        _ proposal: ProposedViewSize,
        uiView textView: GrowingComposerUITextView,
        context: Context
    ) -> CGSize? {
        guard
            let width = proposal.width,
            width.isFinite,
            width > 0
        else {
            return nil
        }
        return context.coordinator.sizeThatFits(width: width, textView: textView)
    }

    @MainActor
    final class Coordinator: NSObject, UITextViewDelegate, UIGestureRecognizerDelegate {
        var parent: ComposerTextView
        private var cachedWidth: CGFloat?
        private var cachedHeight: CGFloat
        private var requiresMeasurement = true
        private var hasScheduledContentHeightRefresh = false
        private var isApplyingExternalText = false
        private var pendingTextChangeUptime: TimeInterval?
        private var voiceLongPressOrigin: CGPoint?
        private var editTapBeganWhileFocused = false
        private var editTapInitialSelection: NSRange?
        weak var voiceLongPressGesture: UILongPressGestureRecognizer?
        weak var editTapGesture: UITapGestureRecognizer?
        weak var editLongPressGesture: UILongPressGestureRecognizer?
        var lastFocusRequestGeneration: Int

        init(parent: ComposerTextView) {
            self.parent = parent
            cachedHeight = parent.minimumHeight
            lastFocusRequestGeneration = parent.focusRequestGeneration
        }

        @discardableResult
        func requireMeasurement() -> Bool {
            let wasRequired = requiresMeasurement
            requiresMeasurement = true
            return !wasRequired
        }

        func sizeThatFits(width: CGFloat, textView: UITextView) -> CGSize {
            let widthChanged = cachedWidth.map { abs($0 - width) >= 0.5 } ?? true
            if requiresMeasurement || widthChanged {
                let started = ProcessInfo.processInfo.systemUptime
                defer { AppDiagnosticLog.shared.recordDuration(.composerLayout, since: started) }
                let fittingSize = textView.sizeThatFits(
                    CGSize(width: width, height: .greatestFiniteMagnitude)
                )
                cachedWidth = width
                requiresMeasurement = false
                updateCachedHeight(
                    contentHeight: fittingSize.height,
                    textView: textView,
                    invalidatingIntrinsicSize: false
                )
            }
            return CGSize(width: width, height: cachedHeight)
        }

        func applyExternalText(_ text: String, to textView: UITextView) {
            pendingTextChangeUptime = nil
            isApplyingExternalText = true
            defer { isApplyingExternalText = false }

            textView.unmarkText()
            textView.text = text
            textView.selectedRange = NSRange(
                location: (text as NSString).length,
                length: 0
            )
            requireMeasurement()
            textView.invalidateIntrinsicContentSize()
        }

        func textViewDidChange(_ textView: UITextView) {
            let started = ProcessInfo.processInfo.systemUptime
            defer { AppDiagnosticLog.shared.recordDuration(.composerEdit, since: started) }
            if let acceptedAt = pendingTextChangeUptime {
                pendingTextChangeUptime = nil
                AppDiagnosticLog.shared.recordDuration(
                    .composerTextMutation, since: acceptedAt, until: started
                )
            }
            if !isApplyingExternalText, parent.text != textView.text {
                parent.text = textView.text
            }
            parent.onTypingActivityChanged(
                textView.isFirstResponder && !textView.text.isEmpty
            )
            parent.onEditMenuDismissed()
            scheduleContentHeightRefresh(for: textView)
        }

        func textViewDidEndEditing(_ textView: UITextView) {
            parent.onTypingActivityChanged(false)
            parent.onEditMenuDismissed()
        }

        func textViewDidChangeSelection(_ textView: UITextView) {
            guard textView.isFirstResponder, textView.markedTextRange == nil else { return }
            if textView.selectedRange.length > 0 {
                requestCustomEditMenu()
            } else {
                parent.onEditMenuDismissed()
            }
        }

        @available(iOS 16.0, *)
        func textView(
            _ textView: UITextView,
            editMenuForTextIn range: NSRange,
            suggestedActions: [UIMenuElement]
        ) -> UIMenu? {
            requestCustomEditMenu(systemAPI: "single-range")
            return UIMenu(children: [])
        }

        @available(iOS 26.0, *)
        func textView(
            _ textView: UITextView,
            editMenuForTextInRanges ranges: [NSValue],
            suggestedActions: [UIMenuElement]
        ) -> UIMenu? {
            requestCustomEditMenu(systemAPI: "multi-range")
            return UIMenu(children: [])
        }

        private func requestCustomEditMenu(systemAPI: String? = nil) {
            if let systemAPI {
                AppDiagnosticLog.shared.record(
                    level: .info,
                    category: "remote-im-ui",
                    event: "composer-system-edit-menu-suppressed",
                    fields: ["api": systemAPI]
                )
            }
            (parent.editingController.textView as? GrowingComposerUITextView)?
                .removeSystemEditMenuInteractions()
            parent.onEditMenuRequested(parent.editingController.menuState())
        }

        func textView(
            _ textView: UITextView,
            shouldChangeTextIn _: NSRange,
            replacementText: String
        ) -> Bool {
            guard
                textView.markedTextRange == nil,
                RemoteIMDraftSubmitPolicy.shouldSubmit(replacementText: replacementText)
            else {
                // Earliest app-side edit callback, not the physical keypress.
                pendingTextChangeUptime = ProcessInfo.processInfo.systemUptime
                return true
            }

            pendingTextChangeUptime = nil
            parent.onSubmit()
            return false
        }

        func gestureRecognizerShouldBegin(_ gestureRecognizer: UIGestureRecognizer) -> Bool {
            if gestureRecognizer === voiceLongPressGesture {
                return parent.voiceTranscriptionEnabled
            }
            if gestureRecognizer === editLongPressGesture {
                return !parent.voiceTranscriptionEnabled
            }
            return true
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer
        ) -> Bool {
            gestureRecognizer === editTapGesture
                || gestureRecognizer === editLongPressGesture
                || otherGestureRecognizer === editTapGesture
                || otherGestureRecognizer === editLongPressGesture
        }

        func gestureRecognizer(
            _ gestureRecognizer: UIGestureRecognizer,
            shouldReceive touch: UITouch
        ) -> Bool {
            if gestureRecognizer === editTapGesture,
               let textView = gestureRecognizer.view as? UITextView {
                // 第一次点击只负责聚焦并打开键盘；已经聚焦后的再次点击才
                // 展示编辑操作条，避免用户每次开始输入都被菜单打扰。
                editTapBeganWhileFocused = textView.isFirstResponder
                editTapInitialSelection = textView.selectedRange
            }
            return true
        }

        @objc func handleEditTap(_ gesture: UITapGestureRecognizer) {
            guard gesture.state == .ended, editTapBeganWhileFocused else { return }
            DispatchQueue.main.async { [weak self] in
                guard let self,
                      let textView = gesture.view as? UITextView,
                      self.editTapInitialSelection == textView.selectedRange
                else { return }
                // 有文本时第一次点到新位置只移动光标；再次点同一位置才弹条。
                // 空文本的选区始终为 0,0，因此聚焦后的再次点击可直接粘贴。
                self.requestCustomEditMenu()
            }
        }

        @objc func handleEditLongPress(_ gesture: UILongPressGestureRecognizer) {
            guard gesture.state == .ended else { return }
            DispatchQueue.main.async { [weak self] in
                self?.requestCustomEditMenu()
            }
        }

        @objc func handleVoiceLongPress(_ gesture: UILongPressGestureRecognizer) {
            let location = gesture.location(in: gesture.view?.window)
            switch gesture.state {
            case .began:
                guard parent.voiceTranscriptionEnabled else { return }
                voiceLongPressOrigin = location
                gesture.view?.resignFirstResponder()
                parent.onVoiceLongPressChanged(.zero, location)
            case .changed:
                guard let origin = voiceLongPressOrigin else { return }
                parent.onVoiceLongPressChanged(
                    CGSize(width: location.x - origin.x, height: location.y - origin.y), location
                )
            case .ended:
                guard let origin = voiceLongPressOrigin else { return }
                voiceLongPressOrigin = nil
                parent.onVoiceLongPressEnded(
                    CGSize(width: location.x - origin.x, height: location.y - origin.y), location
                )
            case .cancelled, .failed:
                voiceLongPressOrigin = nil
                parent.onVoiceLongPressCancelled()
            case .possible:
                break
            @unknown default:
                voiceLongPressOrigin = nil
                parent.onVoiceLongPressCancelled()
            }
        }

        func scheduleContentHeightRefresh(for textView: UITextView) {
            guard !hasScheduledContentHeightRefresh else { return }
            hasScheduledContentHeightRefresh = true
            let enqueuedUptime = ProcessInfo.processInfo.systemUptime
            DispatchQueue.main.async { [weak self, weak textView] in
                guard let self else { return }
                self.hasScheduledContentHeightRefresh = false
                guard let textView else { return }
                AppDiagnosticLog.shared.recordDuration(.composerHeightQueue, since: enqueuedUptime)
                guard
                    let cachedWidth = self.cachedWidth,
                    textView.bounds.width.isFinite,
                    textView.bounds.width > 0,
                    abs(textView.bounds.width - cachedWidth) < 0.5
                else {
                    if self.requireMeasurement() {
                        textView.invalidateIntrinsicContentSize()
                    }
                    return
                }
                self.updateCachedHeight(
                    contentHeight: textView.contentSize.height,
                    textView: textView,
                    invalidatingIntrinsicSize: true
                )
            }
        }

        private func updateCachedHeight(
            contentHeight: CGFloat,
            textView: UITextView,
            invalidatingIntrinsicSize: Bool
        ) {
            let lineHeight = textView.font?.lineHeight ?? 17
            let maximumHeight = ceil(
                lineHeight * parent.maximumLineCount + parent.verticalInset * 2
            )
            let nextHeight = min(
                max(ceil(contentHeight), parent.minimumHeight),
                maximumHeight
            )

            if contentHeight <= maximumHeight, textView.contentOffset != .zero {
                textView.setContentOffset(.zero, animated: false)
            }
            guard abs(cachedHeight - nextHeight) > 0.5 else { return }
            cachedHeight = nextHeight
            if invalidatingIntrinsicSize {
                textView.invalidateIntrinsicContentSize()
            }
        }
    }
}

struct RemoteIMCameraPicker: UIViewControllerRepresentable {
    let onCapture: (UIImage) -> Void
    let onCancel: () -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(onCapture: onCapture, onCancel: onCancel)
    }

    func makeUIViewController(context: Context) -> UIImagePickerController {
        let controller = UIImagePickerController()
        controller.sourceType = .camera
        controller.cameraCaptureMode = .photo
        controller.allowsEditing = false
        controller.delegate = context.coordinator
        return controller
    }

    func updateUIViewController(_: UIImagePickerController, context _: Context) {}

    final class Coordinator: NSObject, UIImagePickerControllerDelegate, UINavigationControllerDelegate {
        private let onCapture: (UIImage) -> Void
        private let onCancel: () -> Void

        init(onCapture: @escaping (UIImage) -> Void, onCancel: @escaping () -> Void) {
            self.onCapture = onCapture
            self.onCancel = onCancel
        }

        func imagePickerController(
            _: UIImagePickerController,
            didFinishPickingMediaWithInfo info: [UIImagePickerController.InfoKey: Any]
        ) {
            guard let image = info[.originalImage] as? UIImage else {
                onCancel()
                return
            }
            onCapture(image)
        }

        func imagePickerControllerDidCancel(_: UIImagePickerController) {
            onCancel()
        }
    }
}

private struct PressToTalkButton: View {
    let isPressing: Bool
    let isCancelling: Bool
    let isEnabled: Bool
    let idleTitle: String
    let onChanged: (CGSize) -> Void
    let onEnded: (CGSize) -> Void

    var body: some View {
        Text(title)
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(isEnabled ? RemoteIMStyle.textPrimary : RemoteIMStyle.textSecondary)
            .frame(maxWidth: .infinity, minHeight: 44)
            .background(backgroundColor, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .stroke(borderColor, lineWidth: isPressing ? 1.5 : 1)
            )
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        onChanged(value.translation)
                    }
                    .onEnded { value in
                        onEnded(value.translation)
                    }
            )
            .allowsHitTesting(isEnabled)
    }

    private var title: String {
        if !isEnabled { return "选择联系人后可发送语音" }
        if isCancelling { return "松开取消" }
        return isPressing ? "松开发送" : idleTitle
    }

    private var backgroundColor: Color {
        if !isEnabled { return Color(.secondarySystemBackground) }
        if isCancelling { return Color.red.opacity(0.08) }
        return isPressing ? RemoteIMStyle.blueSoft : Color.white
    }

    private var borderColor: Color {
        if isCancelling { return .red }
        return isPressing ? RemoteIMStyle.blue : RemoteIMStyle.border
    }
}
