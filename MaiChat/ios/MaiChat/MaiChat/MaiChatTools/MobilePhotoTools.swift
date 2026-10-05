import Foundation
import AVFoundation
import CoreFoundation
import Darwin
import CoreImage
import ImageIO
import MaiChatCore
import Photos
import SwiftUI
import UIKit
import UniformTypeIdentifiers
import Vision

@MainActor
extension AIMobileHostToolProvider {
    private func photoAuthorization() async -> PHAuthorizationStatus {
        let status = PHPhotoLibrary.authorizationStatus(for: .readWrite)
        return status == .notDetermined
            ? await PHPhotoLibrary.requestAuthorization(for: .readWrite)
            : status
    }

    func listPhotos(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        let offset = max(0, arguments["offset"] as? Int ?? 0)
        let limit = min(100, max(1, arguments["limit"] as? Int ?? 50))
        let options = PHFetchOptions()
        options.sortDescriptors = [NSSortDescriptor(key: "creationDate", ascending: false)]
        let albumID = (arguments["album_id"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        let photos: PHFetchResult<PHAsset>
        if !albumID.isEmpty && albumID != "all" {
            guard let album = PHAssetCollection.fetchAssetCollections(
                withLocalIdentifiers: [albumID], options: nil
            ).firstObject else { return .failure(code: "not_found", message: "album not found") }
            photos = PHAsset.fetchAssets(in: album, options: options)
        } else {
            photos = PHAsset.fetchAssets(with: options)
        }
        let end = min(photos.count, offset + limit)
        var items: [[String: Any]] = []
        if offset < end {
            for index in offset..<end {
                let photo = photos.object(at: index)
                let mediaType = Self.galleryMediaType(photo)
                items.append([
                    "id": photo.localIdentifier,
                    "mediaType": mediaType,
                    "created_at_ms": Int64(((photo.creationDate ?? .distantPast).timeIntervalSince1970 * 1000).rounded()),
                    "width": photo.pixelWidth,
                    "height": photo.pixelHeight,
                    "duration_ms": mediaType == "video" ? Int64((photo.duration * 1000).rounded()) : 0,
                    "favorite": photo.isFavorite,
                ])
            }
        }
        return Self.jsonSuccess([
            "access": status == .authorized ? "full" : "limited",
            "total": photos.count, "offset": offset, "items": items,
        ])
    }

    func listAlbums() async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        let albums = PHAssetCollection.fetchAssetCollections(with: .album, subtype: .any, options: nil)
        var items: [[String: Any]] = [["id": "all", "name": "所有照片"]]
        albums.enumerateObjects { album, _, _ in
            items.append(["id": album.localIdentifier, "name": album.localizedTitle ?? "未命名相簿"])
        }
        return Self.jsonSuccess([
            "access": status == .authorized ? "full" : "limited", "albums": items,
        ])
    }

    func readPhoto(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        guard let id = arguments["id"] as? String, !id.isEmpty,
              let asset = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject else {
            return .failure(code: "not_found", message: "photo not found in the authorized library")
        }
        guard asset.mediaType == .image else {
            return .failure(code: "invalid_input",
                            message: "video requires mobile_export_photo_original to get its media file")
        }
        let options = PHImageRequestOptions()
        options.isNetworkAccessAllowed = true
        let data: Data? = await withCheckedContinuation { continuation in
            PHImageManager.default().requestImageDataAndOrientation(for: asset, options: options) {
                data, _, _, _ in continuation.resume(returning: data)
            }
        }
        guard let data, let source = CGImageSourceCreateWithData(data as CFData, nil),
              let preview = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: 2048,
              ] as CFDictionary),
              let jpeg = UIImage(cgImage: preview).jpegData(compressionQuality: 0.88) else {
            return .failure(code: "internal", message: "photo could not be read")
        }
        let temporary = FileManager.default.temporaryDirectory
            .appendingPathComponent("agent-photo-\(UUID().uuidString).jpg")
        do {
            try jpeg.write(to: temporary, options: .atomic)
            defer { try? FileManager.default.removeItem(at: temporary) }
            guard let imported = await AIAssistantModel.shared.importFile(temporary) else {
                return .failure(code: "internal", message: "photo could not be imported")
            }
            return Self.jsonSuccess([
                "id": id, "path": imported.relativePath, "mime_type": imported.mimeType,
                "next_tool": "mobile_preview_image", "vision_tool": "view_image",
            ])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    func exportPhotoOriginal(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        guard let id = arguments["id"] as? String, !id.isEmpty,
              let asset = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject,
              asset.mediaType == .image || asset.mediaType == .video else {
            return .failure(code: "not_found", message: "media item not found in the authorized library")
        }
        let workspacePath = AIAssistantModel.shared.workspacePath
        guard !workspacePath.isEmpty else {
            return .failure(code: "not_configured", message: "Agent working directory is unavailable")
        }
        let component = Self.string(arguments, key: "component")
        if asset.mediaType == .video || component == "video" {
            guard asset.mediaType == .video || asset.mediaSubtypes.contains(.photoLive) else {
                return .failure(code: "invalid_input", message: "this media item has no video component")
            }
            let resources = PHAssetResource.assetResources(for: asset)
            let resource = resources.first(where: {
                $0.type == (asset.mediaType == .video ? .video : .pairedVideo)
            })
            guard let resource else {
                return .failure(code: "not_found", message: "original video resource is unavailable")
            }
            let mediaType = UTType(resource.uniformTypeIdentifier)
            let originalSuffix = URL(fileURLWithPath: resource.originalFilename)
                .pathExtension.lowercased()
            let suffix = ["mov", "mp4", "m4v"].contains(originalSuffix)
                ? originalSuffix : (mediaType?.preferredFilenameExtension ?? "mov")
            let targetName = "gallery-\(UUID().uuidString).\(suffix)"
            let target = URL(fileURLWithPath: workspacePath, isDirectory: true)
                .appendingPathComponent(targetName)
            let options = PHAssetResourceRequestOptions()
            options.isNetworkAccessAllowed = true
            let exportError: Error? = await withCheckedContinuation { continuation in
                PHAssetResourceManager.default().writeData(
                    for: resource, toFile: target, options: options
                ) { error in continuation.resume(returning: error) }
            }
            if let exportError {
                try? FileManager.default.removeItem(at: target)
                return .failure(code: "internal", message: "video export failed: \(exportError.localizedDescription)")
            }
            do {
                let bytes = try target.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
                guard bytes > 0 else {
                    try? FileManager.default.removeItem(at: target)
                    return .failure(code: "internal", message: "original video resource is empty")
                }
                return Self.jsonSuccess([
                    "id": id, "path": targetName,
                    "mediaType": asset.mediaType == .video ? "video" : "livephoto",
                    "mime_type": mediaType?.preferredMIMEType ??
                        (suffix == "mp4" || suffix == "m4v" ? "video/mp4" : "video/quicktime"),
                    "bytes": bytes, "width": asset.pixelWidth,
                    "height": asset.pixelHeight,
                    "duration_ms": Int64((asset.duration * 1000).rounded()),
                ])
            } catch {
                try? FileManager.default.removeItem(at: target)
                return .failure(code: "internal", message: error.localizedDescription)
            }
        }
        let options = PHImageRequestOptions()
        options.isNetworkAccessAllowed = true
        options.version = .original
        let result: (Data, String)? = await withCheckedContinuation { continuation in
            PHImageManager.default().requestImageDataAndOrientation(for: asset, options: options) {
                data, type, _, _ in
                guard let data, let type else { continuation.resume(returning: nil); return }
                continuation.resume(returning: (data, type))
            }
        }
        guard let (data, type) = result, !data.isEmpty, data.count <= 100 * 1024 * 1024 else {
            return .failure(code: "invalid_input", message: "photo is unavailable or exceeds 100 MB")
        }
        guard let imageType = UTType(type), imageType.conforms(to: .image),
              let suffix = imageType.preferredFilenameExtension,
              let mimeType = imageType.preferredMIMEType else {
            return .failure(code: "invalid_input", message: "unsupported system photo format")
        }
        let name = "gallery-\(UUID().uuidString).\(suffix)"
        let target = URL(fileURLWithPath: workspacePath, isDirectory: true)
            .appendingPathComponent(name)
        do {
            try await Task.detached(priority: .utility) {
                try data.write(to: target, options: .atomic)
            }.value
            return Self.jsonSuccess([
                "id": id, "path": name, "mime_type": mimeType,
                "mediaType": Self.galleryMediaType(asset),
                "bytes": data.count, "width": asset.pixelWidth, "height": asset.pixelHeight,
            ])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    private static func galleryMediaType(_ asset: PHAsset) -> String {
        if asset.mediaType == .video { return "video" }
        if asset.mediaType == .image && asset.mediaSubtypes.contains(.photoLive) {
            return "livephoto"
        }
        return "photo"
    }

    func saveImage(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        let path = Self.string(arguments, key: "path")
        guard let source = AIAssistantPathPolicy.resolve(
                  path, workspacePath: AIAssistantModel.shared.workspacePath) else {
            return .failure(code: "invalid_input", message: "image must be inside the App container")
        }
        do {
            let values = try source.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            let suffix = source.pathExtension.lowercased()
            guard values.isRegularFile == true, let bytes = values.fileSize,
                  bytes > 0, bytes <= 50 * 1024 * 1024,
                  ["jpg", "jpeg", "png", "heic", "heif"].contains(suffix),
                  let image = CGImageSourceCreateWithURL(source as CFURL, nil),
                  CGImageSourceGetCount(image) > 0 else {
                return .failure(code: "invalid_input", message: "image must be a JPEG, PNG, or HEIF file up to 50 MB")
            }
            try await performPhotoChanges {
                PHAssetChangeRequest.creationRequestForAssetFromImage(atFileURL: source)
            }
            return Self.jsonSuccess(["saved": true, "source_path": path])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    func saveVideo(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        let path = Self.string(arguments, key: "path")
        guard let source = AIAssistantPathPolicy.resolve(
            path, workspacePath: AIAssistantModel.shared.workspacePath) else {
            return .failure(code: "invalid_input", message: "video must be inside the App container")
        }
        do {
            let values = try source.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            guard values.isRegularFile == true, let bytes = values.fileSize,
                  bytes > 0, bytes <= 2 * 1024 * 1024 * 1024,
                  ["mp4", "mov", "m4v"].contains(source.pathExtension.lowercased()) else {
                return .failure(code: "invalid_input", message: "video must be an MP4, MOV, or M4V file up to 2 GB")
            }
            let tracks = try await AVURLAsset(url: source).loadTracks(withMediaType: .video)
            guard !tracks.isEmpty else {
                return .failure(code: "invalid_input", message: "file has no video track")
            }
            try await performPhotoChanges {
                PHAssetChangeRequest.creationRequestForAssetFromVideo(atFileURL: source)
            }
            return Self.jsonSuccess(["saved": true, "source_path": path, "bytes": bytes])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    func transformImage(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let path = Self.string(arguments, key: "path")
        let workspacePath = AIAssistantModel.shared.workspacePath
        guard !path.isEmpty, !workspacePath.isEmpty,
              let operationData = try? JSONSerialization.data(withJSONObject: arguments),
              let operation = String(data: operationData, encoding: .utf8) else {
            return .failure(code: "invalid_input", message: "image path and an image operation are required")
        }
        guard let source = AIAssistantPathPolicy.resolve(path, workspacePath: workspacePath) else {
            return .failure(code: "invalid_input", message: "image must be inside the App container")
        }
        let workspace = URL(fileURLWithPath: workspacePath, isDirectory: true)
            .resolvingSymlinksInPath().standardizedFileURL
        do {
            let values = try source.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            guard values.isRegularFile == true, let size = values.fileSize,
                  size > 0, size <= 50 * 1024 * 1024 else {
                return .failure(code: "invalid_input", message: "source image must be at most 50 MB")
            }
            let name = "edited-\(UUID().uuidString).png"
            let target = workspace.appendingPathComponent(name)
            let dimensions = try await Task.detached(priority: .userInitiated) {
                guard let image = CIImage(contentsOf: source,
                                          options: [.applyOrientationProperty: true]) else {
                    throw AIBackendError(message: "无法读取图片")
                }
                let extent = image.extent.integral
                let width = Int(extent.width), height = Int(extent.height)
                guard width > 0, height > 0, Int64(width) * Int64(height) <= 12_000_000 else {
                    throw AIBackendError(message: "图片不能超过 1200 万像素")
                }
                var pixels = Data(count: width * height * 4)
                let context = CIContext()
                pixels.withUnsafeMutableBytes { bytes in
                    context.render(image, toBitmap: bytes.baseAddress!, rowBytes: width * 4,
                                   bounds: extent, format: .RGBA8,
                                   colorSpace: CGColorSpaceCreateDeviceRGB())
                }
                let result = pixels.withUnsafeBytes { bytes in
                    operation.withCString { spec in
                        maiImageFilterRgba(bytes.bindMemory(to: UInt8.self).baseAddress,
                                           Int32(width), Int32(height), Int32(width * 4), spec)
                    }
                }
                if let error = result.error {
                    let message = String(cString: error)
                    maiImageFilterFree(error)
                    throw AIBackendError(message: message)
                }
                guard let output = result.rgba, result.width > 0, result.height > 0 else {
                    throw AIBackendError(message: "图片处理没有返回结果")
                }
                defer { maiImageFilterFree(output) }
                let resultWidth = Int(result.width), resultHeight = Int(result.height)
                let outputData = Data(bytes: output, count: resultWidth * resultHeight * 4)
                guard let provider = CGDataProvider(data: outputData as CFData),
                      let image = CGImage(
                        width: resultWidth, height: resultHeight, bitsPerComponent: 8,
                        bitsPerPixel: 32, bytesPerRow: resultWidth * 4,
                        space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue)
                            .union(.byteOrder32Big),
                        provider: provider, decode: nil, shouldInterpolate: true,
                        intent: .defaultIntent),
                      let destination = CGImageDestinationCreateWithURL(
                        target as CFURL, UTType.png.identifier as CFString, 1, nil) else {
                    throw AIBackendError(message: "无法保存处理结果")
                }
                CGImageDestinationAddImage(destination, image, nil)
                guard CGImageDestinationFinalize(destination) else {
                    try? FileManager.default.removeItem(at: target)
                    throw AIBackendError(message: "无法写入处理结果")
                }
                return (resultWidth, resultHeight)
            }.value
            return Self.jsonSuccess([
                "path": name, "mime_type": "image/png", "width": dimensions.0,
                "height": dimensions.1, "next_tool": "mobile_preview_image",
            ])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    func addPhotosToAlbum(_ arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let status = await photoAuthorization()
        guard status == .authorized || status == .limited else {
            return .failure(code: "canceled", message: "photo library access was not granted")
        }
        guard let rawName = arguments["album_name"] as? String,
              let rawIDs = arguments["photo_ids"] as? [String] else {
            return .failure(code: "invalid_input", message: "album_name and photo_ids are required")
        }
        let name = rawName.trimmingCharacters(in: .whitespacesAndNewlines)
        let ids = Array(Set(rawIDs.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }))
        guard !name.isEmpty, name.count <= 64, !ids.isEmpty, ids.count <= 50,
              ids.allSatisfy({ !$0.isEmpty }) else {
            return .failure(code: "invalid_input", message: "invalid album name or photo IDs")
        }
        var assets: [PHAsset] = []
        for id in ids {
            guard let asset = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject,
                  asset.mediaType == .image else {
                return .failure(code: "not_found", message: "a photo is not accessible")
            }
            assets.append(asset)
        }

        func findAlbum() -> PHAssetCollection? {
            let albums = PHAssetCollection.fetchAssetCollections(with: .album, subtype: .any, options: nil)
            for index in 0..<albums.count {
                let album = albums.object(at: index)
                if album.localizedTitle == name { return album }
            }
            return nil
        }

        do {
            var album = findAlbum()
            if album == nil {
                try await performPhotoChanges {
                    PHAssetCollectionChangeRequest.creationRequestForAssetCollection(withTitle: name)
                }
                album = findAlbum()
            }
            guard let album, album.canPerform(.addContent) else {
                return .failure(code: "internal", message: "album cannot be edited")
            }
            try await performPhotoChanges {
                PHAssetCollectionChangeRequest(for: album)?.addAssets(assets as NSArray)
            }
            return Self.jsonSuccess([
                "album_id": album.localIdentifier, "album_name": name,
                "added_count": assets.count, "copied_originals": false,
            ])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }

    private func performPhotoChanges(_ changes: @escaping () -> Void) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            PHPhotoLibrary.shared().performChanges(changes) { success, error in
                if success { continuation.resume() }
                else {
                    continuation.resume(throwing: error ?? NSError(
                        domain: "MaiChat.Photos", code: -1,
                        userInfo: [NSLocalizedDescriptionKey: "相簿操作失败"]
                    ))
                }
            }
        }
    }
}
