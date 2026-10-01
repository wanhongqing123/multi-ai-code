import Foundation
import QuartzCore

@_silgen_name("maiObsIosMetalRenderProbe")
func maiObsIosMetalRenderProbe(_ imagePath: UnsafePointer<CChar>) -> Int32

@_silgen_name("maiObsIosMetalPresentProbe")
func maiObsIosMetalPresentProbe(_ imagePath: UnsafePointer<CChar>,
                                _ effectDirectory: UnsafePointer<CChar>,
                                _ videoPath: UnsafePointer<CChar>,
                                _ layer: UnsafeMutableRawPointer) -> Int32

guard CommandLine.arguments.count == 4 else {
    exit(2)
}
let layer = CAMetalLayer()
layer.frame = CGRect(x: 0, y: 0, width: 64, height: 64)
let result = CommandLine.arguments[1].withCString { imagePath in
    let offscreenResult = maiObsIosMetalRenderProbe(imagePath)
    guard offscreenResult == 0 else { return offscreenResult }
    return CommandLine.arguments[2].withCString { effectDirectory in
        CommandLine.arguments[3].withCString { videoPath in
            maiObsIosMetalPresentProbe(imagePath, effectDirectory, videoPath,
                                       Unmanaged.passUnretained(layer).toOpaque())
        }
    }
}
if result != 0 {
    fputs("iOS Metal render probe failed: \(result)\n", stderr)
}
exit(result)
