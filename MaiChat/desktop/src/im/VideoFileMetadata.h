#pragma once

#include <QString>

class QIODevice;

// 一个待发送视频的基本信息。腾讯 IM C SDK 的视频元素把时长/尺寸都列为「必填」，
// 而桌面端没有链 Qt Multimedia、也不带解码器，所以这些值只能从容器结构里读——
// MP4/MOV 同属 ISO base media，时长在 moov/mvhd，画面尺寸在 moov/trak/tkhd，
// 都在盒子头里，不需要解一帧画面。
struct VideoFileMetadata {
    bool valid = false;
    // SDK 的 video_elem_video_type，取扩展名（"mp4" / "mov"）。
    QString containerType;
    qint64 sizeBytes = 0;
    int durationSeconds = 0;
    int width = 0;
    int height = 0;
};

// 目前只放行 mp4/mov：这两种是 IM 各端都能直接播放的容器。
bool isSupportedVideoFile(const QString& path);
QString videoContainerTypeForPath(const QString& path);

// 读文件并解析。文件读不了或不是 ISO base media 时返回 valid=false。
VideoFileMetadata readVideoFileMetadata(const QString& path);

// 只解析盒子结构，不碰文件系统。sizeBytes 不由这里填写（调用方从文件大小取）。
// 单独暴露出来是为了让测试直接喂内存里构造的盒子。
VideoFileMetadata parseIsoBaseMediaMetadata(QIODevice& device);
