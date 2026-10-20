// 文件作用：展示联系人列表与联系人操作入口，并将选择结果交给 IM 状态层。
// 展示联系人、分组、搜索和添加入口，并将选择结果交给主导航。
// 关系状态与资料更新来自 IM 状态层，本文件负责列表交互。
import MaiChatCore
import AVFoundation
import CoreImage.CIFilterBuiltins
import SwiftUI
import UIKit

private enum FriendQRImage {
    static func image(for url: URL) -> UIImage? {
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(url.absoluteString.utf8)
        filter.correctionLevel = "M"
        guard let output = filter.outputImage,
              let bitmap = CIContext().createCGImage(
                output.transformed(by: CGAffineTransform(scaleX: 12, y: 12)),
                from: output.extent.applying(CGAffineTransform(scaleX: 12, y: 12))
              ) else { return nil }
        return UIImage(cgImage: bitmap)
    }
}

struct ContactsView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Binding var selectedTab: AppTab
    @Binding var activeContact: RemoteIMContact?
    @Binding var movingContact: RemoteIMContact?
    @Binding var groupActionName: String?
    let showAddContact: () -> Void

    var body: some View {
        ZStack {
            NavigationStack {
                ContactList(
                    selectedTab: $selectedTab,
                    activeContact: $activeContact,
                    movingContact: $movingContact,
                    groupActionName: $groupActionName
                )
                .background(RemoteIMStyle.pageBackground.ignoresSafeArea())
                .toolbar {
                    ToolbarItem(placement: .navigationBarTrailing) {
                        Button(action: showAddContact) {
                            Image(systemName: "plus")
                                .font(.system(size: 17, weight: .semibold))
                        }
                        .accessibilityLabel("添加好友")
                    }
                }
                .toolbarBackground(RemoteIMStyle.panelBackground, for: .navigationBar)
                .toolbarBackground(.visible, for: .navigationBar)
                .task {
                    await appState.refreshAcceptedFriends()
                    await appState.refreshFriendRequests()
                }
            }
        }
    }
}

private enum FriendQRDestination: String, Identifiable {
    case scan, mine
    var id: String { rawValue }
}

struct AddContactDialog: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Binding var isPresented: Bool
    @FocusState private var isAccountFocused: Bool
    @State private var isSubmitting = false
    @State private var qrDestination: FriendQRDestination?

    var body: some View {
        ZStack {
            Color.black.opacity(0.28)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture(perform: cancel)

            VStack(alignment: .leading, spacing: 16) {
                VStack(alignment: .leading, spacing: 5) {
                    Text("添加好友")
                        .font(.system(size: 20, weight: .bold))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                    Text("输入手机号或好友账号 ID")
                        .font(.system(size: 13))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                }

                TextField("手机号或账号 ID", text: $appState.newContactUserID)
                    .font(.system(size: 15))
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .submitLabel(.done)
                    .focused($isAccountFocused)
                    .padding(.horizontal, 14)
                    .frame(height: 44)
                    .background(
                        RemoteIMStyle.pageBackground,
                        in: RoundedRectangle(cornerRadius: 9, style: .continuous)
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 9, style: .continuous)
                            .stroke(
                                isAccountFocused ? RemoteIMStyle.blue : RemoteIMStyle.border,
                                lineWidth: isAccountFocused ? 1.5 : 1
                            )
                    )
                    .onSubmit(addContact)

                HStack(spacing: 14) {
                    Button {
                        isAccountFocused = false
                        qrDestination = .scan
                    } label: {
                        Label("扫一扫", systemImage: "qrcode.viewfinder")
                    }
                    Button {
                        isAccountFocused = false
                        qrDestination = .mine
                    } label: {
                        Label("我的二维码", systemImage: "qrcode")
                    }
                }
                .font(.system(size: 14, weight: .medium))
                .foregroundStyle(RemoteIMStyle.blue)

                HStack(spacing: 10) {
                    Button("取消", action: cancel)
                        .buttonStyle(.plain)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                        .frame(maxWidth: .infinity)
                        .frame(height: 42)
                        .background(
                            Color(.secondarySystemBackground),
                            in: RoundedRectangle(cornerRadius: 9, style: .continuous)
                        )

                    Button(isSubmitting ? "发送中" : "发送申请", action: addContact)
                        .buttonStyle(.plain)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(canAdd ? Color.white : RemoteIMStyle.textSecondary)
                        .frame(maxWidth: .infinity)
                        .frame(height: 42)
                        .background(
                            canAdd ? RemoteIMStyle.blue : Color(.secondarySystemBackground),
                            in: RoundedRectangle(cornerRadius: 9, style: .continuous)
                        )
                        .disabled(!canAdd || isSubmitting)
                }
            }
            .padding(20)
            .frame(maxWidth: 340)
            .background(
                RemoteIMStyle.panelBackground,
                in: RoundedRectangle(cornerRadius: 16, style: .continuous)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .stroke(RemoteIMStyle.border, lineWidth: 1)
            )
            .shadow(color: Color.black.opacity(0.16), radius: 24, y: 10)
            .padding(.horizontal, 24)
        }
        .onAppear {
            DispatchQueue.main.async {
                isAccountFocused = true
            }
        }
        .sheet(item: $qrDestination) { destination in
            NavigationStack {
                switch destination {
                case .mine:
                    FriendQRShareView(userID: appState.maiChatAccountID)
                case .scan:
                    FriendQRScanView { userID in
                        qrDestination = nil
                        appState.newContactUserID = userID
                        isAccountFocused = false
                    }
                }
            }
        }
    }

    private var canAdd: Bool {
        !appState.newContactUserID
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .isEmpty
    }

    private func addContact() {
        guard canAdd && !isSubmitting else { return }
        isSubmitting = true
        Task {
            let added = await appState.addContact()
            isSubmitting = false
            if added { isPresented = false }
        }
    }

    private func cancel() {
        appState.newContactUserID = ""
        isPresented = false
    }
}

struct FriendQRShareView: View {
    let userID: String

    var body: some View {
        VStack(spacing: 22) {
            Text("让对方扫一扫添加你为好友")
                .font(.headline)
            if let url = MaiChatFriendLink.url(for: userID),
               let image = FriendQRImage.image(for: url) {
                Image(uiImage: image)
                    .interpolation(.none)
                    .resizable()
                    .frame(width: 240, height: 240)
                    .padding(16)
                    .background(.white, in: RoundedRectangle(cornerRadius: 16))
            } else {
                Label("账号尚未就绪", systemImage: "qrcode")
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
            Text(userID)
                .font(.system(.footnote, design: .monospaced))
                .textSelection(.enabled)
            Button("复制账号 ID") { UIPasteboard.general.string = userID }
                .buttonStyle(.bordered)
            Spacer(minLength: 0)
        }
        .padding(.top, 28)
        .padding(.horizontal, 20)
        .navigationTitle("我的好友二维码")
        .navigationBarTitleDisplayMode(.inline)
    }
}

private struct FriendQRScanView: View {
    @Environment(\.scenePhase) private var scenePhase
    @State private var cameraAuthorized = false
    @State private var cameraDenied = false
    @State private var errorMessage: String?
    @State private var scanGeneration = 0
    let onUserID: (String) -> Void

    var body: some View {
        VStack(spacing: 18) {
            Text("扫描对方的 MaiChat 好友二维码")
                .foregroundStyle(RemoteIMStyle.textSecondary)
            ZStack {
                RoundedRectangle(cornerRadius: 18)
                    .fill(Color(red: 0.08, green: 0.12, blue: 0.19))
                if cameraAuthorized {
                    MaiChatQRCodeCameraView(
                        isActive: scenePhase == .active && errorMessage == nil,
                        onCode: { text in
                            guard let userID = MaiChatFriendLink.userID(from: text) else {
                                errorMessage = "这不是 MaiChat 好友二维码"
                                return
                            }
                            onUserID(userID)
                        },
                        onUnavailable: { errorMessage = "相机不可用" }
                    )
                    .id(scanGeneration)
                    .clipShape(RoundedRectangle(cornerRadius: 18))
                } else if cameraDenied {
                    Text("请在系统设置中允许 MaiChat 使用相机")
                        .foregroundStyle(.white)
                } else {
                    ProgressView("正在打开相机").tint(.white)
                }
                RoundedRectangle(cornerRadius: 14)
                    .strokeBorder(.white.opacity(0.8), lineWidth: 3)
                    .frame(width: 220, height: 220)
                    .allowsHitTesting(false)
            }
            .frame(height: 340)
            if let errorMessage {
                Text(errorMessage).foregroundStyle(.red)
                Button("重新扫描") {
                    self.errorMessage = nil
                    scanGeneration += 1
                }
                .buttonStyle(.bordered)
            }
            if cameraDenied {
                Link("打开系统设置", destination: URL(string: UIApplication.openSettingsURLString)!)
            }
            Spacer(minLength: 0)
        }
        .padding(20)
        .navigationTitle("扫一扫")
        .navigationBarTitleDisplayMode(.inline)
        .task {
            switch AVCaptureDevice.authorizationStatus(for: .video) {
            case .authorized: cameraAuthorized = true
            case .notDetermined:
                cameraAuthorized = await AVCaptureDevice.requestAccess(for: .video)
                cameraDenied = !cameraAuthorized
            default: cameraDenied = true
            }
        }
    }
}

private struct ContactList: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Binding var selectedTab: AppTab
    @Binding var activeContact: RemoteIMContact?
    @Binding var movingContact: RemoteIMContact?
    @Binding var groupActionName: String?
    @State private var collapsedGroups = Set<String>()
    @State private var profileContact: RemoteIMContact?
    @State private var profilePresented = false

    var body: some View {
        VStack(spacing: 0) {
            List {
                if !appState.pendingFriendRequests.isEmpty {
                    Section("好友申请") {
                        ForEach(appState.pendingFriendRequests) { request in
                            HStack(spacing: 12) {
                                RemoteIMContactAvatar(
                                    contact: RemoteIMContact(
                                        userID: request.userID,
                                        displayName: request.displayName,
                                        avatarURL: request.avatarURL
                                    ), isSelected: false,
                                    presenceStatus: .unknown, size: 38
                                )
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(request.displayName).font(.subheadline.weight(.semibold))
                                    Text(request.message.isEmpty ? "请求添加你为好友" : request.message)
                                        .font(.caption)
                                        .foregroundStyle(RemoteIMStyle.textSecondary)
                                }
                                Spacer()
                                Button("同意") {
                                    Task { await appState.acceptFriendRequest(userID: request.userID) }
                                }
                                .buttonStyle(.borderedProminent)
                                Button("拒绝") {
                                    Task { await appState.refuseFriendRequest(userID: request.userID) }
                                }
                                .buttonStyle(.bordered)
                            }
                        }
                    }
                }
                if visibleContacts.isEmpty && appState.chatState.contactGroups.isEmpty {
                    EmptyContacts()
                        .padding(.top, 76)
                        .listRowInsets(EdgeInsets())
                        .listRowSeparator(.hidden)
                        .listRowBackground(RemoteIMStyle.panelBackground)
                } else {
                    ForEach(contactListItems) { item in
                        switch item {
                        case let .group(name, memberCount):
                            groupHeader(name: name, memberCount: memberCount)
                        case let .contact(contact, indented):
                            contactButton(contact, grouped: indented)
                        }
                    }
                }
            }
            .listStyle(.plain)
            .refreshable {
                await appState.refreshAcceptedFriends()
                await appState.refreshFriendRequests()
            }
            .scrollContentBackground(.hidden)
            .scrollDismissesKeyboard(.immediately)
            .background(RemoteIMStyle.panelBackground)
        }
        .background(RemoteIMStyle.panelBackground)
        .navigationDestination(isPresented: $profilePresented) {
            if let contact = profileContact {
                FriendProfileView(userID: contact.userID, onMessage: {
                    let current = appState.chatState.contacts.first {
                        $0.userID == contact.userID
                    } ?? contact
                    appState.selectContact(current)
                    activeContact = current
                    selectedTab = .messages
                }, onDeleted: {
                    if activeContact?.userID == contact.userID { activeContact = nil }
                })
            }
        }
    }

    private var contactListItems: [RemoteIMContactListItem] {
        RemoteIMContactGroupDisplayPolicy.items(
            groups: appState.chatState.contactGroups,
            contacts: visibleContacts,
            collapsedGroupNames: collapsedGroups,
            query: ""
        )
    }

    private var visibleContacts: [RemoteIMContact] {
        appState.chatState.contacts.filter {
            appState.acceptedFriendIMIDs.contains($0.userID)
        }
    }

    private func groupHeader(name: String, memberCount: Int) -> some View {
        let isExpanded = !collapsedGroups.contains(name)
        return Button {
            if collapsedGroups.contains(name) {
                collapsedGroups.remove(name)
            } else {
                collapsedGroups.insert(name)
            }
        } label: {
            HStack(spacing: 8) {
                Image(systemName: isExpanded ? "chevron.down" : "chevron.right")
                    .font(.system(size: 11, weight: .bold))
                Text(name)
                    .font(.system(size: 14, weight: .semibold))
                Spacer()
                Text("\(memberCount)")
                    .font(.system(size: 12))
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
            .foregroundStyle(RemoteIMStyle.textPrimary)
        }
        .buttonStyle(.plain)
        .listRowInsets(EdgeInsets(top: 7, leading: 16, bottom: 7, trailing: 16))
        .listRowSeparator(.hidden)
        .listRowBackground(RemoteIMStyle.panelBackground)
        .onLongPressGesture(minimumDuration: 0.42) {
            groupActionName = name
        }
        .accessibilityAction(named: "分组操作") {
            groupActionName = name
        }
    }

    private func contactButton(_ contact: RemoteIMContact, grouped: Bool) -> some View {
        Button {
            profileContact = contact
            profilePresented = true
        } label: {
            VStack(alignment: .leading, spacing: 1) {
                ContactRow(
                    contact: contact,
                    accountID: appState.maiChatAccountId(forIMUserID: contact.userID),
                    selected: contact.userID == appState.chatState.selectedPeerID,
                    presenceStatus: appState.presenceStatus(for: contact)
                )
                if appState.pendingFriendSyncIMIDs.contains(contact.userID) {
                    Text("聊天服务同步中")
                        .font(.caption2)
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                        .padding(.leading, 50)
                }
            }
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("friend-profile-\(contact.userID)")
        .listRowInsets(EdgeInsets(
            top: 5,
            leading: grouped ? 30 : 16,
            bottom: 5,
            trailing: 16
        ))
        .listRowSeparator(.hidden)
        .listRowBackground(RemoteIMStyle.panelBackground)
        .onLongPressGesture(minimumDuration: 0.42) {
            movingContact = contact
        }
        .accessibilityAction(named: "移动到分组") {
            movingContact = contact
        }
        .swipeActions(edge: .trailing, allowsFullSwipe: true) {
            Button(role: .destructive) {
                Task {
                    if await appState.deleteContact(contact),
                       activeContact?.userID == contact.userID
                    {
                        activeContact = nil
                    }
                }
            } label: {
                Label("删除", systemImage: "trash")
            }
        }
    }

}

private struct FriendProfileView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.dismiss) private var dismiss
    let userID: String
    let onMessage: () -> Void
    let onDeleted: () -> Void
    @State private var showsDeleteConfirmation = false
    @State private var showsBlockConfirmation = false
    @State private var showsReport = false
    @State private var isDeleting = false
    @State private var isBlocking = false

    var body: some View {
        let profile = appState.profile(for: userID)
        let accountID = appState.maiChatAccountId(forIMUserID: userID)
        ScrollView {
            VStack(spacing: 24) {
                RemoteIMContactAvatar(
                    contact: RemoteIMContact(userID: userID,
                        displayName: profile.displayName, avatarURL: profile.avatarURL),
                    isSelected: false, presenceStatus: .unknown, size: 88
                )
                Text(profile.displayName)
                    .font(.title2.weight(.semibold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .multilineTextAlignment(.center)
                    .accessibilityIdentifier("friend-profile-nickname")
                VStack(alignment: .leading, spacing: 12) {
                    Text("MaiChat ID")
                        .font(.caption)
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                    HStack {
                        Text(accountID.isEmpty ? "正在读取账号" : accountID)
                            .font(.subheadline.monospaced())
                            .textSelection(.enabled)
                            .accessibilityIdentifier("friend-profile-account-id")
                        Spacer(minLength: 8)
                        if !accountID.isEmpty {
                            Button("复制") { UIPasteboard.general.string = accountID }
                        }
                    }
                }
                .padding(18)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(RemoteIMStyle.panelBackground,
                    in: RoundedRectangle(cornerRadius: 14, style: .continuous))

                Button(action: onMessage) {
                    Text("发消息").frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .accessibilityIdentifier("friend-profile-message")

                Button("删除好友", role: .destructive) {
                    showsDeleteConfirmation = true
                }
                .disabled(isDeleting)
                .accessibilityIdentifier("friend-profile-delete")

                Button("举报此用户") { showsReport = true }
                    .accessibilityIdentifier("friend-profile-report")
                if appState.blockedIMUserIDs.contains(userID) {
                    Button("解除屏蔽") {
                        Task {
                            isBlocking = true
                            _ = await appState.unblockUser(userID)
                            isBlocking = false
                        }
                    }
                    .disabled(isBlocking)
                } else {
                    Button("屏蔽此用户", role: .destructive) {
                        showsBlockConfirmation = true
                    }
                    .disabled(isBlocking)
                    .accessibilityIdentifier("friend-profile-block")
                }
            }
            .padding(24)
        }
        .background(RemoteIMStyle.pageBackground.ignoresSafeArea())
        .navigationTitle("好友资料")
        .navigationBarTitleDisplayMode(.inline)
        .confirmationDialog("确定删除这位好友？", isPresented: $showsDeleteConfirmation) {
            Button("删除好友", role: .destructive) {
                isDeleting = true
                Task {
                    let deleted = await appState.deleteContact(userID: userID)
                    isDeleting = false
                    if deleted { onDeleted(); dismiss() }
                }
            }
        } message: {
            Text("删除后不再显示在通讯录，已有聊天记录会保留。")
        }
        .confirmationDialog("屏蔽后将不再收到对方的 IM 消息", isPresented: $showsBlockConfirmation) {
            Button("屏蔽此用户", role: .destructive) {
                Task {
                    isBlocking = true
                    _ = await appState.blockUser(userID)
                    isBlocking = false
                }
            }
        }
        .sheet(isPresented: $showsReport) {
            NavigationStack {
                SafetyReportView(targetIMUserID: userID, messageID: nil, excerpt: nil)
            }
        }
        .task {
            await appState.refreshAcceptedFriends()
            await appState.refreshBlockedUsers()
        }
    }
}

struct SafetyReportView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.dismiss) private var dismiss
    let targetIMUserID: String
    let messageID: String?
    let excerpt: String?
    @State private var reason = "harassment"
    @State private var details = ""
    @State private var isSending = false
    @State private var receipt: MaiChatSafetyReportReceipt?
    @State private var errorMessage: String?

    var body: some View {
        Form {
            if let receipt {
                Section {
                    Label("举报已提交", systemImage: "checkmark.circle.fill")
                    Text("编号：\(receipt.reportId)").textSelection(.enabled)
                    Text("可在“设置 → 我的举报”查看处理状态。")
                    Button("完成") { dismiss() }
                }
            } else {
                Section("举报原因") {
                    Picker("原因", selection: $reason) {
                        Text("骚扰辱骂").tag("harassment")
                        Text("垃圾信息").tag("spam")
                        Text("色情内容").tag("sexual")
                        Text("暴力威胁").tag("violence")
                        Text("其他").tag("other")
                    }
                }
                if let excerpt, !excerpt.isEmpty {
                    Section("将提交的消息文字") {
                        Text(String(excerpt.prefix(200)))
                            .font(.footnote)
                            .textSelection(.enabled)
                    }
                }
                Section("补充说明") {
                    TextField("可以补充情况（选填）", text: $details, axis: .vertical)
                        .lineLimit(3 ... 5)
                }
                if let errorMessage { Text(errorMessage).foregroundStyle(.red) }
                Section {
                    Button(isSending ? "提交中…" : "提交举报") {
                        Task { await submit() }
                    }
                    .disabled(isSending)
                    .accessibilityIdentifier("safety-report-submit")
                } footer: {
                    Text("举报会发送所选消息的文字摘要、消息编号和补充说明；不会自动上传附件。")
                }
            }
        }
        .navigationTitle("举报")
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button("关闭") { dismiss() }
            }
        }
    }

    private func submit() async {
        isSending = true
        defer { isSending = false }
        do {
            receipt = try await appState.submitSafetyReport(
                targetIMUserID: targetIMUserID, messageID: messageID,
                excerpt: excerpt, reason: reason, details: details
            )
            errorMessage = nil
        } catch { errorMessage = error.localizedDescription }
    }
}

struct ContactGroupActionDialog: View {
    let groupName: String
    let dismiss: () -> Void
    let rename: () -> Void
    let delete: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.28)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture(perform: dismiss)

            VStack(alignment: .leading, spacing: 12) {
                Text(groupName)
                    .font(.system(size: 18, weight: .bold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .lineLimit(1)

                groupActionButton(
                    title: "重命名分组",
                    systemImage: "pencil",
                    color: RemoteIMStyle.blue,
                    destructive: false,
                    action: rename
                )
                groupActionButton(
                    title: "删除分组",
                    systemImage: "trash",
                    color: .red,
                    destructive: true,
                    action: delete
                )
            }
            .padding(14)
            .frame(maxWidth: 330)
            .background(
                RemoteIMStyle.panelBackground,
                in: RoundedRectangle(cornerRadius: 18, style: .continuous)
            )
            .overlay {
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .stroke(RemoteIMStyle.border, lineWidth: 1)
            }
            .shadow(color: Color.black.opacity(0.16), radius: 24, y: 10)
            .padding(.horizontal, 28)
        }
        .accessibilityElement(children: .contain)
        .accessibilityAction(.escape, dismiss)
    }

    private func groupActionButton(
        title: String,
        systemImage: String,
        color: Color,
        destructive: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            HStack(spacing: 13) {
                Image(systemName: systemImage)
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(color)
                    .frame(width: 30, height: 30)
                    .background(color.opacity(0.1), in: Circle())
                Text(title)
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(destructive ? Color.red : RemoteIMStyle.textPrimary)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 14)
            .frame(height: 52)
            .background(
                RemoteIMStyle.pageBackground,
                in: RoundedRectangle(cornerRadius: 12, style: .continuous)
            )
        }
        .buttonStyle(.plain)
    }
}

struct RenameContactGroupDialog: View {
    let originalName: String
    @Binding var name: String
    let cancel: () -> Void
    let save: () -> Void
    @FocusState private var isFocused: Bool

    var body: some View {
        ZStack {
            Color.black.opacity(0.28)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture(perform: cancel)

            VStack(alignment: .leading, spacing: 16) {
                Text("重命名分组")
                    .font(.system(size: 20, weight: .bold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                TextField(originalName, text: $name)
                    .focused($isFocused)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .padding(.horizontal, 14)
                    .frame(height: 48)
                    .background(
                        RemoteIMStyle.pageBackground,
                        in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                    )
                    .overlay {
                        RoundedRectangle(cornerRadius: 12, style: .continuous)
                            .stroke(RemoteIMStyle.blue, lineWidth: 1.5)
                    }

                HStack(spacing: 12) {
                    dialogButton(title: "取消", primary: false, action: cancel)
                    dialogButton(title: "保存", primary: true, action: save)
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
        .onAppear {
            DispatchQueue.main.async { isFocused = true }
        }
        .accessibilityAction(.escape, cancel)
    }

    private func dialogButton(
        title: String,
        primary: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(title, action: action)
            .buttonStyle(.plain)
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(primary ? Color.white : RemoteIMStyle.textPrimary)
            .frame(maxWidth: .infinity)
            .frame(height: 46)
            .background(
                primary ? RemoteIMStyle.blue : RemoteIMStyle.pageBackground,
                in: RoundedRectangle(cornerRadius: 12, style: .continuous)
            )
    }
}

struct DeleteContactGroupDialog: View {
    let groupName: String
    let memberCount: Int
    let cancel: () -> Void
    let delete: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.28)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture(perform: cancel)

            VStack(alignment: .leading, spacing: 16) {
                Text("删除分组？")
                    .font(.system(size: 20, weight: .bold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                Text(deleteMessage)
                    .font(.system(size: 14))
                    .foregroundStyle(RemoteIMStyle.textSecondary)

                HStack(spacing: 12) {
                    dialogButton(title: "取消", destructive: false, action: cancel)
                    dialogButton(title: "删除", destructive: true, action: delete)
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

    private var deleteMessage: String {
        memberCount == 0
            ? "“\(groupName)”是空分组，删除后不影响联系人。"
            : "删除“\(groupName)”后，\(memberCount) 位联系人会回到未分组。"
    }

    private func dialogButton(
        title: String,
        destructive: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(title, action: action)
            .buttonStyle(.plain)
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(destructive ? Color.red : RemoteIMStyle.textPrimary)
            .frame(maxWidth: .infinity)
            .frame(height: 46)
            .background(
                RemoteIMStyle.pageBackground,
                in: RoundedRectangle(cornerRadius: 12, style: .continuous)
            )
    }
}

struct MoveContactGroupDialog: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    let contact: RemoteIMContact
    let dismiss: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.28)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture(perform: dismiss)

            VStack(alignment: .leading, spacing: 16) {
                VStack(alignment: .leading, spacing: 5) {
                    Text("移动到分组")
                        .font(.system(size: 20, weight: .bold))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                    Text("为 \(contact.displayName) 选择一个分组")
                        .font(.system(size: 13))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                        .lineLimit(2)
                }

                ScrollView(showsIndicators: false) {
                    LazyVStack(spacing: 8) {
                        groupOption(
                            name: "",
                            title: contact.groupName.isEmpty ? "不在分组中" : "移出当前分组",
                            systemImage: "person"
                        )
                        ForEach(appState.chatState.contactGroups) { group in
                            groupOption(
                                name: group.name,
                                title: group.name,
                                systemImage: "folder"
                            )
                        }
                    }
                }
                .frame(height: optionListHeight)

                Button("取消", action: dismiss)
                    .buttonStyle(.plain)
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .frame(maxWidth: .infinity)
                    .frame(height: 42)
                    .background(
                        Color(.secondarySystemBackground),
                        in: RoundedRectangle(cornerRadius: 9, style: .continuous)
                    )
            }
            .padding(20)
            .frame(maxWidth: 360)
            .background(
                RemoteIMStyle.panelBackground,
                in: RoundedRectangle(cornerRadius: 16, style: .continuous)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .stroke(RemoteIMStyle.border, lineWidth: 1)
            )
            .shadow(color: Color.black.opacity(0.16), radius: 24, y: 10)
            .padding(.horizontal, 24)
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("移动 \(contact.displayName) 到分组")
    }

    private var optionListHeight: CGFloat {
        let count = appState.chatState.contactGroups.count + 1
        let contentHeight = count * 46 + max(count - 1, 0) * 8
        return min(CGFloat(contentHeight), 320)
    }

    private func groupOption(
        name: String,
        title: String,
        systemImage: String
    ) -> some View {
        let isCurrent = contact.groupName == name
        return Button {
            if isCurrent {
                dismiss()
                return
            }
            if appState.setContactGroup(userID: contact.userID, groupName: name) {
                dismiss()
            } else {
                appState.errorMessage = "联系人已不存在，无法移动到分组"
                dismiss()
            }
        } label: {
            HStack(spacing: 12) {
                Image(systemName: systemImage)
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(isCurrent ? RemoteIMStyle.blue : RemoteIMStyle.textSecondary)
                    .frame(width: 24)
                Text(title)
                    .font(.system(size: 15, weight: isCurrent ? .semibold : .regular))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .lineLimit(1)
                Spacer(minLength: 8)
                if isCurrent {
                    Image(systemName: "checkmark.circle.fill")
                        .font(.system(size: 18))
                        .foregroundStyle(RemoteIMStyle.blue)
                }
            }
            .padding(.horizontal, 14)
            .frame(height: 46)
            .background(
                isCurrent ? RemoteIMStyle.blueSoft : RemoteIMStyle.pageBackground,
                in: RoundedRectangle(cornerRadius: 10, style: .continuous)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .stroke(
                        isCurrent ? RemoteIMStyle.blue.opacity(0.45) : RemoteIMStyle.border,
                        lineWidth: 1
                    )
            )
        }
        .buttonStyle(.plain)
    }
}

private struct ContactRow: View {
    let contact: RemoteIMContact
    let accountID: String
    let selected: Bool
    let presenceStatus: RemoteIMPresenceStatus

    var body: some View {
        HStack(spacing: 12) {
            RemoteIMContactAvatar(
                contact: contact,
                isSelected: selected,
                presenceStatus: presenceStatus,
                size: 30
            )

            VStack(alignment: .leading, spacing: 3) {
                Text(contact.displayName)
                    .font(.system(size: 15, weight: .bold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Text(accountID)
                    .font(.system(size: 12, weight: .medium, design: .monospaced))
                    .foregroundStyle(RemoteIMStyle.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }

            Spacer()

            VStack(alignment: .trailing, spacing: 6) {
                RemoteIMPresenceBadge(status: presenceStatus)
                RelationBadge(text: contact.relation.displayName)
            }
        }
        .padding(12)
        .background(
            selected ? RemoteIMStyle.blueSoft : Color.white,
            in: RoundedRectangle(cornerRadius: 8, style: .continuous)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .stroke(selected ? Color(red: 0.216, green: 0.725, blue: 1.0) : RemoteIMStyle.border, lineWidth: 1)
        )
    }
}

private struct EmptyContacts: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "person.2")
                .font(.system(size: 28))
                .foregroundStyle(Color(red: 0.56, green: 0.59, blue: 0.64))
            Text("暂无联系人")
                .font(.system(size: 16, weight: .bold))
                .foregroundStyle(RemoteIMStyle.textPrimary)
            Text("添加好友账号后即可开始聊天。")
                .font(.system(size: 13))
                .foregroundStyle(RemoteIMStyle.textSecondary)
        }
        .frame(maxWidth: .infinity)
    }
}
