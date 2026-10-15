// 文件作用：解码创作中心的文本数据，并处理编码边界与无效输入。
// 将 Agent 的文本字节转换为可显示的 Unicode 内容。
// 编码判断与失败回退在此统一，避免不同页面各自猜测字符集。
import CoreFoundation
import Foundation

public enum AIAssistantTextDecoder {
    public static func supports(_ requested: String) -> Bool {
        encoding(for: requested) != nil
    }

    public static func decode(_ bytes: Data, encodingName: String) -> String? {
        guard let encoding = encoding(for: encodingName) else { return nil }
        return String(data: bytes, encoding: encoding)
    }

    private static func encoding(for requested: String) -> String.Encoding? {
        let name = requested.lowercased().replacingOccurrences(of: "_", with: "-")
        switch name {
        case "auto", "gb18030", "gbk", "cp936", "system":
            // CFStringEncodingExt.h 将 GB 18030 定义为 0x0632，Swift 未公开对应枚举值。
            return String.Encoding(rawValue:
                CFStringConvertEncodingToNSStringEncoding(CFStringEncoding(0x0632)))
        case "windows-1252": return .windowsCP1252
        case "latin1", "iso-8859-1": return .isoLatin1
        case "shift-jis", "shift-jis-2004": return .shiftJIS
        default: return nil
        }
    }
}
