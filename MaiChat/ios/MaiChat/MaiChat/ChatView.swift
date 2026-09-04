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
    static let yellowBorder = Color(red: 0.992, green: 0.812, blue: 0.345)
    static let yellowSoft = Color(red: 1.0, green: 0.984, blue: 0.913)
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

@MainActor
private final class RemoteIMAsyncImageState: ObservableObject {
    @Published private(set) var image: UIImage?
    @Published private(set) var hasFinished = false
    private var activeRequest: RemoteIMImageRequest?

    func load(_ request: RemoteIMImageRequest?) async {
        if activeRequest == request, image != nil || hasFinished {
            return
        }
        activeRequest = request
        image = nil
        hasFinished = false
        guard let request else {
            hasFinished = true
            return
        }

        let result = await RemoteIMImagePipeline.shared.image(for: request)
        guard !Task.isCancelled, activeRequest == request else {
            if activeRequest == request {
                activeRequest = nil
            }
            return
        }
        image = result?.image
        hasFinished = true
    }
}

private struct RemoteIMAsyncImage<Content: View, Placeholder: View>: View {
    let filePath: String?
    let maximumPointSize: CGSize
    @ViewBuilder let content: (UIImage) -> Content
    @ViewBuilder let placeholder: (_ hasFailed: Bool) -> Placeholder
    @Environment(\.displayScale) private var displayScale
    @StateObject private var state = RemoteIMAsyncImageState()

    private var request: RemoteIMImageRequest? {
        RemoteIMImageRequest(
            filePath: filePath,
            maximumPixelSize: max(maximumPointSize.width, maximumPointSize.height) * displayScale
        )
    }

    var body: some View {
        Group {
            if let image = state.image {
                content(image)
            } else {
                placeholder(state.hasFinished)
            }
        }
        .task(id: request) {
            await state.load(request)
        }
    }
}

struct ChatView: View {
    @Binding var activeContact: RemoteIMContact?
    let showRemoteDesktop: () -> Void
    @State private var searchTargetMessageID: UUID?

    var body: some View {
        chatContent
            .background(RemoteIMStyle.pageBackground.ignoresSafeArea())
    }

    @ViewBuilder
    private var chatContent: some View {
        if let activeContact {
            ChatDetailView(
                contact: activeContact,
                activeContact: $activeContact,
                searchTargetMessageID: searchTargetMessageID,
                showRemoteDesktop: showRemoteDesktop
            )
        } else {
            VStack(spacing: 0) {
                HeaderView()
                ConversationListView(
                    activeContact: $activeContact,
                    searchTargetMessageID: $searchTargetMessageID
                )
            }
        }
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
                    Text(latestMessage?.text ?? "暂无消息")
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

private final class VoiceMessagePlayer: NSObject, ObservableObject, AVAudioPlayerDelegate {
    @Published var playingMessageID: UUID?

    private var audioPlayer: AVAudioPlayer?

    func toggle(message: RemoteIMMessage) {
        guard let attachment = message.voiceAttachment else { return }
        if playingMessageID == message.id {
            stop()
            return
        }

        do {
            try AVAudioSession.sharedInstance().setCategory(.playback, mode: .spokenAudio)
            try AVAudioSession.sharedInstance().setActive(true)
            let nextPlayer = try AVAudioPlayer(contentsOf: URL(fileURLWithPath: attachment.localFilePath))
            nextPlayer.delegate = self
            nextPlayer.prepareToPlay()
            audioPlayer = nextPlayer
            playingMessageID = message.id
            nextPlayer.play()
        } catch {
            stop()
        }
    }

    func stop() {
        audioPlayer?.stop()
        audioPlayer = nil
        playingMessageID = nil
    }

    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        stop()
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

private struct RemoteIMImageBubbleFramePreferenceKey: PreferenceKey {
    static let defaultValue: CGRect = .zero

    static func reduce(value: inout CGRect, nextValue: () -> CGRect) {
        value = nextValue()
    }
}

private struct ChatDetailView: View {
    let contact: RemoteIMContact
    @Binding var activeContact: RemoteIMContact?
    let searchTargetMessageID: UUID?
    let showRemoteDesktop: () -> Void
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var initialHistoryLoadGeneration = 0
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
                    messages: appState.visibleMessages(with: contact.userID),
                    peerRelation: contact.relation,
                    searchTargetMessageID: quoteTargetMessageID ?? searchTargetMessageID,
                    hasEarlierMessages: appState.hasEarlierMessages(with: contact.userID),
                    initialHistoryLoadGeneration: initialHistoryLoadGeneration,
                    selectingMessageID: selectingMessageID,
                    finishSelectingText: { selectingMessageID = nil },
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
                    draft: appState.draft,
                    transcriptionPresentation: transcriptionPresentation
                )
            }

            VoiceTranscriptionHighlightHost(presentation: transcriptionPresentation)
                .zIndex(10)
                .allowsHitTesting(false)

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
        .simultaneousGesture(edgeSwipeBackGesture)
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
            initialHistoryLoadGeneration &+= 1
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

    private var edgeSwipeBackGesture: some Gesture {
        DragGesture(minimumDistance: 20, coordinateSpace: .local)
            .onEnded { value in
                guard ChatDetailSwipeBackPolicy.shouldReturnToConversationList(
                    startX: Double(value.startLocation.x),
                    translationWidth: Double(value.translation.width),
                    translationHeight: Double(value.translation.height)
                ) else { return }

                dismissKeyboard()
                withAnimation(.easeOut(duration: 0.18)) {
                    activeContact = nil
                }
            }
    }
}

private enum VoiceTranscriptionTarget: Equatable {
    case send
    case cancel
    case edit
    case finishingSend
    case finishingEdit
}

@MainActor
private final class VoiceTranscriptionPresentation: ObservableObject {
    @Published var target: VoiceTranscriptionTarget?
    @Published var liveText = ""
    private var firstLoggedSessionID: UInt64?

    func prepareForNewSession() {
        liveText = ""
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
                ]
            )
        }
        liveText = text
    }

    func reset() {
        target = nil
        liveText = ""
        firstLoggedSessionID = nil
    }
}

private struct VoiceTranscriptionHighlightHost: View {
    @ObservedObject var presentation: VoiceTranscriptionPresentation

    var body: some View {
        Group {
            if let target = presentation.target {
                VoiceTranscriptionHighlight(
                    target: target,
                    transcript: presentation.liveText
                )
                .transition(.opacity)
            }
        }
        .animation(.easeOut(duration: 0.14), value: presentation.target)
    }
}

private struct VoiceTranscriptionHighlight: View {
    let target: VoiceTranscriptionTarget
    let transcript: String

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
                                .shadow(color: RemoteIMStyle.blue.opacity(0.45), radius: 20)
                        )
                        .padding(.horizontal, 30)
                        .transition(.scale(scale: 0.92).combined(with: .opacity))

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
        .animation(.easeOut(duration: 0.12), value: target)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(statusText)
    }

    private func listeningIndicator(size: CGFloat) -> some View {
        ZStack {
            Circle()
                .fill(indicatorColor.opacity(0.25))
                .frame(width: size * 1.34, height: size * 1.34)
                .blur(radius: size * 0.1)
            Circle()
                .fill(indicatorColor)
                .frame(width: size, height: size)
                .shadow(color: indicatorColor.opacity(0.9), radius: size * 0.2)
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
        .scaleEffect(selected ? 1.12 : 1)
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
                    Task { await appState.stopRemoteDesktopView() }
                } else {
                    Task {
                        await appState.requestRemoteDesktopView(of: contact)
                        if session.state.isActive {
                            showRemoteDesktop()
                        }
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

            StatusPill(state: appState.connectionState)
        }
        .padding(.horizontal, 10)
        .padding(.top, 8)
        .padding(.bottom, 10)
        .background(RemoteIMStyle.panelBackground)
        .overlay(alignment: .bottom) {
            Divider().background(RemoteIMStyle.border)
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
            return .red
        case .idle, .failed:
            return canUseRemoteButton ? RemoteIMStyle.blue : RemoteIMStyle.textSecondary
        }
    }

    private var remoteButtonBackground: Color {
        switch session.state {
        case .inviting, .connecting:
            return Color.orange.opacity(0.12)
        case .viewing:
            return Color.red.opacity(0.1)
        case .idle, .failed:
            return Color(.secondarySystemBackground)
        }
    }

    private var remoteButtonAccessibilityLabel: String {
        switch session.state {
        case .inviting, .connecting:
            return "取消远程连接"
        case .viewing:
            return "断开远程桌面"
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

private struct MessageListView: View {
    let messages: [RemoteIMMessage]
    let peerRelation: RemoteIMContactRelation
    let searchTargetMessageID: UUID?
    let hasEarlierMessages: Bool
    let initialHistoryLoadGeneration: Int
    let selectingMessageID: UUID?
    let finishSelectingText: () -> Void
    let presentImagePreview: (RemoteIMImagePreviewItem, UIImage, CGRect) -> Void
    let showMessageActions: (RemoteIMMessage, CGRect) -> Void
    let replyToMessage: (RemoteIMMessage) -> Void
    let openQuote: (RemoteIMQuote) -> Void
    let loadEarlierMessages: () async -> Void
    @EnvironmentObject private var appState: RemoteIMAppState
    @StateObject private var voicePlayer = VoiceMessagePlayer()
    @State private var videoPreviewItem: RemoteIMVideoPreviewItem?
    @State private var filePreviewItem: RemoteIMFilePreviewItem?
    @State private var latestMessageID: UUID?
    @State private var isLoadingEarlierMessages = false
    @State private var isNearBottom = true
    @State private var hasUnseenLatestMessage = false

    private let bottomAnchorID = "message-list-bottom"

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
        ScrollViewReader { proxy in
            ScrollView {
                // Keep the initial page eagerly laid out so the dedicated bottom anchor has its
                // final position before the first scroll. LazyVStack recreates the historical
                // "first open is blank until the user drags" failure with variable-height Markdown
                // bubbles, even when the anchor scroll is repeated on the next main-loop turn.
                VStack(alignment: .leading, spacing: 14) {
                    if messages.isEmpty {
                        EmptyMessagesView()
                            .padding(.top, 72)
                    } else {
                        ForEach(messages) { message in
                            MessageBubbleView(
                                message: message,
                                approvalDecisionState: message.approvalRequest.map {
                                    decisionStates[$0.token] ?? .available
                                } ?? .available,
                                senderProfile: appState.profile(for: message.fromUserID),
                                incomingRelation: peerRelation,
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
                                .id(message.id)
                        }
                    }
                    Color.clear
                        .frame(height: 1)
                        .id(bottomAnchorID)
                        .onAppear {
                            isNearBottom = true
                            hasUnseenLatestMessage = false
                        }
                        .onDisappear {
                            isNearBottom = false
                        }
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 18)
                .background(
                    ScrollViewKeyboardDismissInstaller { window in
                        dismissKeyboard(in: window)
                        if selectingMessageID != nil {
                            finishSelectingText()
                        }
                    }
                )
            }
            .refreshable {
                await loadEarlierMessagesKeepingAnchor(proxy: proxy)
            }
            .scrollDismissesKeyboard(.interactively)
            .background(RemoteIMStyle.panelBackground)
            .onAppear {
                latestMessageID = messages.last?.id
                if !scrollToSearchTarget(proxy: proxy) {
                    scrollToLatestMessage(proxy: proxy)
                }
            }
            .onChange(of: searchTargetMessageID) { _ in
                _ = scrollToSearchTarget(proxy: proxy)
            }
            .onChange(of: messages.last?.id) { _ in
                let nextLatestMessageID = messages.last?.id
                guard nextLatestMessageID != latestMessageID else { return }
                latestMessageID = nextLatestMessageID
                let latestMessage = messages.last
                let shouldScroll = searchTargetMessageID == nil &&
                    (isNearBottom || latestMessage?.direction == .outgoing)
                if let latestMessage, latestMessage.approvalRequest != nil {
                    AppDiagnosticLog.shared.record(
                        level: .info,
                        category: "approval",
                        event: "list-update",
                        fields: [
                            "message": DiagnosticLogPrivacy.stableTag(
                                latestMessage.remoteID ?? latestMessage.id.uuidString,
                                prefix: "m"
                            ),
                            "near_bottom": isNearBottom ? "true" : "false",
                            "scroll_action": shouldScroll ? "scroll-to-bottom" : "show-new-message-indicator",
                        ]
                    )
                }
                if shouldScroll {
                    hasUnseenLatestMessage = false
                    scrollToLatestMessage(proxy: proxy)
                } else {
                    hasUnseenLatestMessage = true
                }
            }
            .onChange(of: initialHistoryLoadGeneration) { _ in
                latestMessageID = messages.last?.id
                hasUnseenLatestMessage = false
                if !scrollToSearchTarget(proxy: proxy) {
                    scrollToLatestMessage(proxy: proxy)
                }
            }
            .overlay(alignment: .bottomTrailing) {
                if hasUnseenLatestMessage {
                    Button {
                        hasUnseenLatestMessage = false
                        scrollToLatestMessage(proxy: proxy)
                    } label: {
                        Label("新消息", systemImage: "arrow.down")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(.white)
                            .padding(.horizontal, 12)
                            .frame(height: 34)
                            .background(RemoteIMStyle.blue, in: Capsule())
                    }
                    .buttonStyle(.plain)
                    .padding(12)
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

    @discardableResult
    private func scrollToSearchTarget(proxy: ScrollViewProxy) -> Bool {
        guard let targetID = searchTargetMessageID,
              messages.contains(where: { $0.id == targetID })
        else { return false }
        DispatchQueue.main.async {
            withAnimation(.easeOut(duration: 0.2)) {
                proxy.scrollTo(targetID, anchor: .center)
            }
        }
        return true
    }

    private func scrollToLatestMessage(proxy: ScrollViewProxy) {
        guard MessageListAutoScrollPolicy.latestMessageID(from: messages) != nil else {
            return
        }
        DispatchQueue.main.async {
            scrollToBottom(proxy: proxy)
            // The first pass establishes the content layout. Repeating on the next main-loop
            // turn accounts for multiline Markdown text whose final height is resolved then.
            DispatchQueue.main.async {
                scrollToBottom(proxy: proxy)
            }
        }
    }

    private func scrollToBottom(proxy: ScrollViewProxy) {
        var transaction = Transaction(animation: nil)
        transaction.disablesAnimations = true
        withTransaction(transaction) {
            proxy.scrollTo(bottomAnchorID, anchor: .bottom)
        }
    }

    @MainActor
    private func loadEarlierMessagesKeepingAnchor(proxy: ScrollViewProxy) async {
        guard hasEarlierMessages, !isLoadingEarlierMessages else { return }
        let previousFirstMessageID = messages.first?.id
        isLoadingEarlierMessages = true
        defer { isLoadingEarlierMessages = false }

        await loadEarlierMessages()
        guard let previousFirstMessageID else { return }
        await withCheckedContinuation { continuation in
            DispatchQueue.main.async {
                var transaction = Transaction(animation: nil)
                transaction.disablesAnimations = true
                withTransaction(transaction) {
                    proxy.scrollTo(previousFirstMessageID, anchor: .top)
                }
                continuation.resume()
            }
        }
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
    let senderProfile: RemoteIMUserProfile
    let incomingRelation: RemoteIMContactRelation
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
    @State private var textSelectionController = MessageTextSelectionController()

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            if message.direction == .outgoing {
                Spacer(minLength: 50)
            }

            if message.direction == .incoming {
                RemoteIMUserAvatar(profile: senderProfile, outgoing: false, size: 40)
            }

            VStack(alignment: message.direction == .outgoing ? .trailing : .leading, spacing: 6) {
                messageMetadata
                messageContent
            }
            .padding(.top, 4)
            .layoutPriority(1)

            if message.direction == .outgoing {
                RemoteIMUserAvatar(profile: senderProfile, outgoing: true, size: 40)
            }

            if message.direction == .incoming {
                Spacer(minLength: 50)
            }
        }
        .frame(maxWidth: .infinity, alignment: message.direction == .outgoing ? .trailing : .leading)
        // 不使用系统 contextMenu：它会把被长按的消息单独提亮/放大，且视觉风格
        // 与 MaiChat 完全不同。长按只打开根层自绘卡片，消息本身保持原样。
        .onLongPressGesture(minimumDuration: 0.42) {
            guard !isSelectingText else { return }
            showActions(actionSourceFrame)
        }
        .accessibilityAction(named: "消息操作") {
            showActions(actionSourceFrame)
        }
        .accessibilityAction(named: "引用回复") {
            reply()
        }
    }

    private var messageMetadata: some View {
        HStack(spacing: 8) {
            Text(message.fromUserID)
                .font(.system(size: 13, weight: .bold, design: .monospaced))
                .foregroundStyle(RemoteIMStyle.textPrimary)
                .lineLimit(1)
                .truncationMode(.middle)
            if let relationText {
                RelationBadge(text: relationText)
            }
            Text(RemoteIMTimestampTextPolicy.displayText(for: message.createdAt))
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(RemoteIMStyle.textSecondary)
        }
    }

    private var relationText: String? {
        message.direction == .outgoing ? nil : incomingRelation.displayName
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
                        let fileState = RemoteIMVideoFileState(
                            attachment: videoAttachment,
                            isDownloadingHint: isVideoDownloading
                        )
                        Button(action: previewVideo) {
                            VideoBubbleContent(
                                attachment: videoAttachment,
                                isIncoming: message.direction == .incoming,
                                fileState: fileState
                            )
                        }
                        .buttonStyle(.plain)
                        .disabled(!fileState.isPlayable)
                        .accessibilityIdentifier("remote-im-video-bubble")
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
                    MarkdownLikeText(message.text)
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
            .background(bubbleBackground, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .stroke(bubbleBorder, lineWidth: 1)
            )

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
                    .onChange(of: geometry.frame(
                        in: .named(RemoteIMImagePreviewLayout.coordinateSpaceName)
                    )) { nextFrame in
                        actionSourceFrame = nextFrame
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
        message.direction == .outgoing ? Color.white : RemoteIMStyle.yellowSoft
    }

    private var bubbleBorder: Color {
        message.direction == .outgoing ? Color(red: 0.764, green: 0.873, blue: 0.996) : RemoteIMStyle.yellowBorder
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

    var body: some View {
        NavigationStack {
            Group {
                if let integrityError {
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
                    RemoteIMHTMLPreview(filePath: item.attachment.localFilePath)
                } else if item.attachment.mimeType == "text/markdown" ||
                            item.attachment.fileName.lowercased().hasSuffix(".md")
                {
                    RemoteIMMarkdownFilePreview(filePath: item.attachment.localFilePath)
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
    }

    private var isGitDiff: Bool {
        RemoteIMGitDiffDisplayPolicy.isGitDiff(item.attachment)
    }

    private var integrityError: String? {
        guard isGitDiff,
              let expected = RemoteIMGitDiffDisplayPolicy.expectedSHA256(
                  fileName: item.attachment.fileName
              )
        else { return nil }
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: item.attachment.localFilePath))
        else { return "无法读取 Diff 文件。" }
        let actual = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        return actual == expected ? nil : "文件内容与发送方提供的 SHA256 不一致，已停止渲染。"
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
    let filePath: String

    var body: some View {
        ScrollView {
            MarkdownLikeText(previewText)
                .font(.system(size: 14))
                .padding(18)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .background(RemoteIMStyle.panelBackground)
    }

    private var previewText: String {
        (try? String(contentsOfFile: filePath, encoding: .utf8)) ?? "文件暂不可预览"
    }
}

private struct RemoteIMHTMLPreview: UIViewRepresentable {
    let filePath: String

    func makeUIView(context _: Context) -> WKWebView {
        let configuration = WKWebViewConfiguration()
        configuration.preferences.javaScriptEnabled = false
        let webView = WKWebView(frame: .zero, configuration: configuration)
        webView.backgroundColor = .white
        return webView
    }

    func updateUIView(_ webView: WKWebView, context _: Context) {
        let html = (try? String(contentsOfFile: filePath, encoding: .utf8)) ?? "<p>文件暂不可预览</p>"
        webView.loadHTMLString(html, baseURL: URL(fileURLWithPath: filePath).deletingLastPathComponent())
    }
}

private struct RemoteIMVideoFileState: Equatable {
    let isPlayable: Bool
    let isDownloading: Bool

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
    @State private var sourceFrame: CGRect = .zero

    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            RemoteIMAsyncImage(
                filePath: attachment.localFilePath,
                maximumPointSize: CGSize(width: 220, height: 180)
            ) { image in
                Button {
                    previewImage(image, sourceFrame)
                } label: {
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFit()
                        .frame(maxWidth: 220, maxHeight: 180)
                        .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                        .background(Color(red: 0.945, green: 0.957, blue: 0.973), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
                        .accessibilityLabel("消息图片")
                        .accessibilityIdentifier("remote-im-message-image")
                        .background {
                            GeometryReader { geometry in
                                Color.clear.preference(
                                    key: RemoteIMImageBubbleFramePreferenceKey.self,
                                    value: geometry.frame(
                                        in: .named(
                                            RemoteIMImagePreviewLayout.coordinateSpaceName
                                        )
                                    )
                                )
                            }
                        }
                }
                .buttonStyle(.plain)
                .onPreferenceChange(RemoteIMImageBubbleFramePreferenceKey.self) {
                    sourceFrame = $0
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

private final class MarkdownBlocksBox {
    let value: [MarkdownBlock]

    init(_ value: [MarkdownBlock]) {
        self.value = value
    }
}

private final class MarkdownAttributedTextBox {
    let value: AttributedString?

    init(_ value: AttributedString?) {
        self.value = value
    }
}

private final class MarkdownRenderCache: @unchecked Sendable {
    static let shared = MarkdownRenderCache()

    private let blocks = NSCache<NSString, MarkdownBlocksBox>()
    private let attributedTexts = NSCache<NSString, MarkdownAttributedTextBox>()

    private init() {
        blocks.countLimit = 256
        blocks.totalCostLimit = 2 * 1_024 * 1_024
        attributedTexts.countLimit = 512
        attributedTexts.totalCostLimit = 2 * 1_024 * 1_024
    }

    func blocks(for text: String) -> [MarkdownBlock] {
        let key = text as NSString
        if let cached = blocks.object(forKey: key) {
            return cached.value
        }
        let parsed = parseMarkdownBlocks(text)
        blocks.setObject(
            MarkdownBlocksBox(parsed),
            forKey: key,
            cost: text.lengthOfBytes(using: .utf8)
        )
        return parsed
    }

    func attributedText(for text: String) -> AttributedString? {
        let key = text as NSString
        if let cached = attributedTexts.object(forKey: key) {
            return cached.value
        }
        let parsed = try? AttributedString(markdown: text)
        attributedTexts.setObject(
            MarkdownAttributedTextBox(parsed),
            forKey: key,
            cost: text.lengthOfBytes(using: .utf8)
        )
        return parsed
    }
}

private struct MarkdownLikeText: View {
    private let blocks: [MarkdownBlock]

    init(_ text: String) {
        self.blocks = MarkdownRenderCache.shared.blocks(for: text)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(blocks) { block in
                switch block.kind {
                case .markdown(let text):
                    MarkdownInlineText(text: text)
                case .heading(let level, let text):
                    MarkdownHeadingView(level: level, text: text)
                case .unorderedList(let list):
                    MarkdownListView(list: list)
                case .code(let code):
                    MarkdownCodeBlock(code: code)
                case .table(let table):
                    MarkdownTableView(table: table)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .lineLimit(nil)
        .multilineTextAlignment(.leading)
        .fixedSize(horizontal: false, vertical: true)
        // 普通态长按统一交给 MessageActionDialog。这里启用系统选择会优先弹出
        // Copy / Share…，绕过 MaiChat 的自绘菜单；需要选字时由“选择”动作切换到
        // MenuSuppressingTextView。
    }
}

private struct MarkdownBlock: Identifiable {
    let id = UUID()
    let kind: MarkdownBlockKind
}

private enum MarkdownBlockKind {
    case markdown(String)
    case heading(level: Int, text: String)
    case unorderedList(MarkdownList)
    case code(String)
    case table(MarkdownTable)
}

private struct MarkdownList {
    let items: [MarkdownListItem]
}

private struct MarkdownListItem: Identifiable {
    let id = UUID()
    var text: String
}

private struct MarkdownTable {
    let headers: [String]
    let rows: [[String]]
}

private struct MarkdownInlineText: View {
    let text: String
    private let attributedText: AttributedString?

    init(text: String) {
        self.text = text
        self.attributedText = MarkdownRenderCache.shared.attributedText(for: text)
    }

    var body: some View {
        Group {
            if let attributedText {
                Text(attributedText)
            } else {
                Text(text)
            }
        }
        .lineLimit(nil)
        .multilineTextAlignment(.leading)
        .fixedSize(horizontal: false, vertical: true)
    }
}

private struct MarkdownHeadingView: View {
    let level: Int
    let text: String

    var body: some View {
        MarkdownInlineText(text: text)
            .font(.system(size: fontSize, weight: .bold))
            .foregroundStyle(RemoteIMStyle.textPrimary)
            .padding(.top, level <= 2 ? 2 : 0)
    }

    private var fontSize: CGFloat {
        switch level {
        case 1:
            return 17
        case 2:
            return 15
        default:
            return 14
        }
    }
}

private struct MarkdownListView: View {
    let list: MarkdownList

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(list.items) { item in
                HStack(alignment: .top, spacing: 7) {
                    Text("•")
                        .font(.system(size: 13, weight: .bold))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                        .frame(width: 10, alignment: .center)
                        .padding(.top, 1)
                    MarkdownInlineText(text: item.text)
                        .font(.system(size: 13, weight: .regular))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                        .lineSpacing(3)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
    }
}

private struct MarkdownCodeBlock: View {
    let code: String

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            Text(code.isEmpty ? " " : code)
                .font(.system(size: 12, weight: .regular, design: .monospaced))
                .foregroundStyle(RemoteIMStyle.textPrimary)
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .background(Color(red: 0.945, green: 0.957, blue: 0.973), in: RoundedRectangle(cornerRadius: 7, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .stroke(RemoteIMStyle.border, lineWidth: 1)
        )
    }
}

private struct MarkdownTableView: View {
    let table: MarkdownTable

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            Grid(alignment: .leading, horizontalSpacing: 0, verticalSpacing: 0) {
                GridRow {
                    ForEach(0..<columnCount, id: \.self) { column in
                        tableCell(
                            table.headers[safe: column] ?? "",
                            isHeader: true,
                            width: columnWidths[column]
                        )
                    }
                }
                ForEach(Array(table.rows.enumerated()), id: \.offset) { _, row in
                    GridRow {
                        ForEach(0..<columnCount, id: \.self) { column in
                            tableCell(
                                row[safe: column] ?? "",
                                isHeader: false,
                                width: columnWidths[column]
                            )
                        }
                    }
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 7, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 7, style: .continuous)
                    .stroke(RemoteIMStyle.border, lineWidth: 1)
            )
        }
    }

    private var columnCount: Int {
        max(table.headers.count, table.rows.map(\.count).max() ?? 0)
    }

    private var columnWidths: [CGFloat] {
        (0..<columnCount).map { column in
            let values = [table.headers[safe: column] ?? ""] +
                table.rows.map { $0[safe: column] ?? "" }
            let longestLineLength = values
                .flatMap { $0.components(separatedBy: .newlines) }
                .map(\.count)
                .max() ?? 0
            return min(180, max(78, CGFloat(longestLineLength) * 7 + 18))
        }
    }

    private func tableCell(_ text: String, isHeader: Bool, width: CGFloat) -> some View {
        MarkdownInlineText(text: text)
            .font(.system(size: 12, weight: isHeader ? .semibold : .regular))
            .foregroundStyle(RemoteIMStyle.textPrimary)
            .lineLimit(nil)
            .frame(width: max(1, width - 18), alignment: .topLeading)
            .padding(.horizontal, 9)
            .padding(.vertical, 7)
            .frame(width: width, alignment: .topLeading)
            .background(isHeader ? Color(red: 0.93, green: 0.945, blue: 0.965) : Color.white.opacity(0.72))
            .overlay(
                Rectangle()
                    .stroke(RemoteIMStyle.border, lineWidth: 0.5)
            )
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

        if trimmed.hasPrefix("```") {
            flushMarkdown()
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
            blocks.append(MarkdownBlock(kind: .code(codeLines.joined(separator: "\n"))))
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
            blocks.append(MarkdownBlock(kind: .unorderedList(parsed.list)))
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

        if let itemText = parseMarkdownListItem(line) {
            items.append(MarkdownListItem(text: itemText))
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

private func parseMarkdownListItem(_ line: String) -> String? {
    let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
    for marker in ["- ", "* ", "+ "] {
        if trimmed.hasPrefix(marker) {
            return String(trimmed.dropFirst(marker.count)).trimmingCharacters(in: .whitespacesAndNewlines)
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

@MainActor
private final class VoiceMessageRecorder: NSObject, ObservableObject {
    @Published var isRecording = false

    private var recorder: AVAudioRecorder?
    private var startedAt: Date?
    private var recordingURL: URL?

    func start() async throws {
        guard !isRecording else { return }
        let granted = await requestRecordPermission()
        guard granted else {
            throw VoiceRecorderError.microphonePermissionDenied
        }

        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playAndRecord, mode: .spokenAudio, options: [.defaultToSpeaker])
        try session.setActive(true)

        let url = RemoteIMMediaStorage.fileURL(
            category: .outgoingVoices,
            stem: "remote-im-voice-\(UUID().uuidString)",
            pathExtension: "m4a"
        )
        let settings: [String: Any] = [
            AVFormatIDKey: Int(kAudioFormatMPEG4AAC),
            AVSampleRateKey: 16_000,
            AVNumberOfChannelsKey: 1,
            AVEncoderAudioQualityKey: AVAudioQuality.medium.rawValue
        ]
        let nextRecorder = try AVAudioRecorder(url: url, settings: settings)
        nextRecorder.prepareToRecord()
        guard nextRecorder.record() else {
            throw VoiceRecorderError.startFailed
        }

        recorder = nextRecorder
        recordingURL = url
        startedAt = Date()
        isRecording = true
    }

    func stop() -> RemoteIMVoiceRecording? {
        guard isRecording, let recorder, let recordingURL else { return nil }
        recorder.stop()
        self.recorder = nil
        self.recordingURL = nil
        isRecording = false
        let duration = max(1, Int(ceil(Date().timeIntervalSince(startedAt ?? Date()))))
        startedAt = nil
        return RemoteIMVoiceRecording(fileURL: recordingURL, durationSeconds: duration)
    }

    func cancel() {
        let url = recordingURL
        recorder?.stop()
        recorder = nil
        recordingURL = nil
        startedAt = nil
        isRecording = false
        if let url {
            try? FileManager.default.removeItem(at: url)
        }
    }

    private func requestRecordPermission() async -> Bool {
        await withCheckedContinuation { continuation in
            AVAudioSession.sharedInstance().requestRecordPermission { granted in
                continuation.resume(returning: granted)
            }
        }
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
            let sourceExtension = sourceURL.pathExtension.trimmingCharacters(in: .whitespacesAndNewlines)
            let targetURL = RemoteIMMediaStorage.fileURL(
                category: .outgoingVideos,
                stem: "remote-im-video-\(UUID().uuidString)",
                pathExtension: sourceExtension.isEmpty ? "mov" : sourceExtension
            )
            try FileManager.default.copyItem(at: sourceURL, to: targetURL)
            return RemoteIMPickedVideoTransfer(fileURL: targetURL)
        }
    }
}

private struct ComposerAttachmentPanel: View {
    let canSendImage: Bool
    let canSendVideo: Bool
    let canSendFile: Bool
    let canSendVoice: Bool
    let openLibrary: () -> Void
    let openCamera: () -> Void
    let openFile: () -> Void
    let openVoiceInput: () -> Void

    private let columns = Array(
        repeating: GridItem(.flexible(), spacing: 12, alignment: .top),
        count: 4
    )

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
            actionButton(
                title: "语音输入",
                systemImage: "mic.fill",
                enabled: canSendVoice,
                action: openVoiceInput
            )
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
    @State private var isAttachmentPanelPresented = false
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
                            text: $draft.text,
                            onSubmit: submitDraft,
                            focusRequestGeneration: composerFocusRequestGeneration,
                            editingController: composerEditingController,
                            onEditMenuRequested: { state in
                                withAnimation(.easeOut(duration: 0.14)) {
                                    composerEditMenuState = state.hasActions ? state : nil
                                }
                            },
                            onEditMenuDismissed: {
                                withAnimation(.easeOut(duration: 0.12)) {
                                    composerEditMenuState = nil
                                }
                            },
                            voiceTranscriptionEnabled: appState.canSendVoice && draft.text.isEmpty,
                            onVoiceLongPressChanged: { translation in
                                handleVoicePressChanged(
                                    translation: translation,
                                    showsTranscriptionHighlight: true
                                )
                            },
                            onVoiceLongPressEnded: { translation in
                                Task {
                                    await handleVoicePressEnded(
                                        translation: translation,
                                        sendsVoiceDirectly: false
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
                                    withAnimation(.easeOut(duration: 0.12)) {
                                        composerEditMenuState = nil
                                    }
                                },
                                perform: performComposerEditAction
                            )
                            .offset(x: 4, y: -50)
                            .transition(.scale(scale: 0.94, anchor: .bottomLeading).combined(with: .opacity))
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
                        .rotationEffect(.degrees(isAttachmentPanelPresented ? 45 : 0))
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
            realtimeSpeechRecognizer.onLiveTextUpdate = {
                [weak transcriptionPresentation] text, sessionID in
                guard transcriptionPresentation?.target != nil else { return }
                transcriptionPresentation?.updateLiveText(text, sessionID: sessionID)
            }
        }
        .onDisappear {
            composerEditMenuState = nil
            realtimeSpeechRecognizer.onLiveTextUpdate = nil
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
        Task { await appState.sendDraft() }
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
        showsTranscriptionHighlight: Bool
    ) {
        guard appState.canSendVoice else { return }
        if !isPressingVoice {
            isPressingVoice = true
            if showsTranscriptionHighlight {
                transcriptionPresentation.prepareForNewSession()
                transcriptionPresentation.target = .send
                AppDiagnosticLog.shared.record(
                    level: .info,
                    category: "asr",
                    event: "gesture-started",
                    fields: ["mode": "transcription"]
                )
                realtimeStartTask = Task { await startRealtimeTranscription() }
            } else {
                Task { await startVoiceRecording() }
            }
        }
        if showsTranscriptionHighlight {
            let target = transcriptionTarget(for: translation)
            isCancellingVoice = target == .cancel
            transcriptionPresentation.target = target
        } else {
            isCancellingVoice = translation.height < -70
            transcriptionPresentation.target = nil
        }
    }

    private func handleVoicePressEnded(
        translation: CGSize,
        sendsVoiceDirectly: Bool
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
            guard let recording = voiceRecorder.stop() else { return }
            await appState.sendVoiceRecording(recording)
            return
        }

        let releaseTarget = transcriptionTarget(for: translation)
        isCancellingVoice = false
        if releaseTarget == .cancel {
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "asr",
                event: "gesture-ended",
                fields: ["action": "cancel"]
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
            fields: ["action": shouldEdit ? "edit" : "send"]
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
                appState.errorMessage = "没有识别到文字，未发送"
                return
            }
            if shouldEdit {
                draft.text = text
                composerFocusRequestGeneration &+= 1
            } else {
                await appState.sendText(text)
            }
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "asr",
                event: "transcription-applied",
                fields: [
                    "action": shouldEdit ? "edit" : "send",
                    "characters": String(text.count),
                ]
            )
        } catch is CancellationError {
            transcriptionPresentation.reset()
        } catch {
            transcriptionPresentation.reset()
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

    private func transcriptionTarget(for translation: CGSize) -> VoiceTranscriptionTarget {
        if translation.width < -70 {
            return .cancel
        }
        if translation.width > 70, translation.height < -35 {
            return .edit
        }
        return .send
    }

    private func startRealtimeTranscription() async -> Bool {
        guard realtimeSpeechRecognizer.isAvailable else {
            transcriptionPresentation.target = nil
            isPressingVoice = false
            appState.errorMessage = "语音转文字凭证未配置"
            return false
        }
        do {
            try await realtimeSpeechRecognizer.start()
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
                    "error_code": String((error as NSError).code),
                ]
            )
            return false
        }
    }

    private func startVoiceRecording() async {
        do {
            try await voiceRecorder.start()
        } catch {
            transcriptionPresentation.target = nil
            isPressingVoice = false
            isCancellingVoice = false
            appState.errorMessage = error.localizedDescription
        }
    }

    private func sendSelectedMedia(_ items: [PhotosPickerItem]) async {
        defer { selectedMediaItems = [] }
        var sentCount = 0
        var failedReadCount = 0

        for item in items {
            do {
                if item.supportedContentTypes.contains(where: { $0.conforms(to: .movie) }) {
                    let video = try await preparePickedVideo(item)
                    await appState.sendVideoFile(video)
                } else {
                    guard let data = try await item.loadTransferable(type: Data.self) else {
                        failedReadCount += 1
                        continue
                    }
                    let imageFile = try savePickedImage(
                        data: data,
                        contentTypes: item.supportedContentTypes
                    )
                    await appState.sendImageFile(imageFile)
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
        guard let coverData = UIImage(cgImage: generatedCover.image).jpegData(compressionQuality: 0.86) else {
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
        return RemoteIMVideoFile(
            fileURL: fileURL,
            coverFileURL: coverFileURL,
            fileType: fileType.isEmpty ? "mp4" : fileType.lowercased(),
            durationSeconds: durationSeconds,
            width: width,
            height: height,
            sizeBytes: Int64(max(0, resourceValues.fileSize ?? 0))
        )
    }

    private func sendCapturedPhoto(_ image: UIImage) async {
        do {
            guard let data = image.jpegData(compressionQuality: 0.9) else {
                appState.errorMessage = "拍摄的照片处理失败"
                return
            }
            let imageFile = try savePickedImage(data: data, contentTypes: [.jpeg])
            await appState.sendImageFile(imageFile)
        } catch {
            appState.errorMessage = error.localizedDescription
        }
    }

    private func sendSelectedFile(_ result: Result<[URL], Error>) async {
        do {
            guard let selectedURL = try result.get().first else { return }
            let file = try copyImportedFileToCache(selectedURL)
            await appState.sendFile(file)
        } catch {
            appState.errorMessage = error.localizedDescription
        }
    }

    private func copyImportedFileToCache(_ sourceURL: URL) throws -> RemoteIMFile {
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

    private func savePickedImage(
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
    @Binding var text: String
    let onSubmit: () -> Void
    let focusRequestGeneration: Int
    let editingController: ComposerTextEditingController
    let onEditMenuRequested: (ComposerEditMenuState) -> Void
    let onEditMenuDismissed: () -> Void
    let voiceTranscriptionEnabled: Bool
    let onVoiceLongPressChanged: (CGSize) -> Void
    let onVoiceLongPressEnded: (CGSize) -> Void
    let onVoiceLongPressCancelled: () -> Void

    private let minimumHeight: CGFloat = 44
    private let maximumLineCount: CGFloat = 5
    private let verticalInset: CGFloat = 11

    func makeCoordinator() -> Coordinator {
        Coordinator(parent: self)
    }

    func makeUIView(context: Context) -> GrowingComposerUITextView {
        let textView = GrowingComposerUITextView()
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
        context.coordinator.parent = self
        editingController.textView = textView
        textView.removeSystemEditMenuInteractions()
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
            if !isApplyingExternalText {
                parent.text = textView.text
            }
            parent.onEditMenuDismissed()
            scheduleContentHeightRefresh(for: textView)
        }

        func textViewDidEndEditing(_ textView: UITextView) {
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
                return true
            }

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
            let location = gesture.location(in: gesture.view)
            switch gesture.state {
            case .began:
                guard parent.voiceTranscriptionEnabled else { return }
                voiceLongPressOrigin = location
                gesture.view?.resignFirstResponder()
                parent.onVoiceLongPressChanged(.zero)
            case .changed:
                guard let origin = voiceLongPressOrigin else { return }
                parent.onVoiceLongPressChanged(
                    CGSize(width: location.x - origin.x, height: location.y - origin.y)
                )
            case .ended:
                guard let origin = voiceLongPressOrigin else { return }
                voiceLongPressOrigin = nil
                parent.onVoiceLongPressEnded(
                    CGSize(width: location.x - origin.x, height: location.y - origin.y)
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
            DispatchQueue.main.async { [weak self, weak textView] in
                guard let self else { return }
                self.hasScheduledContentHeightRefresh = false
                guard let textView else { return }
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

private struct RemoteIMCameraPicker: UIViewControllerRepresentable {
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
