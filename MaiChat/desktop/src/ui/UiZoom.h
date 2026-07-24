#pragma once

#include <QString>
#include <QtGlobal>

// 全局界面缩放（飞书式 Ctrl+= / Ctrl+- / Ctrl+0）。
// factor 范围 0.8~2.0、步进 0.1，持久化到 QSettings("ui/zoomFactor")（组织名未设置
// 时——如单测环境——只存内存，不碰注册表）。s(px) 缩放单个像素值；scaleQss 把样式表
// 字符串里的所有 "<n>px" 按当前倍率缩放（0px 保持 0，非零值最小 1px 不被缩没）。
namespace UiZoom {

qreal factor();
// 设置倍率并返回实际生效值（越界被夹紧到 [minFactor, maxFactor]）。
qreal setFactor(qreal value);
qreal minFactor();
qreal maxFactor();
qreal step();

int s(int px);
QString scaleQss(const QString& qss);

}  // namespace UiZoom
