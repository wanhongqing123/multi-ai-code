#include <windows.h>

#include <vector>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiBlockingCheck.h"
#include "MaiThread.h"

// Windows 实现：一律走**宽字符** API。
//
// 窄字符那套（CreateFileA / FindFirstFileA）会把路径按当前 ANSI 代码页
// 解释，中文机器上是 GBK，而我们的路径是 UTF-8——这正是 std::filesystem
// 当初让进程挂掉的同一个原因。

namespace {

// UTF-8 -> UTF-16。空串直接返回空，别让 MultiByteToWideChar 去处理
// 长度 0 的情况（它会返回 0，和失败没法区分）。
std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                          needed);
    return wide;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                             static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (needed <= 0) return {};
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                          needed, nullptr, nullptr);
    return utf8;
}

// 把 Win32 错误码翻成我们的错误码。
//
// 这是自己调系统 API 换来的好处之一：std::filesystem 把这些都糊成一个
// 笼统的 error_code，界面上就只能显示"操作失败"。
MaiErrorCode toErrorCode(DWORD lastError) {
    switch (lastError) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return MaiErrorCode::NotFound;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
            return MaiErrorCode::InvalidInput;
        default:
            return MaiErrorCode::Internal;
    }
}

std::string describe(DWORD lastError) {
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string message;
    if (length > 0 && buffer) {
        std::wstring wide(buffer, length);
        // FormatMessage 结尾会带 \r\n，去掉
        while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n')) wide.pop_back();
        message = narrow(wide);
    }
    if (buffer) ::LocalFree(buffer);
    if (message.empty()) message = "windows error " + std::to_string(lastError);
    return message;
}

MaiError lastErrorAs(const char* what) {
    const DWORD code = ::GetLastError();
    return MaiError::make(toErrorCode(code), std::string(what) + ": " + describe(code));
}

bool attributesOf(const MaiFilePath& path, WIN32_FILE_ATTRIBUTE_DATA& out) {
    if (path.isEmpty()) return false;
    return ::GetFileAttributesExW(path.value().c_str(), GetFileExInfoStandard, &out) != FALSE;
}

}  // namespace

// ── MaiFilePath 的边界转换 ──────────────────────────────────────

MaiFilePath MaiFilePath::fromUtf8(const std::string& utf8) {
    return MaiFilePath(widen(utf8));
}

std::string MaiFilePath::toUtf8() const {
    return narrow(mValue);
}

std::string MaiFilePath::toGenericUtf8() const {
    std::wstring copy = mValue;
    for (wchar_t& character : copy) {
        if (character == L'\\') character = L'/';
    }
    return narrow(copy);
}

// ── MaiFileSystem ───────────────────────────────────────────────

bool MaiFileSystem::exists(const MaiFilePath& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return attributesOf(path, data);
}

bool MaiFileSystem::isDirectory(const MaiFilePath& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!attributesOf(path, data)) return false;
    return (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool MaiFileSystem::fileSize(const MaiFilePath& path, std::uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!attributesOf(path, data)) return false;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return true;
}

MaiError MaiFileSystem::readFile(const MaiFilePath& path, std::string& contents,
                                 std::uint64_t maxBytes, bool* truncated) {
    contents.clear();
    if (truncated) *truncated = false;
    if (path.isEmpty()) return MaiError::make(MaiErrorCode::InvalidInput, "empty path");

    maiAssertBlockingAllowed("MaiFileSystem::readFile");
    // FILE_SHARE_READ | FILE_SHARE_WRITE：别人正开着这个文件时我们也能读。
    // 不给 SHARE_WRITE 的话，读一个编辑器正打开的文件会失败。
    const HANDLE handle = ::CreateFileW(path.value().c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return lastErrorAs("cannot open file for reading");

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle, &size)) {
        const MaiError error = lastErrorAs("cannot read file size");
        ::CloseHandle(handle);
        return error;
    }

    std::uint64_t toRead = static_cast<std::uint64_t>(size.QuadPart);
    if (maxBytes > 0 && toRead > maxBytes) {
        toRead = maxBytes;
        if (truncated) *truncated = true;
    }

    contents.resize(static_cast<std::size_t>(toRead));
    std::uint64_t done = 0;
    while (done < toRead) {
        // ReadFile 一次最多 DWORD 能表示的量；大文件要分几次。
        const DWORD chunk = static_cast<DWORD>(
            (toRead - done) > 0x10000000ull ? 0x10000000ull : (toRead - done));
        DWORD got = 0;
        if (!::ReadFile(handle, contents.data() + done, chunk, &got, nullptr)) {
            const MaiError error = lastErrorAs("read failed");
            ::CloseHandle(handle);
            return error;
        }
        if (got == 0) break;  // 提前到头了（有人同时截断了它）
        done += got;
    }
    contents.resize(static_cast<std::size_t>(done));
    ::CloseHandle(handle);
    return {};
}

MaiError MaiFileSystem::writeFile(const MaiFilePath& path, const std::string& contents) {
    if (path.isEmpty()) return MaiError::make(MaiErrorCode::InvalidInput, "empty path");

    maiAssertBlockingAllowed("MaiFileSystem::writeFile");
    const HANDLE handle = ::CreateFileW(path.value().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return lastErrorAs("cannot open file for writing");

    std::size_t done = 0;
    while (done < contents.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (contents.size() - done) > 0x10000000u ? 0x10000000u : (contents.size() - done));
        DWORD written = 0;
        if (!::WriteFile(handle, contents.data() + done, chunk, &written, nullptr)) {
            const MaiError error = lastErrorAs("write failed");
            ::CloseHandle(handle);
            return error;
        }
        done += written;
    }
    ::CloseHandle(handle);
    return {};
}

MaiError MaiFileSystem::createDirectories(const MaiFilePath& path) {
    if (path.isEmpty()) return MaiError::make(MaiErrorCode::InvalidInput, "empty path");
    if (isDirectory(path)) return {};

    // 先建父目录再建自己。递归比循环好写，而路径深度是有上限的，
    // 不会把栈用光。
    const MaiFilePath parent = path.dirName();
    if (!parent.isEmpty() && parent != path) {
        const MaiError error = createDirectories(parent);
        if (error.hasError()) return error;
    }

    if (::CreateDirectoryW(path.value().c_str(), nullptr)) return {};
    // 并发时别人可能刚好抢先建好了，那不算错。
    if (::GetLastError() == ERROR_ALREADY_EXISTS) return {};
    return lastErrorAs("cannot create directory");
}

void MaiFileSystem::walk(const MaiFilePath& root,
                         const std::function<MaiWalkAction(const MaiFileEntry&)>& visit) {
    if (root.isEmpty() || !visit) return;

    // 自己维护待遍历队列，不用递归：目录树可能很深，而且这样
    // SkipDirectory 的实现就是"不往队列里放"，一目了然。
    std::vector<MaiFilePath> pending{root};

    while (!pending.empty()) {
        const MaiFilePath directory = pending.back();
        pending.pop_back();

        const MaiFilePath pattern = directory.append(MaiFilePath(L"*"));
        WIN32_FIND_DATAW data{};
        // FindExInfoBasic：不要 cAlternateFileName（8.3 短名），少一次查询。
        // 遍历大目录时这个差别是实打实的。
        const HANDLE handle = ::FindFirstFileExW(pattern.value().c_str(), FindExInfoBasic, &data,
                                                 FindExSearchNameMatch, nullptr,
                                                 FIND_FIRST_EX_LARGE_FETCH);
        if (handle == INVALID_HANDLE_VALUE) continue;  // 没权限之类，跳过这个目录

        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;

            MaiFileEntry entry;
            entry.path = directory.append(MaiFilePath(name));
            entry.nameUtf8 = narrow(name);
            entry.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;

            const MaiWalkAction action = visit(entry);
            if (action == MaiWalkAction::Stop) {
                ::FindClose(handle);
                return;
            }
            if (!entry.isDirectory || action == MaiWalkAction::SkipDirectory) continue;

            // 不跟进重解析点（符号链接、目录联接）。跟进的话一个指回上级
            // 的联接就能让遍历无限转下去。
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;

            pending.push_back(entry.path);
        } while (::FindNextFileW(handle, &data));

        ::FindClose(handle);
    }
}

MaiFilePath MaiFileSystem::resolve(const MaiFilePath& path) {
    if (path.isEmpty()) return {};

    // 存在的话用 GetFinalPathNameByHandleW：它会解析符号链接和联接，
    // 这是判断越界时必须做的一步——root 里放一个指向 C:\ 的联接，
    // 光靠词法规范化是看不出来的。
    //
    // FILE_FLAG_BACKUP_SEMANTICS 是打开**目录**句柄所必需的，少了它
    // CreateFileW 对目录一律失败。
    const HANDLE handle = ::CreateFileW(path.value().c_str(), 0,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                        nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        std::wstring buffer(1024, L'\0');
        DWORD length = ::GetFinalPathNameByHandleW(handle, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length > buffer.size()) {
            buffer.resize(length);
            length = ::GetFinalPathNameByHandleW(handle, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()),
                                                 FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        }
        ::CloseHandle(handle);
        if (length > 0) {
            buffer.resize(length);
            // 返回的是 "\\?\C:\..." 形式。那个前缀是给 API 用的，
            // 留着会让后面逐段比对时两边形状对不上，剥掉。
            constexpr wchar_t kPrefix[] = L"\\\\?\\";
            if (buffer.rfind(kPrefix, 0) == 0) buffer.erase(0, 4);
            return MaiFilePath(buffer);
        }
    }

    // 不存在（write 要新建的文件就是这种）：尽力而为——
    // 用 GetFullPathNameW 做词法规范化，它会消掉 "." 和 ".."，
    // 并把相对路径按当前目录补全。符号链接解析不了，但路径都还不存在，
    // 也就无从链接起。
    std::wstring buffer(1024, L'\0');
    DWORD length = ::GetFullPathNameW(path.value().c_str(), static_cast<DWORD>(buffer.size()),
                                      buffer.data(), nullptr);
    if (length > buffer.size()) {
        buffer.resize(length);
        length = ::GetFullPathNameW(path.value().c_str(), static_cast<DWORD>(buffer.size()),
                                    buffer.data(), nullptr);
    }
    if (length == 0) return {};
    buffer.resize(length);
    return MaiFilePath(buffer);
}

MaiFilePath MaiFileSystem::temporaryDirectory() {
    std::wstring buffer(MAX_PATH + 1, L'\0');
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0) return MaiFilePath(L".");
    buffer.resize(length);
    while (!buffer.empty() && MaiFilePath::isSeparator(buffer.back())) buffer.pop_back();
    return MaiFilePath(buffer);
}

void MaiFileSystem::removeRecursively(const MaiFilePath& path) {
    if (path.isEmpty() || !exists(path)) return;

    if (!isDirectory(path)) {
        ::DeleteFileW(path.value().c_str());
        return;
    }

    // 先收集再删。一边遍历一边删同一个目录，FindNextFileW 的行为是
    // 未定义的。
    std::vector<MaiFilePath> files;
    std::vector<MaiFilePath> directories;
    walk(path, [&](const MaiFileEntry& entry) {
        if (entry.isDirectory) {
            directories.push_back(entry.path);
        } else {
            files.push_back(entry.path);
        }
        return MaiWalkAction::Continue;
    });

    for (const auto& file : files) ::DeleteFileW(file.value().c_str());
    // 子目录一定排在父目录后面删：walk 是先父后子给出来的，倒着来即可。
    for (auto it = directories.rbegin(); it != directories.rend(); ++it)
        ::RemoveDirectoryW(it->value().c_str());
    ::RemoveDirectoryW(path.value().c_str());
}

// ── MaiThread ───────────────────────────────────────────────────
// 放在这个文件里而不是单开一个：它就两个函数，而且和上面一样是
// "平台相关的一小段"，单开文件反而让人多找一次。

namespace {

// 自己留一份。Windows 有 GetThreadDescription 可以读回来，但它是
// Win10 1607+ 才有的，而且要 LocalFree 释放，为了一句日志不值当。
thread_local std::string tThreadName;

}  // namespace

namespace {

// 只收裸指针，函数体里不出现任何带析构函数的对象——否则 MSVC 报 C2712。
void raiseThreadNameException(const char* name) {
    constexpr DWORD kVisualStudioThreadNameException = 0x406D1388;
#pragma pack(push, 8)
    struct ThreadNameInfo {
        DWORD type;      // 必须是 0x1000
        LPCSTR name;     // 指向名字，窄字符
        DWORD threadId;  // -1 表示当前线程
        DWORD flags;     // 保留，必须是 0
    };
#pragma pack(pop)
    ThreadNameInfo info{0x1000, name, static_cast<DWORD>(-1), 0};

    __try {
        ::RaiseException(kVisualStudioThreadNameException, 0, sizeof(info) / sizeof(ULONG_PTR),
                         reinterpret_cast<const ULONG_PTR*>(&info));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 没人接就算了
    }
}

}  // namespace

void MaiThread::setCurrentName(const std::string& name) {
    tThreadName = name;

    // SetThreadDescription 是 Win10 1607 才有的，动态取。
    // 它的好处是**不挂调试器也生效**——抓 dump、看任务管理器、事后用
    // WinDbg 打开都能看见名字。
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const auto setThreadDescription = reinterpret_cast<SetThreadDescriptionFn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
    if (setThreadDescription) {
        setThreadDescription(::GetCurrentThread(), widen(name).c_str());
    }

    // 老系统上的兜底：MSVC 调试器约定的那个魔法异常。只有挂着调试器时
    // 才有人接，没挂的话白抛一次，所以先问一句。
    if (!::IsDebuggerPresent()) return;
    raiseThreadNameException(name.c_str());
}

std::string MaiThread::currentName() {
    return tThreadName;
}
