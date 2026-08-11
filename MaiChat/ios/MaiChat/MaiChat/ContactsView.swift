import MaiChatCore
import SwiftUI

struct ContactsView: View {
    @Binding var selectedTab: AppTab
    @Binding var activeContact: RemoteIMContact?
    let showAddContact: () -> Void

    var body: some View {
        NavigationStack {
            ContactList(selectedTab: $selectedTab, activeContact: $activeContact)
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

    var body: some View {
        List {
            if appState.chatState.contacts.isEmpty {
                EmptyContacts()
                    .padding(.top, 76)
                    .listRowInsets(EdgeInsets())
                    .listRowSeparator(.hidden)
                    .listRowBackground(RemoteIMStyle.panelBackground)
            } else {
                ForEach(appState.chatState.contacts) { contact in
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
                    .listRowInsets(EdgeInsets(top: 5, leading: 16, bottom: 5, trailing: 16))
                    .listRowSeparator(.hidden)
                    .listRowBackground(RemoteIMStyle.panelBackground)
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
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
        .background(RemoteIMStyle.panelBackground)
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
