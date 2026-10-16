# MaiChat Desktop login window was oversized

- Reported: 2026-10-17, Windows screenshot of the QR-expired state.
- Source baseline: `6a0c66326da31bdca585bd8ec556f5c9cc74bf7a` (published Desktop 2.1.0).
- Reproduction: open the desktop login dialog, wait for the QR request to expire. The dialog occupied approximately 1400 × 1100 physical pixels at the reporter's display scale, and repeated the expired message in the QR placeholder, status text, help text, and action button.
- Root cause: `LoginDialog::buildUi()` requested a 920 × 700 logical-pixel window with a 720 × 680 minimum, a 440-pixel panel, and a 260-pixel QR frame. `setState()` filled several separate labels with equivalent instructions for the same state. The Windows test's previous landscape-size assertion reinforced the oversized layout.
- Change: request a 520 × 560 logical-pixel window with a 480 × 540 minimum, scale the panel and QR frame proportionally, and show no duplicate status/detail text when the QR is absent. Keep the comparison code and countdown visible while the QR is active.
- Verification: before the change, `usesCompactDesktopSize` failed on the width assertion. After the change, Windows `login_dialog_test` passed, the desktop App `--smoke` exited 0, and native Qt captures at 100% and 150% scale showed the QR, code, actions, and expired state without clipping.
- Boundary: the Windows test uses a local QR fixture. macOS appearance and installation of a build containing this change have not yet been verified. The already published 2.1.0 assets remain at their frozen baseline.
