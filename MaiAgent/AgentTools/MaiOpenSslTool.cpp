#include "MaiOpenSslTool.h"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using Json = nlohmann::json;

const EVP_MD* digestAlgorithm(const std::string& algorithm) {
    if (algorithm == "sha256") return EVP_sha256();
    if (algorithm == "sha512") return EVP_sha512();
    if (algorithm == "sha3-256") return EVP_sha3_256();
    if (algorithm == "blake2b-512") return EVP_blake2b512();
    if (algorithm == "md5") return EVP_md5();
    return nullptr;
}

std::string hex(const unsigned char* bytes, std::size_t count) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(count * 2);
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back(alphabet[bytes[index] >> 4]);
        result.push_back(alphabet[bytes[index] & 15]);
    }
    return result;
}

std::string bioText(BIO* bio) {
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    return data && length > 0 ? std::string(data, static_cast<std::size_t>(length)) : std::string{};
}

std::string nameText(X509_NAME* name) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!bio || X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_RFC2253) < 0) return {};
    return bioText(bio.get());
}

std::string timeText(const ASN1_TIME* value) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!bio || ASN1_TIME_print(bio.get(), value) != 1) return {};
    return bioText(bio.get());
}

MaiResult<std::string> readWorkspaceFile(const MaiToolContext& context,
                                         const std::string& candidate, std::uint64_t maximumBytes) {
    const std::string path = context.resolvePath(candidate);
    if (path.empty()) return {MaiErrorCode::InvalidInput, "file path is outside the workspace"};
    const MaiFilePath file = MaiFilePath::fromUtf8(path);
    std::uint64_t size = 0;
    if (!MaiFileSystem::fileSize(file, size) || size > maximumBytes)
        return {MaiErrorCode::InvalidInput, "file is missing or exceeds the size limit"};
    std::string bytes;
    const MaiError result = MaiFileSystem::readFile(file, bytes, maximumBytes);
    if (result.hasError()) return result;
    return bytes;
}

class DigestTool final : public MaiTool {
public:
    std::string name() const override {
        return "crypto_digest";
    }
    std::string description() const override {
        return "Compute a SHA-256, SHA-512, SHA3-256, BLAKE2b-512, or MD5 checksum of a "
               "workspace file or short text using OpenSSL. MD5 is for legacy checksum matching, "
               "not security. Specify exactly one of path or text. File limit 64 MB.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string"},"text":{"type":"string"},"algorithm":{"type":"string","enum":["sha256","sha512","sha3-256","blake2b-512","md5"]}},"oneOf":[{"required":["path"]},{"required":["text"]}],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || (args.contains("path") && !args["path"].is_string()) ||
            (args.contains("text") && !args["text"].is_string()) ||
            (args.contains("algorithm") && !args["algorithm"].is_string()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid digest arguments");
        const std::string path = args.value("path", std::string{});
        const std::string text = args.value("text", std::string{});
        const std::string algorithm = args.value("algorithm", std::string("sha256"));
        if (args.contains("path") == args.contains("text") ||
            (args.contains("path") && path.empty()) || digestAlgorithm(algorithm) == nullptr ||
            text.size() > 1024 * 1024)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "specify one path or text and a supported algorithm");
        const auto data = args.contains("text")
                              ? MaiResult<std::string>(text)
                              : readWorkspaceFile(context, path, 64 * 1024 * 1024);
        if (!data) return MaiToolResult::failure(data.error().code(), data.error().message());
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "digest was canceled");
        const auto digest = maiOpenSslDigestHex(data.value(), algorithm);
        if (!digest) return MaiToolResult::failure(digest.error().code(), digest.error().message());
        return MaiToolResult::success(Json{
            {"algorithm", algorithm},
            {"hex", digest.value()},
            {"bytes", data.value().size()},
            {"openssl_version",
             OPENSSL_VERSION_TEXT}}.dump());
    }
};

class CertificateTool final : public MaiTool {
public:
    std::string name() const override {
        return "crypto_certificate_info";
    }
    std::string description() const override {
        return "Inspect a PEM or DER X.509 certificate file in the workspace using OpenSSL. "
               "Returns subject, issuer, validity, SHA-256 fingerprint, public key type, "
               "and DNS names. Does not change the certificate or contact a network service.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("path", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "certificate path is required");
        const std::string path = args["path"].get<std::string>();
        const auto bytes = readWorkspaceFile(context, path, 1024 * 1024);
        if (!bytes) return MaiToolResult::failure(bytes.error().code(), bytes.error().message());
        std::unique_ptr<BIO, decltype(&BIO_free)> bio(
            BIO_new_mem_buf(bytes.value().data(), static_cast<int>(bytes.value().size())),
            BIO_free);
        if (!bio) return MaiToolResult::failure(MaiErrorCode::Internal, "OpenSSL BIO failed");
        X509* rawCertificate = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
        if (!rawCertificate) {
            const unsigned char* cursor =
                reinterpret_cast<const unsigned char*>(bytes.value().data());
            rawCertificate = d2i_X509(nullptr, &cursor, static_cast<long>(bytes.value().size()));
        }
        std::unique_ptr<X509, decltype(&X509_free)> certificate(rawCertificate, X509_free);
        if (!certificate)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "file is not a PEM or DER X.509 certificate");
        std::array<unsigned char, EVP_MAX_MD_SIZE> fingerprint{};
        unsigned int fingerprintBytes = 0;
        if (X509_digest(certificate.get(), EVP_sha256(), fingerprint.data(), &fingerprintBytes) !=
            1)
            return MaiToolResult::failure(MaiErrorCode::Internal, "certificate fingerprint failed");
        Json dnsNames = Json::array();
        auto* rawNames = static_cast<GENERAL_NAMES*>(
            X509_get_ext_d2i(certificate.get(), NID_subject_alt_name, nullptr, nullptr));
        std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)> names(rawNames,
                                                                            GENERAL_NAMES_free);
        if (names) {
            for (int index = 0; index < sk_GENERAL_NAME_num(names.get()) && index < 100; ++index) {
                const GENERAL_NAME* name = sk_GENERAL_NAME_value(names.get(), index);
                if (name->type != GEN_DNS) continue;
                const unsigned char* data = ASN1_STRING_get0_data(name->d.dNSName);
                const int length = ASN1_STRING_length(name->d.dNSName);
                if (!data || length <= 0 || length > 253) continue;
                std::string dns(reinterpret_cast<const char*>(data),
                                static_cast<std::size_t>(length));
                const bool printable = std::all_of(
                    dns.begin(), dns.end(), [](unsigned char ch) { return ch >= 33 && ch <= 126; });
                if (printable) dnsNames.push_back(std::move(dns));
            }
        }
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> publicKey(
            X509_get_pubkey(certificate.get()), EVP_PKEY_free);
        Json output = {
            {"path", path},
            {"subject", nameText(X509_get_subject_name(certificate.get()))},
            {"issuer", nameText(X509_get_issuer_name(certificate.get()))},
            {"not_before", timeText(X509_get0_notBefore(certificate.get()))},
            {"not_after", timeText(X509_get0_notAfter(certificate.get()))},
            {"expired", X509_cmp_current_time(X509_get0_notAfter(certificate.get())) < 0},
            {"sha256", hex(fingerprint.data(), fingerprintBytes)},
            {"dns_names", dnsNames},
            {"is_ca", X509_check_ca(certificate.get()) > 0},
            {"openssl_version", OPENSSL_VERSION_TEXT}};
        if (publicKey) {
            output["public_key_algorithm"] = OBJ_nid2sn(EVP_PKEY_base_id(publicKey.get()));
            output["public_key_bits"] = EVP_PKEY_bits(publicKey.get());
        }
        return MaiToolResult::success(output.dump(-1, ' ', false, Json::error_handler_t::replace));
    }
};

}  // namespace

MaiResult<std::string> maiOpenSslDigestHex(const std::string& bytes, const std::string& algorithm) {
    const EVP_MD* method = digestAlgorithm(algorithm);
    if (!method) return {MaiErrorCode::InvalidInput, "unsupported digest algorithm"};
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, method, nullptr) != 1)
        return {MaiErrorCode::Internal, "OpenSSL digest failed"};
    return hex(digest.data(), length);
}

std::unique_ptr<MaiTool> makeMaiOpenSslDigestTool() {
    return std::make_unique<DigestTool>();
}

std::unique_ptr<MaiTool> makeMaiOpenSslCertificateTool() {
    return std::make_unique<CertificateTool>();
}
