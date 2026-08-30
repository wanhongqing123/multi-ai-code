import MaiChatCore
import SwiftUI

struct ContactsView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Binding var selectedTab: AppTab
    @Binding var activeContact: RemoteIMContact?
    let showAddContact: () -> Void
    @State private var isCreatingGroup = false
    @State private var groupNameDraft = ""
    @State private var isBroadcasting = false

    var body: some View {
        ZStack {
            NavigationStack {
                ContactList(
                    selectedTab: $selectedTab,
                    activeContact: $activeContact,
                    showCreateGroup: {
                        groupNameDraft = ""
                        isCreatingGroup = true
                    },
                    showBroadcast: { isBroadcasting = true }
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
                .sheet(isPresented: $isBroadcasting) {
                    BroadcastComposeView()
                }
            }

            if isCreatingGroup {
                CreateContactGroupDialog(
                    isPresented: $isCreatingGroup,
                    groupName: $groupNameDraft
                )
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(10)
            }
        }
        .animation(.easeOut(duration: 0.18), value: isCreatingGroup)
    }
}

private struct CreateContactGroupDialog: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Binding var isPresented: Bool
    @Binding var groupName: String
    @FocusState private var isNameFocused: Bool

    var body: some View {
        ZStack {
            Color.black.opacity(0.28)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture(perform: cancel)

            VStack(alignment: .leading, spacing: 16) {
                VStack(alignment: .leading, spacing: 5) {
                    Text("新建分组")
                        .font(.system(size: 20, weight: .bold))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                    Text("新分组会按创建顺序显示，空分组也会保留。")
                        .font(.system(size: 13))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                        .fixedSize(horizontal: false, vertical: true)
                }

                TextField("分组名", text: $groupName)
                    .font(.system(size: 15))
                    .submitLabel(.done)
                    .focused($isNameFocused)
                    .padding(.horizontal, 14)
                    .frame(height: 44)
                    .background(
                        RemoteIMStyle.pageBackground,
                        in: RoundedRectangle(cornerRadius: 9, style: .continuous)
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 9, style: .continuous)
                            .stroke(
                                isNameFocused ? RemoteIMStyle.blue : RemoteIMStyle.border,
                                lineWidth: isNameFocused ? 1.5 : 1
                            )
                    )
                    .onSubmit(createGroup)

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

                    Button("新建", action: createGroup)
                        .buttonStyle(.plain)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(canCreate ? Color.white : RemoteIMStyle.textSecondary)
                        .frame(maxWidth: .infinity)
                        .frame(height: 42)
                        .background(
                            canCreate ? RemoteIMStyle.blue : Color(.secondarySystemBackground),
                            in: RoundedRectangle(cornerRadius: 9, style: .continuous)
                        )
                        .disabled(!canCreate)
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
            DispatchQueue.main.async { isNameFocused = true }
        }
    }

    private var canCreate: Bool {
        !groupName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private func createGroup() {
        guard canCreate else { return }
        if appState.createContactGroup(name: groupName) {
            isPresented = false
            groupName = ""
        } else {
            appState.errorMessage = "分组名不能为空，且不能与已有分组重名"
        }
    }

    private func cancel() {
        groupName = ""
        isPresented = false
    }
}

struct AddContactDialog: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Binding var isPresented: Bool
    @FocusState private var isAccountFocused: Bool

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
                    Text("请输入要添加的好友账号")
                        .font(.system(size: 13))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                }

                TextField("好友账号", text: $appState.newContactUserID)
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

                    Button("添加", action: addContact)
                        .buttonStyle(.plain)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(canAdd ? Color.white : RemoteIMStyle.textSecondary)
                        .frame(maxWidth: .infinity)
                        .frame(height: 42)
                        .background(
                            canAdd ? RemoteIMStyle.blue : Color(.secondarySystemBackground),
                            in: RoundedRectangle(cornerRadius: 9, style: .continuous)
                        )
                        .disabled(!canAdd)
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
    }

    private var canAdd: Bool {
        !appState.newContactUserID
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .isEmpty
    }

    private func addContact() {
        guard canAdd else { return }
        appState.addContact()
        if appState.newContactUserID.isEmpty {
            isPresented = false
        }
    }

    private func cancel() {
        appState.newContactUserID = ""
        isPresented = false
    }
}

private struct ContactList: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Binding var selectedTab: AppTab
    @Binding var activeContact: RemoteIMContact?
    let showCreateGroup: () -> Void
    let showBroadcast: () -> Void
    @State private var searchText = ""
    @State private var collapsedGroups = Set<String>()
    @State private var renamingGroup: String?
    @State private var renameDraft = ""
    @State private var deletingGroup: String?
    @State private var movingContact: RemoteIMContact?

    var body: some View {
        List {
            HStack(spacing: 10) {
                Button(action: showCreateGroup) {
                    Label("新建分组", systemImage: "folder.badge.plus")
                        .frame(maxWidth: .infinity)
                }
                Button(action: showBroadcast) {
                    Label("群发消息", systemImage: "paperplane")
                        .frame(maxWidth: .infinity)
                }
                .disabled(appState.chatState.contacts.isEmpty)
            }
            .buttonStyle(.bordered)
            .listRowInsets(EdgeInsets(top: 6, leading: 16, bottom: 8, trailing: 16))
            .listRowSeparator(.hidden)
            .listRowBackground(RemoteIMStyle.panelBackground)

            if appState.chatState.contacts.isEmpty && appState.chatState.contactGroups.isEmpty {
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
        .scrollContentBackground(.hidden)
        .background(RemoteIMStyle.panelBackground)
        .searchable(text: $searchText, prompt: "搜索联系人")
        .alert("重命名分组", isPresented: Binding(
            get: { renamingGroup != nil },
            set: { if !$0 { renamingGroup = nil } }
        )) {
            TextField("分组名", text: $renameDraft)
            Button("取消", role: .cancel) { renamingGroup = nil }
            Button("保存") {
                if let group = renamingGroup,
                   !appState.renameContactGroup(from: group, to: renameDraft)
                {
                    appState.errorMessage = "分组名不能为空，且不能与已有分组重名"
                }
                renamingGroup = nil
            }
        }
        .confirmationDialog(
            "删除分组？",
            isPresented: Binding(
                get: { deletingGroup != nil },
                set: { if !$0 { deletingGroup = nil } }
            ),
            titleVisibility: .visible
        ) {
            Button("删除分组", role: .destructive) {
                if let group = deletingGroup {
                    _ = appState.deleteContactGroup(name: group)
                    collapsedGroups.remove(group)
                }
                deletingGroup = nil
            }
            Button("取消", role: .cancel) { deletingGroup = nil }
        } message: {
            Text(deleteGroupMessage)
        }
        .overlay {
            if let movingContact {
                MoveContactGroupDialog(contact: movingContact) {
                    self.movingContact = nil
                }
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
            }
        }
        .animation(.easeOut(duration: 0.18), value: movingContact?.userID)
    }

    private var normalizedSearch: String {
        searchText.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    }

    private var contactListItems: [RemoteIMContactListItem] {
        RemoteIMContactGroupDisplayPolicy.items(
            groups: appState.chatState.contactGroups,
            contacts: appState.chatState.contacts,
            collapsedGroupNames: collapsedGroups,
            query: searchText
        )
    }

    private func groupHeader(name: String, memberCount: Int) -> some View {
        let isExpanded = !normalizedSearch.isEmpty || !collapsedGroups.contains(name)
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
        .contextMenu {
            Button("重命名分组") {
                renameDraft = name
                renamingGroup = name
            }
            Button("删除分组", role: .destructive) { deletingGroup = name }
        }
    }

    private func contactButton(_ contact: RemoteIMContact, grouped: Bool) -> some View {
        Button {
            appState.selectContact(contact)
            activeContact = contact
            selectedTab = .messages
        } label: {
            ContactRow(
                contact: contact,
                selected: contact.userID == appState.chatState.selectedPeerID,
                presenceStatus: appState.presenceStatus(for: contact)
            )
        }
        .buttonStyle(.plain)
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

    private var deleteGroupMessage: String {
        guard let group = deletingGroup else { return "" }
        let count = appState.chatState.contacts.filter { $0.groupName == group }.count
        return count == 0
            ? "这个分组是空的，删除后不影响任何联系人。"
            : "组里的 \(count) 位联系人会直接列在通讯录里，好友本身不会被删除。"
    }
}

private struct MoveContactGroupDialog: View {
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

                ScrollView {
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
        min(CGFloat(appState.chatState.contactGroups.count + 1) * 54, 320)
    }

    private func groupOption(
        name: String,
        title: String,
        systemImage: String
    ) -> some View {
        let isCurrent = contact.groupName == name
        return Button {
            if isCurrent || appState.setContactGroup(userID: contact.userID, groupName: name) {
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

private struct BroadcastComposeView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.dismiss) private var dismiss
    @State private var messageText = ""
    @State private var pickerState = RemoteIMBroadcastRecipientPickerState()
    @State private var isConfirming = false
    @State private var isSending = false
    @State private var resultMessage: String?

    var body: some View {
        NavigationStack {
            VStack(spacing: 12) {
                Text("勾选的每个人都会单独收到一条私聊消息。")
                    .font(.system(size: 13))
                    .foregroundStyle(RemoteIMStyle.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)

                TextField("筛选联系人", text: Binding(
                    get: { pickerState.filterText },
                    set: { pickerState.setFilterText($0) }
                ))
                    .textFieldStyle(.roundedBorder)

                List {
                    ForEach(recipientListItems) { item in
                        switch item {
                        case let .group(name, memberCount):
                            groupSelectionRow(name: name, memberCount: memberCount)
                        case let .contact(contact, indented):
                            recipientRow(contact, indented: indented)
                        }
                    }
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
                .background(RemoteIMStyle.pageBackground)

                TextEditor(text: $messageText)
                    .frame(minHeight: 88, maxHeight: 120)
                    .padding(8)
                    .background(
                        RemoteIMStyle.pageBackground,
                        in: RoundedRectangle(cornerRadius: 10, style: .continuous)
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 10, style: .continuous)
                            .stroke(RemoteIMStyle.border, lineWidth: 1)
                    )

                Button {
                    isConfirming = true
                } label: {
                    HStack {
                        if isSending { ProgressView().tint(.white) }
                        Text(pickerState.selectedUserIDs.isEmpty
                            ? "发送"
                            : "发送给 \(pickerState.selectedUserIDs.count) 人")
                    }
                    .font(.system(size: 15, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(maxWidth: .infinity)
                    .frame(height: 44)
                    .background(
                        canSend ? RemoteIMStyle.blue : Color.gray.opacity(0.4),
                        in: RoundedRectangle(cornerRadius: 10, style: .continuous)
                    )
                }
                .buttonStyle(.plain)
                .disabled(!canSend)
            }
            .padding(16)
            .background(RemoteIMStyle.panelBackground.ignoresSafeArea())
            .navigationTitle("群发消息")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("取消") { dismiss() }
                        .disabled(isSending)
                }
            }
            .alert("确认群发", isPresented: $isConfirming) {
                Button("取消", role: .cancel) {}
                Button("发送给 \(pickerState.selectedUserIDs.count) 人") { sendBroadcast() }
            } message: {
                Text("以下每个人会各收到一条相同的私聊消息：\n\n\(selectedNames.joined(separator: "、"))")
            }
            .alert("群发结果", isPresented: Binding(
                get: { resultMessage != nil },
                set: { if !$0 { resultMessage = nil } }
            )) {
                Button("知道了") {
                    let succeeded = resultMessage?.contains("都收到了") == true
                    resultMessage = nil
                    if succeeded { dismiss() }
                }
            } message: {
                Text(resultMessage ?? "")
            }
        }
    }

    private var canSend: Bool {
        !isSending && appState.connectionState == .connected
            && !pickerState.selectedUserIDs.isEmpty
            && !messageText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private var selectedContacts: [RemoteIMContact] {
        appState.chatState.contacts.filter { pickerState.selectedUserIDs.contains($0.userID) }
    }

    private var selectedNames: [String] { selectedContacts.map(\.displayName) }

    private var recipientListItems: [RemoteIMBroadcastRecipientListItem] {
        pickerState.visibleItems(
            groups: appState.chatState.contactGroups,
            contacts: appState.chatState.contacts
        )
    }

    private func groupSelectionRow(name: String, memberCount: Int) -> some View {
        let state = pickerState.groupState(
            groupName: name,
            contacts: appState.chatState.contacts
        )
        let symbol = state == .all ? "checkmark.square.fill"
            : state == .partial ? "minus.square.fill" : "square"
        return Button {
            pickerState.toggleGroup(
                groupName: name,
                contacts: appState.chatState.contacts
            )
        } label: {
            HStack {
                Image(systemName: symbol)
                    .foregroundStyle(RemoteIMStyle.blue)
                Text(name).font(.system(size: 14, weight: .semibold))
                Spacer()
                Text("\(memberCount)").foregroundStyle(RemoteIMStyle.textSecondary)
            }
        }
        .buttonStyle(.plain)
        .listRowSeparator(.hidden)
    }

    private func recipientRow(_ contact: RemoteIMContact, indented: Bool) -> some View {
        Button {
            pickerState.toggleContact(userID: contact.userID)
        } label: {
            HStack {
                Image(systemName: pickerState.selectedUserIDs.contains(contact.userID)
                    ? "checkmark.square.fill" : "square")
                    .foregroundStyle(RemoteIMStyle.blue)
                Text(contact.displayName)
                Spacer()
                Text(contact.userID)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
        }
        .buttonStyle(.plain)
        .listRowInsets(EdgeInsets(
            top: 7,
            leading: indented ? 30 : 16,
            bottom: 7,
            trailing: 16
        ))
        .listRowSeparator(.hidden)
    }

    private func sendBroadcast() {
        let recipients = selectedContacts.map(\.userID)
        let text = messageText.trimmingCharacters(in: .whitespacesAndNewlines)
        isSending = true
        Task {
            let result = await appState.broadcastText(to: recipients, text: text)
            isSending = false
            if result.failedUserIDs.isEmpty {
                resultMessage = "\(result.total) 个人都收到了。"
                return
            }
            let failedNames = appState.chatState.contacts
                .filter { result.failedUserIDs.contains($0.userID) }
                .map(\.displayName)
            resultMessage = "\(result.total) 个人里有 \(failedNames.count) 个没发出去：\n\n"
                + failedNames.joined(separator: "、")
                + "\n\n失败消息保留在各自会话里，可以单独重发。"
        }
    }
}

private struct ContactRow: View {
    let contact: RemoteIMContact
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
                Text(contact.userID)
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
