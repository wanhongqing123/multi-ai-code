# iOS App Store submission gate (2026-10-20)

## Current product scope

- MaiChat has one-to-one friend chat with text, images, videos, voice and files.
- Creation Center can generate and inspect media, but has no public video feed.
- MaiChat pays the model costs for ordinary users. The app does not sell credits.
- Tencent IM hosts message delivery. Tencent IM cloud content moderation is not enabled
  and must not be enabled for this submission. Reports are handled by MaiChat staff.

## App Review Guideline 1.2

The first-party report, block and contact flows are present in source. A reviewer
can open a friend's profile to report or block the account and can report an
incoming message. The backend records reports and has an internal resolution page.

Private images and videos are not queued for item-by-item review or sent to a
third-party moderation service. Friend approval gives users control over their
contacts; the recipient can report a message, block its sender, and reach MaiChat support.
An operator can resolve reports. Before submission, exercise those flows with
two test accounts, confirm the operator response procedure, and describe the
actual safeguards in App Review notes. Apple requires a method for filtering
objectionable material but does not prescribe per-image inspection. Whether
these safeguards satisfy Guideline 1.2 is ultimately App Review's decision;
do not claim that unimplemented scanning exists. A review-only feature flag
or hidden disabled chat path would misrepresent the submitted app.

## Account deletion operation

The iOS path is Settings → Delete Account. A password or SMS challenge and the
MaiChat ID confirm ownership. Acceptance disables the account and revokes its
auth-service sessions immediately. Existing Tencent IM connections require the
provider cleanup step before their termination can be claimed. The user receives
a receipt token, request ID, due date and
an unauthenticated receipt-status query. Completion is a separate operator action
after provider cleanup; the receipt remains usable after the user row is deleted.

For each pending request:

1. Inspect the request with `services/auth/complete-account-deletion.js --list`.
2. Delete its Tencent IM account using `services/auth/purge-tencent-account.js`.
   Verify provider success or the documented already-absent result.
3. Purge account-scoped Session Sync rows with
   `services/auth/purge-session-sync-account.js --execute <request-id>`.
4. Purge new owner-scoped OSS objects using
   `services/oss-media-signer/purge-account-assets.js --execute <account-id> <account-id>`.
   Historical date-only OSS keys and provider-side Ark assets need a manual inventory
   and matching deletion evidence; they are **not** covered by the prefix script.
5. Review shared chat content and any legally required retention. Record what was
   removed, retained, and why. Do not mark this step complete by merely inspecting
   the account row.
6. Write a private evidence JSON file with `operator`, the four boolean fields
   named in `services/auth/account-deletion.js`, and a `records` object containing
   a concrete reference for each field. Run
   `services/auth/complete-account-deletion.js --complete <request-id> <evidence.json>`.
   The command checks that the evidence file has owner-only permissions.
7. Confirm the receipt status changes to `completed` and that the due date was met.

Never run deletion commands on a production account to test the workflow. Use
local fixtures for code tests and a dedicated disposable account for deployment
acceptance after the user authorizes that specific account.

## Submission and deployment gates

- Deploy the auth service and matching legal pages before shipping an iOS build
  that advertises account deletion or report status. Test the actual public routes.
- Verify the report queue has an operator, response procedure and contact mailbox.
- Build a distribution-signed archive and verify its privacy manifest, entitlements,
  purpose strings and embedded third-party manifests. The local Debug simulator
  build is not a substitute for this archive.
- Provide App Review a stable test account and describe where report, block and
  deletion live, plus how first-party filtering works. State that AI model costs
  are covered by MaiChat and no credit purchase is available.
- The current machine has an Apple Development identity, not an Apple Distribution
  identity. App Store Connect metadata and final submission remain external gates.

## App Review notes draft

Provide a stable reviewer account and a second account for testing friend chat.
In the Notes field, state: "MaiChat provides one-to-one friend chat and private
AI creation; it has no public video feed. Ordinary users do not buy model credits.
In the iOS app, open a friend profile to block or report that account, or use the
action menu on an incoming message to report it. Reports go to MaiChat staff for
review and action; support is available in Settings. Account deletion is under
Settings → Delete Account and can be confirmed with the account password or SMS."
Add the tested first-party filtering details only when they exist in the deployed
service. Do not present the absence of Tencent cloud moderation as a substitute
for the safeguards that actually run.
