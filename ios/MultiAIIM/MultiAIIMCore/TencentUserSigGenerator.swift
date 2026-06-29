import Compression
import CryptoKit
import Foundation

public enum TencentUserSigError: Error, Equatable, LocalizedError {
    case invalidSDKAppID
    case blankUserID
    case blankSecretKey
    case compressionFailed

    public var errorDescription: String? {
        switch self {
        case .invalidSDKAppID:
            return "SDKAppID is required to generate UserSig"
        case .blankUserID:
            return "UserID is required to generate UserSig"
        case .blankSecretKey:
            return "SecretKey is required to generate UserSig"
        case .compressionFailed:
            return "Failed to deflate Tencent UserSig payload"
        }
    }
}

public enum TencentUserSigGenerator {
    public static func generate(
        sdkAppID: Int,
        userID: String,
        secretKey: String,
        expireSeconds: Int = 604_800,
        currentTime: Int = Int(Date().timeIntervalSince1970)
    ) throws -> String {
        let cleanUserID = userID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanSecretKey = secretKey.trimmingCharacters(in: .whitespacesAndNewlines)
        guard sdkAppID > 0 else { throw TencentUserSigError.invalidSDKAppID }
        guard !cleanUserID.isEmpty else { throw TencentUserSigError.blankUserID }
        guard !cleanSecretKey.isEmpty else { throw TencentUserSigError.blankSecretKey }

        let contentToSign =
            "TLS.identifier:\(cleanUserID)\n" +
            "TLS.sdkappid:\(sdkAppID)\n" +
            "TLS.time:\(currentTime)\n" +
            "TLS.expire:\(expireSeconds)\n"
        let signature = hmacSHA256Base64(secretKey: cleanSecretKey, content: contentToSign)
        let payload: [String: Any] = [
            "TLS.ver": "2.0",
            "TLS.identifier": cleanUserID,
            "TLS.sdkappid": sdkAppID,
            "TLS.expire": expireSeconds,
            "TLS.time": currentTime,
            "TLS.sig": signature
        ]
        let jsonData = try JSONSerialization.data(withJSONObject: payload)
        let compressed = try deflateZlib(jsonData)
        return compressed
            .base64EncodedString()
            .replacingOccurrences(of: "+", with: "*")
            .replacingOccurrences(of: "/", with: "-")
            .replacingOccurrences(of: "=", with: "_")
    }

    private static func hmacSHA256Base64(secretKey: String, content: String) -> String {
        let key = SymmetricKey(data: Data(secretKey.utf8))
        let signature = HMAC<SHA256>.authenticationCode(
            for: Data(content.utf8),
            using: key
        )
        return Data(signature).base64EncodedString()
    }

    private static func deflateZlib(_ input: Data) throws -> Data {
        let rawDeflate = try deflateRaw(input)
        var output = Data([0x78, 0x01])
        output.append(rawDeflate)
        output.append(adler32(input))
        return output
    }

    private static func deflateRaw(_ input: Data) throws -> Data {
        let outputCapacity = max(64, input.count + 64)
        var output = Data(count: outputCapacity)
        let encodedCount = output.withUnsafeMutableBytes { outputBuffer in
            input.withUnsafeBytes { inputBuffer in
                compression_encode_buffer(
                    outputBuffer.bindMemory(to: UInt8.self).baseAddress!,
                    outputCapacity,
                    inputBuffer.bindMemory(to: UInt8.self).baseAddress!,
                    input.count,
                    nil,
                    COMPRESSION_ZLIB
                )
            }
        }
        guard encodedCount > 0 else { throw TencentUserSigError.compressionFailed }
        return output.prefix(encodedCount)
    }

    private static func adler32(_ input: Data) -> Data {
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
