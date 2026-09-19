# MaiAgent 编码规范

这份规范和 `MaiChat/desktop` 保持一致——两个工程以后要互相链接，
风格不统一会让人每切换一次目录就要重新适应一遍。

规范是硬约束，不是建议。新增文件按这套来，改到旧文件时顺手纠正。

---

## 速查：硬约束

赶时间的话看这一段就够开工了。每一条后面的章节讲的是**为什么**——
改到相关代码之前，把对应那节读掉。

| # | 约束 | 细节 |
|---|---|---|
| 0 | agent 的形状、协议取值看 **codex**；通用基础组件看 **chromium 的 base**。不要自己发挥 | 0 / 0.5 |
| 1 | 文件名大驼峰 + `Mai` 前缀，`.h`/`.cpp` 成对同名 | 1 |
| 2 | 类型大驼峰 + `Mai` 前缀，**不用命名空间** | 2 |
| 3 | 成员函数小驼峰；布尔查询 `is` / `has` 开头 | 3 |
| 4 | 名字说清"角色"，不是只说"技术"；**不用晦涩简写** | 3.5 / 3.6 |
| 5 | 成员变量 `mXxxYyy`；公开数据结构体的字段不加前缀 | 4 |
| 6 | **`.h` 里不写函数实现**（模板和 `= default` 除外） | 5 |
| 7 | `#pragma once`；4 空格；访问说明符顶格；100 列；改完跑 `clang-format` | 6 / 6.5 |
| 8 | include 顺序：自己的头第一行，然后标准库、第三方、本项目 | 7 |
| 9 | **代码里除注释外不出现中文**。被测的非 ASCII 数据写成 `\u` 转义 | 7.5 |
| 10 | 测试先问"要测的东西在哪一层"；协议断言要对着规范，不能对着自己 | 7.6 |
| 11 | **公开头的注释要详细全面**：契约、线程、失败、生命周期、哨兵值、坑 | 7.7 |
| 12 | 注释写"为什么"；**改名时把注释一起改** | 8 |
| 13 | **中文注释在标点处断行**，不要在词组中间断；一行排到 100 显示列再换 | 8.5 |
| 14 | 核心不认识 HTTP 服务端；公开头里不出现 JSON / HTTP / Qt | 9 |
| 15 | **先落库，再广播** | 10 |
| 16 | 闸门的兜底方向**永远是"不放行"** | 11 |
| 17 | 回灌给模型的话**必须是真的** | 12 |
| 18 | 碰文件走系统 API，**不用 `std::filesystem` / `fstream`** | 12.5 |
| 19 | 线程要有名字；事件处理函数里不许做慢活（有守卫） | 12.6 |
| 20 | 下结论前先问"这套观测能不能看见目标"；改断言做红→绿对照 | 13 |

改完代码跑一遍：

```bash
clang-format -i --style=file <改过的文件>
cmake --build build && ctest --test-dir build   # 或直接跑 build/bin/Mai*Tests
```

---

# 一、命名与排版

## 0. agent 相关的看 codex

**参照系是 codex，不是 opencode。** agent 的形状、命名、协议取值有疑问，
先去 `third_party/aicli/codex/codex-rs` 里看它怎么做的。

（通用基础组件看另一个参照系，见下一节。）

已经照着它来的：

| 我们 | codex |
|---|---|
| `MaiAgent::submit(MaiOperation)` + 事件流 | `codex_thread.rs` 的 `submit(op)` / `next_event()` |
| `MaiPermissionDecision` 的取值 | `protocol.rs` 的 `ReviewDecision` |
| `MaiOpenAiClient` 里的行缓冲 | `ollama/src/line_buffer.rs` |

**opencode 只在一个地方还有影响力：线上 id 的前缀**（`ses_` / `msg_` /
`prt_` / `evt_` / `per_`）。那不是我们选的风格，是当初那个 Electron 界面
（派生自 opencode）在两处硬校验：`packages/sdk/openapi.json` 里的 pattern，
以及路由里的 `startsWith("ses")`。属于被迫兼容，不是设计偏好——
`MaiIdGenerator.h` 的注释里写明了，并记着 codex 用的是裸 UUIDv7。

抄之前先核对真实源码，别照着印象写。这条踩过坑：曾经把"codex 接不了智谱"
当成事实讲出去，而实际上人家早就加了 Responses 端点。

## 0.5 通用组件先看 chromium 的 base，别自己发挥

路径、文件、线程、字符串、容器、时间、同步原语——**这类谁都要用的基础
组件，动手之前先去 `E:\OpenSource\chromium\src\base` 看它怎么做的。**

两个参照系分工不同：

| 看哪个 | 管什么 |
|---|---|
| `third_party/aicli/codex/codex-rs` | agent 的形状、协议取值、词汇 |
| `E:\OpenSource\chromium\src\base` | 通用基础组件 |

### 为什么不能自由发挥

这类东西看着简单，坑全在边角上，而且**坑的代价通常是"悄悄错"而不是
"编译不过"**。base 是被几亿台设备、十几年、所有主流平台锤过的，
它每个奇怪的决定背后都有一个我们还没遇到的 bug。

这一节的两条都是这次现学现卖的教训：

**一、路径为什么不能统一成 UTF-8**

本来已经写好了一版 "内部一律 UTF-8 字符串" 的 `MaiPath`。翻
`base/files/file_path.h` 的开头注释时看到这句：

> ...has an impact on **correctness** on platforms that do not have
> well-defined encodings for pathnames.

POSIX 的文件名是**任意字节序列**，根本不保证是合法 UTF-8。Linux 上一个
用 Latin-1 命名的文件，强行当 UTF-8 处理就会丢掉或改写它的名字。
Windows 那边是 UTF-16，可能含未配对代理项，转一圈回不来。

所以 base 的做法是内部存平台原生串，只在边界转。整版推倒重写，
才有了现在的 `MaiFilePath`。**自己想是想不到这一层的**——在 Windows 上
测永远不会发现。

**二、`__try` 不能写在有析构对象的函数里**

`platform_thread_win.cc` 里那段设置线程名的代码，上面明明白白写着：

> This function has try handling, so it is separated out of its caller.

我读到了这句，没当回事，照着自己的想法把 `__try` 写进了
`setCurrentName`——MSVC 当场 C2712。它把那段单拎出来不是风格偏好，
是语言限制。

### "参考"是什么意思

不是把 base 整个搬过来。base 自己的 README 就写着：

> The bar for adding stuff to base is that it must have demonstrated wide
> applicability. ... sometimes even duplication is OK and inevitable.

做法是：**看懂它为什么那么做，取我们真正需要的那一小撮，把理由写进
注释。** 现在这样借过的有：

| 我们的 | 对应 base 里的 |
|---|---|
| `MaiFilePath` | `base::FilePath`（原生串、`GetComponents`、`IsParent`）|
| `MaiFileSystem` | `base/files/file_util.h` + `file_enumerator.h` |
| `MaiThread::setCurrentName` | `base::PlatformThread::SetName` |
| `MaiScopedDisallowBlocking` | `base/threading/thread_restrictions.h` |

以后可能会用上、但现在还不到时候的：

    base/strings/        字符串工具、编码转换
    base/containers/     flat_map / small_vector 这类
    base/synchronization/ WaitableEvent、Lock
    base/threading/sequence_bound.h   对象绑到某个线程（接 Qt 时会用上）
    base/threading/hang_watcher.h     卡死检测（有线程池之后）
    base/numerics/       安全的数值转换、溢出检查

**抄之前先核对真实源码**，别照着印象写——这条在第 0 节也说过一次，
是同一个道理。

## 1. 文件命名

**大驼峰，`Mai` 前缀，`.h` / `.cpp` 成对同名。**

```
MaiTime.h        MaiTime.cpp
MaiError.h       MaiError.cpp
MaiSessionStore.h
MaiFilePath.h    MaiFilePath.cpp
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

（那个界面后来撤了，适配器和 `maiagent-bridge` 跟着整个删掉，仓库里已经
找不到这两个文件——**别去搜**。这一条留着是因为教训还在：名字取错的那几个月，
每个新来的人都要被口头纠正一次"这不是服务端"。）

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

## 7.6 测试：先问"要测的东西在哪一层"

**别为了测上层逻辑去搭下层的真实环境。**

`MaiModelClient` 是个抽象接口，`MaiAgent` 构造时接的就是它。所以测"一轮
对话的生命周期""工具循环""权限闸门"，直接实现那个接口就行
（`tests/support/MaiFakeModelClient`），不需要 HTTP。

以前那三个文件各起一个 httplib 服务端，于是每条断言都要绕一圈：

```
组装 MaiModelRequest -> 序列化成 JSON -> TCP -> curl -> SSE 解析 -> 回调
```

想断言"第二轮带上了上一轮的回答"，得去 `body["messages"][1]["content"]`
里翻——而那句话本来就在 `MaiModelRequest` 这个结构体里放着。

换掉之后：断言写 `request.messages[1].role == MaiModelRole::Assistant`，
编译器帮着查类型；没有端口、线程和 sleep，也就没有"机器一慢就偶发失败"。

**分层同样要守住**：

| 要测什么 | 在哪测 | 用什么 |
|---|---|---|
| agent 的行为 | `MaiTurnRunnerTests` / `MaiToolLoopTests` / `MaiPermissionTests` | `MaiFakeModelClient`，进程内 |
| SSE 解析、分片聚合 | `MaiModelClientTests` | 真 socket，能造出真实的分片 |
| **线格式**（字段名、嵌套） | `MaiModelClientTests` | 真 socket，断言**发出去的字节** |
| 整条链路 | `tests/e2e/*.py` | 真 bridge + 真 curl |

codex 也是这么分的：`core` 的集成测试用 wiremock 起真 HTTP（dev 依赖，
不进产物），而 `line_buffer` 那种纯解析逻辑是直接喂字节的单元测试
（`codex-rs/ollama/src/line_buffer_tests.rs`）。

### 测协议时，断言要对着规范，不能对着自己

这条是有代价才学到的。`buildRequestBody` 曾经把工具结果的字段写成
`toolCallId`，而 OpenAI 的线格式是 `tool_call_id`。当时的用例写的是：

```cpp
CHECK(m.value("toolCallId", "") == callId);   // 永远绿
```

拿自己的字段名去核自己的输出，什么也没验证。真跑起来服务端会说缺
`tool_call_id`，或者模型认不出这是哪次调用的结果，下一轮把同样的工具
再调一遍。

所以：**协议相关的断言，字段名要从规范或真实样例里抄，不能从自己的
结构体里抄。** 改完之后做一次红→绿对照——把 bug 放回去，确认用例真的
会红。

## 7.7 公开头文件的注释要详细、要全面

`.h` 是这个库的**说明书**。用的人多数时候不会去翻 `.cpp`——他们读头文件，
然后按自己的理解去用。头文件里没写清楚的东西，就会以 bug 的形式被重新
发现一遍。

所以公开头（`include/` 下面那些）的注释密度和别处不是一个标准。
实现文件里"代码本身说清楚了就别复述"，头文件里则要把**代码说不出来的
那些事**全部写出来：

### 一个类型/接口要交代什么

| 项 | 说明 |
|---|---|
| **它是什么** | 一句话说清职责，以及它**不**负责什么 |
| **为什么是这个形状** | 当初否掉的方案和否掉的理由 |
| **谁填、谁读** | 数据从哪来、到哪去，依赖方向 |
| **线程契约** | 谁可以调、在哪个线程上调、回调在哪个线程上触发 |
| **失败时会怎样** | 返回什么、已经交付的东西还算不算数 |
| **生命周期** | 指针/引用能活多久，能不能持有 |
| **不变量** | 哪些字段创建后不能变、哪些取值绑死了外部契约 |
| **坑** | "看起来可以那样写但实际不行"的地方 |

### 字段也要逐个说

别只给类型加一段总述就完事。每个字段该说的是：它的含义、合法取值、
空值代表什么、谁负责填、改了会影响谁。

```cpp
// 差：类型名已经说了的东西，重复一遍
std::string callId;  // 调用 id

// 好：说了代码说不出来的
// 模型给的 tool_call_id，回灌结果时要**原样带回**。
// 对不上的话模型认不出这是哪次调用的结果，下一轮会把同样的工具再调一遍。
std::string callId;
```

```cpp
// 差
MaiMillis completed = 0;

// 好：哨兵值的含义、谁来填
// 这一轮结束的时间。**0 表示还在进行中**——流式期间 assistant 消息
// 一直是 0，直到 MaiTurnRunner::finish 填上。
MaiMillis completed = 0;
```

### 哨兵值和"绑死了的取值"必须写

这两类是最容易出事的：

- **哨兵值**：`completed == 0` 表示进行中、`temperature < 0` 表示不传、
  `timeoutMs == 0` 表示无限等。不写的话下一个人会用 0 当"零超时"。
- **绑死了外部契约的取值**：`MaiToolState` 的枚举顺序和 SQLite 里的列值
  绑死，`ses_` / `msg_` 前缀和界面的校验绑死。这种地方要明确写"**不要改**"
  以及改了会怎样。

### 默认实现的虚函数，要写"忘了覆盖会怎样"

`MaiTool::requiresApproval()` 默认返回 false。加一个会改东西的新工具时
忘了覆盖它，后果是模型可以不经用户同意改文件，**而且没有任何报错**。
这种"漏了不会报错"的地方，头文件里必须点名。

### 衡量

不是凑字数。判断标准是：**一个没读过实现的人，只看这个头文件，能不能
正确地用它、并且知道什么时候会出错。** 不能就是还没写够。

现在 `include/` 下的注释占比在 30%~75% 之间，最低的那几个是最早写的——
改到它们的时候顺手补上。

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


## 8.5 中文注释不要动不动就换行

**在标点处断行，不要在词组中间断。** 一行排到 100 个显示列（中文字算两列）
再考虑换。

这是一条真实的返工：曾经有 1034 行中文注释，其中 371 行（35%）是在半句话
中间断掉的，断点处只有 48 到 69 个显示列——远没到需要换行的宽度。读起来
是这样：

```cpp
// 坏：在词组中间断，眼睛要跨行才能把一个意思读完
// 这是工具层唯一的安全边界。模型会试着越界——有时是它自己想看看
// ../.env，有时是被提示词注入诱导的。所以这里按"默认拒绝"来
// 写。

// 好：断在句号处，每行是一个完整的意思
// 这是工具层唯一的安全边界。模型会试着越界——有时是它自己想看看 ../.env，有时是被提示词注入诱导的。
// 所以这里按"默认拒绝"来写。
```

为什么值得单独立一条：英文换行断在空格处，读的人天然知道下一行是同一个
词组的延续；中文没有词间空格，断行处就是视觉上的一个停顿，断在词组中间等于
在句子里插了一个假的标点。注释本来是为了省读代码的力气，排得碎反而更费劲。

可以断的位置：`。！？；：，、` 之后，以及破折号和括号的边界。
不要断在：词组中间、数字和单位之间、标识符中间（`MaiContextBuilder` 不能
拆成两行）、`——` 的两个字符之间。

几类东西**不参与**这个规则，照原样排：列表项（`//   1.` `//   - `）、
表格、`// ──────` 这类分隔线、注释里的代码示例。它们的换行是结构性的。


---

# 二、架构与行为约定

命名管的是"读起来顺不顺"，这一部分管的是"写错了会出什么事"。
每一条后面都跟着一次真实的代价。

## 9. 核心不认识 HTTP 服务端

产物是 `maiagent` 这个库。仓库里**一个 HTTP 服务端都没有**了。

这一条以前的说法是"`adapters/` 和 `cli/` 是可摘的壳"。后来真摘了：
Electron 界面撤掉，`adapters/MaiHttpAdapter`（REST + SSE）和
`maiagent-bridge` 一起删，637 行。**核心一行都没改**——那就是当初要有这条
边界的全部意义。现在唯一的宿主进程是 `maiagent-console`，
Qt / iOS / Android / 嵌入式都和它一样：进程内构造 `MaiAgent`，
订阅事件总线，`submit` 操作。

**别把两个 HTTP 搞混**，它们方向相反，现在只剩一个：

| | 是什么 | 谁用 | 现状 |
|---|---|---|---|
| libcurl | HTTP **客户端** | 核心去调大模型 | 在，摘不掉 |
| cpp-httplib | HTTP **服务端** | 以前是适配器，现在只剩 `MaiModelClientTests` 拿它当假模型 | 产物里没有 |

这条边界可以直接量，不用靠自觉：

```bat
:: 产物里不该有服务端
dumpbin /symbols build\lib\maiagent.lib | findstr /C:httplib   :: 应为 0

:: 更硬的一道：maiagent-console 只链 maiagent，httplib 只在
:: mai_thirdparty_for_tests 里。谁把服务端漏进核心，这个目标当场链不过。
:: tests/e2e/console_e2e.py 还会翻产出的 exe 复核一遍。
```

同理，公开头 `include/` 里不出现 JSON、HTTP、Qt 的类型。实现里 JSON 只在
"协议本身就是 JSON"的地方出现（SSE 载荷、工具参数、存进库的片段）。

## 10. 先落库，再广播

事件说"某个 part 更新了"，界面拿着那个 id 去拉全量却拉不到——因为那会儿
还没入库。工具跑几秒是常事，等授权更是以分钟计，这个窗口期足够用户刷新
一次。

```cpp
// 对
mStore->putMessage(sessionId, message);
mEmitter->emitPart(MaiEventType::MessagePartUpdated, ...);

// 错：顺序反了，事件先出去，界面拉不到
```

这个洞是写"等授权期间工具卡应当是 Pending"那条断言时才暴露的——
本来只想验证状态机。

## 11. 闸门的兜底方向永远是"不放行"

权限、路径边界这类地方，**每一条异常路径都要收敛到拒绝**：

- 中断醒来、超时醒来 → 拒绝
- 没接闸门（`mPermissions == nullptr`）→ 拒绝，不是放行
- `decision` 字符串认不出来 → 返回 false 并且**不改 out 参数**，HTTP 层 400
- 析构时还在等的 → 拒绝

兜底成放行意味着"没人点头"也能改用户的文件，而且毫无痕迹。界面写错一个
拼写就等于闸门被拆掉。

写解析函数时尤其注意：`maiParsePermissionDecision` 认不出来时如果顺手把
`out` 设成了某个默认值，调用方忘了看返回值就会拿到一个它没要求的裁决。

## 12. 回灌给模型的话必须是真的

模型会拿你给它的话当事实，然后据此行动。所以那些话不是提示文案，是输入。

超时曾经被并进"拒绝"，于是回灌的是 "The user denied this tool call...
try a different approach"。用户可能根本没看见那个请求——模型照着"换个
做法"去试，而真相是没人在，换什么做法都一样没人批。现在超时有自己的话。

同理，拒绝时必须明确写"不要重试同样的操作"。不写的话模型会拿一模一样的
参数再调一次，把 `maxToolIterations` 那 12 圈全烧在同一个被拒的操作上，
用户看到的是 agent 卡住了。

## 12.5 碰文件走系统 API，不用 std::filesystem

**库代码里不出现 `<filesystem>` 和 `<fstream>`。** 路径用 `MaiFilePath`，
文件操作用 `MaiFileSystem`。Windows 上它们走宽字符 Win32 API，
其它平台走 POSIX。

三条理由，第一条是踩出来的：

1. **MSVC 的 `fs::path` 把 narrow 字符串按当前 ANSI 代码页解释**（中文
   机器是 GBK），而我们的路径全部来自 JSON，是 UTF-8。中文文件名不是
   优雅失败，是进程直接挂掉（`STATUS_STACK_BUFFER_OVERRUN`）。
2. **错误信息会被糊掉。** `std::filesystem` 把 Win32 错误码塞进
   `std::error_code` 之后，"没找到"和"没权限"变成同一句含糊的话。
   自己调就能直接拿 `GetLastError()` / `errno`，分得出 `NotFound` 和
   `InvalidInput`。
3. **嵌入式和移动端的工具链未必带 `<filesystem>`**，而这个项目就是奔着
   那些平台去的。

### 内部存平台原生串，不统一成 UTF-8

这一条是从 chromium 的 `base::FilePath`
（`E:\OpenSource\chromium\src\base\files\file_path.h`）学来的，
理由是**正确性**不是性能：

- Windows 文件名是 UTF-16，可能含未配对代理项，转成 UTF-8 再转回来不
  保证原样。
- POSIX 文件名是任意字节序列，**根本不保证是合法 UTF-8**。Linux 上一个
  用 Latin-1 命名的文件，强行当 UTF-8 处理就会丢掉或改写它的名字。

所以 `MaiFilePath::StringType` 在 Windows 上是 `std::wstring`，其它平台
上是 `std::string`。只在两个边界上转 UTF-8：模型送进来的 JSON 参数，
和回给模型的文本。那两处本来就避不开，因为模型只会说 UTF-8。

### 判断"在不在某个目录下"，先解析再逐段比

两步都不能省：

```cpp
const MaiFilePath resolved = MaiFileSystem::resolve(target);  // 消 ".."，解符号链接
if (resolved != rootPath && !rootPath.isParentOf(resolved)) return {};  // 逐段比
```

- 只做词法规范化不够：root 里放一个指向 `C:\` 的符号链接或目录联接，
  词法上看它就在 root 里面。
- 比字符串前缀更不行：`/server/app-secrets` 确实以 `/server/app` 开头，
  却是另一个目录。`isParentOf` 是逐段比的，Windows 上还不分大小写。

这条有红→绿对照守着：把 `isParentOf` 换成前缀比较，`MaiToolTests` 里
7 条断言会变红。

### 测试里可以留 std::filesystem

`MaiFilePathTests` 故意用 `std::filesystem` 造目录和文件，再用
`MaiFileSystem` 去读。两个独立实现互相印证——要是造和读都用自己那套，
编码转换整个写错了也能自洽，测试一路绿，而别的程序建的文件我们全读不了。
那正是当初那个中文路径 bug 的形状。

## 12.6 线程要有名字，契约要有人守

两件从 chromium 的 `base/threading` 借来的事。

### 起名字

`MaiThread::setCurrentName("mai-turn")`。agent 是多会话并发的，一个进程里
同时跑着好几个轮次的工作线程；出问题抓一个 dump，没名字的话调试器里只有
一串线程 ID，得靠调用栈一个个认。

Windows 上走 `SetThreadDescription`（Win10 1607+，动态取），**不挂调试器
也生效**——任务管理器、事后用 WinDbg 打开 dump 都看得见。老系统降级成
MSVC 那套魔法异常（`RaiseException(0x406D1388)`），只在挂着调试器时有用。

名字要短：Linux 的 `prctl(PR_SET_NAME)` **上限 15 字节**，超了会被截断。

> 踩过的坑：`__try` 不能写在"需要对象展开的函数"里（MSVC 报 C2712）。
> 把它单独拎成一个只收裸指针的函数。chromium 的
> `platform_thread_win.cc` 里那条注释就写着这件事，我读到了却没照做，
> 编译器当场教育了一遍。

### 契约要有人守

这个工程里有条约定一直只写在注释里：

> MaiEventBus 的处理函数**在 publish 的那个线程上同步调用**，流式期间
> 那就是网络读线程。处理函数里不要做慢活。

光靠注释守不住。以后有人在事件处理函数里顺手读个文件、写一次库，表现
出来不是崩溃，是"模型吐字变卡了"——这种症状没人会联想到事件总线。

现在 `MaiEventBus::publish` 调处理函数时套上 `MaiScopedDisallowBlocking`，
而 `MaiFileSystem` 的读写会先问一句 `maiAssertBlockingAllowed()`。
违反了当场终止，并打印一段说清楚"为什么不行、该怎么改"的话。

**不做成 Debug-only**：这个项目的测试是 Release 构建的，只在 Debug 里查
等于自己的测试永远查不到。检查本身是读一个 `thread_local` 的 bool，
和它守着的那些系统调用比可以忽略。

确实必须在那儿做慢活时，用 `MaiScopedAllowBlocking` 明确开口子——
但先想想能不能把慢活挪出去（控制台的渲染线程就是这么做的：事件处理函数只入队，
排版和写都在另一个线程上）。

## 13. 验证之前先问"这套观测能不能看见目标"

报"全绿"或者"发现 bug"之前，先确认你的观测手段真的能看见要看的东西。

两次相反方向的教训：

- **看不见**：`CHECK(m.value("toolCallId", "") == callId)` 拿自己的字段名
  核自己的输出，永远绿。真实字段名是 `tool_call_id`，这个 bug 一直没被
  测出来。协议相关的断言要从规范或真实样例里抄。
- **以为看不见，其实看得见**：我曾断定"`chunk=1` 的分片测试没生效，TCP
  会把字节合并掉"。插探针实测：写回调被调 973 次，其中 665 次确实是
  1 字节。猜错了。

所以：**改断言之前做红→绿对照**——把 bug 放回去，确认用例真的会红；
下结论之前先测量，别从印象推断。
