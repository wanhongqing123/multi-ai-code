# Private media cloud privacy readiness (2026-10-26)

This is a release checklist for the optional account-private image and video
cloud. The public legal page was updated on 2026-10-26. The production account
API is currently enabled for all signed-in accounts at the user's request;
App Store Connect and Play Console declarations have not been updated, and
older installed clients do not gain the new image UI automatically.

## Verified product behavior

- A user selects an MP4/MOV video or a JPEG/PNG/WebP/HEIC image, or approves an
  Agent upload. The client sends file bytes directly to the dedicated private
  Alibaba Cloud OSS bucket. Videos are limited to 2 GB, images to 20 MB, and
  all files share a 10 GB account quota. The account
  service keeps the account-linked filename, size, MIME type, creation time,
  storage key, upload identifier and state. The service does not receive file
  bytes. Uploading to the cloud does not publish to a chat, the video circle, or
  a model automatically. See `services/auth/cloud-drive-service.js` and
  `docs/design/maichat-private-video-cloud-20261025.md`.
- Completed images and videos have no automatic expiration in the current service. An
  unfinished upload expires after seven days; the dedicated bucket also has an
  eight-day rule for aborting incomplete multipart uploads. Deleting a file
  removes it from the owner's list first. OSS deletion can finish later, with
  retries and reserved quota until cleanup completes. Account deletion waits
  for cloud cleanup before completion.
- The authenticated owner can request 60-second signed original-file and
  thumbnail URLs. The URL is a bearer capability: anyone who receives it may
  read that file until it expires. The app should not present it as permanently
  private. OSS makes image thumbnails and H.264/H.265 video snapshots on demand;
  unsupported video codecs show a client fallback icon.

## Public privacy-policy update

- `services/legal/site/legal/privacy.html` has a separate private-cloud
  paragraph covering voluntary image/video upload, embedded source metadata,
  account-linked metadata, storage,
  cross-device use, seven-day upload expiry, asynchronous object deletion, and
  signed-link sharing risk. It was published to `https://ichat.life/legal/privacy.html`
  on 2026-10-26 with that effective date. Nginx HTTPS loopback and a public HTTPS
  request from the ECS both returned 200 and the new paragraph/date. The page
  was updated again that day to include images before new image clients were
  distributed. Review whether additional user notice is required by the
  policy's update clause.
- The existing iOS `PrivacyInfo.xcprivacy` already lists
  `NSPrivacyCollectedDataTypePhotosorVideos`, linked to the user, for app
  functionality. The privacy manifest is not an App Store Connect App Privacy
  response. No cloud-specific change to the manifest was needed here.

## Store submissions to reconcile

| Surface | Required review for this feature | Current evidence |
|---|---|---|
| App Store Connect App Privacy | Confirm **Photos or Videos**, linked to the account, for **App Functionality**; inspect the whole app and integrated SDKs before final answers. Refresh the public privacy-policy URL and App Review notes for the current feature flags. | No App Store Connect declaration export or access is in this repository. The 2026-10-20 review-note draft predates the cloud. |
| Google Play Data safety | Declare user-selected **Photos and Videos** transmitted off device for cloud storage, and inspect the form's collection, optionality, sharing/service-provider, encryption, and deletion answers against the deployed service and SDKs. | No Play Console Data safety export is in this repository. Android source currently links the policy at login. |
| Public policy and in-app access | Verify the updated page is reachable from the app and store listing. | The public page is published and HTTPS 200 from ECS. iOS Settings has a policy link. Android and Desktop source searches found policy links on login surfaces only; verify a signed-in user can still reach the policy without logging out. |
| Account deletion | Ensure cloud object cleanup evidence is included before completing account deletion, and verify the Google Play external account-deletion URL or request route. | The account-deletion service blocks completion while cloud cleanup remains pending; this document did not inspect live Play Console settings. |

Apple's privacy-policy guideline also asks for protection by third parties that
receive user data. The product/legal owner should check the applicable Alibaba
Cloud service agreement and processing terms. The current policy says providers
follow their own rules; this audit
cannot establish contractual equivalence and does not add such a promise.

The store forms cover the **entire app**, not only this feature. Do not copy the
rows above as complete declarations. The source now purges a deleted file's
account metadata only after exact-key multipart listing and abort, object HEAD
absence, the applicable 24-hour race hold, and a cleanup-revision check. This
also applies to older rows previously marked cleanup-complete; a successful
recheck does not newly charge the account quota, while uncertainty restores a
pending cleanup reservation. Account deletion waits until all such rows are gone.
The signer and auth changes were deployed to versioned production releases
after local tests. The new signer passed an empty/one/empty Multipart
round trip against the dedicated OSS bucket, including internal-key and
cross-account rejection. Six pre-upgrade deleted rows were requeued for their
remaining 24-hour race hold; their eventual purge is still to be observed.
The image schema migration preserved the one existing user video row byte for
byte and the OSS original passed size/MIME HEAD verification. A real OSS probe
returned private JPEG thumbnails for disposable H.264 MP4, PNG and HEIC; an
iOS simulator's production-test account completed JPEG upload, thumbnail,
preview and deletion, with zero remaining DB/OSS rows and uploads.
The published policy describes asynchronous cleanup and makes no
immediate-erasure promise.

Official submission references:

- Apple [App Privacy Details](https://developer.apple.com/app-store/app-privacy-details/)
  [App Store Connect App Privacy](https://developer.apple.com/help/app-store-connect/manage-app-information/manage-app-privacy),
  and [App Review Guideline 5.1.1](https://developer.apple.com/app-store/review/guidelines/).
- Google Play [Data safety guidance](https://support.google.com/googleplay/android-developer/answer/10787469)
  and [User Data policy](https://support.google.com/googleplay/android-developer/answer/10144311).
