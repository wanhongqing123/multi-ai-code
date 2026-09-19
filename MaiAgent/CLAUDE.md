# MaiAgent

MaiChatBuddy 的 agent 核心，纯 C++，可移植。产物是 `maiagent` 这个库——
`adapters/` 和 `cli/` 是可摘的壳，不是产物本身。

## 动代码之前先读编码规范

**[`docs/CodingStyle.md`](docs/CodingStyle.md) 是硬约束，不是建议。**

开头有一张「速查：硬约束」表，19 条，扫一遍就能开工；每条后面对应的章节
讲的是**为什么**——改到相关代码之前，把那一节读掉。里面每条规则背后都有
一次真实的代价，不是风格偏好。

最容易被想当然做错的四条，先记住：

1. **代码里除注释外不出现中文。** 注释是中文，字面量一律英文。
2. **碰文件不要用 `std::filesystem` / `fstream`**，用 `MaiFilePath` +
   `MaiFileSystem`（走系统 API）。MSVC 的 `fs::path` 按 ANSI 代码页解释
   narrow 字符串，中文路径会让进程直接挂掉。
3. **`.h` 里不写函数实现**（模板和 `= default` 除外）。
4. **通用基础组件先看 `E:\OpenSource\chromium\src\base` 怎么做的，
   agent 相关的看 `third_party/aicli/codex/codex-rs`。** 不要自己发挥——
   这两处已经各让我翻过一次车，规范 0.5 节写着具体是哪两次。

## 改完之后

```bash
clang-format -i --style=file <改过的文件>
```

构建和跑测试见 [`README.md`](README.md)。**构建不需要网络**——第三方依赖
全在 `third_party/` 里。

提交前至少跑一遍八套单元测试；动到 HTTP 适配器或存储的话，
`tests/e2e/` 下那四个脚本也跑一下。

---

> 这个文件只是入口。规则的**唯一出处**是 `docs/CodingStyle.md`——
> 规则有变动就改那一份，不要往这里抄，抄了必然走样。
> `AGENTS.md` 和这个文件内容一致，是给认那个名字的工具看的。
