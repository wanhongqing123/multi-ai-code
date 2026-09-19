// MaiFilePath / MaiFileSystem 的测试。
//
// ── 为什么这个文件里还留着 std::filesystem ──────────────────────
// 别的地方都把它换掉了，但测试里**故意**留着：用它来造目录和文件，
// 再用 MaiFileSystem 去读。
//
// 两个独立实现互相印证。要是造和读都用 MaiFileSystem，那么它的编码转换
// 哪怕整个是错的，自己写自己读也能对上——测试会一路绿，而别的程序建的
// 文件我们全读不了。这正是当初那个中文路径 bug 的形状。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace fs = std::filesystem;

// append 在 Windows 上用 '\'，POSIX 上用 '/'。断言里要按平台写。
#if defined(_WIN32)
#define MAI_SEP "\\"
#else
#define MAI_SEP "/"
#endif

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

MaiFilePath fromUtf8(const char* text) {
    return MaiFilePath::fromUtf8(text);
}

std::string utf8(const MaiFilePath& path) {
    return path.toUtf8();
}

// ── 纯词法：不碰磁盘，每个平台都该这么答 ───────────────────────

void test_append() {
    std::printf("-> test_append\n");
    CHECK(utf8(fromUtf8("a").append(fromUtf8("b"))) == "a" MAI_SEP "b");
    // 已经有分隔符时不要拼出两个
    CHECK(utf8(fromUtf8("a/").append(fromUtf8("b"))) == "a/b");
    CHECK(utf8(fromUtf8("a").append(fromUtf8("/b"))) == "/b");  // 右边是绝对路径
    CHECK(utf8(fromUtf8("").append(fromUtf8("b"))) == "b");
    CHECK(utf8(fromUtf8("a").append(fromUtf8(""))) == "a");
}

void test_dir_name_and_base_name() {
    std::printf("-> test_dir_name_and_base_name\n");
    CHECK(utf8(fromUtf8("a/b/c.txt").dirName()) == "a/b");
    CHECK(utf8(fromUtf8("a/b/c.txt").baseName()) == "c.txt");

    // 结尾的分隔符不该影响结果
    CHECK(utf8(fromUtf8("a/b/").dirName()) == "a");
    CHECK(utf8(fromUtf8("a/b/").baseName()) == "b");

    // 只有一段时没有父目录
    CHECK(fromUtf8("a").dirName().isEmpty());
    CHECK(utf8(fromUtf8("a").baseName()) == "a");

    // 根的父目录还是根，不能变成空——变空的话 createDirectories 会无限递归
    CHECK(utf8(fromUtf8("/a").dirName()) == "/");
    CHECK(utf8(fromUtf8("/").dirName()) == "/");
}

void test_is_absolute() {
    std::printf("-> test_is_absolute\n");
    CHECK(fromUtf8("/etc/passwd").isAbsolute());
    CHECK(!fromUtf8("etc/passwd").isAbsolute());
    CHECK(!fromUtf8("./etc").isAbsolute());
    CHECK(!fromUtf8("").isAbsolute());
#if defined(_WIN32)
    CHECK(fromUtf8("C:\\Windows").isAbsolute());
    CHECK(fromUtf8("C:/Windows").isAbsolute());
    CHECK(fromUtf8("\\\\server\\share").isAbsolute());
    // "C:x" 是**相对**路径（C 盘的当前目录下的 x），不是绝对。
    // 把它当绝对路径的话，越界检查会从一个错误的起点开始算。
    CHECK(!fromUtf8("C:x").isAbsolute());
#endif
}

void test_components() {
    std::printf("-> test_components\n");
    // 根要单独成一段，否则绝对路径和相对路径会被切成一样的东西
    const auto relative = fromUtf8("a/b").components();
    CHECK(relative.size() == 2);

    const auto absolute = fromUtf8("/a/b").components();
    CHECK(absolute.size() == 3);  // 根 + a + b

    // 重复的分隔符要被吃掉
    CHECK(fromUtf8("a//b///c").components().size() == 3);
#if defined(_WIN32)
    // C: 和它后面的分隔符各算一段，和 chromium 的 GetComponents 一致
    CHECK(fromUtf8("C:\\a\\b").components().size() == 4);
#endif
}

void test_is_parent_of() {
    std::printf("-> test_is_parent_of\n");
    CHECK(fromUtf8("/a").isParentOf(fromUtf8("/a/b")));
    CHECK(fromUtf8("/a").isParentOf(fromUtf8("/a/b/c")));

    // 自己不是自己的父目录
    CHECK(!fromUtf8("/a").isParentOf(fromUtf8("/a")));

    // **同前缀的兄弟目录不算在里面**。用字符串前缀判断的实现会在这里出错，
    // 而那是这一层最容易犯、后果最严重的 bug：root 旁边放一个同前缀的
    // 目录，越界检查就全漏了。
    CHECK(!fromUtf8("/server/app").isParentOf(fromUtf8("/server/app-secrets")));
    CHECK(!fromUtf8("/server/app").isParentOf(fromUtf8("/server/app-secrets/x.txt")));

    // 反向不成立
    CHECK(!fromUtf8("/a/b").isParentOf(fromUtf8("/a")));

    // 绝对和相对不能混为一谈
    CHECK(!fromUtf8("a").isParentOf(fromUtf8("/a/b")));
#if defined(_WIN32)
    // Windows 路径不分大小写。不这么做的话，模型送来 "C:\\Work\\a.txt"
    // 而 root 是 "C:\\work"，会被误判成越界。
    CHECK(fromUtf8("C:\\work").isParentOf(fromUtf8("C:\\WORK\\a.txt")));
#endif
}

void test_utf8_round_trip() {
    std::printf("-> test_utf8_round_trip\n");
    // 被测数据，不是文案：这一条测的就是非 ASCII 往返。
    //   \u4e2d\u6587\u76ee\u5f55  中文目录
    const char* cjk = "\u4e2d\u6587\u76ee\u5f55";
    CHECK(utf8(fromUtf8(cjk)) == cjk);

    const std::string mixed = std::string("a/") + cjk + "/b.txt";
    CHECK(utf8(fromUtf8(mixed.c_str())) == mixed);

    // generic 形式一律用 '/'
    CHECK(fromUtf8("a/b").append(fromUtf8("c")).toGenericUtf8() == "a/b/c");
}

// ── 碰磁盘的那一半 ─────────────────────────────────────────────

struct TempDir {
    fs::path root;
    TempDir() {
        root = fs::temp_directory_path() / ("maiagent-fs-" + std::to_string(std::rand()));
        fs::create_directories(root);
    }
    ~TempDir() {
        std::error_code errorCode;
        fs::remove_all(root, errorCode);
    }
    MaiFilePath path(const char* name) const {
        return MaiFilePath::fromUtf8((root / name).u8string());
    }
};

void test_read_write_round_trip() {
    std::printf("-> test_read_write_round_trip\n");
    TempDir temp;
    const MaiFilePath file = temp.path("a.txt");

    const std::string body = "line one\nline two\n";
    CHECK(!MaiFileSystem::writeFile(file, body).hasError());
    CHECK(MaiFileSystem::exists(file));
    CHECK(!MaiFileSystem::isDirectory(file));

    std::uint64_t size = 0;
    CHECK(MaiFileSystem::fileSize(file, size));
    CHECK(size == body.size());

    std::string readBack;
    CHECK(!MaiFileSystem::readFile(file, readBack).hasError());
    CHECK(readBack == body);

    // 覆盖写，不是追加
    CHECK(!MaiFileSystem::writeFile(file, "short").hasError());
    CHECK(!MaiFileSystem::readFile(file, readBack).hasError());
    CHECK(readBack == "short");
}

void test_read_respects_limit() {
    std::printf("-> test_read_respects_limit\n");
    TempDir temp;
    const MaiFilePath file = temp.path("big.txt");
    CHECK(!MaiFileSystem::writeFile(file, std::string(1000, 'x')).hasError());

    std::string contents;
    bool truncated = false;
    CHECK(!MaiFileSystem::readFile(file, contents, 100, &truncated).hasError());
    CHECK(contents.size() == 100);
    CHECK(truncated);

    // 没到上限时不该说截断了——说了的话模型会以为还有东西没看到
    CHECK(!MaiFileSystem::readFile(file, contents, 100000, &truncated).hasError());
    CHECK(contents.size() == 1000);
    CHECK(!truncated);
}

void test_missing_file_reports_not_found() {
    std::printf("-> test_missing_file_reports_not_found\n");
    TempDir temp;
    std::string contents;
    const MaiError error = MaiFileSystem::readFile(temp.path("nope.txt"), contents);
    CHECK(error.hasError());
    // 错误码要分得出来。走系统 API 的好处之一就是这个——
    // std::filesystem 会把"没找到"和"没权限"糊成一个笼统的 error_code。
    CHECK(error.code() == MaiErrorCode::NotFound);
}

void test_create_directories() {
    std::printf("-> test_create_directories\n");
    TempDir temp;
    const MaiFilePath deep = temp.path("a").append(fromUtf8("b")).append(fromUtf8("c"));
    CHECK(!MaiFileSystem::createDirectories(deep).hasError());
    CHECK(MaiFileSystem::isDirectory(deep));
    // 已存在不算错
    CHECK(!MaiFileSystem::createDirectories(deep).hasError());
}

void test_walk() {
    std::printf("-> test_walk\n");
    TempDir temp;
    fs::create_directories(temp.root / "src");
    fs::create_directories(temp.root / "skipme" / "deep");
    std::ofstream(temp.root / "a.txt", std::ios::binary) << "a";
    std::ofstream(temp.root / "src" / "b.txt", std::ios::binary) << "bb";
    std::ofstream(temp.root / "skipme" / "deep" / "c.txt", std::ios::binary) << "ccc";

    std::vector<std::string> seen;
    MaiFileSystem::walk(MaiFilePath::fromUtf8(temp.root.u8string()),
                        [&](const MaiFileEntry& entry) {
                            if (entry.isDirectory && entry.nameUtf8 == "skipme")
                                return MaiWalkAction::SkipDirectory;
                            if (!entry.isDirectory) seen.push_back(entry.nameUtf8);
                            return MaiWalkAction::Continue;
                        });

    CHECK(seen.size() == 2);  // a.txt 和 b.txt，skipme 里的不算
    bool sawA = false, sawB = false, sawC = false;
    for (const auto& name : seen) {
        if (name == "a.txt") sawA = true;
        if (name == "b.txt") sawB = true;
        if (name == "c.txt") sawC = true;
    }
    CHECK(sawA);
    CHECK(sawB);
    CHECK(!sawC);  // SkipDirectory 必须真的不进去
}

void test_walk_stop() {
    std::printf("-> test_walk_stop\n");
    TempDir temp;
    for (int i = 0; i < 20; ++i)
        std::ofstream(temp.root / ("f" + std::to_string(i) + ".txt"), std::ios::binary) << "x";

    int visited = 0;
    MaiFileSystem::walk(MaiFilePath::fromUtf8(temp.root.u8string()), [&](const MaiFileEntry&) {
        ++visited;
        return MaiWalkAction::Stop;
    });
    CHECK(visited == 1);  // Stop 要立刻停，不是走完再说
}

void test_walk_reports_size() {
    std::printf("-> test_walk_reports_size\n");
    TempDir temp;
    std::ofstream(temp.root / "sized.txt", std::ios::binary) << "12345";

    std::uint64_t reported = 0;
    MaiFileSystem::walk(MaiFilePath::fromUtf8(temp.root.u8string()),
                        [&](const MaiFileEntry& entry) {
                            if (!entry.isDirectory) reported = entry.size;
                            return MaiWalkAction::Continue;
                        });
    // 大小要顺路带出来。grep 靠它跳过大文件，再 stat 一次就是白跑一趟系统调用。
    CHECK(reported == 5);
}

void test_non_ascii_paths() {
    std::printf("-> test_non_ascii_paths\n");
    TempDir temp;
    // 被测数据：用 std::filesystem 造目录，用 MaiFileSystem 读——
    // 两个独立实现互相印证。
    //   \u4e2d\u6587\u76ee\u5f55  中文目录
    //   \u6587\u4ef6.txt          文件.txt
    const char* directory = "\u4e2d\u6587\u76ee\u5f55";
    const char* fileName = "\u6587\u4ef6.txt";

    const fs::path madeByStd = temp.root / fs::u8path(directory);
    fs::create_directories(madeByStd);
    std::ofstream(madeByStd / fs::u8path(fileName), std::ios::binary) << "hello";

    const MaiFilePath viaOurLayer = MaiFilePath::fromUtf8(temp.root.u8string())
                                        .append(MaiFilePath::fromUtf8(directory))
                                        .append(MaiFilePath::fromUtf8(fileName));

    CHECK(MaiFileSystem::exists(viaOurLayer));
    std::string contents;
    CHECK(!MaiFileSystem::readFile(viaOurLayer, contents).hasError());
    CHECK(contents == "hello");

    // 反过来：我们写的，std::filesystem 也要认
    const MaiFilePath ours =
        MaiFilePath::fromUtf8(temp.root.u8string())
            .append(MaiFilePath::fromUtf8(directory))
            .append(MaiFilePath::fromUtf8("\u6211\u4eec\u5199\u7684.txt"));  // 我们写的.txt
    CHECK(!MaiFileSystem::writeFile(ours, "written by us").hasError());
    CHECK(fs::exists(madeByStd / fs::u8path("\u6211\u4eec\u5199\u7684.txt")));
}

void test_resolve() {
    std::printf("-> test_resolve\n");
    TempDir temp;
    fs::create_directories(temp.root / "a" / "b");

    const MaiFilePath root = MaiFilePath::fromUtf8(temp.root.u8string());
    // ".." 要被消掉
    const MaiFilePath messy = root.append(fromUtf8("a"))
                                  .append(fromUtf8(".."))
                                  .append(fromUtf8("a"))
                                  .append(fromUtf8("b"));
    const MaiFilePath resolved = MaiFileSystem::resolve(messy);
    CHECK(!resolved.isEmpty());
    CHECK(resolved == MaiFileSystem::resolve(root.append(fromUtf8("a")).append(fromUtf8("b"))));

    // 不存在的路径也要能解析（write 要新建的文件就是这种）
    const MaiFilePath notYet = root.append(fromUtf8("a")).append(fromUtf8("new.txt"));
    CHECK(!MaiFileSystem::resolve(notYet).isEmpty());
}

void test_remove_recursively() {
    std::printf("-> test_remove_recursively\n");
    TempDir temp;
    const fs::path tree = temp.root / "tree";
    fs::create_directories(tree / "x" / "y");
    std::ofstream(tree / "x" / "y" / "z.txt", std::ios::binary) << "z";
    std::ofstream(tree / "top.txt", std::ios::binary) << "t";

    MaiFileSystem::removeRecursively(MaiFilePath::fromUtf8(tree.u8string()));
    CHECK(!fs::exists(tree));
}

}  // namespace

int main() {
    test_append();
    test_dir_name_and_base_name();
    test_is_absolute();
    test_components();
    test_is_parent_of();
    test_utf8_round_trip();

    test_read_write_round_trip();
    test_read_respects_limit();
    test_missing_file_reports_not_found();
    test_create_directories();
    test_walk();
    test_walk_stop();
    test_walk_reports_size();
    test_non_ascii_paths();
    test_resolve();
    test_remove_recursively();

    if (failures) {
        std::printf("\n%d checks failed\n", failures);
        return 1;
    }
    std::printf("\nfile path tests passed\n");
    return 0;
}
