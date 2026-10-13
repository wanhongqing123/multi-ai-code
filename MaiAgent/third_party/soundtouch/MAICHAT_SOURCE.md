# SoundTouch 源码来源

- 上游：<https://www.surina.net/soundtouch/>
- 发布包：`soundtouch-2.3.3.tar.gz`
- 下载地址：<https://www.surina.net/soundtouch/soundtouch-2.3.3.tar.gz>
- 发布包 SHA-256：`43b23dfac2f64a3aff55d64be096ffc7b73842c3f5665caff44975633a975a99`
- 许可证：LGPL-2.1-or-later，原文见 `COPYING.TXT`。

本目录直接保存上游发布包中的源码，没有引入 Git 子模块。为适配当前构建，
将 `CMakeLists.txt` 的最低 CMake 兼容版本从 3.1 提至 3.5；移除了发布包中
不参与编译的示例 `.so`、`.dll`、`.ico` 和 Gradle wrapper `.jar`。播放器以 16 位整数 PCM
构建 SoundTouch，通过 `setTempo` 实现变速不变调。
Git 以文本自动归一化保存上游文件的换行符，检出时按平台还原。
