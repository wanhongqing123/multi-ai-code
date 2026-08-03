import MaiChatCore
import SwiftUI
import UIKit

struct RemoteDesktopView: View {
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        RemoteDesktopContent(
            session: appState.remoteDesktop,
            appState: appState
        )
    }
}

private struct RemoteDesktopContent: View {
    @ObservedObject var session: RemoteDesktopSession
    let appState: RemoteIMAppState

    var body: some View {
        VStack(spacing: 0) {
            header
            if session.state.isActive {
                activeSession
            } else {
                deviceList
            }
        }
        .background(RemoteIMStyle.pageBackground.ignoresSafeArea())
        .onDisappear {
            guard session.state.isActive else { return }
            Task { await appState.stopRemoteDesktopView() }
        }
    }

    private var header: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 3) {
                Text("远程查看")
                    .font(.system(size: 21, weight: .bold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                Text(session.state.statusText)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(statusColor)
                    .lineLimit(1)
            }

            Spacer(minLength: 8)

            if session.state.isActive {
                Button {
                    Task { await appState.stopRemoteDesktopView() }
                } label: {
                    Image(systemName: "xmark")
                        .font(.system(size: 15, weight: .bold))
                        .frame(width: 36, height: 36)
                }
                .buttonStyle(.plain)
                .foregroundStyle(Color.white)
                .background(Color.red, in: Circle())
                .accessibilityLabel("停止远程查看")
            }
        }
        .padding(.horizontal, 16)
        .padding(.top, 8)
        .padding(.bottom, 10)
        .background(RemoteIMStyle.panelBackground)
        .overlay(alignment: .bottom) {
            Divider().background(RemoteIMStyle.border)
        }
    }

    private var deviceList: some View {
        Group {
            if appState.chatState.contacts.isEmpty {
                VStack(spacing: 12) {
                    Image(systemName: "display")
                        .font(.system(size: 34, weight: .medium))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                    Text("暂无可查看设备")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundStyle(RemoteIMStyle.textPrimary)
                    Text("请先在通讯录中添加对方账号")
                        .font(.system(size: 13))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(appState.chatState.contacts) { contact in
                            deviceRow(contact)
                            Divider()
                                .padding(.leading, 76)
                        }
                    }
                }
                .background(Color.white)
            }
        }
    }

    private func deviceRow(_ contact: RemoteIMContact) -> some View {
        HStack(spacing: 12) {
            RemoteIMContactAvatar(
                contact: contact,
                isSelected: false,
                presenceStatus: appState.presenceStatus(for: contact),
                size: 46
            )

            VStack(alignment: .leading, spacing: 4) {
                Text(contact.displayName)
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .lineLimit(1)
                HStack(spacing: 6) {
                    Text(contact.userID)
                        .font(.system(size: 12, weight: .medium, design: .monospaced))
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    RemoteIMPresenceBadge(status: appState.presenceStatus(for: contact))
                }
            }

            Spacer(minLength: 8)

            Button {
                Task { await appState.requestRemoteDesktopView(of: contact) }
            } label: {
                Image(systemName: "display")
                    .font(.system(size: 16, weight: .semibold))
                    .frame(width: 38, height: 38)
            }
            .buttonStyle(.plain)
            .foregroundStyle(appState.connectionState == .connected ? Color.white : RemoteIMStyle.textSecondary)
            .background(
                appState.connectionState == .connected
                    ? RemoteIMStyle.blue
                    : Color(red: 0.91, green: 0.925, blue: 0.945),
                in: RoundedRectangle(cornerRadius: 8, style: .continuous)
            )
            .disabled(appState.connectionState != .connected || !session.canStart)
            .accessibilityLabel("查看 \(contact.displayName) 的屏幕")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    private var activeSession: some View {
        ZStack {
            Color.black
            RemoteDesktopRenderSurface { view in
                session.bindRenderView(view)
            }

            if session.state != .viewing {
                VStack(spacing: 14) {
                    ProgressView()
                        .tint(.white)
                        .scaleEffect(1.15)
                    Text(session.state.statusText)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(Color.white.opacity(0.82))
                }
            }

            if let noticeText = session.noticeText {
                VStack {
                    Text(noticeText)
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 9)
                        .background(Color.orange.opacity(0.9), in: RoundedRectangle(cornerRadius: 8))
                        .padding(12)
                    Spacer()
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var statusColor: Color {
        if case .failed = session.state {
            return .red
        }
        return RemoteIMStyle.textSecondary
    }
}

private struct RemoteDesktopRenderSurface: UIViewRepresentable {
    let bind: (UIView?) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(bind: bind)
    }

    func makeUIView(context: Context) -> UIView {
        let view = UIView()
        view.backgroundColor = .black
        context.coordinator.bind(view)
        return view
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        context.coordinator.bind(uiView)
    }

    static func dismantleUIView(_ uiView: UIView, coordinator: Coordinator) {
        coordinator.bind(nil)
    }

    final class Coordinator {
        let bind: (UIView?) -> Void

        init(bind: @escaping (UIView?) -> Void) {
            self.bind = bind
        }
    }
}
