import Foundation
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

private final class AIPDFPageRenderer: UIPrintPageRenderer {
    private let page = CGRect(x: 0, y: 0, width: 595, height: 842)

    override var paperRect: CGRect { page }
    override var printableRect: CGRect { page.insetBy(dx: 36, dy: 36) }
}

@MainActor
extension AIMobileHostToolProvider {
    func decodeLegacyText(_ arguments: [String: Any]) -> AIMaiChatHostToolExecution {
        guard let base64 = arguments["base64"] as? String,
              let bytes = Data(base64Encoded: base64), bytes.count <= 8 * 1024 * 1024,
              let requested = arguments["encoding"] as? String else {
            return .failure(code: "invalid_input", message: "valid base64 text and encoding are required")
        }
        guard AIAssistantTextDecoder.supports(requested) else {
            return .failure(code: "invalid_input", message: "unsupported file encoding: \(requested)")
        }
        guard let text = AIAssistantTextDecoder.decode(bytes, encodingName: requested) else {
            return .failure(code: "invalid_input", message: "file could not be decoded as \(requested)")
        }
        return .success(text)
    }

    func generatePDF(_ arguments: [String: Any]) -> AIMaiChatHostToolExecution {
        guard let html = arguments["html"] as? String, !html.isEmpty,
              let outputPath = arguments["output_path"] as? String, !outputPath.isEmpty else {
            return .failure(code: "invalid_input", message: "HTML and PDF output path are required")
        }
        guard let destination = AIAssistantPathPolicy.resolve(
                  outputPath, workspacePath: AIAssistantModel.shared.workspacePath),
              destination.path.lowercased().hasSuffix(".pdf"),
              !FileManager.default.fileExists(atPath: destination.path) else {
            return .failure(code: "invalid_input", message: "PDF output must be a new file in the App container")
        }
        let formatter = UIMarkupTextPrintFormatter(markupText: html)
        let renderer = AIPDFPageRenderer()
        renderer.addPrintFormatter(formatter, startingAtPageAt: 0)
        let pages = renderer.numberOfPages
        guard pages > 0, pages <= 200 else {
            return .failure(code: "invalid_input", message: "HTML document must fit within 200 PDF pages")
        }
        let data = UIGraphicsPDFRenderer(bounds: renderer.paperRect).pdfData { context in
            for page in 0..<pages {
                context.beginPage()
                renderer.drawPage(at: page, in: renderer.paperRect)
            }
        }
        guard data.starts(with: Data("%PDF-".utf8)), data.count <= 100 * 1024 * 1024 else {
            return .failure(code: "internal", message: "iOS could not render a valid PDF")
        }
        do {
            try data.write(to: destination, options: .atomic)
            return Self.jsonSuccess(["pages": pages, "bytes": data.count])
        } catch {
            return .failure(code: "internal", message: error.localizedDescription)
        }
    }
}
