# iOS 构建脚本空数组回归（2026-10-19）

## 复现与观察

- 平台：macOS 26.4、系统 Bash 3.2.57、Xcode 26.6、iPhone 17 Pro iOS 26.5 模拟器。
- 源码：`a11465dc620d6edb5d5731448603fa8ff7958bbb`。在 `MaiChat/ios/MaiChat`
  运行 workspace 的 MaiChat Debug UI 测试；`Build MaiAgent` 脚本进入 FFmpeg 构建。
- 必现输出：`MaiAgent/third_party/build-media-codecs.sh: line 104:
  meson_install_options[@]: unbound variable`，随后 `ninja: build stopped`，
  Xcode `Command PhaseScriptExecution failed`。App 与测试尚未启动。
- 与目标账号登录 UI 无关：报错发生于编译阶段，首次出现于上述提交新增的
  `meson_install_options=()` 与 `"${meson_install_options[@]}"`。Mac 分支数组为空，
  Bash 3.2 在 `set -u` 下展开空数组会把它视为未定义。

## 修复与边界

- 对 Windows Android 分支显式调用 `meson install --destdir "$install_root"`；
  其余平台直接调用 `meson install`，避免空数组展开。Windows 的目标路径语义不变。
- `bash -n` 已通过。同一 workspace/模拟器/目标 UI 测试重新运行，`Build MaiAgent`
  通过，`testAccountPasswordModeAcceptsMaiChatId` 通过，Xcode `TEST SUCCEEDED`；
  此次复测覆盖 Mac Bash 3.2 的实际 FFmpeg 构建。Windows 分支保持原有
  `--destdir` 调用形式，Windows 侧实盘复核及最终提交 hash 待补。
