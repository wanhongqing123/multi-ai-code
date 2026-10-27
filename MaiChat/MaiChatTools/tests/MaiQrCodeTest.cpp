#include "MaiQrCode.h"

#include <QCoreApplication>
#include <QImage>
#include <QTemporaryDir>

#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <json.hpp>

#include "MaiFileSystem.h"

extern "C" {
#include <quirc.h>
}

namespace {

void require(bool value, const char* reason) {
    if (!value) {
        std::cerr << reason << '\n';
        std::exit(1);
    }
}

std::string scan(const std::string& path) {
    const QImage source(QString::fromUtf8(path.c_str()));
    require(!source.isNull(), "generated PNG cannot be decoded");
    const QImage grayscale = source.convertToFormat(QImage::Format_Grayscale8);
    std::unique_ptr<quirc, decltype(&quirc_destroy)> decoder(quirc_new(), quirc_destroy);
    require(decoder && quirc_resize(decoder.get(), grayscale.width(), grayscale.height()) == 0,
            "QR scanner cannot read PNG dimensions");
    int width = 0;
    int height = 0;
    unsigned char* pixels = quirc_begin(decoder.get(), &width, &height);
    require(pixels && width == grayscale.width() && height == grayscale.height(),
            "QR scanner cannot read PNG pixels");
    for (int row = 0; row < height; ++row)
        std::memcpy(pixels + row * width, grayscale.constScanLine(row), width);
    quirc_end(decoder.get());
    require(quirc_count(decoder.get()) == 1, "generated image is not one QR code");
    quirc_code code{};
    quirc_data data{};
    quirc_extract(decoder.get(), 0, &code);
    require(quirc_decode(&code, &data) == QUIRC_SUCCESS,
            "independent QR scanner cannot decode generated image");
    return std::string(reinterpret_cast<const char*>(data.payload),
                       std::size_t(data.payload_len));
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir workspace;
    require(workspace.isValid(), "temporary workspace is unavailable");
    const MaiFilePath directory = MaiFilePath::fromUtf8(
        workspace.path().toUtf8().toStdString());
    const MaiFilePath output = directory.append(MaiFilePath::fromUtf8("二维码.png"));
    const std::string content = "https://example.com/路径?key=测试";
    require(!maiGenerateQrCodePng(content, output).hasError(),
            "ordinary QR generator did not save a PNG");
    require(scan(output.toUtf8()) == content,
            "independent scanner recovered different UTF-8 text");
    require(maiGenerateQrCodePng("different", output).hasError(),
            "QR generator overwrote an existing output");
    require(scan(output.toUtf8()) == content,
            "rejected overwrite modified an existing image");
    const std::string longText(768, 'a');
    const MaiFilePath large = directory.append(MaiFilePath::fromUtf8("large.png"));
    require(!maiGenerateQrCodePng(longText, large).hasError(),
            "the largest supported QR text cannot be generated");
    require(scan(large.toUtf8()) == longText,
            "the largest supported QR text cannot be scanned");
    require(!maiEncodeQrCodePng(std::string(769, 'a')).isOk(),
            "QR generator accepted text beyond its documented limit");

    MaiToolRegistry tools;
    registerMaiQrCodeTools(tools);
    MaiTool* tool = tools.findForModel("maichat_generate_qr_code");
    require(tool && tool->requiresPerCallApproval("{}"),
            "QR Agent tool is missing or lacks write approval");
    require(nlohmann::json::parse(tool->parametersSchema(), nullptr, false).is_object(),
            "QR Agent tool schema is invalid");
    MaiToolContext context;
    require(tool->execute(R"({"text":"hello"})", context).hasError(),
            "QR Agent tool accepted an absent workspace");
    context.root = directory.toUtf8();
    require(tool->execute(R"({"text":""})", context).hasError() &&
            tool->execute(R"({"text":"hello","path":"other.png"})", context).hasError(),
            "QR Agent tool accepted invalid arguments");
    const MaiToolResult result = tool->execute(
        R"({"text":"https://maichat.example/hello"})", context);
    require(!result.hasError() && result.images().size() == 1 &&
            result.images()[0].mimeType == "image/png",
            "QR Agent tool did not return an image");
    const auto body = nlohmann::json::parse(result.output(), nullptr, false);
    require(body.is_object() && body.value("format", "") == "png" &&
            body.value("file_path", "") ==
                MaiFilePath::fromUtf8(result.images()[0].path).toGenericUtf8(),
            "QR Agent tool did not return its saved file path");
    require(scan(result.images()[0].path) == "https://maichat.example/hello",
            "QR Agent image does not contain the requested URL");
}
