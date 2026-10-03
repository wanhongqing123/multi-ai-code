#include "MaiImageDecode.h"

#include <QByteArray>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    if (argc == 4 || argc == 5) {
        const MaiImageDecodeResult image = maiImageDecodeFile(argv[1], 0, 0);
        const int expectedWidth = QByteArray(argv[2]).toInt();
        const int expectedHeight = QByteArray(argv[3]).toInt();
        bool matches = image.rgba && image.error_code == 0 &&
                       image.width == expectedWidth && image.height == expectedHeight;
        if (matches && argc == 5) {
            const QImage decoded(image.rgba, image.width, image.height, image.stride,
                                 QImage::Format_RGBA8888);
            matches = decoded.save(argv[4], "PNG");
        }
        maiImageDecodeFree(image.rgba);
        return matches ? 0 : 8;
    }
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;

    const QString path = temporary.filePath(QStringLiteral("large-red.png"));
    QImage source(3024, 1898, QImage::Format_RGBA8888);
    source.fill(Qt::red);
    if (!source.save(path, "PNG")) return 3;

    const QByteArray utf8 = path.toUtf8();
    const MaiImageDecodeResult image = maiImageDecodeFile(utf8.constData(), 560, 400);
    if (!image.rgba || image.error_code != 0 || image.width != 560 || image.height < 300 ||
        image.height > 400 || image.stride != image.width * 4) {
        maiImageDecodeFree(image.rgba);
        return 4;
    }
    const bool red = image.rgba[0] == 255 && image.rgba[1] == 0 &&
                     image.rgba[2] == 0 && image.rgba[3] == 255;
    maiImageDecodeFree(image.rgba);
    if (!red) return 5;

    const QString invalid = temporary.filePath(QStringLiteral("invalid.png"));
    QFile file(invalid);
    if (!file.open(QIODevice::WriteOnly) || file.write("not an image") < 0) return 6;
    file.close();
    const QByteArray invalidUtf8 = invalid.toUtf8();
    const MaiImageDecodeResult rejected = maiImageDecodeFile(invalidUtf8.constData(), 560, 400);
    maiImageDecodeFree(rejected.rgba);
    return rejected.rgba == nullptr && rejected.error_code < 0 ? 0 : 7;
}
