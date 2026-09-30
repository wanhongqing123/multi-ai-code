import MaiChatCore
import SwiftUI

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
            }
        }
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
    @Binding var movingContact: RemoteIMContact?
    @Binding var groupActionName: String?
    @State private var searchText = ""
    @State private var collapsedGroups = Set<String>()

    var body: some View {
        VStack(spacing: 0) {
            ContactSearchField(text: $searchText, placeholder: "搜索联系人")

            List {
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
            .scrollDismissesKeyboard(.immediately)
            .background(RemoteIMStyle.panelBackground)
        }
        .background(RemoteIMStyle.panelBackground)
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
        .onLongPressGesture(minimumDuration: 0.42) {
            groupActionName = name
        }
        .accessibilityAction(named: "分组操作") {
            groupActionName = name
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

private struct ContactSearchField: View {
    @Binding var text: String
    let placeholder: String
    var horizontalPadding: CGFloat = 16
    var verticalPadding: CGFloat = 10
    var usesSoftFill = false
    @FocusState private var isFocused: Bool

    var body: some View {
        HStack(spacing: 9) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(
                    isFocused || usesSoftFill ? RemoteIMStyle.blue : RemoteIMStyle.textSecondary
                )

            TextField(placeholder, text: $text)
                .font(.system(size: 15))
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .focused($isFocused)
                .submitLabel(.search)
                .onSubmit { isFocused = false }

            if !text.isEmpty {
                Button {
                    text = ""
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 16))
                        .foregroundStyle(RemoteIMStyle.textSecondary.opacity(0.72))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("清空搜索")
            }
        }
        .padding(.horizontal, 13)
        .frame(height: 44)
        .background(
            usesSoftFill ? RemoteIMStyle.blueSoft.opacity(0.58) : RemoteIMStyle.pageBackground,
            in: RoundedRectangle(cornerRadius: 12, style: .continuous)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .stroke(
                    usesSoftFill
                        ? Color.clear
                        : (isFocused ? RemoteIMStyle.blue : RemoteIMStyle.border),
                    lineWidth: usesSoftFill ? 0 : (isFocused ? 1.5 : 1)
                )
        )
        .padding(.horizontal, horizontalPadding)
        .padding(.vertical, verticalPadding)
        .background(RemoteIMStyle.panelBackground)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("remote-im-contact-search-field")
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
