#include "MaiOpenSslTool.h"
#if MAI_HAS_OPENSSL_CLI
#include "MaiOpenSslCliTool.h"
#endif

#include <cstdio>
#include <string>

#include <json.hpp>

namespace {

int failures = 0;
#define CHECK(value)                                                    \
    do {                                                                \
        if (!(value)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (false)

}  // namespace

int main() {
    const auto sha256 = maiOpenSslDigestHex("abc", "sha256");
    CHECK(sha256);
    if (sha256)
        CHECK(sha256.value() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(!maiOpenSslDigestHex("abc", "unknown"));

    MaiToolContext context;
    context.root = MAI_OPENSSL_TEST_CERT_DIR;
    auto digest = makeMaiOpenSslDigestTool();
    auto certificate = makeMaiOpenSslCertificateTool();
    const auto hashed = digest->execute(R"({"text":"abc"})", context);
    CHECK(!hashed.hasError());
    if (!hashed.hasError())
        CHECK(nlohmann::json::parse(hashed.output()).value("hex", std::string{}) == sha256.value());
    CHECK(digest->execute(R"({"path":"openssl-ee-cert.pem","text":"abc"})", context).hasError());
    CHECK(certificate->execute(R"({"path":"../../../../etc/passwd"})", context).hasError());
    const auto info = certificate->execute(R"({"path":"openssl-ee-cert.pem"})", context);
    CHECK(!info.hasError());
    if (!info.hasError()) {
        const auto result = nlohmann::json::parse(info.output());
        CHECK(result.value("subject", std::string{}).find("server.example") != std::string::npos);
        CHECK(result.value("issuer", std::string{}).find("CA") != std::string::npos);
        CHECK(result.value("sha256", std::string{}).size() == 64);
    }
#if MAI_HAS_OPENSSL_CLI
    const auto runVersion = maiRunOpenSslCommand({"version"}, context);
    CHECK(runVersion);
    if (runVersion) CHECK(runVersion.value().exitCode == 0);
    const auto runDigest = maiRunOpenSslCommand({"dgst", "-sha256", "openssl-ee-cert.pem"}, context);
    CHECK(runDigest);
    if (runDigest) CHECK(runDigest.value().exitCode == 0);
    CHECK(!maiRunOpenSslCommand({"dgst", "-sha256", "../../../../etc/passwd"}, context));
    const auto runAgain = maiRunOpenSslCommand({"x509", "-in", "openssl-ee-cert.pem", "-subject"}, context);
    CHECK(runAgain);
    if (runAgain) CHECK(runAgain.value().exitCode == 0);
    CHECK(!maiRunOpenSslCommand({"genpkey", "-algorithm", "RSA"}, context));
#endif
    return failures ? 1 : 0;
}
