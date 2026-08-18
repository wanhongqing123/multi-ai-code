#pragma once

#include <QString>

// 发送一条视频消息所需的全部字段。
//
// 腾讯 IM C SDK 的 VideoElem 把视频和封面的类型/大小/尺寸/时长全部列为「读写(必填)」，
// 且这些值 SDK 自己不会推导——桌面端既没有链 Qt Multimedia 也不带解码器，所以：
//   * 时长和画面尺寸由 VideoFileMetadata 从 MP4/MOV 的盒子结构里解出来；
//   * 封面由 VideoCoverImage 生成（Windows 取系统缩略图，其余平台画占位图）。
// 两件事都在应用层做完，客户端只负责把它们摆进 SDK 的 JSON，保持 IM 客户端是一层薄封装。
struct RemoteIMVideoPayload {
    QString videoPath;
    // SDK 的 video_elem_video_type，取容器扩展名（"mp4" / "mov"）。
    QString videoType;
    qint64 videoSizeBytes = 0;
    int durationSeconds = 0;

    QString coverPath;
    // SDK 的 video_elem_image_type（"jpg" / "png"）。
    QString coverType;
    qint64 coverSizeBytes = 0;
    int coverWidth = 0;
    int coverHeight = 0;

    bool isValid() const {
        return !videoPath.isEmpty() && !videoType.isEmpty() && videoSizeBytes > 0
               && !coverPath.isEmpty() && !coverType.isEmpty() && coverSizeBytes > 0
               && coverWidth > 0 && coverHeight > 0;
    }
};
