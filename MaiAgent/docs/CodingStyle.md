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
MaiHttpAdapter.h  MaiHttpAdapter.cpp
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

## 3.5 名字要说清"这是什么角色"，别只说"这是什么技术"

一个名字如果只交代了实现技术，读的人会自己脑补它在架构里的地位，
而那个脑补往往是错的。

真事：HTTP 适配器最早叫 `MaiHttpServer`，放在 `adapters/` 下、364 行、
核心一个字节都不依赖它。但凡看到这个名字的人第一反应都是
**"这个 agent 是个服务端"**——产物明明是 `maiagent` 这个库，HTTP 只是
为了让说不了 C++ 的 Electron 界面能连上来临时贴的一层壳。

改成 `MaiHttpAdapter` 之后歧义就没了：`Http` 说技术，`Adapter` 说角色。

```
MaiHttpServer   -> MaiHttpAdapter    Adapter 才是它的角色
maiagent-server -> maiagent-bridge   它不是产物，是座桥
```

同一条的其它落点：

```cpp
MaiTurnRunner      // 不叫 MaiLoop —— Loop 只说了形状，没说跑的是什么
MaiContextBuilder  // 不叫 MaiHistory —— 它在组装，不是在存
MaiSessionTitler   // 不叫 MaiTitleUtil —— Util 等于没说
```

判断方法：把名字念给一个没读过这份代码的人听，问他这东西在架构里
处于什么位置。答错了就是名字的问题，不是他的问题。

## 3.6 不用晦涩的简写

**名字要能读出它装的是什么。** 局部变量也一样——单字母和随手砍掉几个
音节的简写，读代码的人得往上翻好几行才知道它是什么。

```cpp
// 坏
const auto ctx = ...;        for (const auto& p : msg.parts)
MaiTurnRunner::Deps d;       std::mutex mu;

// 好
const auto context = ...;    for (const auto& part : message.parts)
MaiTurnRunner::Dependencies dependencies;   std::mutex mutex;
```

危害最大的是**一个字母在不同地方装不同东西**。改之前这个仓库里：
`e` 同时是 event、error、std::regex_error；`s` 同时是 session、string、
MaiToolState；`p` 同时是 fs::path、MessagePart 和 glob 匹配的下标。
读到 `s.id` 得先猜它是哪一个。

留下来没展开的，是展开之后反而更糟的几类：

| 保留 | 理由 |
|---|---|
| `i` `n` | 纯粹的下标和计数，没有别的含义 |
| `it` | 迭代器，C++ 里通用到不需要解释 |
| `fs` `json` | `namespace fs = std::filesystem`、`using json = nlohmann::json`，标准写法 |
| `HTTP` `SSE` `JSON` `UTF-8` | 标准缩略语，全称反而没人念 |
| `ses_` `msg_` `prt_` `evt_` `per_` | 线上 id 前缀，是契约的一部分，改了就不兼容 |

pimpl 的那个成员**不在**保留名单里：写全 `mImplementation` / `Implementation`。
`Impl` 是惯用法不假，但惯用法不等于自明——这条规范的标准是"读的人不用猜"，
不是"圈内人认得"。


**批量改名时注意 `#include`**：`\bh\b` 这种正则会把 `<curl/curl.h>` 改成
`<curl/curl.header>`。这个坑踩过一次，编译错误是"找不到头文件"，离真正的
原因很远。

## 4. 成员变量

**类的成员变量用 `m` 前缀 + 大驼峰**：

```cpp
class MaiTurnRunner {
private:
    std::string mSessionId;
    MaiMessage mAssistant;
    MaiError mError;
};
```

前缀是为了在成员函数里一眼分清成员和局部变量、参数。没有前缀时，
`session = x;` 这行要往上翻几十行才知道改的是成员还是局部。

**公开的纯数据结构体不加前缀**：

```cpp
struct MaiSession {
    std::string id;
    std::string title;
};
```

它们按字段名聚合初始化、按字段名序列化上线，加前缀既起不到区分作用，
又会让 C++ 字段名和线上字段名对不上。

> 注意：`MaiChat/desktop` 用的是尾下划线（`currentUserId_`，344 处）。
> 这一条是 MaiAgent 和它不一致的地方，按项目要求走 `m` 前缀。
> 两边哪天要统一，得动的是 MaiChat 那 344 处。

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

## 7.5 字符串字面量一律英文

**除注释外，代码里不出现中文。** 错误信息、日志、工具描述、CLI 帮助、
测试里的断言文案，全部英文。

理由有三层：

- 核心是个要被链进各种壳的库。把中文写死在库里，等于所有壳都被迫说中文，
  本地化就没地方做了——那是界面的事。
- 工具的 description 和 JSON Schema 是**给模型看的**，各家模型对英文
  工具描述的训练数据都多得多。
- 这台机器上 bash / PowerShell 会把 UTF-8 按 GBK 解释。之前有两次把
  shell 毁掉的中文当成了服务端的 bug 去查。

**唯一的例外是"被测数据"**：几个用例测的就是非 ASCII 路径和内容能不能
原样往返（MSVC 的 `fs::path` 按 ANSI 代码页解释 narrow 字符串，中文路径
当初是让进程直接挂掉，不是返回错误）。这种地方字节不能改，写成转义：

```cpp
// \u65b0\u76ee\u5f55 = 新目录
const char* kCjkDir = "\u65b0\u76ee\u5f55";
```

这样源码里没有汉字，被测的字节又一个不差。汉字写在注释里说明它是什么。

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
