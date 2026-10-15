// 文件作用：实现桌面端敏感字符串的系统安全存储，不落入 QSettings 或普通文件。
// 主要入口：read、write、remove，供账号刷新令牌和独立命名的模型服务令牌复用。
// 实现边界：macOS 使用 SecItem，Windows 使用 WinCred；函数不记录令牌内容。
#include "auth/SecureRefreshTokenStore.h"

#include <QByteArray>
#include <utility>

#ifdef Q_OS_MACOS
#include <Security/Security.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace {

#ifdef Q_OS_MACOS
CFMutableDictionaryRef keychainQuery(const QString& service, const QString& account) {
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    const QByteArray serviceBytes = service.toUtf8();
    const QByteArray accountBytes = account.toUtf8();
    CFStringRef serviceName = CFStringCreateWithCString(
        kCFAllocatorDefault, serviceBytes.constData(), kCFStringEncodingUTF8);
    CFStringRef accountName = CFStringCreateWithCString(
        kCFAllocatorDefault, accountBytes.constData(), kCFStringEncodingUTF8);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, serviceName);
    CFDictionarySetValue(query, kSecAttrAccount, accountName);
    CFRelease(serviceName);
    CFRelease(accountName);
    return query;
}
#endif

}  // namespace

SecureRefreshTokenStore::SecureRefreshTokenStore(QString service, QString account)
    : service_(std::move(service)), account_(std::move(account)) {}

QString SecureRefreshTokenStore::read() const {
    if (service_.isEmpty() || account_.isEmpty()) return {};
#ifdef Q_OS_MACOS
    CFMutableDictionaryRef query = keychainQuery(service_, account_);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || !result) return {};
    CFDataRef data = static_cast<CFDataRef>(result);
    const QString token = QString::fromUtf8(
        reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
        int(CFDataGetLength(data)));
    CFRelease(data);
    return token;
#elif defined(Q_OS_WIN)
    const QString target = service_ == QStringLiteral("com.maichat.desktop.auth") &&
                                   account_ == QStringLiteral("refresh_token") ?
                               QStringLiteral("MaiChat Desktop Auth Refresh Token") :
                               service_ + QLatin1Char(':') + account_;
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC,
                   0, &credential)) return {};
    const QString token = QString::fromUtf8(
        reinterpret_cast<const char*>(credential->CredentialBlob),
        int(credential->CredentialBlobSize));
    CredFree(credential);
    return token;
#else
    return {};
#endif
}

bool SecureRefreshTokenStore::write(const QString& token) const {
    if (service_.isEmpty() || account_.isEmpty()) return false;
    const QByteArray bytes = token.toUtf8();
    if (bytes.isEmpty()) return false;
#ifdef Q_OS_MACOS
    CFMutableDictionaryRef query = keychainQuery(service_, account_);
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
                                  reinterpret_cast<const UInt8*>(bytes.constData()),
                                  CFIndex(bytes.size()));
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attributes, kSecValueData, data);
    OSStatus status = SecItemUpdate(query, attributes);
    if (status == errSecItemNotFound) {
        CFDictionarySetValue(query, kSecValueData, data);
        status = SecItemAdd(query, nullptr);
    }
    CFRelease(attributes);
    CFRelease(data);
    CFRelease(query);
    return status == errSecSuccess;
#elif defined(Q_OS_WIN)
    const QString target = service_ == QStringLiteral("com.maichat.desktop.auth") &&
                                   account_ == QStringLiteral("refresh_token") ?
                               QStringLiteral("MaiChat Desktop Auth Refresh Token") :
                               service_ + QLatin1Char(':') + account_;
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(
        reinterpret_cast<LPCWSTR>(target.utf16()));
    credential.CredentialBlobSize = DWORD(bytes.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(bytes.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"MaiChat");
    return CredWriteW(&credential, 0) != 0;
#else
    return false;
#endif
}

bool SecureRefreshTokenStore::remove() const {
    if (service_.isEmpty() || account_.isEmpty()) return false;
#ifdef Q_OS_MACOS
    CFMutableDictionaryRef query = keychainQuery(service_, account_);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    return status == errSecSuccess || status == errSecItemNotFound;
#elif defined(Q_OS_WIN)
    const QString target = service_ == QStringLiteral("com.maichat.desktop.auth") &&
                                   account_ == QStringLiteral("refresh_token") ?
                               QStringLiteral("MaiChat Desktop Auth Refresh Token") :
                               service_ + QLatin1Char(':') + account_;
    return CredDeleteW(reinterpret_cast<LPCWSTR>(target.utf16()),
                       CRED_TYPE_GENERIC, 0) != 0 ||
           GetLastError() == ERROR_NOT_FOUND;
#else
    return true;
#endif
}
