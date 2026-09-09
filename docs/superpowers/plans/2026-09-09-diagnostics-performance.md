# Diagnostics recipients and performance implementation plan

> 用户随后要求关闭 superpowers；此计划仅作工作记录，后续由主代理直接实现与检查，没有派生子代理。

**Goal:** Send A+B diagnostic reports to B or C and include bounded input/UI/storage performance evidence with honest coverage.

**Architecture:** Keep existing collectors and sanitizers. Add scoped, numeric performance aggregates, export coverage, and preserve fields through the report chain. Keep selection separate from collection.

**Tech Stack:** Swift/SwiftUI, Qt/C++, Electron TypeScript.

## Tasks
- [x] Recipient regression: modify `MaiChat/desktop/tests/RemoteDiagnosticsUiTest.cpp` to expect the current peer and select it through the actual menu. Run the test to demonstrate failure; remove only peer exclusion in `src/ui/MainWindow.cpp`, add a current-conversation marker and keep owner exclusion. Verify B-only and B/C delivery, cancellation and account switch.
- [x] iOS: extend `MaiChatTests/RemoteDiagnosticsTests.swift` for scoped performance numeric fields, unrelated-account exclusion, coverage and final-report preservation. Add account transitions in `AppDiagnosticLog.swift`, wire from `RemoteIMAppState.swift`, update `MaiChatCore/RemoteDiagnostics.swift` and UI copy. Run focused tests before/after, full tests and app build.
- [x] Qt: add `diagnostics/PerformanceLog.h/.cpp` and unit tests with injected clock, bounded events/aggregates, reset-on-inactivity and account boundaries. Wire UI loop heartbeat and scoped timers for input, list update, history IO and image decoding. Export through `RemoteDiagnosticsController.cpp` with coverage. Build with CMake and run relevant Qt tests.
- [x] Transport: inspect `electron/remote-im/codexDiagnostics.ts` and both client protocol sanitizers; add finite bounded numeric performance fields and source coverage only where real source supports it. Test unknown fields stripped and valid fields preserved end to end. Explicitly mark unavailable B UI metrics.
- [x] Review spec compliance, then code quality; address findings, run Swift/Core tests + iOS build and Qt suite. Update spec with implementation/verification limits. Integrate only task changes, preserving user signing edits; no release or real-user messages.

## Commands
Qt configure: `cmake -S MaiChat/desktop -B release/diagnostics-build -DBUILD_TESTING=ON` (reuse local Qt/vendor locations when required).
Qt build: `cmake --build release/diagnostics-build -j 6`.
Qt tests: `QT_QPA_PLATFORM=offscreen ctest --test-dir release/diagnostics-build --output-on-failure`; run video preview with Cocoa if needed.
iOS: use the existing workspace/scheme and installed simulator via xcodebuild; keep original signing configuration unchanged.
