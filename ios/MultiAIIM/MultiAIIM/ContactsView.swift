import MultiAIIMCore
import SwiftUI

struct ContactsView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Binding var selectedTab: AppTab
    @Binding var activeContact: RemoteIMContact?

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                Header()
                AddContactBox()
                ContactList(selectedTab: $selectedTab, activeContact: $activeContact)
            }
            .background(RemoteIMStyle.pageBackground.ignoresSafeArea())
            .toolbar(.hidden, for: .navigationBar)
        }
    }
}

private struct Header: View {
    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text("通讯录")
                    .font(.system(size: 21, weight: .bold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                Text("可信好友")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
            Spacer()
        }
        .padding(.horizontal, 16)
        .padding(.top, 8)
        .padding(.bottom, 10)
        .background(RemoteIMStyle.panelBackground)
        .overlay(alignment: .bottom) {
            Divider().background(RemoteIMStyle.border)
        }
    }
}

private struct AddContactBox: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        VStack(spacing: 10) {
            HStack(spacing: 8) {
                TextField("添加好友账号", text: $appState.newContactUserID)
                    .font(.system(size: 15))
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .padding(.horizontal, 12)
                    .frame(height: 40)
                    .background(Color.white, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
                    .overlay(
                        RoundedRectangle(cornerRadius: 8, style: .continuous)
                            .stroke(RemoteIMStyle.border, lineWidth: 1)
                    )

                Button {
                    appState.addContact()
                } label: {
                    Image(systemName: "plus")
                        .font(.system(size: 18, weight: .bold))
                        .frame(width: 46, height: 40)
                }
                .buttonStyle(.plain)
                .foregroundStyle(.white)
                .background(RemoteIMStyle.blue, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
            }
        }
        .padding(16)
        .background(RemoteIMStyle.panelBackground)
        .overlay(alignment: .bottom) {
            Divider().background(RemoteIMStyle.border)
        }
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
                            selected: contact.userID == appState.chatState.selectedPeerID
                        )
                    }
                    .buttonStyle(.plain)
                    .listRowInsets(EdgeInsets(top: 5, leading: 16, bottom: 5, trailing: 16))
                    .listRowSeparator(.hidden)
                    .listRowBackground(RemoteIMStyle.panelBackground)
                    .swipeActions(edge: .trailing, allowsFullSwipe: true) {
                        Button(role: .destructive) {
                            appState.deleteContact(contact)
                            if activeContact?.userID == contact.userID {
                                activeContact = nil
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

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "desktopcomputer")
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(selected ? Color(red: 0.035, green: 0.376, blue: 0.667) : RemoteIMStyle.textSecondary)
                .frame(width: 30, height: 30)
                .background(
                    selected ? RemoteIMStyle.blueSoft : Color(red: 0.953, green: 0.961, blue: 0.973),
                    in: RoundedRectangle(cornerRadius: 8, style: .continuous)
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

            RelationBadge(text: contact.relation.displayName)
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
