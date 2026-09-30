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
            // CFStringEncodingExt.h defines GB 18030 as 0x0632; Swift does not expose its enum case.
            return String.Encoding(rawValue:
                CFStringConvertEncodingToNSStringEncoding(CFStringEncoding(0x0632)))
        case "windows-1252": return .windowsCP1252
        case "latin1", "iso-8859-1": return .isoLatin1
        case "shift-jis", "shift-jis-2004": return .shiftJIS
        default: return nil
        }
    }
}
