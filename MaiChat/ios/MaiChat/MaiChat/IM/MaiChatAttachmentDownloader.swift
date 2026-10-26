// Tencent IM supplies signed media URLs. The portable MaiChat libcurl core owns
// the transfer, size limit, TLS verification, cancellation and atomic file commit.
import Foundation

struct MaiChatAttachmentDownloadResult: Sendable {
    let status: Int32
    let httpStatus: Int
    let curlCode: Int32
    let bytes: UInt64

    var succeeded: Bool { status == Int32(MAI_DOWNLOAD_OK.rawValue) }
}

private final class MaiChatAttachmentCancelToken: @unchecked Sendable {
    let pointer: OpaquePointer

    init?() {
        guard let pointer = mai_download_cancel_create() else { return nil }
        self.pointer = pointer
    }

    deinit { mai_download_cancel_destroy(pointer) }

    func cancel() { mai_download_cancel_request(pointer) }
}

final class MaiChatAttachmentProxyContext {
    let buffer = UnsafeMutablePointer<CChar>.allocate(capacity: 4096)

    init() { buffer.initialize(repeating: 0, count: 4096) }
    deinit { buffer.deallocate() }

    func resolve(_ url: UnsafePointer<CChar>?, output: UnsafeMutablePointer<MaiDownloadProxy>?) -> Int32 {
        guard let url, let output,
              maiChatHttpProxyForURL(url, buffer, 4096) == 1 else { return 0 }
        output.pointee.url_utf8 = UnsafePointer(buffer)
        output.pointee.username_utf8 = nil
        output.pointee.password_utf8 = nil
        return 1
    }
}

let maiChatAttachmentProxyResolver: MaiDownloadProxyResolver = { url, context, output in
    guard let context else { return 0 }
    return Unmanaged<MaiChatAttachmentProxyContext>.fromOpaque(context)
        .takeUnretainedValue().resolve(url, output: output)
}

/// Each IM login owns its in-flight downloads. Logging out cancels transfers
/// before another account can reuse the client and its media cache directories.
final class MaiChatAttachmentDownloader: @unchecked Sendable {
    private let lock = NSLock()
    private var transfers: [ObjectIdentifier: MaiChatAttachmentCancelToken] = [:]
    private var currentGeneration: UInt64 = 0

    var generation: UInt64 {
        lock.lock()
        defer { lock.unlock() }
        return currentGeneration
    }

    func cancelAll() {
        lock.lock()
        currentGeneration &+= 1
        for transfer in transfers.values { transfer.cancel() }
        lock.unlock()
    }

    func download(
        signedURL: String,
        targetURL: URL,
        maximumBytes: UInt64,
        generation: UInt64? = nil,
        certificatePEM: Data? = nil,
        completion: @escaping @Sendable (MaiChatAttachmentDownloadResult) -> Void
    ) {
        guard let token = MaiChatAttachmentCancelToken() else {
            completion(Self.failed(status: Int32(MAI_DOWNLOAD_NETWORK_ERROR.rawValue)))
            return
        }
        let identifier = ObjectIdentifier(token)
        lock.lock()
        guard generation.map({ $0 == currentGeneration }) ?? true else {
            lock.unlock()
            completion(Self.failed(status: Int32(MAI_DOWNLOAD_CANCELED.rawValue)))
            return
        }
        transfers[identifier] = token
        lock.unlock()

        DispatchQueue.global(qos: .utility).async { [self] in
            defer {
                lock.lock()
                transfers.removeValue(forKey: identifier)
                lock.unlock()
            }
            guard let url = URL(string: signedURL),
                  let components = URLComponents(url: url, resolvingAgainstBaseURL: false),
                  components.scheme?.lowercased() == "https", components.host?.isEmpty == false,
                  components.user == nil, components.password == nil, components.fragment == nil,
                  targetURL.isFileURL, targetURL.path.hasPrefix("/") else {
                completion(Self.failed(status: Int32(MAI_DOWNLOAD_INVALID_ARGUMENT.rawValue)))
                return
            }
            let certificatePath: String
            if certificatePEM != nil {
                certificatePath = ""
            } else {
                guard let certificate = Bundle.main.url(
                    forResource: "cacert", withExtension: "pem",
                    subdirectory: "MaiAgentCertificates"
                ) else {
                    completion(Self.failed(status: Int32(MAI_DOWNLOAD_NETWORK_ERROR.rawValue)))
                    return
                }
                certificatePath = certificate.path
            }
            // The shared core invokes this resolver for each HTTPS redirect.
            // PAC changes across CDN hosts cannot silently turn into direct access.
            let proxyContext = MaiChatAttachmentProxyContext()
            var nativeResult = MaiDownloadResult()
            signedURL.withCString { urlPointer in
                targetURL.path.withCString { pathPointer in
                    certificatePath.withCString { certificatePointer in
                        let invoke: (UnsafeRawPointer?, Int) -> Void = { certificateBytes, length in
                            var options = MaiDownloadOptions()
                            options.url_utf8 = urlPointer
                            options.target_path_utf8 = pathPointer
                            options.max_bytes = maximumBytes
                            options.ca_pem = certificateBytes?.assumingMemoryBound(to: CChar.self)
                            options.ca_pem_length = length
                            options.ca_path_utf8 = certificatePEM == nil ? certificatePointer : nil
                            options.proxy_resolver = maiChatAttachmentProxyResolver
                            options.proxy_context = Unmanaged.passUnretained(proxyContext).toOpaque()
                            options.connect_timeout_ms = 10_000
                            options.transfer_timeout_ms = maximumBytes > 64 * 1024 * 1024
                                ? 600_000 : 120_000
                            mai_download_file(&options, token.pointer, &nativeResult)
                        }
                        if let certificatePEM {
                            certificatePEM.withUnsafeBytes { bytes in
                                invoke(bytes.baseAddress, bytes.count)
                            }
                        } else {
                            invoke(nil, 0)
                        }
                    }
                }
            }
            completion(MaiChatAttachmentDownloadResult(
                status: Int32(nativeResult.status.rawValue),
                httpStatus: Int(nativeResult.http_status),
                curlCode: nativeResult.curl_code,
                bytes: nativeResult.bytes
            ))
        }
    }

    private static func failed(status: Int32) -> MaiChatAttachmentDownloadResult {
        MaiChatAttachmentDownloadResult(status: status, httpStatus: 0, curlCode: 0, bytes: 0)
    }
}
