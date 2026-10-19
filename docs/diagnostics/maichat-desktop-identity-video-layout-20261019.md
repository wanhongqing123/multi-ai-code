# MaiChat Desktop 会话栏、本人资料与视频窗口布局

- 时间：2026-10-19，Windows VS2019/Qt 5.15.2；用户现场截图为 macOS Desktop 2.2.3。
- 现场版本：已发布 Desktop 2.2.3 源码 `71a5bdddb3ab416e677f80502114bf93a7264c3b`。
- 修复提交：`a13862954b396a26d26aa8806a7a277324703c70`。本次没有替换已发布的 2.2.3 安装包。

## 复现与证据

1. 打开消息页：左侧会话列表的顶部只剩靠右的“申请”和加号，左侧大块空白，没有栏目标题。
2. 点击左上角自己的头像：原控件是普通 `QWidget`，没有点击动作，无法从这里查看本人资料。
3. 在宽屏桌面端打开聊天中的视频：`MainWindow::syncVideoDock()` 只固定播放器宽度，水平布局却让它占满整窗高度，形成高竖框与大片黑边；`QBoxLayout` 中的固定尺寸也没有可拖动的调整边界。
4. Windows 的 `MaiFfplayVideoDialogTest::mainWindowDocksSamePlayerAcrossNavigationAndFullscreen` 加入横向比例断言后，修前按预期失败；好友资料和会话栏的两个新 `MainWindowLayoutTest` 用例也在旧界面下失败。

## 修复

- 会话栏顶部增加清晰的“消息”标题，将“好友申请”收成浅色按钮，添加入口保留在右侧。
- 左上头像变为可点击按钮，进入设置页顶部的本人资料卡，展示头像、昵称和 MaiChat ID；原会话选择不变。
- 视频与聊天区改用 Qt 原生 `QSplitter`。播放器默认以横向比例展开，拖动分隔条会同步改变播放器宽高；窄窗改为上下分区，原有播放、导航、全屏和关闭流程保留。

## 验证边界

- Windows Release 主程序增量重链成功；隔离数据目录运行 `maichat.exe --smoke` 退出码 0。
- 原生 Windows Qt 下 `MaiFfplayVideoDialogTest`、`main_window_layout_test` 完整 CTest 2/2 通过；单独的视频用例实际显示画面，拖动后播放器尺寸改变。我查看了视频、会话栏及本人资料截图。
- `npm run check:line-width` 与 `git diff --check` 通过。
- macOS arm64 同提交原生 Qt 增量构建、两项 CTest、隔离 `--smoke` 均通过；Mac 协作者查看了会话栏、本人资料、真实视频帧和拖动截图，并验证全屏往返、导航及关闭。宽窗的视频栏下方仍有明显的浅色留白，这是 `videoPaneLayout` 顶部对齐及剩余空间布局的结果，不是播放器内部黑边；后续视觉迭代需单独决定如何利用这块空间。
- Windows 当前已安装版以及既有 Desktop 2.2.3 Release 均未替换，不能把源码修复等同于用户已更新。
