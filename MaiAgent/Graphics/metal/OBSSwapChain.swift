/******************************************************************************
 Copyright (C) 2024 by Patrick Heyer <PatTheMav@users.noreply.github.com>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#if os(macOS)
import AppKit
typealias OBSPlatformView = NSView
#elseif os(iOS)
import UIKit
typealias OBSPlatformView = UIView
#endif
import CoreVideo
import Foundation
import Metal
import QuartzCore

class OBSSwapChain {
    enum ColorRange {
        case sdr
        case hdrPQ
        case hdrHLG
    }

    private weak var device: MetalDevice?
    private var view: OBSPlatformView?

    var colorRange: ColorRange
    var edrHeadroom: CGFloat = 0.0
    let layer: CAMetalLayer
    var renderTarget: MetalTexture?
    var viewSize: MTLSize
    var fence: MTLFence
    var discard: Bool = false

    init?(device: MetalDevice, size: MTLSize, colorSpace: gs_color_format,
          existingLayer: CAMetalLayer? = nil) {
        self.device = device
        self.viewSize = size
        self.layer = existingLayer ?? CAMetalLayer()
        self.layer.framebufferOnly = false
        self.layer.device = device.device
        self.layer.drawableSize = CGSize(width: viewSize.width, height: viewSize.height)
        self.layer.pixelFormat = .bgra8Unorm_srgb
        self.layer.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
#if os(macOS)
        self.layer.wantsExtendedDynamicRangeContent = false
        self.layer.edrMetadata = nil
        self.layer.displaySyncEnabled = false
#endif
        self.colorRange = .sdr

        guard let fence = device.device.makeFence() else { return nil }

        self.fence = fence
    }

    /// Updates the provided view to use the `CAMetalLayer` managed by the ``OBSSwapChain``
    /// - Parameter view: `NSView` instance to update
    ///
    /// > Important: This function has to be called from the main thread
    @MainActor
    func updateView(_ view: OBSPlatformView) {
        self.view = view
#if os(macOS)
        view.layer = self.layer
        view.wantsLayer = true
#else
        layer.frame = view.bounds
        layer.contentsScale = view.contentScaleFactor
        view.layer.addSublayer(layer)
#endif

        updateEdrHeadroom()
    }

    /// Updates the EDR headroom value on the ``OBSSwapChain`` with the value from the screen the managed `NSView` is
    /// associated with.
    ///
    /// This is necessary to ensure that the projector uses the appropriate SDR or EDR output depending on the screen
    /// the view is on.
    @MainActor
    func updateEdrHeadroom() {
#if os(macOS)
        guard let view = self.view else {
            return
        }

        if let screen = view.window?.screen {
            self.edrHeadroom = screen.maximumPotentialExtendedDynamicRangeColorComponentValue
        } else {
            self.edrHeadroom = CGFloat(1.0)
        }
#else
        self.edrHeadroom = CGFloat(1.0)
#endif
    }

    /// Resizes the drawable of the managed `CAMetalLayer` to the provided size
    /// - Parameter size: Desired new size of the drawable
    ///
    /// This is usually achieved via a delegate method directly on the associated `NSView` instance, but because the
    /// view is managed by Qt, the resize event is routed manually into the ``OBSSwapChain`` instance by `libobs`.
    func resize(_ size: MTLSize) {
        guard viewSize.width != size.width || viewSize.height != size.height else { return }

        viewSize = size
        layer.drawableSize = CGSize(
            width: viewSize.width,
            height: viewSize.height)
        renderTarget = nil
    }

    /// Gets an opaque pointer for the ``OBSSwapChain`` instance and increases its reference count by one
    /// - Returns: `OpaquePointer` to class instance
    ///
    /// > Note: Use this method when the instance is to be shared via an `OpaquePointer` and needs to be retained. Any
    /// opaque pointer shared this way needs to be converted into a retained reference again to ensure automatic
    /// deinitialization by the Swift runtime.
    func getRetained() -> OpaquePointer {
        let retained = Unmanaged.passRetained(self).toOpaque()

        return OpaquePointer(retained)
    }

    /// Gets an opaque pointer for the ``OBSSwapChain`` instance without increasing its reference count
    /// - Returns: `OpaquePointer` to class instance
    func getUnretained() -> OpaquePointer {
        let unretained = Unmanaged.passUnretained(self).toOpaque()

        return OpaquePointer(unretained)
    }
}
