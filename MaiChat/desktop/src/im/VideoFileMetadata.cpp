#include "im/VideoFileMetadata.h"

#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSet>

#include <algorithm>
#include <limits>

namespace {

// moov → trak → tkhd 只有三层，多给几层余量即可。畸形文件里盒子可以无限自嵌套，
// 没有这个上限就会一路递归到栈溢出。
constexpr int kMaxBoxDepth = 8;
// 盒子头最长 16 字节：size(4) + type(4) + largesize(8)。
constexpr qint64 kMaxBoxHeaderBytes = 16;

bool readExactly(QIODevice& device, char* buffer, qint64 count) {
    qint64 got = 0;
    while (got < count) {
        const qint64 chunk = device.read(buffer + got, count - got);
        if (chunk <= 0) return false;
        got += chunk;
    }
    return true;
}

quint32 readU32(const uchar* p) {
    return (static_cast<quint32>(p[0]) << 24) | (static_cast<quint32>(p[1]) << 16)
           | (static_cast<quint32>(p[2]) << 8) | static_cast<quint32>(p[3]);
}

quint64 readU64(const uchar* p) {
    return (static_cast<quint64>(readU32(p)) << 32) | static_cast<quint64>(readU32(p + 4));
}

// 16.16 定点数转整数（tkhd 的 width/height 就是这种编码）。
int fixed16ToInt(quint32 value) {
    return static_cast<int>(value >> 16);
}

struct BoxHeader {
    QByteArray type;
    qint64 contentOffset = 0;
    qint64 contentSize = 0;
    qint64 nextOffset = 0;
};

// 读一个盒子头。end 是当前容器的结束偏移（不含），越界一律判失败。
bool readBoxHeader(QIODevice& device, qint64 offset, qint64 end, BoxHeader& out) {
    if (offset + 8 > end) return false;
    if (!device.seek(offset)) return false;
    uchar header[kMaxBoxHeaderBytes];
    if (!readExactly(device, reinterpret_cast<char*>(header), 8)) return false;

    qint64 boxSize = static_cast<qint64>(readU32(header));
    qint64 headerSize = 8;
    if (boxSize == 1) {
        if (offset + 16 > end) return false;
        if (!readExactly(device, reinterpret_cast<char*>(header + 8), 8)) return false;
        const quint64 large = readU64(header + 8);
        if (large > static_cast<quint64>(std::numeric_limits<qint64>::max())) return false;
        boxSize = static_cast<qint64>(large);
        headerSize = 16;
    } else if (boxSize == 0) {
        // size 0 表示「一直到文件结尾」，只允许出现在最后一个盒子上。
        boxSize = end - offset;
    }

    if (boxSize < headerSize) return false;
    if (offset + boxSize > end) return false;

    out.type = QByteArray(reinterpret_cast<const char*>(header) + 4, 4);
    out.contentOffset = offset + headerSize;
    out.contentSize = boxSize - headerSize;
    out.nextOffset = offset + boxSize;
    return true;
}

void parseMovieHeader(QIODevice& device, qint64 offset, qint64 size, VideoFileMetadata& out) {
    if (size < 4) return;
    if (!device.seek(offset)) return;
    uchar version = 0;
    if (!readExactly(device, reinterpret_cast<char*>(&version), 1)) return;

    // version + flags(3) 之后：v0 是 creation(4) modification(4) timescale(4) duration(4)，
    // v1 把两个时间戳和 duration 都加宽到 8 字节。
    const qint64 fieldsOffset = offset + 4;
    quint32 timescale = 0;
    quint64 duration = 0;
    if (version == 1) {
        if (size < 4 + 8 + 8 + 4 + 8) return;
        if (!device.seek(fieldsOffset + 16)) return;
        uchar buffer[12];
        if (!readExactly(device, reinterpret_cast<char*>(buffer), 12)) return;
        timescale = readU32(buffer);
        duration = readU64(buffer + 4);
    } else {
        if (size < 4 + 4 + 4 + 4 + 4) return;
        if (!device.seek(fieldsOffset + 8)) return;
        uchar buffer[8];
        if (!readExactly(device, reinterpret_cast<char*>(buffer), 8)) return;
        timescale = readU32(buffer);
        duration = readU32(buffer + 4);
        // v0 里 0xFFFFFFFF 是「时长未知」的哨兵值，不是 49710 天的视频。
        if (duration == 0xFFFFFFFFull) duration = 0;
    }

    if (timescale == 0 || duration == 0) return;
    const double seconds = static_cast<double>(duration) / static_cast<double>(timescale);
    if (seconds <= 0.0 || seconds > 24.0 * 3600.0) return;
    out.durationSeconds = static_cast<int>(seconds + 0.5);
    if (out.durationSeconds < 1) out.durationSeconds = 1;
}

void parseTrackHeader(QIODevice& device, qint64 offset, qint64 size, VideoFileMetadata& out) {
    // 已经拿到画面尺寸就不再看后面的轨道：第一条有尺寸的就是视频轨。
    if (out.width > 0 && out.height > 0) return;
    if (size < 4) return;
    if (!device.seek(offset)) return;
    uchar version = 0;
    if (!readExactly(device, reinterpret_cast<char*>(&version), 1)) return;

    // version+flags(4) 之后的可变段：v0 是 20 字节，v1 是 32 字节。再之后固定是
    // reserved(8) layer(2) alternate_group(2) volume(2) reserved(2) matrix(36) w(4) h(4)。
    const qint64 variableBytes = version == 1 ? 32 : 20;
    const qint64 tailOffset = offset + 4 + variableBytes + 8 + 2 + 2 + 2 + 2;
    if (4 + variableBytes + 8 + 2 + 2 + 2 + 2 + 36 + 8 > size) return;
    if (!device.seek(tailOffset)) return;
    uchar tail[44];
    if (!readExactly(device, reinterpret_cast<char*>(tail), 44)) return;

    const quint32 matrixA = readU32(tail);
    const quint32 matrixB = readU32(tail + 4);
    const quint32 matrixC = readU32(tail + 12);
    const quint32 matrixD = readU32(tail + 16);
    int width = fixed16ToInt(readU32(tail + 36));
    int height = fixed16ToInt(readU32(tail + 40));
    if (width <= 0 || height <= 0) return;

    // 手机竖屏录的视频常常是「横着存 + 旋转矩阵」。a=d=0 且 b、c 非零就是 90/270 度，
    // 此时存储的宽高要对调，否则封面会画成躺倒的比例。
    if (matrixA == 0 && matrixD == 0 && matrixB != 0 && matrixC != 0) {
        std::swap(width, height);
    }

    out.width = width;
    out.height = height;
}

void parseContainer(QIODevice& device, qint64 start, qint64 end, int depth, VideoFileMetadata& out) {
    if (depth > kMaxBoxDepth) return;
    qint64 offset = start;
    while (offset < end) {
        BoxHeader box;
        if (!readBoxHeader(device, offset, end, box)) return;
        if (box.type == "moov" || box.type == "trak") {
            parseContainer(device, box.contentOffset, box.contentOffset + box.contentSize, depth + 1, out);
        } else if (box.type == "mvhd") {
            parseMovieHeader(device, box.contentOffset, box.contentSize, out);
        } else if (box.type == "tkhd") {
            parseTrackHeader(device, box.contentOffset, box.contentSize, out);
        }
        if (box.nextOffset <= offset) return;  // 防止畸形 size 造成原地打转
        offset = box.nextOffset;
    }
}

const QSet<QString>& supportedVideoExtensions() {
    static const QSet<QString> extensions{QStringLiteral("mp4"), QStringLiteral("mov")};
    return extensions;
}

}  // namespace

QString videoContainerTypeForPath(const QString& path) {
    const QString suffix = QFileInfo(path.trimmed()).suffix().toLower();
    return supportedVideoExtensions().contains(suffix) ? suffix : QString();
}

bool isSupportedVideoFile(const QString& path) {
    return !videoContainerTypeForPath(path).isEmpty();
}

VideoFileMetadata parseIsoBaseMediaMetadata(QIODevice& device) {
    VideoFileMetadata metadata;
    // 必须可 seek：moov 经常排在 mdat 之后（没做 faststart 的文件都是这样），
    // 顺序流没法跳过几十 MB 的媒体数据去读它。
    if (!device.isOpen() || !device.isReadable() || device.isSequential()) return metadata;
    const qint64 size = device.size();
    if (size <= 8) return metadata;
    parseContainer(device, 0, size, 0, metadata);
    metadata.valid = metadata.durationSeconds > 0 || (metadata.width > 0 && metadata.height > 0);
    return metadata;
}

VideoFileMetadata readVideoFileMetadata(const QString& path) {
    VideoFileMetadata metadata;
    const QString cleanPath = path.trimmed();
    if (cleanPath.isEmpty()) return metadata;

    QFile file(cleanPath);
    if (!file.open(QIODevice::ReadOnly)) return metadata;
    metadata = parseIsoBaseMediaMetadata(file);
    metadata.sizeBytes = file.size();
    metadata.containerType = videoContainerTypeForPath(cleanPath);
    return metadata;
}
