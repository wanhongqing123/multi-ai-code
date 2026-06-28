# Remote IM Chat Design

## Goal

Remote IM should behave like a normal IM chat surface, with master/slave/friend as relationship and permission labels. It should not look like a node operations dashboard.

## Decisions

- The main Remote IM screen uses a two-column layout:
  - Left: conversation/contact list.
  - Right: selected chat.
- There is no persistent contact detail panel.
- The chat header may show the selected UserID, relation label, online state, and a small overflow menu.
- Contacts are identified by UserID only in the first version.
- Nicknames and remarks are not included in the first version.
- SDKAppID, SecretKey, UserSig mode, credential presets, and local UserID stay in Settings.
- Add-contact flow does not contain SDKAppID or SecretKey.

## Main UI

The Remote IM entry opens a regular IM-style panel.

Left column:

- Search input: search by UserID.
- Tabs: recent, friend, master, slave.
- Conversation rows show:
  - UserID.
  - Relationship label: friend, master, or slave.
  - Online/connection hint when available.
  - Last message preview.
  - Time/unread count when available.
- A `+` action opens add-contact.

Right column:

- Header shows selected UserID and relation label.
- Messages render as normal chat bubbles:
  - Local outgoing messages on the right.
  - Remote incoming messages on the left.
  - AICLI output appears as a message from the remote client and supports Markdown.
  - Low-value system acknowledgements remain hidden from the main timeline.
- Composer is a normal chat input.
- Slave mode disables manual sending, with a short placeholder explaining that slave mode receives tasks and returns AICLI output.

## Add Contact

The add-contact dialog contains only:

- Relationship type:
  - Friend.
  - Master.
  - Slave.
- UserID.
- Optional permission toggle when relation needs it:
  - For master relation on a slave client: allow this master to control current AICLI.
  - For slave relation on a master client: allow sending tasks to this slave.

The first version does not support nickname, remark, QR code, or invitation token.

## Settings

Remote IM Settings own all communication credentials and local identity:

- Enabled.
- SDKAppID.
- Credential preset.
- UserSig mode.
- SecretKey or UserSig endpoint.
- Local UserID.
- Local role: master or slave.

The settings page can also show raw relation lists for compatibility, but the target user workflow should be adding contacts from the IM surface.

## Relationship Rules

- Friend to friend: normal IM messages, not routed to AICLI.
- Master to slave: message is sent as a normal chat message, but the receiving slave routes it into AICLI.
- Slave to master: slave cannot manually initiate normal chat in the first version. AICLI output and system replies can be returned to the task sender.
- Master to master: normal peer IM messages.
- Slave to slave: blocked.

## Data Model

Introduce a contact concept instead of relying only on raw arrays in the UI:

```ts
export type RemoteImContactRelation = 'friend' | 'master' | 'slave'

export interface RemoteImContact {
  userId: string
  relation: RemoteImContactRelation
  enabled: boolean
  createdAt: number
  updatedAt: number
}
```

For compatibility with existing permission code:

- Contacts with `relation === 'master'` derive `masterUserIds`.
- Contacts with `relation === 'slave'` derive `slaveUserIds`.
- Friend contacts are allowed for normal peer chat but are not task controllers.

Existing `masterUserIds`, `slaveUserIds`, and `allowedUserIds` stay readable during migration.

## Message Grouping

Messages already have `fromUserId` and `toUserId`. Conversation grouping should compute the peer UserID:

- Incoming message: peer is `fromUserId`.
- Outgoing message: peer is `toUserId`.

The selected conversation filters messages by that peer UserID. This prevents messages from different slaves from mixing in one timeline.

## First Version Scope

Included:

- Two-column IM UI.
- Contact list grouped by relationship.
- Add friend/master/slave by UserID.
- Send to the selected peer instead of defaulting to the first configured peer.
- Filter chat history by selected peer.
- Keep credential configuration in Settings.

Not included:

- Nicknames or remarks.
- QR code or invitation-based adding.
- Group chat.
- Broadcast to multiple slaves.
- Tencent online status API integration.
- Full account/profile system.

## Preview

Design preview:

- `docs/design/remote-im-chat-simple-preview.png`
- `docs/design/remote-im-chat-simple-preview.svg`
