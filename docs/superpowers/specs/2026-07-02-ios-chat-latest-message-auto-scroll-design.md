# iOS Chat Latest Message Auto Scroll Design

## Background

iOS Remote IM already persists local chat history and shows each contact's conversation in `ChatView.swift`.
The message list currently scrolls to the latest message only when `messages.count` changes. When a user enters an existing conversation with many historical messages, the count has not changed, so the view opens near the top and the user must manually scroll to the newest message.

## Goal

When the user taps a contact from the iOS conversation list and enters the chat detail page, the message list should automatically jump to the latest message.

The existing behavior where newly sent or received messages scroll the visible chat to the latest message should remain unchanged.

## Non-Goals

- Do not change message persistence, loading, or ordering.
- Do not change conversation selection behavior.
- Do not add unread markers, pagination, or "jump to latest" buttons.
- Do not redesign the chat UI.

## Current State

`MasterChatState` normalizes persisted messages by `createdAt` ascending order. `MessageListView` renders the selected peer's messages in that order and assigns each bubble `.id(message.id)`.

`MessageListView` currently has this behavior:

- It wraps the scroll view in `ScrollViewReader`.
- It listens for `messages.count` changes.
- On count change, it scrolls to `messages.last` with `anchor: .bottom`.

This misses the initial appearance path for an already populated conversation.

## Proposed Design

Add a small local helper inside `MessageListView`:

```swift
private func scrollToLatestMessage(proxy: ScrollViewProxy) {
    guard let last = messages.last else { return }
    DispatchQueue.main.async {
        proxy.scrollTo(last.id, anchor: .bottom)
    }
}
```

Use the helper from two view lifecycle events:

- `.onAppear`: handles entering a contact's chat page with existing history.
- `.onChange(of: messages.count)`: preserves automatic scrolling when new messages are sent or received while already in the chat page.

Scheduling the scroll on the next main-queue turn gives SwiftUI a chance to lay out the `LazyVStack` and attach row IDs before `scrollTo` runs.

## Data Flow

1. User taps a contact in `ConversationListView`.
2. `appState.selectContact(contact)` updates `selectedPeerID`.
3. `ChatDetailView` renders `MessageListView(messages: appState.chatState.messages(with: contact.userID), ...)`.
4. `MessageListView.onAppear` scrolls to the last rendered message ID.
5. Later message count changes continue to scroll to the last message through the existing update path.

## Error Handling

If the conversation has no messages, the helper returns without doing anything and the existing empty state remains visible.

If SwiftUI rebuilds the view during navigation, repeated scroll calls are harmless because they target the same last message ID.

## Testing

Add a focused Core test around a small pure policy helper only if needed to keep the scroll target logic testable outside SwiftUI. The main verification should include:

- `MultiAIIMCoreTests` unit tests still pass.
- iOS app builds successfully.
- Manual check: enter a contact with existing history and confirm the view lands on the newest message.

## Acceptance Criteria

- Entering a populated chat from the conversation list automatically positions the latest message at the bottom.
- Receiving or sending a message while already in the chat still scrolls to the new latest message.
- Empty conversations still show the existing empty state.
- No unrelated Remote IM behavior changes.
