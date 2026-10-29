// 文件作用：验证普通 PDF 加密 API 的密码、原子发布、取消和旧文件保护契约。
// 独立查看器的正确/错误密码读取由外部验收补充；这里检查 qpdf 生成的真实 PDF 而非桩输出。
#include "MaiPdfDocumentTools.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiThread.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 最小单页 PDF 明文含可检索标记，额外随机流用于取消期间确实存在进行中的写入。
std::string makePdf(std::size_t payloadBytes = 0) {
    const std::string content = "BT /F1 12 Tf 30 100 Td (MAICHAT_PDF_PRIVATE_SENTINEL) Tj ET\n";
    std::vector<std::string> objects{
        "<< /Type /Catalog /Pages 2 0 R /MaiPayload 6 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Resources << /Font << /F1 4 0 R >> "
        ">> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream"};
    std::string payload(payloadBytes, 'x');
    std::uint32_t state = 0x12345678;
    for (char& byte : payload) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<char>(33 + state % 90);
    }
    objects.push_back("<< /Length " + std::to_string(payload.size()) + " >>\nstream\n" + payload +
                      "\nendstream");
    std::string pdf = "%PDF-1.7\n";
    std::vector<std::size_t> offsets{0};
    for (std::size_t index = 0; index < objects.size(); ++index) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(index + 1) + " 0 obj\n" + objects[index] + "\nendobj\n";
    }
    const std::size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (std::size_t index = 1; index < offsets.size(); ++index) {
        char row[32]{};
        std::snprintf(row, sizeof(row), "%010llu 00000 n \n",
                      static_cast<unsigned long long>(offsets[index]));
        pdf += row;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

std::string read(const MaiFilePath& path) {
    std::string result;
    require(!MaiFileSystem::readFile(path, result), "read failed");
    return result;
}

std::size_t stagingCount(const MaiFilePath& root) {
    std::size_t count = 0;
    MaiFileSystem::walk(root, [&](const MaiFileEntry& entry) {
        if (entry.nameUtf8.find(".maichat-pdf-") == 0) ++count;
        return MaiWalkAction::Continue;
    });
    return count;
}

void run(const MaiFilePath& root) {
    const auto source = root.append(MaiFilePath::fromUtf8("source.pdf"));
    const auto output = root.append(MaiFilePath::fromUtf8("protected-unicode-path.pdf"));
    const std::string original = makePdf();
    require(!MaiFileSystem::writeFile(source, original), "source write failed");
    // 只用公开测试口令，不把真实用户密码写入测试、参数日志或产物说明。
    const MaiPdfEncryptionOptions options{"Test-PDF-2026!", false};
    const auto encrypted = maiEncryptPdfFile(source, output, options);
    if (!encrypted) std::printf("core error: %s\n", encrypted.error().message().c_str());
    require(encrypted.isOk(), "encryption failed");
    require(encrypted.value().pageCount == 1 && encrypted.value().bytes > 8,
            "result metadata mismatch");
    require(read(source) == original, "source was changed");
    const std::string protectedBytes = read(output);
    require(protectedBytes.find("/R 6") != std::string::npos &&
                protectedBytes.find("/V 5") != std::string::npos,
            "not AES-256 R6");
    require(protectedBytes.find("MAICHAT_PDF_PRIVATE_SENTINEL") == std::string::npos,
            "plaintext content leaked");
    require(protectedBytes.find(options.userPassword) == std::string::npos,
            "password leaked to PDF bytes");
    require(!maiEncryptPdfFile(source, output, options), "existing output was overwritten");
    require(read(output) == protectedBytes, "failed operation changed old output");

    const auto unicode = root.append(MaiFilePath::fromUtf8("unicode-password.pdf"));
    require(
        maiEncryptPdfFile(source, unicode,
                          MaiPdfEncryptionOptions{"T\xc3\xa9st-\xe5\xaf\x86\xe7\xa0\x81", false})
            .isOk(),
        "UTF-8 password failed");
    for (const std::string password : {std::string{}, std::string(128, 'a'),
                                       std::string("bad\0value", 9), std::string("\xff")}) {
        const auto invalid = root.append(MaiFilePath::fromUtf8("invalid-password.pdf"));
        require(!maiEncryptPdfFile(source, invalid, MaiPdfEncryptionOptions{password, false}),
                "invalid password accepted");
        require(!MaiFileSystem::exists(invalid), "invalid password published output");
    }
    const auto malformed = root.append(MaiFilePath::fromUtf8("malformed.pdf"));
    require(!MaiFileSystem::writeFile(malformed, "%PDF-1.7\nnot a real document\n"),
            "malformed write failed");
    require(
        !maiEncryptPdfFile(malformed, output, MaiPdfEncryptionOptions{options.userPassword, true}),
        "malformed source accepted");
    require(read(output) == protectedBytes, "malformed source replaced old output");
    require(stagingCount(root) == 0, "failure left staging file");

    // 原地保护只在显式 replaceExisting 下允许，提交前取消和格式错误均不能删旧 PDF。
    std::atomic<bool> cancel{true};
    const auto canceled = root.append(MaiFilePath::fromUtf8("canceled.pdf"));
    const auto preCanceled = maiEncryptPdfFile(source, canceled, options, &cancel);
    require(!preCanceled && preCanceled.error().code() == MaiErrorCode::Canceled,
            "pre-cancel not honored");
    require(!MaiFileSystem::exists(canceled), "pre-cancel published output");

    const auto large = root.append(MaiFilePath::fromUtf8("large.pdf"));
    require(!MaiFileSystem::writeFile(large, makePdf(8u * 1024 * 1024)), "large write failed");
    cancel.store(false);
    bool observedStage = false;
    bool leakedPlaintext = false;
    std::atomic<bool> finished{false};
    std::thread observer([&] {
        MaiThread::setCurrentName("MaiPdfCancelProbe");
        while (!finished.load()) {
            if (stagingCount(root) > 0) {
                observedStage = true;
                cancel.store(true);
                break;
            }
            if (MaiFileSystem::exists(canceled))
                leakedPlaintext =
                    read(canceled).find("MAICHAT_PDF_PRIVATE_SENTINEL") != std::string::npos;
            std::this_thread::yield();
        }
    });
    const auto midCanceled = maiEncryptPdfFile(large, canceled, options, &cancel);
    finished.store(true);
    observer.join();
    require(observedStage, "cancel test did not observe real staging file");
    require(!midCanceled && midCanceled.error().code() == MaiErrorCode::Canceled,
            "in-progress cancel not honored");
    require(!leakedPlaintext && !MaiFileSystem::exists(canceled) && stagingCount(root) == 0,
            "cancel left final/plaintext/staging output");
    require(read(source) == original, "cancel changed source");

    require(maiEncryptPdfFile(source, source, MaiPdfEncryptionOptions{options.userPassword, true})
                .isOk(),
            "atomic in-place protection failed");
    require(read(source).find("MAICHAT_PDF_PRIVATE_SENTINEL") == std::string::npos,
            "in-place result is plaintext");
    require(!maiEncryptPdfFile(source, canceled, options), "already encrypted input accepted");
    require(stagingCount(root) == 0, "success left staging file");
}

}  // namespace

int main() {
    const char* retained = std::getenv("MAI_PDF_TEST_ARTIFACTS");
    const MaiFilePath root =
        retained ? MaiFilePath::fromUtf8(retained)
                 : MaiFileSystem::temporaryDirectory().append(
                       MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_pdf_encrypt_")));
    try {
        require(!MaiFileSystem::createDirectories(root), "workspace create failed");
        run(root);
        if (!retained) MaiFileSystem::removeRecursively(root);
        std::puts("MaiPdfDocumentToolsTests PASS");
        return 0;
    } catch (const std::exception& error) {
        std::printf("MaiPdfDocumentToolsTests FAIL: %s\n", error.what());
        if (!retained) MaiFileSystem::removeRecursively(root);
        return 1;
    }
}
