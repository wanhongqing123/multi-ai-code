// 文件作用：复用进程内 qpdf 加密作业，验证后原子交付带打开密码的 PDF。
// 密码只在普通 API 的当前调用内使用；不经过模型参数、日志或外部命令行。
#include "MaiPdfDocumentTools.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include <openssl/rand.h>
#include <qpdf/Pipeline.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFJob.hh>
#include <qpdf/QPDFLogger.hh>

#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr std::uint64_t kMaxPdfBytes = 100u * 1024 * 1024;

// qpdf 在内部使用异常中止 pipeline。异常只在这一实现文件内传播，公开 API 全部转换成 MaiError。
class MaiPdfFailure final : public std::exception {
public:
    MaiPdfFailure(MaiErrorCode code, const char* message) : mError(code, message) {}
    const char* what() const noexcept override {
        return "PDF operation failed";
    }
    const MaiError& error() const {
        return mError;
    }

private:
    MaiError mError;
};

void checkCancellation(const std::atomic<bool>* cancel) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
        throw MaiPdfFailure(MaiErrorCode::Canceled, "PDF encryption was canceled");
}

bool isValidPassword(const std::string& password) {
    if (password.empty() || password.size() > 127 || password.find('\0') != std::string::npos)
        return false;
    // R6 接收 UTF-8；先拒绝坏编码及超过规范字节上限的输入，不能让引擎静默截断密码。
    for (std::size_t index = 0; index < password.size();) {
        const auto first = static_cast<unsigned char>(password[index++]);
        if (first < 0x80) continue;
        unsigned count = 0;
        std::uint32_t scalar = first;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1;
            scalar &= 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            scalar &= 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            scalar &= 7;
            minimum = 0x10000;
        } else
            return false;
        if (index + count > password.size()) return false;
        while (count-- != 0) {
            const auto byte = static_cast<unsigned char>(password[index++]);
            if ((byte & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (byte & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            return false;
    }
    return true;
}

std::string ownerPassword() {
    std::array<unsigned char, 32> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        throw MaiPdfFailure(MaiErrorCode::Internal,
                            "PDF encryption credentials could not be created");
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(random.size() * 2, '0');
    for (std::size_t index = 0; index < random.size(); ++index) {
        result[index * 2] = digits[random[index] >> 4];
        result[index * 2 + 1] = digits[random[index] & 15];
    }
    return result;
}

class MaiPdfOwnedOutput final {
public:
    explicit MaiPdfOwnedOutput(const MaiFilePath& parent) {
        // qpdf 的二进制输出走保留着的原生文件句柄，而不是按窄字符路径再次 fopen。
        // 这样既覆盖 Windows 中文路径，也不会在校验临时名称后重新打开到被替换的链接。
        for (int attempt = 0; attempt < 4; ++attempt) {
            mPath = parent.append(
                MaiFilePath::fromUtf8(".maichat-pdf-" + MaiIdGenerator::generate("tmp") + ".tmp"));
#if defined(_WIN32)
            HANDLE handle = CreateFileW(mPath.value().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                        nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)
                    continue;
                mPath = {};
                throw MaiPdfFailure(MaiErrorCode::Internal,
                                    "PDF staging file could not be created");
            }
            const int descriptor =
                _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_BINARY);
            if (descriptor < 0) {
                CloseHandle(handle);
                cleanup();
                throw MaiPdfFailure(MaiErrorCode::Internal,
                                    "PDF staging handle could not be opened");
            }
            mFile = _fdopen(descriptor, "wb");
            if (mFile == nullptr) _close(descriptor);
#else
            const int descriptor = open(mPath.value().c_str(),
                                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (descriptor < 0) {
                if (errno == EEXIST) continue;
                mPath = {};
                throw MaiPdfFailure(MaiErrorCode::Internal,
                                    "PDF staging file could not be created");
            }
            mFile = fdopen(descriptor, "wb");
            if (mFile == nullptr) close(descriptor);
#endif
            if (mFile == nullptr) {
                cleanup();
                throw MaiPdfFailure(MaiErrorCode::Internal,
                                    "PDF staging stream could not be opened");
            }
            return;
        }
        mPath = {};
        throw MaiPdfFailure(MaiErrorCode::Busy, "PDF staging file names are busy");
    }

    ~MaiPdfOwnedOutput() {
        cleanup();
    }
    MaiPdfOwnedOutput(const MaiPdfOwnedOutput&) = delete;
    MaiPdfOwnedOutput& operator=(const MaiPdfOwnedOutput&) = delete;
    const MaiFilePath& path() const {
        return mPath;
    }
    std::FILE* file() const {
        return mFile;
    }

    void flushAndClose() {
        if (mFile == nullptr) return;
        bool ok = std::fflush(mFile) == 0;
#if defined(_WIN32)
        if (ok) ok = _commit(_fileno(mFile)) == 0;
#else
        if (ok) ok = fsync(fileno(mFile)) == 0;
#endif
        if (std::fclose(mFile) != 0) ok = false;
        mFile = nullptr;
        if (!ok)
            throw MaiPdfFailure(MaiErrorCode::Internal, "PDF staging file could not be flushed");
    }

private:
    void cleanup() {
        if (mFile != nullptr) std::fclose(mFile);
        mFile = nullptr;
        if (!mPath.isEmpty()) MaiFileSystem::removeFile(mPath);
        mPath = {};
    }

    MaiFilePath mPath;
    std::FILE* mFile = nullptr;
};

class MaiPdfOutputPipeline final : public Pipeline {
public:
    MaiPdfOutputPipeline(MaiPdfOwnedOutput& output, const std::atomic<bool>* cancel)
        : Pipeline("maichat-pdf-output", nullptr), mOutput(output), mCancel(cancel) {}

    void write(const unsigned char* data, std::size_t length) override {
        checkCancellation(mCancel);
        if (length > kMaxPdfBytes - mBytes)
            throw MaiPdfFailure(MaiErrorCode::InvalidInput, "Encrypted PDF exceeds 100 MB");
        if (std::fwrite(data, 1, length, mOutput.file()) != length)
            throw MaiPdfFailure(MaiErrorCode::Internal, "Encrypted PDF could not be written");
        mBytes += length;
    }

    void finish() override {
        if (std::fflush(mOutput.file()) != 0)
            throw MaiPdfFailure(MaiErrorCode::Internal, "Encrypted PDF could not be flushed");
    }

private:
    MaiPdfOwnedOutput& mOutput;
    const std::atomic<bool>* mCancel;
    std::uint64_t mBytes = 0;
};

std::shared_ptr<QPDFLogger> privateLogger() {
    // 上游用法错误可能包含配置文本。密码不进入进程日志，也不把 exception.what() 回传给模型。
    const auto logger = QPDFLogger::create();
    logger->setInfo(logger->discard());
    logger->setWarn(logger->discard());
    logger->setError(logger->discard());
    return logger;
}

void verifyEncryptedOutput(const MaiFilePath& path, const std::string& password,
                           std::uint64_t pages, const std::atomic<bool>* cancel) {
    checkCancellation(cancel);
    QPDF verified;
    verified.setLogger(privateLogger());
    const std::string filename = path.toUtf8();
    verified.processFile(filename.c_str(), password.c_str());
    int revision = 0, permissions = 0, version = 0;
    QPDF::encryption_method_e streams{}, strings{}, files{};
    if (!verified.isEncrypted(revision, permissions, version, streams, strings, files) ||
        revision != 6 || version != 5 || streams != QPDF::e_aesv3 || strings != QPDF::e_aesv3 ||
        !verified.userPasswordMatched() || verified.getAllPages().size() != pages)
        throw MaiPdfFailure(MaiErrorCode::Internal, "Encrypted PDF validation failed");

    // 非空打开密码必须真的阻止无密码读取，不能只写 owner 权限标志冒充密码保护。
    bool required = false;
    try {
        QPDF locked;
        locked.setLogger(privateLogger());
        locked.processFile(filename.c_str(), "");
    } catch (const QPDFExc& error) {
        if (error.getErrorCode() != qpdf_e_password) throw;
        required = true;
    }
    if (!required)
        throw MaiPdfFailure(MaiErrorCode::Internal, "Encrypted PDF did not require a password");
    checkCancellation(cancel);
}

MaiError replaceOutput(const MaiFilePath& source, const MaiFilePath& destination) {
    // 临时文件与目标同目录，最后一步仅做同文件系统原子提交，不先删旧文件。
#if defined(_WIN32)
    if (!MoveFileExW(source.value().c_str(), destination.value().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return {MaiErrorCode::Internal, "Encrypted PDF could not replace the destination"};
#else
    if (rename(source.value().c_str(), destination.value().c_str()) != 0)
        return {MaiErrorCode::Internal, "Encrypted PDF could not replace the destination"};
    const int parent =
        open(destination.dirName().value().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent >= 0) {
        fsync(parent);
        close(parent);
    }
#endif
    return {};
}

}  // namespace

MaiResult<MaiPdfEncryptionResult> maiEncryptPdfFile(const MaiFilePath& sourcePath,
                                                    const MaiFilePath& outputPath,
                                                    const MaiPdfEncryptionOptions& options,
                                                    const std::atomic<bool>* cancel) {
    try {
        checkCancellation(cancel);
        if (!isValidPassword(options.userPassword))
            return {MaiErrorCode::InvalidInput,
                    "PDF password must be nonempty UTF-8 without NUL and at most 127 bytes"};
        if (!sourcePath.isAbsolute() || !outputPath.isAbsolute() ||
            MaiFileSystem::isSymbolicLink(sourcePath) ||
            MaiFileSystem::isSymbolicLink(outputPath) || MaiFileSystem::isDirectory(sourcePath) ||
            MaiFileSystem::isDirectory(outputPath))
            return {MaiErrorCode::InvalidInput,
                    "PDF paths must be absolute regular file paths without symbolic links"};
        const MaiFilePath input = MaiFileSystem::resolve(sourcePath);
        const MaiFilePath output = MaiFileSystem::resolve(outputPath);
        if (input.isEmpty() || output.isEmpty() || !MaiFileSystem::isDirectory(output.dirName()))
            return {MaiErrorCode::InvalidInput,
                    "PDF paths or destination directory are unavailable"};
        if (MaiFileSystem::exists(output) && !options.replaceExisting)
            return {MaiErrorCode::InvalidInput, "PDF output already exists; choose a new filename"};
        std::uint64_t sourceBytes = 0;
        std::string header;
        if (!MaiFileSystem::fileSize(input, sourceBytes))
            return {MaiErrorCode::NotFound, "PDF source does not exist"};
        if (sourceBytes < 8 || sourceBytes > kMaxPdfBytes ||
            MaiFileSystem::readFile(input, header, 8).hasError() ||
            header.compare(0, 5, "%PDF-") != 0)
            return {MaiErrorCode::InvalidInput, "PDF source must be a PDF file of at most 100 MB"};

        MaiPdfOwnedOutput staged(output.dirName());
        std::uint64_t pages = 0;
        {
            // QPDFJob 是上游 CLI 共用的可重入命令实现，不调用带 exit()/stdout 的 main。
            // 私有 save pipeline 持有排他句柄，输出 "-" 在这里不会触及真实 stdout。
            QPDFJob job;
            const auto logger = privateLogger();
            const auto sink = std::make_shared<MaiPdfOutputPipeline>(staged, cancel);
            logger->setSave(sink, false);
            job.setLogger(logger);
            // 上游 QUtil::safe_fopen 在 Windows 将 UTF-8 转 UTF-16 后调用 _wfopen_s，
            // 这里的 UTF-8 是该库明确约定的边界，不经过系统 ANSI 代码页。
            job.config()->inputFile(input.toUtf8())->outputFile("-")->progress();
            job.config()->passwordMode("unicode");
            job.config()->encrypt(256, options.userPassword, ownerPassword())->endEncrypt();
            job.registerProgressReporter([cancel](int) { checkCancellation(cancel); });
            auto document = job.createQPDF();
            if (!document || document->isEncrypted())
                return {MaiErrorCode::InvalidInput, "PDF source must be unencrypted"};
            pages = document->getAllPages().size();
            if (pages == 0) return {MaiErrorCode::InvalidInput, "PDF source has no pages"};
            checkCancellation(cancel);
            job.writeQPDF(*document);
        }
        staged.flushAndClose();
        std::uint64_t encryptedBytes = 0;
        if (!MaiFileSystem::fileSize(staged.path(), encryptedBytes) || encryptedBytes < 8 ||
            encryptedBytes > kMaxPdfBytes)
            return {MaiErrorCode::Internal, "Encrypted PDF output is incomplete"};
        verifyEncryptedOutput(staged.path(), options.userPassword, pages, cancel);
        checkCancellation(cancel);
        const MaiError committed = options.replaceExisting
                                       ? replaceOutput(staged.path(), output)
                                       : MaiFileSystem::publishNewFile(staged.path(), output);
        if (committed.hasError()) return committed;
        // 这是唯一提交点。取消若在此后到达，不回滚或删除已经交付的目标文件。
        return MaiPdfEncryptionResult{output, encryptedBytes, pages};
    } catch (const MaiPdfFailure& failure) {
        return failure.error();
    } catch (const QPDFExc&) {
        return {MaiErrorCode::InvalidInput, "PDF document could not be parsed or encrypted"};
    } catch (const std::exception&) {
        return {MaiErrorCode::Internal, "PDF encryption failed"};
    } catch (...) {
        return {MaiErrorCode::Internal, "PDF encryption failed"};
    }
}
