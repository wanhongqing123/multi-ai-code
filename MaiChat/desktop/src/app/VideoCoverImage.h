#pragma once

#include <QImage>
#include <QString>

#include "im/VideoFileMetadata.h"

// 一张已经落到磁盘、可以交给 IM SDK 上传的视频封面。腾讯 IM C SDK 的视频元素把
// 封面的路径/类型/大小/宽/高全列为「必填」，所以这五项必须一起备齐。
struct VideoCoverImage {
    bool valid = false;
    QString path;
    // SDK 的 video_elem_image_type。优先 "jpg"，JPEG 编码器不可用时退回 "png"。
    QString type;
    qint64 sizeBytes = 0;
    int width = 0;
    int height = 0;
};

// 生成封面并写进 outputDirectory。
// 先问系统要缩略图（Windows 走资源管理器那套缩略图提供程序，拿到的就是真实画面），
// 系统给不出来就画一张占位封面——宁可是一张比例正确的占位图，也不能因为拿不到首帧
// 就整条视频消息发不出去。
VideoCoverImage createVideoCoverImage(const QString& videoPath,
                                      const VideoFileMetadata& metadata,
                                      const QString& outputDirectory);

// 占位封面的画法：按视频比例的深色底 + 居中播放三角。只用 QPainter 的基本图形，
// 不写字——省掉对字体库的依赖，测试里不需要真的起一个 GUI 环境。
QImage renderPlaceholderVideoCover(const VideoFileMetadata& metadata);

// 把一张图按 IM 封面要求落盘（先试 JPEG，失败退 PNG），并回填必填字段。
VideoCoverImage writeVideoCoverImage(const QImage& image, const QString& outputPathWithoutSuffix);

// 系统缩略图。拿不到（非 Windows、格式不支持、系统没缓存）时返回空 QImage。
QImage loadSystemVideoThumbnail(const QString& videoPath, int maxEdgePixels);
