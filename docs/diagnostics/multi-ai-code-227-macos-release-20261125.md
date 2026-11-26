# Multi-AI Code 2.2.7 macOS release

## Frozen identity and scope

User-authorized joint Windows x64/macOS arm64 release, coordinated with
`house-multi-ai-code`. Only Multi-AI Code is in scope; MaiChat is a separate product.

- Product source: `39e8eed575d9978535eba59a7a72a3a8a9ad1206`.
- Codex gitlink: `6e09a1b58f759848942e3d5b7653ab714f045ada`.
- Host: `2.2.7`; actual expected CLI: `codex-cli 0.172.0+multi-ai.3d86e43b07`.
- Official `rust-v0.172.0` annotated tag object:
  `fc3036dba44468096953f0e832f3fecd3adc0228`; peeled release commit:
  `3d86e43b075adeba2193ec5c22ac92f55afd82e6`. Independently read through GitHub API.
- Draft: `458049005`, tag `electron-20261125`, target exactly the frozen product source.
  The previous public Release `442021899` is not modified.

## Mac producer and local environment

Local acceptance machine: macOS 27.2 / arm64, Xcode 27.1, Node 22.19.0,
npm 10.9.3. Homebrew Rust is 1.96.1 without rustup; upstream requires 1.99.1.
Initially about 12 GiB disk was available, about 9 GiB after fetching the new
submodule. Existing files/caches and running installed apps were not deleted,
stopped, replaced, or rebound.

The initial formal-production route uses `build-macos-release.yml` on a macos-15 arm64
runner, Rust 1.99.1, three Cargo jobs, release thin LTO/four codegen units;
the existing workflow disables debug info and incremental compilation. It
checks the exact source/gitlink and actual packaged version. Target-platform
local acceptance exercises the exact formal installer, not source-tree
binaries or the previous 0.167 cache.

## Reproduced dependency-fetch failure and correction

Run [45609132818](https://github.com/wanhongqing123/multi-ai-code/actions/runs/45609132818)
passed checkout, arm64/source identity, Rust installation, and host prechecks.
At 2026-11-25 12:15 UTC it failed before compiling the kernel:

- Cargo's libgit2/SecureTransport failed fetching the pinned rusty_v8 `buildtools`
  submodule from `https://chromium.googlesource.com/chromium/src/buildtools.git`.
- Three approximately four-minute retries ended with connection-closed errors,
  then Cargo exit 101. This is dependency retrieval, not an observed Rust source
  compilation or disk-full failure.
- Cargo itself recommended `net.git-fetch-with-cli`; Windows was already using
  the process-level equivalent successfully.

Workflow-only correction `1764add23ef848451eff1e3382684b0b7af353d9` adds
`CARGO_NET_GIT_FETCH_WITH_CLI: 'true'` and a comment. No product/gitlink/lock or
Release target change. Initial push hit an SSH connection closure; ordinary
retry succeeded, without force-push.

Retry [45613044379](https://github.com/wanhongqing123/multi-ai-code/actions/runs/45613044379)
uses the new workflow definition but retains `source_commit=39e8eed...` and the
original expected Codex version. It initially queued for a Mac runner; runner
`GitHub Actions 1000000947` was assigned at 12:32 UTC. Cargo release finished successfully in 102m36s at 14:16 UTC. Packaged native
checks/ad-hoc signing passed, and DMG/blockmap generation finished at 14:18 UTC.
The job then failed because electron-builder inferred CI publishing and required
GH_TOKEN. No publishing token was supplied. Artifact upload and cache save were
skipped; the hosted VM output is no longer retrievable. This successful compile
is not a delivered or locally accepted Mac package.

Workflow-only commits `8fa5cb188972fbaca0ad396e65f444f3b3124eca` and
`c28b1218c41bee1a3775574e525bfe693141a286` split kernel/installer steps, pass
`--publish never`, use unique run/attempt cache keys with restore prefixes, save
recovered compilation caches on non-cancelled failures, and retain artifacts with
an attempt suffix. Contents permissions remain read-only. New run
[45636594514](https://github.com/wanhongqing123/multi-ai-code/actions/runs/45636594514)
uses workflow c28b with unchanged product input 39e8eed. It failed after
15m1s without acquiring a runner: annotations explicitly report hosted-runner
acquisition failure and macOS arm64 capacity constraints. The run is `failure`,
its unstarted job is `cancelled`, with no steps. No source/build failure or human
cancellation is inferred.

A local fallback is also authorized, using detached snapshots at exact parent
39e8eed and child 6e09, private Rust 1.99.1, three jobs/debug=0/no incremental,
and the source release thin-LTO/four-codegen configuration. APFS clones isolate
mutable Cargo/node_modules caches; default Homebrew Rust 1.96.1 is unchanged.
An owned process-group guard stops the build below 4 GiB free. No pre-existing
files have been deleted. The first local attempt stopped before compilation
because the downloaded V8 archive failed its pinned checksum; that gate was
retained. The official asset metadata independently matches the pinned digest;
re-download into a private temp cache passed the unchanged pinned checks:
archive 24,581,993 bytes / SHA256
`feab9ae0384bebaa99c1d000654462c9d42123d587185c7876aaeef7d76ff749`;
binding 40,056 bytes /
`b9d5652ebca515eb2bf4c2ee4a6551b2567117b5aa720f430d2032616dd7e3b5`.
The official build script rechecked them and entered Rust compilation. Local
formal output and installer acceptance are still pending.

## Acceptance design and prechecks

Local source checks passed: typecheck/line-width; IM 48 files, 507 passed and
2 platform skips; selected launch/packaging six files, 138 passed and 2 skips.
These are fixture/source checks and do not prove the new packaged kernel.

Prepared under `.cache/release-2.2.7-macos-20261125/acceptance/`:

- `code-mode-grpc-probe.mjs`: Node built-in HTTP/2 over the child's stdio with
  protobuf fields read from the pinned `codex.code_mode.v1.proto`. No library,
  model, or account request. It opens two sessions on one host, computes 42,
  checks `undefined` in the second session, closes the first, computes 42 in
  the second, closes sessions and transport/stdin, and checks exit 0/no kill.
- The old 0.167 V1 helper fails the same probe; an independent HTTP/2 fixture
  validates framing/close handling only and is not V8 acceptance. Windows
  independently checked fields and passed both its official Rust client and
  this Node probe against new 0.172 raw dev/release hosts; that is not yet Mac
  packaged-host evidence.
- Closing HTTP/2 and stdin are combined teardown actions. No single-cause
  stdin-EOF experiment is claimed. An outer owned process-group deadline
  prevents unbounded probe cleanup; a deliberately hung Node child confirms
  timeout/termination is observed as failure.
- `verify-bundle.mjs`: exact host/CLI/gitlink/manifest, strict ad-hoc signature,
  arm64/dependencies, real packaged Electron native modules, and isolated
  login-status configuration checks. It rejects the previous 2.2.6 bundle.
- `gui-pty-probe.mjs`: actual packaged renderer/preload/version/login screenshot;
  fresh app root and Chromium profile; an offline namespace with null SDKAppID
  and no signature endpoint/key, not a live IM account. Actual `cc:spawn` uses
  empty caller args, then lifecycle PID/path/source and process argv must prove
  the host injected `--no-daemon`. The isolated file-credential home must stay
  under the test root. Authentication menu and the existing Ctrl+C-for-copy guard are checked;
  explicit host close must reap the owned PTY. A separate raw PTY checks actual
  kernel Ctrl+C exit 0, with no model prompt. The original installed processes are not addressed.
- GUI harness calibration on the old bundle exposed two test harness errors:
  sampling the DOM before rendering, and leaving the main debugger attached
  while awaiting exit. Waiting for real login rendering and detaching after
  owned `app.quit()` gives screenshot/preload/normal exit 0. No app assertion,
  source behavior, or security control was weakened. Old-bundle calibration
  is not 2.2.7 acceptance.

Windows package QA also identified an incorrect test assumption: main-terminal
Ctrl+C is intentionally reserved for copy on macOS and Windows
(`shouldSuppressMainTuiCtrlC`), not forwarded as ETX. The Mac host probe was
aligned with that existing contract before formal execution. Product code is
unchanged; host-controlled close is not mislabeled as natural kernel exit 0.

The same Node probe also passed on the physical Mac against the official
0.172 arm64 helper as a harness/platform control (2.067s, 42/undefined/42,
exit 0, no force kill). Official archive SHA256
`eab24f88e6e20c881d8c11b7f70ee220fd19a94c586ec6bcead12a20e02cb6e4`
was checked against release metadata before safe extraction; binary SHA256
`0694f0e5c606a0015786cb94fb527b9526b4d892574cf5ba0e6bc7618cbc0d9d`.
This is upstream 3d86 control evidence only; the custom 6e09 DMG helper still
requires independent package-specific acceptance. It is not copied into bin/ or
used as a substitute product artifact.

Official CLI arm64 was also used only to calibrate the raw PTY harness. Its
archive SHA256 `bd121f8abd6b28b3fc476da04a348e25a83e89eda19d39107f68f8e966053092`
was verified after resuming two interrupted downloads. Binary SHA256 is
`f30a99714dc984876864239ef60563949b1e21421a7c9180422f51fd39186bc6`.
The first parser missed labels because ratatui can replace spaces with cursor
moves; labels are now compared ignoring whitespace. A synthetic ETX sent during
first paint was discarded: onboarding explicitly draws, discards pending input,
then creates its event stream. Waiting for the complete menu/footer and 500ms
before ETX gives actual Ctrl+C exit 0 without increasing the 30s deadline or
changing the product guard. This controls the probe only, not custom-package
acceptance. Host copy protection remains a separate test.

## Actual Mac package evidence

Local production completed on 2026-11-25 at 23:25:19 CST: official build script
exit 0 / 1843.184s, Cargo release 30m21s. Minimum observed free disk was
6.893 GiB; the 4 GiB guard did not trigger. Unused warnings and the linker
compact-unwind `__eh_frame` >16 MiB warning are retained.

The agent did not immediately continue to installer creation after kernel
completion and failed to synchronize this result overnight. On the next status
request (2026-11-26 15:53 CST), the actual exit record and absence of build
processes/DMG were checked and accurately reported. Installer production then
continued from the unchanged exact snapshot with `--publish never`, exit 0.

Actual DMG checksum verification and readonly mount passed. Packaged staging,
mounted App and independent copied App match across 536 files/symlinks,
814,548,775 bytes. Inventory SHA256:
`588eab1df226f7fb19bc4399677a13d789119b5948d92da21d88848789d9e696`.

The initial static-audit attempt failed because Apple otool treats the final
Helper `(GPU)` filename as archive-member syntax. QA-only correction inspects
SHA-identical temporary bytes; dependency resolution retains original bundle
anchors for LC_RPATH, @loader_path and @executable_path. Both actual bundles
pass all ten inspected Mach-O images with zero unresolved bundle references.
No missing-dependency assertion was waived, and product bytes were unchanged.

Both readonly DMG and copied App passed the same formal probes: exact
2.2.7/0.172.0+multi-ai.3d86e43b07 and 6e09 manifest; deep strict ad-hoc
verification; ABI130/SQLite3.49.2/node-pty loading; six isolated login-status
configs; two gRPC sessions with 42/undefined/42 and normal combined close/exit0;
actual GUI login pages personally viewed; default host IPC with no caller args
and actual --no-daemon selection; three auth choices, Ctrl+C copy/no ETX,
explicit controlled PTY close/reaping; independent raw-kernel Ctrl+C exit0.
App quit0 and all owned App/kernel/helper PIDs are gone, without forced kill.

Kernel SHA256 `018c981fabe54cebd4e4cc5b8b70835e26698413f93f8f01124c661ef43cd06e`;
helper SHA256 `98e719e58a1c6da269338abf3bfda4417a5e9abd95c44bc92cb187a3f6714801`.
Mach-O minOS 11.0 is a recorded header fact, not an older-system execution claim.

Mac upload initially hit `unexpected EOF` for the DMG; the other three files
were already uploaded and left unchanged. Retrying only the missing DMG
succeeded (no clobber, no segmentation). Final API readback matches local
recomputed sizes/SHA256 for all ten assets; the previous Release
442021899 still has the same ten asset IDs/sizes/digests. The Windows executor independently downloaded the entire 302,963,678-byte DMG
and recomputed the same SHA256, reviewed the other three raw files/checksum
coverage, and approved all ten immutable server identities and final body.

Published Release 458049005 at `2026-11-26T08:15:20Z` (16:15:20 CST), tag
`electron-20261125`, marked Latest. Windows independent unauthenticated checks
passed public tag commit 39e8eed, gitlink 6e09, latest, all ten asset identities,
both downloaded checksum files and HTTP 200. Coordinator independently repeated
an unauthenticated public check at 08:17:18 UTC: Latest/tag/gitlink, all ten
asset IDs/sizes/digests, both actual checksum files and the previous Release
asset identities passed. No asset/tag/source change was
made for the final documentation.

[Published Release](https://github.com/wanhongqing123/multi-ai-code/releases/tag/electron-20261125).

Final immutable Mac upload identities:

| Asset | Bytes | SHA256 |
| --- | ---: | --- |
| `MultiAICode-2.2.7-arm64-SHA256SUMS.txt` | 305 | `5bd4c40151b319f93944cfdd4c4c8e9a78f174bce3e69b54977f56312a8ef5f8` |
| `MultiAICode-2.2.7-arm64-verification.json` | 94365 | `a0f8e22db295f73e9cf4ab6a3651be2ec17eedf624d6b663267cfad2ee86978f` |
| `MultiAICode-2.2.7-arm64.dmg` | 302963678 | `d7704a8232a58d38086d372c1cb3aea31c4b693580ce6b659aec9b42f511485b` |
| `MultiAICode-2.2.7-arm64.dmg.blockmap` | 317338 | `e8a62ed5868adb404153c8efe988da0157cabf394299d1afa18075b6e5cd2197` |

Full login/real IM/model tasks, older macOS/Intel execution, Developer ID and
Apple notarization are outside the acceptance proof. Signature/packaged-runtime
facts are recorded as observed, not inferred from fixture passes.
