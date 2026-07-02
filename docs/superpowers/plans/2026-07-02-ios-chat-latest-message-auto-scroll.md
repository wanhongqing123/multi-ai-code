# iOS Chat Latest Message Auto Scroll Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the iOS Remote IM chat detail page automatically scroll to the latest message whenever a user enters an existing conversation.

**Architecture:** Add a tiny Core policy for selecting the latest message ID so the behavior has deterministic unit coverage. Reuse that policy from `MessageListView` and call the same local scroll helper from both `onAppear` and the existing `messages.count` change handler.

**Tech Stack:** Swift 6, SwiftUI, XCTest, iOS 16 deployment target, Xcode workspace under `ios/MultiAIIM`.

---

## File Structure

- Modify `ios/MultiAIIM/MultiAIIMCore/MasterChatState.swift`
  - Add `MessageListAutoScrollPolicy`, a pure helper responsible only for selecting the latest rendered message ID from a message array.
- Modify `ios/MultiAIIM/MultiAIIMTests/MasterChatStateTests.swift`
  - Add focused XCTest coverage for empty and populated message arrays.
- Modify `ios/MultiAIIM/MultiAIIM/ChatView.swift`
  - Add a local `scrollToLatestMessage(proxy:)` helper inside `MessageListView`.
  - Call it from `.onAppear` and from the existing `.onChange(of: messages.count)` path.

## Task 1: Add Auto-Scroll Target Policy

**Files:**
- Modify: `ios/MultiAIIM/MultiAIIMTests/MasterChatStateTests.swift`
- Modify: `ios/MultiAIIM/MultiAIIMCore/MasterChatState.swift`

- [ ] **Step 1: Write the failing unit tests**

Append these tests inside `final class MasterChatStateTests: XCTestCase` in `ios/MultiAIIM/MultiAIIMTests/MasterChatStateTests.swift`:

```swift
    func testMessageListAutoScrollPolicyTargetsLatestMessageID() {
        let firstID = UUID(uuidString: "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa")!
        let latestID = UUID(uuidString: "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb")!
        let first = RemoteIMMessage(
            id: firstID,
            fromUserID: "ios-master",
            toUserID: "mac-quark-pc",
            text: "第一条",
            direction: .outgoing,
            status: .sent,
            createdAt: Date(timeIntervalSince1970: 100)
        )
        let latest = RemoteIMMessage(
            id: latestID,
            fromUserID: "mac-quark-pc",
            toUserID: "ios-master",
            text: "最新回复",
            direction: .incoming,
            status: .received,
            createdAt: Date(timeIntervalSince1970: 120)
        )

        XCTAssertEqual(
            MessageListAutoScrollPolicy.latestMessageID(from: [first, latest]),
            latestID
        )
    }

    func testMessageListAutoScrollPolicyIgnoresEmptyMessages() {
        XCTAssertNil(MessageListAutoScrollPolicy.latestMessageID(from: []))
    }
```

- [ ] **Step 2: Run the tests and verify they fail for the missing policy**

Run:

```bash
xcodebuild test \
  -workspace ios/MultiAIIM/MultiAIIM.xcworkspace \
  -scheme MultiAIIMCoreTests \
  -destination 'platform=iOS Simulator,name=iPhone 16'
```

Expected: FAIL with a Swift compiler error similar to `cannot find 'MessageListAutoScrollPolicy' in scope`.

- [ ] **Step 3: Add the minimal policy implementation**

In `ios/MultiAIIM/MultiAIIMCore/MasterChatState.swift`, insert this enum after `ChatDetailSwipeBackPolicy` and before `RemoteIMCredential`:

```swift
public enum MessageListAutoScrollPolicy {
    public static func latestMessageID(from messages: [RemoteIMMessage]) -> RemoteIMMessage.ID? {
        messages.last?.id
    }
}
```

- [ ] **Step 4: Run the Core tests and verify they pass**

Run:

```bash
xcodebuild test \
  -workspace ios/MultiAIIM/MultiAIIM.xcworkspace \
  -scheme MultiAIIMCoreTests \
  -destination 'platform=iOS Simulator,name=iPhone 16'
```

Expected: PASS for `MultiAIIMCoreTests`, including the two new `MessageListAutoScrollPolicy` tests.

- [ ] **Step 5: Commit the policy and tests**

Run:

```bash
git add ios/MultiAIIM/MultiAIIMCore/MasterChatState.swift ios/MultiAIIM/MultiAIIMTests/MasterChatStateTests.swift
git commit -m "test: add ios chat auto-scroll target policy"
```

## Task 2: Wire Chat Detail Initial Auto-Scroll

**Files:**
- Modify: `ios/MultiAIIM/MultiAIIM/ChatView.swift`

- [ ] **Step 1: Replace the current inline count-change scroll logic**

In `MessageListView` in `ios/MultiAIIM/MultiAIIM/ChatView.swift`, replace the current trailing part:

```swift
            .onChange(of: messages.count) { _ in
                if let last = messages.last {
                    proxy.scrollTo(last.id, anchor: .bottom)
                }
            }
```

with:

```swift
            .onAppear {
                scrollToLatestMessage(proxy: proxy)
            }
            .onChange(of: messages.count) { _ in
                scrollToLatestMessage(proxy: proxy)
            }
```

Then add this helper inside `MessageListView`, after `var body: some View` and before the closing brace of the struct:

```swift
    private func scrollToLatestMessage(proxy: ScrollViewProxy) {
        guard let latestMessageID = MessageListAutoScrollPolicy.latestMessageID(from: messages) else {
            return
        }
        DispatchQueue.main.async {
            proxy.scrollTo(latestMessageID, anchor: .bottom)
        }
    }
```

- [ ] **Step 2: Run the Core tests to catch shared model regressions**

Run:

```bash
xcodebuild test \
  -workspace ios/MultiAIIM/MultiAIIM.xcworkspace \
  -scheme MultiAIIMCoreTests \
  -destination 'platform=iOS Simulator,name=iPhone 16'
```

Expected: PASS.

- [ ] **Step 3: Build the iOS app**

Run:

```bash
xcodebuild build \
  -workspace ios/MultiAIIM/MultiAIIM.xcworkspace \
  -scheme MultiAIIM \
  -destination 'platform=iOS Simulator,name=iPhone 16'
```

Expected: BUILD SUCCEEDED.

- [ ] **Step 4: Manual verification**

Use an iOS Simulator or device with a contact that already has multiple persisted messages:

1. Open the iOS app.
2. Enter the `消息` tab.
3. Tap the populated contact.
4. Confirm the chat detail view lands on the latest message at the bottom.
5. Send or receive one more message and confirm the view still scrolls to that new latest message.
6. Open an empty contact and confirm the existing empty state still appears.

- [ ] **Step 5: Commit the SwiftUI wiring**

Run:

```bash
git add ios/MultiAIIM/MultiAIIM/ChatView.swift
git commit -m "fix: scroll ios chat to latest message on entry"
```
