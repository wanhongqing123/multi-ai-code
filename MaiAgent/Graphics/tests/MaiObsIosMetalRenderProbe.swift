import Foundation

@_silgen_name("maiObsIosMetalRenderProbe")
func maiObsIosMetalRenderProbe() -> Int32

let result = maiObsIosMetalRenderProbe()
if result != 0 {
    fputs("iOS Metal render probe failed: \(result)\n", stderr)
}
exit(result)
