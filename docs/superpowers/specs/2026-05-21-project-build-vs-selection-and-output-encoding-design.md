# 项目构建环境 Visual Studio 实例选择与输出编码设计

## 背景

当前项目构建功能已经支持按步骤执行 `MSYS2` 或 `Visual Studio Developer Command Prompt` 环境下的构建命令，但仍有两个明显缺口：

1. `Visual Studio` 环境解析当前固定走 `vswhere -latest`，用户无法明确指定某个已安装版本或实例。
2. 构建输出当前按默认字符串方式解码，Windows 客户机上常见的 `GBK` / ANSI 输出会在构建面板中出现乱码。

这两个问题在真实客户环境中会直接影响可用性：

- 一台机器可能同时安装 `Visual Studio 2022 Community`、`Build Tools`、`2026 Preview` 等多个实例，构建步骤必须绑定到用户明确选中的实例。
- Windows 本地工具链、MSBuild、第三方脚本、MSYS2 子进程的输出编码并不统一，必须允许每个步骤单独覆盖解码策略。

## 目标

- 允许每个 `Visual Studio` 构建步骤明确绑定到一个本机可用安装实例。
- 如果步骤绑定的实例失效，运行时直接报错并阻止执行，不自动回退。
- 允许每个构建步骤单独配置输出编码，覆盖 `MSYS2` 和 `Visual Studio` 两类环境。
- 在设置页中展示当前机器上可用的 Visual Studio 安装实例，并支持手动刷新。
- 在构建面板和失败上下文中保留足够信息，帮助定位“实例错误”或“编码错误”。

## 非目标

- 不支持自动回退到“最新实例”或“同版本实例”。
- 不支持在第一版中区分或管理多个远程构建主机。
- 不支持在第一版中增加 `Shift-JIS`、`Big5` 等额外编码选项。
- 不修改现有构建步骤的执行顺序、停止逻辑或失败分析总体流程。

## 现状

### 构建配置

当前 `BuildStepConfig` 仅包含：

- `id`
- `name`
- `envType`
- `cwd`
- `command`
- `enabled`

因此：

- `visual-studio` 步骤没有可持久化的实例绑定字段。
- 所有步骤都没有输出编码字段。

### Visual Studio 环境解析

当前 `electron/build/visualStudio.ts` 仅提供单一解析入口：

- 通过 `vswhere -latest` 获取安装路径
- 拼接 `VsDevCmd.bat`
- 读取 `set` 输出并构造环境变量

因此用户无法控制到底使用哪一个安装实例。

### 构建日志解码

当前 `electron/build/runner.ts` 在处理 `stdout` / `stderr` 时直接对数据块做默认字符串转换。

这意味着：

- `UTF-8` 输出通常可读
- Windows ANSI / `GBK` 输出容易乱码
- 用户无法按步骤手动修正

## 设计概览

本次采用“实例精确绑定 + 步骤级编码覆盖”的方案：

1. 扩展构建步骤配置，加入 `visualStudioInstanceId` 和 `outputEncoding`。
2. 新增 Visual Studio 实例枚举能力，供设置页和运行时共享。
3. `visual-studio` 步骤运行时按实例 id 精确解析开发环境，不再允许 `latest`。
4. 每个步骤均可选择 `自动 / UTF-8 / GBK` 输出编码。
5. 构建日志在进入运行态日志模型前先按步骤编码解码。

## 数据模型

### BuildStepConfig 扩展

新增字段：

- `visualStudioInstanceId?: string`
- `outputEncoding?: 'auto' | 'utf8' | 'gbk'`

约束规则：

- `envType === 'visual-studio'` 时，`visualStudioInstanceId` 必填。
- `envType === 'msys'` 时，`visualStudioInstanceId` 被忽略。
- `outputEncoding` 对两类环境均生效。

默认值：

- `visualStudioInstanceId = ''`
- `outputEncoding = 'auto'`

### Visual Studio 实例信息

新增渲染与运行时共用的数据结构：

- `instanceId: string`
- `displayName: string`
- `installationPath: string`
- `productLineVersion: string | null`
- `isPrerelease: boolean`

`displayName` 用于设置页展示，例如：

- `Visual Studio 2022 Community`
- `Visual Studio 2022 Build Tools`
- `Visual Studio 2026 Preview`

## 配置读取、迁移与校验

### 读取时迁移

旧配置读取后自动补默认值：

- 缺失 `outputEncoding` 时补为 `auto`
- 缺失 `visualStudioInstanceId` 时补为空字符串

读取阶段不直接报错，以便旧项目仍能打开设置页。

### 保存时校验

保存构建配置时新增校验：

- `visual-studio` 步骤必须填写 `visualStudioInstanceId`
- `outputEncoding` 必须是 `auto / utf8 / gbk`

错误信息应保留现有结构化明细格式，例如：

- `build_config.steps[0].visualStudioInstanceId`
- `visual studio instance must be selected for visual-studio steps`

## Visual Studio 实例发现

### 新增能力

在主进程新增实例枚举能力：

- `listVisualStudioInstallations()`

它负责调用 `vswhere` 返回当前机器所有可用构建实例，而不是只取最新版本。

### 设计要求

- 只枚举满足构建所需组件的安装实例
- 输出结果对设置页和运行时保持一致
- 设置页刷新与构建前校验必须复用同一套发现逻辑

### 环境解析

`resolveVisualStudioEnvironment()` 改为按 `visualStudioInstanceId` 工作：

- 先查找实例
- 再解析该实例的 `VsDevCmd.bat`
- 再导出环境变量

如果找不到实例，返回明确错误，不回退。

## 设置页设计

### 通用字段

每个构建步骤新增一个 `输出编码` 下拉：

- `自动`
- `UTF-8`
- `GBK`

该字段在 `MSYS2` 和 `Visual Studio` 步骤上都显示。

### Visual Studio 专有字段

当 `envType === 'visual-studio'` 时，额外显示：

- `Visual Studio 实例` 下拉
- `刷新实例列表` 按钮

行为要求：

- 下拉展示当前机器所有可用实例
- 选项值保存为 `instanceId`
- `displayName` 作为用户可读标签

### 失效实例提示

如果当前步骤保存的 `visualStudioInstanceId` 不在最新扫描结果中：

- 步骤卡片显示错误提示
- 保存时阻止继续保存无效配置
- 即使旧配置已存在，运行时也会再次校验并直接失败

### 环境切换

- 从 `MSYS2` 切到 `Visual Studio`：要求用户补选实例
- 从 `Visual Studio` 切回 `MSYS2`：清空实例 id，避免保留无意义状态

## 运行时执行

### MSYS2 步骤

保持现有执行方式：

- `bash -lc`
- 现有 `cwd` 与环境变量处理逻辑不变

### Visual Studio 步骤

执行前步骤：

1. 根据 `visualStudioInstanceId` 查找实例
2. 若不存在，当前步骤直接失败
3. 若存在，解析对应 `VsDevCmd.bat`
4. 用该环境启动 `cmd.exe /d /s /c <command>`

### 失效实例行为

运行时如果实例不存在或无法初始化环境：

- 当前步骤标记为失败
- 整次构建停止
- 错误信息明确指出是“所选 Visual Studio 实例不可用”或“Visual Studio 环境初始化失败”

不允许：

- 自动回退到任意最新实例
- 自动回退到同版本其他实例

## 输出编码与乱码处理

### 编码选项

每个步骤支持：

- `auto`
- `utf8`
- `gbk`

### 自动策略

`auto` 的默认行为：

- `MSYS2`：优先按 `UTF-8` 解码
- `Visual Studio`：优先按 Windows ANSI 代码页解码

### 手动覆盖

当客户输出出现乱码时，可将当前步骤手动改为：

- `UTF-8`
- `GBK`

该覆盖仅影响当前步骤，不影响其他步骤。

### 实现要求

日志采集必须在进入构建运行态日志模型前先按步骤编码解码。

为完整支持 `GBK`，实现层应引入成熟的编码库，而不是依赖 Node 默认解码能力。

## 构建面板与失败上下文

### 构建面板展示

构建步骤卡片建议补充展示：

- 当前环境类型
- 若为 `Visual Studio`，显示所绑定的实例名称
- 当前输出编码设置

### 失败上下文

失败上下文建议补充：

- `visualStudioInstanceId`
- `visualStudioDisplayName`
- `outputEncoding`

这样后续 AI 失败分析可区分：

- 是实例绑定问题
- 还是输出乱码导致的误判

## IPC 与前端接口

新增预加载 / IPC 接口：

- 列出 Visual Studio 安装实例
- 刷新实例列表

现有构建配置读写接口继续沿用，但数据结构升级为包含新增字段。

## 测试策略

### visualStudio.ts

- 枚举多个安装实例
- 指定实例环境解析成功
- 指定实例不存在时报错
- 环境初始化失败时报错

### config.ts

- 旧配置迁移补默认值
- `visual-studio` 未选实例时报校验错误
- `outputEncoding` 非法时报校验错误

### runner.ts

- `visual-studio` 步骤按指定实例启动
- 实例不存在时直接失败
- `auto / utf8 / gbk` 三条解码路径可覆盖

### 设置页

- 渲染 `输出编码` 下拉
- 渲染 `Visual Studio 实例` 下拉
- 失效实例提示可见
- 环境切换时字段展示与清理逻辑正确

## 风险与权衡

### 风险

- `vswhere` 返回字段与客户机安装形态可能存在差异
- `auto` 编码判断只能覆盖主流情形，无法保证所有第三方脚本都自动正确
- 老项目中的 `visual-studio` 步骤升级后需要用户首次补选实例

### 权衡

- 不自动回退实例会让一部分旧配置在客户机变更后立即失败，但这是符合预期的显式失败，比悄悄换环境更安全
- 将编码配置下沉到每个步骤会增加设置页复杂度，但能最小化用户对不同工具链的排障成本

## 验收标准

- 用户可以在设置页为每个 `Visual Studio` 步骤明确选择一个本机可用实例。
- 所选实例信息会被保存到项目构建配置中，并在下次打开时恢复。
- 如果所选实例失效，设置页和运行时都会明确报错，且构建不会自动切换到其他版本。
- 用户可以为任意步骤选择 `自动 / UTF-8 / GBK` 输出编码。
- `MSYS2` 与 `Visual Studio` 两类常见乱码场景可通过步骤级编码覆盖修复。
- 构建面板能展示步骤所用环境与编码信息。
