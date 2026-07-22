import Compression
import CryptoKit
import XCTest
@testable import MaiChatCore

final class UserSigGeneratorTests: XCTestCase {
    func testGeneratesDeterministicTencentUserSigPayload() throws {
        let userSig = try TencentUserSigGenerator.generate(
            sdkAppID: 1_400_000_000,
            userID: "ios-master",
            secretKey: "test-secret",
            expireSeconds: 604_800,
            currentTime: 1_700_000_000
        )

        let payload = try DecodedUserSigPayload.decode(from: userSig)
        XCTAssertEqual(payload.version, "2.0")
        XCTAssertEqual(payload.identifier, "ios-master")
        XCTAssertEqual(payload.sdkAppID, 1_400_000_000)
        XCTAssertEqual(payload.expire, 604_800)
        XCTAssertEqual(payload.time, 1_700_000_000)
        XCTAssertEqual(payload.signature, expectedSignature())
    }

    func testGeneratedUserSigUsesTencentZlibWrapper() throws {
        let userSig = try TencentUserSigGenerator.generate(
            sdkAppID: 1_400_000_000,
            userID: "ios-master",
            secretKey: "test-secret",
            expireSeconds: 604_800,
            currentTime: 1_700_000_000
        )
        let base64 = userSig
            .replacingOccurrences(of: "*", with: "+")
            .replacingOccurrences(of: "-", with: "/")
            .replacingOccurrences(of: "_", with: "=")
        let compressed = try XCTUnwrap(Data(base64Encoded: base64))
        XCTAssertEqual(Array(compressed.prefix(2)), [0x78, 0x01])
        XCTAssertEqual(
            try DecodedUserSigPayload.adler32(
                DecodedUserSigPayload.inflateZlib(compressed)
            ),
            compressed.suffix(4)
        )
    }

    func testRejectsInvalidUserSigInputs() {
        XCTAssertThrowsError(
            try TencentUserSigGenerator.generate(
                sdkAppID: 0,
                userID: "ios-master",
                secretKey: "test-secret",
                expireSeconds: 604_800,
                currentTime: 1_700_000_000
            )
        )
        XCTAssertThrowsError(
            try TencentUserSigGenerator.generate(
                sdkAppID: 1_400_000_000,
                userID: "   ",
                secretKey: "test-secret",
                expireSeconds: 604_800,
                currentTime: 1_700_000_000
            )
        )
        XCTAssertThrowsError(
            try TencentUserSigGenerator.generate(
                sdkAppID: 1_400_000_000,
                userID: "ios-master",
                secretKey: "   ",
                expireSeconds: 604_800,
                currentTime: 1_700_000_000
            )
        )
    }

    private func expectedSignature() -> String {
        let contentToSign = """
        TLS.identifier:ios-master
        TLS.sdkappid:1400000000
        TLS.time:1700000000
        TLS.expire:604800

        """
        let key = SymmetricKey(data: Data("test-secret".utf8))
        let signature = HMAC<SHA256>.authenticationCode(
            for: Data(contentToSign.utf8),
            using: key
        )
        return Data(signature).base64EncodedString()
    }
}

private struct DecodedUserSigPayload: Decodable {
    let version: String
    let identifier: String
    let sdkAppID: Int
    let expire: Int
    let time: Int
    let signature: String

    enum CodingKeys: String, CodingKey {
        case version = "TLS.ver"
        case identifier = "TLS.identifier"
        case sdkAppID = "TLS.sdkappid"
        case expire = "TLS.expire"
        case time = "TLS.time"
        case signature = "TLS.sig"
    }

    static func decode(from userSig: String) throws -> DecodedUserSigPayload {
        let base64 = userSig
            .replacingOccurrences(of: "*", with: "+")
            .replacingOccurrences(of: "-", with: "/")
            .replacingOccurrences(of: "_", with: "=")
        let compressed = try XCTUnwrap(Data(base64Encoded: base64))
        let json = try inflateZlib(compressed)
        return try JSONDecoder().decode(DecodedUserSigPayload.self, from: json)
    }

    static func inflateZlib(_ input: Data) throws -> Data {
        guard input.count > 6 else {
            throw NSError(domain: "MaiChatTests", code: 1)
        }
        let header = UInt16(input[input.startIndex]) << 8 | UInt16(input[input.index(after: input.startIndex)])
        guard input[input.startIndex] == 0x78, header % 31 == 0 else {
            throw NSError(domain: "MaiChatTests", code: 2)
        }
        let rawDeflate = input.dropFirst(2).dropLast(4)
        let outputCapacity = max(4096, input.count * 16)
        var output = Data(count: outputCapacity)
        let decodedCount = output.withUnsafeMutableBytes { outputBuffer in
            rawDeflate.withUnsafeBytes { inputBuffer in
                compression_decode_buffer(
                    outputBuffer.bindMemory(to: UInt8.self).baseAddress!,
                    outputCapacity,
                    inputBuffer.bindMemory(to: UInt8.self).baseAddress!,
                    rawDeflate.count,
                    nil,
                    COMPRESSION_ZLIB
                )
            }
        }
        if decodedCount == 0 {
            throw NSError(domain: "MaiChatTests", code: 1)
        }
        return output.prefix(decodedCount)
    }

    static func adler32(_ input: Data) -> Data {
        var low: UInt32 = 1
        var high: UInt32 = 0

        for byte in input {
            low = (low + UInt32(byte)) % 65_521
            high = (high + low) % 65_521
        }

        let checksum = (high << 16) | low
        return Data([
            UInt8((checksum >> 24) & 0xff),
            UInt8((checksum >> 16) & 0xff),
            UInt8((checksum >> 8) & 0xff),
            UInt8(checksum & 0xff)
        ])
    }
}
