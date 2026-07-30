#pragma once

#include <QtGlobal>

namespace RemoteInput {

// 远程输入协议统一使用 Windows VK 值作为平台无关的物理键标识。
// 控制端先把 Qt 按键转换成该标识，被控端再映射为本机键码。
quint32 canonicalKeyCodeFromQt(int qtKey, bool keypad);

// 返回 macOS CGKeyCode；无法映射时返回 -1。单独暴露为纯函数，便于在
// 非 macOS 构建机上验证协议兼容性。
int macKeyCodeFromCanonical(quint32 keyCode);

}  // namespace RemoteInput
