# 二维码长内容在 Windows 扫描失败（2026-10-27）

## 复现与证据

- 源码 `e22dca634334192a372e60dadca2019fb962cf07`；Windows VS2019、
  Qt 5.15.2 Release。`mai_qr_code_test` 编译链接成功，但 `mai_qr_code`
  CTest 在独立 quirc 扫描器的 `quirc_count != 1` 处失败。
- 首张中文 URL 二维码为 481×481，29×29 模块、每模块 13 像素、四边留白，
  黑像素 71318；Windows quirc 与 OpenCV 都检测成功，和 Mac 输出一致。
- 失败的是第二张 `large.png`，内容为 2048 个小写 `a`：354×354，
  每模块仅 2 像素。Windows quirc 未检出，独立 OpenCV `detect=false`；
  PNG 可由 Qt 打开，黑白像素和留白均存在。Mac 原始 PNG 与 Windows 的
  SHA-256 均为 `6ac86b2935a47f8585b3b9d5d46a9001cc28e91b66e5727abe38a1c63a2b8d0f`，
  排除了 Windows 专属编码差异。Mac 重跑原图时 quirc 也未稳定检出。

## 修复与验证边界

- 单纯把 2048 字节图增至每模块 4 像素后，Mac quirc 仍未稳定检出；
  Windows 对原图的内存放大图用 OpenCV 检测也未成功。因此高版本密集矩阵
  的识别兼容性是问题，不能只靠放大声称修好。
- 本地用独立 quirc 扫描器从 256、512、768、1024、1280、1536、1792、2048
  字节逐级复现：前三档可扫描，1024 起失败。工具把 UTF-8 输入上限设为
  768 字节，并至少绘制为每模块 4 像素，保留四模块留白。超限直接报错，
  避免返回高密度且当前已知无法识别的二维码。
- Mac 独立扫码回归和 Windows 原生 768 字节用例仍须从修复提交复验。
  生产 App 未替换。
