import Foundation

@_silgen_name("maiObsIosMetalRenderProbe")
func maiObsIosMetalRenderProbe(_ imagePath: UnsafePointer<CChar>) -> Int32

guard CommandLine.arguments.count == 2 else {
    exit(2)
}
let result = CommandLine.arguments[1].withCString { maiObsIosMetalRenderProbe($0) }
if result != 0 {
    fputs("iOS Metal render probe failed: \(result)\n", stderr)
}
exit(result)
