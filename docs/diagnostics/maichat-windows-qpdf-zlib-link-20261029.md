# Windows Desktop 2.2.32 的 qpdf/zlib 链接冲突

- 时间与平台：2026-10-29，Windows 11、VS2019 x64 Release、Qt 5.15.2。
- 基线源码：`45dd43f5ad8ec3cebb1763f571e42cdf7061e0cc`，Desktop 2.2.32。
- 复现：在全功能构建中链接 `maichat`；FFmpeg、OpenCV、RemoteDesktop 均为 ON，
  Qt5WebSockets Config 和 DLL 可用；没有设置临时 `CL` 编译标志。

## 观察与原因

新增的 qpdf PDF 路径把 `zs.lib` 的压缩对象拉进最终链接。MSVC 报
`zs.lib(deflate.c.obj/inflate.c.obj)` 的 `z_deflate`、`z_inflate`、初始化及结束函数
与 `Qt5Core.lib(Qt5Core.dll)` 多重定义（LNK2005），最终 LNK1169。
原有 `Z_PREFIX` 只把符号改为通用的 `z_`，Qt5Core.dll 恰好也导出该前缀；
因此“有前缀”并未形成独立命名空间。`/FORCE:MULTIPLE` 无法保证调用落到正确实现。

## 修复

修复提交：本文所在提交。Windows 构建从 zlib 生成的 `zconf.h` 派生私有覆盖头，
将链接符号统一命名为 `mai_z_`，同时让 zlib 静态库、qpdf 和 `MaiZlibTool` 读取同一覆盖头。
zlib.h 的五个 init 包装宏仍保持它实际定义的 `z_` 宏名，但内部真实函数采用
`mai_z_`；`gzgetc` 有实际导出函数，也改为私有名字。其他平台保持原配置。

## 红→绿与回归

- 原始 `maichat` 链接必现 LNK2005/LNK1169；修复后同一全功能 VS2019 目标链接成功，
  构建后验证 90 项 PE 导入及 Qt 插件，未使用 `/FORCE`。
- `dumpbin /symbols` 回读 `zs.lib`：114 个 `mai_z_` 定义、0 个旧 `z_` 定义；
  qpdf 归档：6 个 `mai_z_` 压缩函数引用、0 个旧 `z_` 引用。
- 独立 MaiAgent 干净重链后，核心、模型流、TurnRunner、工具、权限、存储、截屏、
  Session 改名、zlib 往返及两组 PDF 测试共 12/12 CTest 通过。
- Desktop Host、云盘、Markdown、Agent、启动、登录、布局、密码与 PDF 提示等
  13/13 CTest 通过；构建目录的 `maichat.exe --smoke` 在裸 PATH 下退出 0，日志
  回读版本 2.2.32、Qt 5.15.2、OpenSSL 3.5.9 和实际可执行文件路径。
- 构建目录中的 `QPDF_LICENSE.txt`、`QPDF_NOTICE.md` 与上游源码文件逐项 SHA-256
  一致，Windows 打包脚本另有缺失即失败的复制/哈希检查。

边界：尚未从最终跨端冻结提交制作 Windows EXE/ZIP，也未运行安装器、上传资产或
替换现装 App；Mac/iOS/Android 不以本次 Windows 构建结果代替各自验收。
