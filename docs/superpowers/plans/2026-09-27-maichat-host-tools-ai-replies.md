# MaiChat Host Tools and One-shot Reply Suggestions

## Goal

Expose MaiChat-specific capabilities to MaiAgent without coupling MaiAgent to Qt, and add one-shot AI reply suggestions to normal friend chats. Suggestions provide natural, casual, and professional variants, fill the composer when selected, and never send automatically.

## Architecture

- Keep `MaiAgent/` unchanged. Its existing `MaiToolRegistry` is the extension point.
- Let `AgentController` accept an optional host tool registrar. Built-in tools are registered first, then the host registrar adds MaiChat tools.
- Implement MaiChat tools in the Desktop application layer, where contacts, messages, and sending live. Tool execution marshals all Qt state access to the application thread.
- Add explicit-peer send operations to `RemoteIMApplication`; tools must not mutate the selected conversation to send.
- Generate reply suggestions through a transient model request with no tools and no persistent conversation. The request contains only recent messages for the currently selected peer.
- Bind every result to account, peer, and latest-message identity. Discard stale results after account, peer, or conversation changes.

## Phase 1: Host tool registration

1. Add tests proving a host registrar contributes tool specs and that write tools require approval.
2. Add an optional registrar to `AgentController` constructors.
3. Add `MaiChatHostTools` with read tools for contacts, conversations, messages, search, and unread summary.
4. Add approved write tools for sending text, replying, and attachments.
5. Register tools from `MainWindow` using `RemoteIMApplication` as the host bridge.

## Phase 2: Reply suggestion service

1. Add parsing and stale-result tests for exactly three labelled suggestions.
2. Add a transient `ReplySuggestionController` that issues one model request without tools or persistent storage.
3. Format the recent conversation as UTF-8 and request strict JSON for `natural`, `casual`, and `professional` replies.
4. Parse defensively and surface retryable errors without modifying the draft.

## Phase 3: Desktop UI

1. Add `AI 回复` at the lower-left of the normal IM composer.
2. Render three suggestion chips above the composer with refresh and close controls.
3. Insert the selected suggestion into `messageEditor`; do not call any send method.
4. Invalidate suggestions when the peer or latest message changes.

## Phase 4: Mobile UI

1. Add `AI 帮我回复` to the existing iOS and Android composer menus.
2. Reuse the transient request contract and render the three choices above the composer.
3. Selecting a choice only replaces the current draft.

## Verification

- MaiAgent unit tests.
- Desktop focused controller/tool/layout tests, then all Desktop tests.
- iOS presentation tests and Release build.
- Android JVM tests and Debug/Release builds for both ABIs.
- Inspect the final diff, commit directly to `main`, rebase on the current remote tip, and push.
