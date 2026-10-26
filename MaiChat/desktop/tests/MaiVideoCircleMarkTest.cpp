#include "MaiVideoCircleMark.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

QByteArray digest(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) hash.addData(file.read(64 * 1024));
    return hash.result().toHex();
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc == 4 && std::strcmp(argv[1], "--mark-once") == 0) {
        MaiVideoCircleMarkResult result{};
        mai_video_circle_prepare_marked_mp4(argv[2], argv[3], nullptr, &result);
        std::printf("status=%d duration_ms=%lld size_bytes=%llu sha256=%s detail=%s\n", result.status,
                    static_cast<long long>(result.duration_ms), static_cast<unsigned long long>(result.size_bytes),
                    result.sha256_hex, result.detail_utf8);
        return result.status == MAI_VIDEO_CIRCLE_MARK_OK ? 0 : 23;
    }
    if (argc == 3 && std::strcmp(argv[1], "--cancel-in-flight") == 0) {
        QTemporaryDir temporary;
        if (!temporary.isValid()) return 20;
        const QString target = temporary.filePath(QStringLiteral("canceled.mp4"));
        const QByteArray targetUtf8 = target.toUtf8();
        MaiVideoCircleMarkCancel* cancel = mai_video_circle_mark_cancel_create();
        if (!cancel) return 21;
        MaiVideoCircleMarkResult result{};
        std::thread worker(
            [&] { mai_video_circle_prepare_marked_mp4(argv[2], targetUtf8.constData(), cancel, &result); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        mai_video_circle_mark_cancel_request(cancel);
        worker.join();
        mai_video_circle_mark_cancel_destroy(cancel);
        return result.status == MAI_VIDEO_CIRCLE_MARK_CANCELED && !QFile::exists(target) &&
                       QDir(temporary.path()).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty()
                   ? 0
                   : 22;
    }
    if (argc != 3 && argc != 4) return 1;
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    const QString source = temporary.filePath(QStringLiteral("source.mp4"));
    const QString target = temporary.filePath(QStringLiteral("marked.mp4"));
    if (!QFile::copy(QString::fromUtf8(argv[1]), source)) return 3;
    const QByteArray originalHash = digest(source);
    if (originalHash.isEmpty()) return 4;
    const QByteArray sourceUtf8 = source.toUtf8();
    const QByteArray targetUtf8 = target.toUtf8();
    MaiVideoCircleMarkResult marked{};
    mai_video_circle_prepare_marked_mp4(sourceUtf8.constData(), targetUtf8.constData(), nullptr, &marked);
    if (marked.status != MAI_VIDEO_CIRCLE_MARK_OK || !marked.visible_mark_verified || !marked.metadata_mark_verified ||
        marked.duration_ms != 1000 || marked.size_bytes == 0 ||
        marked.size_bytes != static_cast<uint64_t>(QFile(target).size()))
        return 5;
    if (originalHash != digest(source) || digest(target) != marked.sha256_hex || digest(target) == originalHash)
        return 6;

    MaiVideoCircleMarkResult conflict{};
    mai_video_circle_prepare_marked_mp4(sourceUtf8.constData(), targetUtf8.constData(), nullptr, &conflict);
    if (conflict.status != MAI_VIDEO_CIRCLE_MARK_INVALID_ARGUMENT || digest(target) != marked.sha256_hex) return 7;

    MaiVideoCircleMarkCancel* cancel = mai_video_circle_mark_cancel_create();
    if (!cancel) return 8;
    mai_video_circle_mark_cancel_request(cancel);
    const QString canceledPath = temporary.filePath(QStringLiteral("canceled.mp4"));
    const QByteArray canceledUtf8 = canceledPath.toUtf8();
    MaiVideoCircleMarkResult canceled{};
    mai_video_circle_prepare_marked_mp4(sourceUtf8.constData(), canceledUtf8.constData(), cancel, &canceled);
    mai_video_circle_mark_cancel_destroy(cancel);
    if (canceled.status != MAI_VIDEO_CIRCLE_MARK_CANCELED || QFile::exists(canceledPath)) return 9;

    const QString badSource = temporary.filePath(QStringLiteral("not-video.mp4"));
    QFile invalid(badSource);
    if (!invalid.open(QIODevice::WriteOnly) || invalid.write("not mp4") != 7) return 10;
    invalid.close();
    const QByteArray badSourceUtf8 = badSource.toUtf8();
    const QString badTarget = temporary.filePath(QStringLiteral("bad-output.mp4"));
    const QByteArray badTargetUtf8 = badTarget.toUtf8();
    MaiVideoCircleMarkResult rejected{};
    mai_video_circle_prepare_marked_mp4(badSourceUtf8.constData(), badTargetUtf8.constData(), nullptr, &rejected);
    if (rejected.status != MAI_VIDEO_CIRCLE_MARK_UNSUPPORTED_MP4 || QFile::exists(badTarget)) return 11;

    const QString largeSource = temporary.filePath(QStringLiteral("too-large.mp4"));
    QFile large(largeSource);
    if (!large.open(QIODevice::WriteOnly) || !large.resize(200'000'001)) return 12;
    large.close();
    const QByteArray largeUtf8 = largeSource.toUtf8();
    const QString largeTarget = temporary.filePath(QStringLiteral("large-output.mp4"));
    const QByteArray largeTargetUtf8 = largeTarget.toUtf8();
    MaiVideoCircleMarkResult tooLarge{};
    mai_video_circle_prepare_marked_mp4(largeUtf8.constData(), largeTargetUtf8.constData(), nullptr, &tooLarge);
    if (tooLarge.status != MAI_VIDEO_CIRCLE_MARK_SIZE_LIMIT || QFile::exists(largeTarget)) return 13;

    const QString longTarget = temporary.filePath(QStringLiteral("long-output.mp4"));
    const QByteArray longTargetUtf8 = longTarget.toUtf8();
    MaiVideoCircleMarkResult tooLong{};
    mai_video_circle_prepare_marked_mp4(argv[2], longTargetUtf8.constData(), nullptr, &tooLong);
    if (tooLong.status != MAI_VIDEO_CIRCLE_MARK_DURATION_LIMIT || QFile::exists(longTarget)) return 14;

    if (argc == 4 && !QFile::copy(target, QString::fromUtf8(argv[3]))) return 15;
    return 0;
}
