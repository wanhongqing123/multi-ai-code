# MaiAgent 编码规范

这份规范和 `MaiChat/desktop` 保持一致——两个工程以后要互相链接，
风格不统一会让人每切换一次目录就要重新适应一遍。

规范是硬约束，不是建议。新增文件按这套来，改到旧文件时顺手纠正。

---

## 1. 文件命名

**大驼峰，`Mai` 前缀，`.h` / `.cpp` 成对同名。**

```
MaiTime.h        MaiTime.cpp
MaiError.h       MaiError.cpp
MaiSessionStore.h
MaiHttpServer.h  MaiHttpServer.cpp
```

不要这样：

```
time.h           ← 撞 C 标准库
error.h          ← glibc 里就有一个
types.h          ← 谁的 types？
util/helpers.h   ← util/ 是最容易撞的目录前缀
model_client.h   ← 下划线风格，和 MaiChat 不一致
```

**为什么一定要前缀**：把某个目录加进 include 路径，等于把它下面每一级
目录名都变成全局的 include 前缀。`util/`、`http/`、`agent/` 这类名字迟早
和某个第三方库撞上，而撞了之后编译器报的错离真正原因很远——往往是
"某个不认识的类型"，要查很久才发现是引错了头文件。

一个文件原则上只放一个主类型，文件名就是那个类型的名字。

## 2. 类型命名

**大驼峰，`Mai` 前缀。不使用命名空间**（与 MaiChat 一致）。

```cpp
class MaiAgent { ... };
class MaiEventBus { ... };
struct MaiSession { ... };
enum class MaiErrorCode { ... };
```

前缀承担了命名空间的职责。两者都要就会写出 `mai::MaiAgent` 这种重复。

**这条同时解决了通用词的冲突问题**：`Error`、`Result`、`Session`、
`Message`、`Event` 单独出现时都极易撞车；加上前缀之后
`MaiError`、`MaiResult` 是独一份的，而且不依赖调用方是否写了
`using namespace`——那个我们管不了。

## 3. 函数命名

**类的成员函数用小驼峰**：

```cpp
class MaiTime {
 public:
  static MaiMillis getCurrentTime();
};

class MaiSessionStore {
 public:
  virtual void putSession(const MaiSession& session) = 0;
  virtual bool getSession(const std::string& id, MaiSession& out) const = 0;
  virtual std::vector<MaiSession> listSessions() const = 0;
};
```

自由函数同样小驼峰，并且名字要能脱离上下文读懂：

```cpp
std::string newSessionId();   // 好：一眼看出在生成什么
std::string session();        // 坏：像是在取某个会话
```

查询类的布尔方法用 `is` / `has` 开头：`isEmpty()`、`isBusy()`、`hasError()`。

## 4. 成员变量

**小驼峰 + 尾下划线**（与 MaiChat 一致）：

```cpp
class MaiTurnRunner {
 private:
  std::string sessionId_;
  MaiMessage assistant_;
};
```

公开的纯数据结构体不加下划线：

```cpp
struct MaiSession {
  std::string id;
  std::string title;
};
```

## 5. 头文件里不写函数实现

**`.h` 只有声明，实现一律进 `.cpp`。**

```cpp
// MaiTime.h
class MaiTime {
 public:
  static MaiMillis getCurrentTime();   // 只声明
};
```

```cpp
// MaiTime.cpp
MaiMillis MaiTime::getCurrentTime() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
```

理由：改一行实现就让所有 include 了这个头的翻译单元重新编译；
而且头里的实现会把它依赖的头也一起拖进来，依赖关系越滚越大。

**两个例外**，都是语言本身要求的：

- 模板（`MaiResult<T>`）——实例化需要看得见定义。
- 纯数据结构体的聚合初始化和 `= default`。

例外之外一律进 `.cpp`，包括看起来只有一行的 getter。

## 6. 头文件保护

用 `#pragma once`，不用 include guard 宏。

## 6.5 排版

4 空格缩进，不用 tab。`public:` / `private:` **顶格**，不缩进
（和 MaiChat 一致）。每行不超过 100 列。`const std::string&` 而不是
`const std::string &`。

```cpp
class MaiEventEmitter {
public:
    explicit MaiEventEmitter(MaiEventBus& bus);

private:
    MaiEventBus& bus_;
};
```

仓库根目录有 `.clang-format`，上面这些都在里面。**改完代码跑一遍**：

```bash
clang-format -i --style=file <改过的文件>
```

有这份配置，这一节就不是纸面约定——机器能替人执行。
其余各节（命名、头文件不放实现）clang-format 管不了，靠 review。

## 7. include 顺序

```cpp
#include "MaiTurnRunner.h"   // 1. 自己的头，放第一行
                             //    （能证明这个头可以独立编译）
#include <string>            // 2. 标准库
#include <vector>

#include <curl/curl.h>       // 3. 第三方
#include <json.hpp>

#include "MaiIdGenerator.h"  // 4. 本项目其它头
#include "MaiSessionStore.h"
```

## 8. 注释

写"为什么"，不写"做了什么"——后者代码本身就说了。

```cpp
// 好：解释了不这么做会怎样
// 落库只在这里做一次。每个 delta 落一次盘等于每秒几十次 fsync。

// 坏：复述代码
// 把消息存进 store
```

踩过的坑要写进注释，尤其是"看起来可以那样写但实际上不行"的地方——
下一个人（包括三个月后的自己）会照着直觉重新踩一遍。

**改名时把注释一起改**。注释里留着旧名字比没有注释更糟：读的人会去
搜那个名字，搜不到，然后开始怀疑自己看错了代码。
