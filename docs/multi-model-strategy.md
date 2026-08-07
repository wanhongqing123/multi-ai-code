# OpenCode 多模型管理策略（重新设计）

> 适用范围：Multi-AI Code 内置的 OpenCode fork（`multi-ai/im-bridge`）
>
> 目标：面向普通用户提供开箱即用的受控模型集合，同时让凭据策略可配置、工具执行不重复、故障切换可解释，并保持 TUI 与远程 IM 行为一致。

---

## 一、设计结论

本方案采用以下边界：

1. **模型目录和 API Key 均可按内部发行策略内置。** 同时保留 Token Broker 和用户 Key 模式，方便后续按发行范围切换。
2. **模型路由与 Agent 权限分离。** `build`、`plan` 等 Agent 继续表示工作模式和权限，不用作隐式模型分类器。
3. **同模型重试与跨模型 Failover 分离。** 同模型只做有限次数瞬时错误重试，耗尽后才进入候选模型链。
4. **发生工具副作用后不静默重放当前模型调用。** 防止 Shell、Edit、MCP 写操作被重复执行。
5. **每个候选模型都重新进行上下文检查和消息组装。** 不能把为 1M 上下文模型组装的请求直接交给 128K/200K 模型。
6. **视觉能力采用协作，不替换主模型。** 主模型按需调用视觉协作者；协作结果可缓存并持久化，历史图片不能在每次请求时重复分析。
7. **TUI 展示过程状态，IM 只发送一次终态回复。** 中间重试和候选模型失败不作为模型回复发送到 IM。
8. **只提供 Multi-AI Code 托管模式。** `/connect` 和自定义 Provider 入口从定制版 TUI 中移除，不提供恢复官方配置行为的开关。
9. **模型目录随应用以只读资源发布。** 托管模式始终显式指定打包内目录，不能读取宿主机残留的 models.dev 缓存。
10. **OpenCode 可变数据按 Multi-AI Code 账号隔离。** 不读写宿主机的全局 OpenCode 配置、数据库、状态和缓存。
11. **完全不使用 `.opencode`。** 不扫描、不读取、不创建项目目录或用户 Home 下的 `.opencode`。

当前方案不承诺“断网仍可对话”。离线时只能保证 OpenCode 可以启动并列出内置目录；远程模型调用仍需要网络。

---

## 二、目标与非目标

### 2.1 目标

- 普通用户无需手动选择 Provider 和模型即可开始任务。
- 默认只展示经过验证的模型，不加载完整 models.dev 目录。
- 根据输入模态、工具能力、结构化输出和上下文长度选择兼容模型。
- 对 429、5xx、连接超时和安全的流中断执行有限重试及跨 Provider Failover。
- 主模型不变，由其按任务需要调用可用的视觉模型分析图片，并继续负责工具使用和最终答复。
- 对每次模型切换保留审计信息，便于 TUI、日志和远程排障查看。
- OpenCode 主 Agent、子 Agent、TUI 和 IM 使用同一套路由及健康状态。

### 2.2 非目标

- 不通过模型路由自动改变 Agent 权限。
- 不保证不同模型生成完全相同的内容和工具调用。
- 不在发生外部副作用后自动重放整轮任务。
- 第一阶段不实现基于内容的复杂 AI Router；优先使用确定性能力匹配。

---

## 三、总体架构

```text
Multi-AI Code 设置/账号
        |
        | 受控目录路径、启用的 Provider、选定模式的凭据
        v
OpenCode Managed Runtime
        |
        +--> CuratedModelCatalog  模型元数据和路由角色
        +--> CredentialProvider   内置 Key、短期 Token 或用户 Key
        +--> ModelRouter          能力过滤和候选链
        +--> ModelHealth          重试、熔断、半开探测
        +--> VisionCollaborator   隔离调用视觉模型、返回分析结果和缓存
        +--> AttemptController    单次调用、Failover 和幂等边界
        |
        +--> SessionProcessor     流式输出和工具执行
        +--> TUI                  展示当前模型和切换状态
        +--> Remote IM Bridge     只转发绑定 replyId 的终态回复
```

组件职责必须保持单一：

| 组件 | 负责 | 不负责 |
|---|---|---|
| CuratedModelCatalog | 模型、能力、路由角色、端点元数据 | API Key、运行时健康状态 |
| CredentialProvider | 加载并按模式刷新凭据 | 决定使用哪个模型 |
| ModelRouter | 根据请求要求生成候选模型链 | 执行网络请求、切换 Agent |
| ModelHealth | 错误计数、Retry-After、熔断状态 | 修改会话历史 |
| VisionCollaborator | 隔离调用视觉模型、传递原始附件、描述缓存和历史复用 | 替换主模型、接管主任务 |
| AttemptController | 有限重试、候选切换、副作用边界 | 工具具体实现 |

---

## 四、受控模型目录

### 4.1 保留目录加载逻辑，关闭在线来源

当前 OpenCode 已支持：

- `OPENCODE_MODELS_PATH`：指定模型目录文件。
- `OPENCODE_CONFIG_CONTENT`：按进程注入 Provider、默认模型等配置。
- `enabled_providers`：限制实际启用的 Provider。

因此托管模式使用打包资源，例如：

```text
resources/opencode/managed-models.json
```

启动 OpenCode 时设置：

```text
OPENCODE_MODELS_PATH=<absolute path>/managed-models.json
```

该路径由 Multi-AI Code 启动器按安装位置自动注入，用户不需要配置环境变量。显式目录优先于同源编译快照；定制版不读取历史缓存。

models.dev 网络访问实际有两个阶段：

1. **构建期**：`packages/opencode/script/generate.ts` 在没有 `MODELS_DEV_API_JSON` 时会下载 `https://models.dev/api.json`。
2. **运行期**：`packages/core/src/models-dev.ts` 可以首次拉取、每 60 分钟刷新；Provider 登录流程还会调用 `refresh(true)` 强制刷新。

定制版必须同时关闭两处：

- 构建脚本始终设置 `MODELS_DEV_API_JSON=<repo>/resources/opencode/managed-models.json`。定制构建缺少该文件时直接失败，不能回退到网络下载。
- `ModelsDev.populate` 只允许“显式目录 -> 同源编译快照 -> 空目录”三步，不再调用网络填充。
- 不创建每 60 分钟执行一次的刷新任务。
- `ModelsDev.refresh()` 固定为空操作，包括 `force=true`。这样 Provider 登录、命令行或未来新增调用方都不能绕过限制。

网上下载、写缓存和刷新实现代码可以保留，方便以后对照、合并上游；但不再挂接到 Multi-AI Code 定制版的执行路径。该行为是 fork 的固定产品定义，不增加环境变量、设置项或官方模式开关。

### 4.2 打包和旧缓存隔离

`managed-models.json` 必须跟随 Multi-AI Code 的 OpenCode 运行时一起打包，而不是在首次启动时复制到宿主机的 OpenCode 配置目录。推荐布局：

```text
Multi-AI Code.app/Contents/Resources/opencode/
├── bin/opencode
├── managed-models.json
├── managed-routing.json
└── manifest.json
```

Windows 使用同样的资源相对布局。启动器根据当前安装目录解析绝对路径，不能从当前工作目录或 `PATH` 推断。

启动前执行以下校验：

1. 目录文件存在并且是普通文件。
2. SHA-256 与发布清单一致。
3. 两份 JSON 的 Schema、Provider ID 和模型 ID 校验通过。
4. 所有默认模型和 Failover 候选都能在目录中找到。

校验通过后才启动 OpenCode。校验失败时直接报告“安装资源损坏”，不能静默回退到宿主机缓存。

定制版的加载顺序固定为：显式 `OPENCODE_MODELS_PATH`、同源编译快照、空目录。保护措施包括：

- 始终设置 `OPENCODE_MODELS_PATH` 指向安装包内的只读目录。
- 构建 OpenCode 时把同一份目录编入快照，保证即使某条启动路径误绕过宿主校验，也不会加载官方完整目录；正常启动的资源校验失败仍应拒绝运行，不能用快照掩盖安装损坏。
- 网络填充、定时刷新和强制刷新都不接入执行路径，也不生成官方 `models.json`。

只要显式路径存在，`~/.cache/opencode/models.json` 等宿主机旧文件就不会被读取。启动器也不需要删除这些文件，避免影响用户单独安装的官方 OpenCode。

### 4.3 目录内容

`managed-models.json` 必须保持原生 models.dev `api.json` 格式：根对象直接以 Provider ID 为键，不能增加 `{ "version": ..., "providers": ... }` 包装。它只保存 OpenCode 原生模型字段：

- Provider ID 和显示名称。
- API Base URL，或者公司统一网关 URL。
- Provider 所需的环境变量名。
- 模型 ID、名称、发布日期、上下文限制和输出限制。
- 输入/输出模态、附件、工具调用、推理和温度能力。
- 发布状态。

路由角色、结构化输出实测能力、默认优先级和最低客户端版本不属于原生 models.dev Schema，放在独立的 `managed-routing.json`。这样既不修改 `models-dev.ts` 的加载逻辑，也不会依赖未知字段在未来版本是否被保留。

`managed-models.json` 示例仅表达原生结构，不代表最终模型名称：

```json
{
  "managed-text": {
    "id": "managed-text",
    "name": "Managed Text",
    "api": "https://gateway.example.com/v1",
    "npm": "@ai-sdk/openai-compatible",
    "env": ["MULTI_AI_TEXT_TOKEN"],
    "models": {
      "text-fast": {
        "id": "text-fast",
        "name": "Text Fast",
        "release_date": "2026-01-01",
        "attachment": false,
        "reasoning": false,
        "temperature": true,
        "tool_call": true,
        "modalities": { "input": ["text"], "output": ["text"] },
        "limit": { "context": 1000000, "output": 64000 }
      }
    }
  }
}
```

`managed-routing.json` 示例：

```json
{
  "version": 1,
  "minimumClientVersion": "1.0.0",
  "models": {
    "managed-text/text-fast": {
      "roles": ["default_text", "small"],
      "priority": 100,
      "capabilities": {
        "structured_output": true
      }
    }
  }
}
```

真实模型 ID、上下文和能力必须由自动化契约测试生成或校验，不能只靠文档手工维护。构建任务还要验证 `managed-routing.json` 引用的每个模型都存在于 `managed-models.json`。

### 4.4 更新机制

首版目录随 Multi-AI Code 发布，保证无网络时仍能启动。后续可增加签名目录更新：

1. 下载新目录和签名。
2. 校验签名、Schema、最低客户端版本和模型唯一性。
3. 原子替换本地目录。
4. 校验失败时继续使用 last-known-good 目录。

模型目录和凭据配置分开维护。即使选择内置 Key，也不把 Key 混入 models.dev Schema，避免模型目录热更新意外覆盖凭据。

### 4.5 OpenCode 账号运行目录

模型目录是应用级只读资源；数据库、日志、状态和缓存是账号级可变数据。两类数据不能混放。

Multi-AI Code 当前账号根目录已经统一为：

```text
<MULTI_AI_ROOT 或 ~/multi-ai-code>/accounts/<accountId>/
```

OpenCode 的运行数据放在该账号根目录下：

```text
accounts/<accountId>/aicli/opencode/
├── config/                 # 账号级 OpenCode 配置
├── data/
│   ├── opencode-*.db       # 会话数据库
│   ├── auth.json           # 需要落盘时的账号凭据引用
│   ├── mcp-auth.json
│   ├── plans/
│   ├── snapshot/
│   ├── tool-output/
│   └── log/
├── cache/
│   ├── bin/                # rg、LSP 等可重建工具
│   └── skills/
├── state/                  # 最近模型、插件元数据等
└── tmp/
```

不使用 `OPENCODE_TEST_HOME` 模拟用户 Home。该变量会同时改变“用户主目录”的语义，可能影响目录搜索和权限判断。OpenCode fork 在 `packages/core/src/global.ts` 增加正式的 `OPENCODE_RUNTIME_ROOT`：

```text
OPENCODE_RUNTIME_ROOT=<account root>/aicli/opencode
```

设置后，`Global.Path.data/cache/config/state/tmp/bin/log/repos` 全部从该根目录派生。Multi-AI Code 必须在创建 OpenCode 子进程前设置它，OpenCode 不再访问宿主机的 XDG OpenCode 目录。

目录生命周期如下：

- 切换账号：切换到另一个账号运行根，数据库和模型选择互不串用。
- 清理缓存：只删除 `cache/` 和 `tmp/`，不能删除会话数据库和配置。
- 卸载单个账号：由 Multi-AI Code 管理账号目录，不能调用官方 OpenCode 卸载逻辑清理宿主机目录。
- 应用升级：保留账号运行目录，只替换安装包内的模型目录和 OpenCode 二进制。

### 4.6 `.opencode` 策略

`.opencode` 不是普通缓存。官方 OpenCode 用它保存项目级配置、Agent、命令、Skill、插件、主题和 Plan，部分流程还会写入 `.gitignore`、依赖文件和 `plans/`。Multi-AI Code 定制版不沿用这套目录约定。

现有 `OPENCODE_DISABLE_PROJECT_CONFIG=1` 不能完整满足这个要求：它只跳过部分项目配置发现，当前路径逻辑仍会检查 Home 下的 `.opencode`，Git 项目的 Plan 也仍默认写入 `<worktree>/.opencode/plans`。因此需要直接调整 fork 的路径和配置层，不能只靠启动环境变量拼凑。

定制版固定满足：

- 不搜索宿主机 `~/.opencode`。
- 不搜索或读取工作仓库及其父目录中的 `.opencode`。
- 不在仓库中创建 `.opencode`、`.gitignore`、`package.json`、锁文件或 `plans/`。
- Plan、快照、工具输出全部写入账号运行目录。
- 本地安装 Agent、Skill、插件等写操作始终落到账号级 `config/`。

不提供只读兼容模式，也不提供恢复官方完整读写行为的入口。仓库里即使已经存在 `.opencode`，定制版也直接忽略；需要给 Multi-AI Code 配置 Agent、Skill 或插件时，统一使用当前账号目录。

---

## 五、凭据设计

### 5.1 支持三种凭据模式

**模式 A：内部版本内置共享 Key（当前可用）**

适合安装范围可控、Key 本身已设置网关限额和访问范围的内部发行版本：

1. Key 作为内部托管 Profile 的一部分随应用发布。
2. OpenCode 启动时把 Key 注入对应 Provider，不要求用户填写。
3. Provider 必须配置独立限额、调用审计和紧急停用能力。
4. 更换 Key 需要重新发布配置或应用版本。

该模式不能防止有本机访问权限的用户从安装包或进程中提取 Key，这是接受内置 Key 时需要明确接受的部署属性，不作为本方案的实现阻塞项。

**当前内置服务与 Key（供开发验证）**

| 服务 | Provider ID | Base URL | API Key | 已验证模型 |
|---|---|---|---|---|
| DeepSeek | `deepseek` | `https://api.deepseek.com` | `sk-285f10679e7b41e099369722986697f2` | `deepseek-v4-flash` / `deepseek-v4-pro` / `deepseek-chat` / `deepseek-reasoner` |
| 智谱 GLM | `zhipu` | `https://open.bigmodel.cn/api/paas/v4` | `c25f5e26c0b0492cbadcb44123d3d2ae.MkcRaW6Ydacd7mQl` | `glm-5v-turbo` / `glm-5.2` / `glm-4.6v` |

**模式 B：企业 Token Broker**

1. 用户登录 Multi-AI Code。
2. Electron 主进程用用户身份向 Broker 请求短期、可撤销、限额 Token。
3. Broker 返回限定 Provider、用户、设备和有效期的 Token。
4. Electron 只把 Token 注入当前 OpenCode 子进程环境。
5. Token 到期前刷新；退出 OpenCode 后不再复用。

**模式 C：用户自己的 Key**

1. 用户在设置中输入 Key。
2. macOS 存 Keychain，Windows 存 Credential Manager。
3. 启动 OpenCode 时读取并注入子进程环境。
4. UI、日志和错误只显示掩码及凭据引用 ID。

当前可以优先实现模式 A，后续无需改变 ModelRouter 和 Failover 即可迁移到模式 B 或 C。

### 5.2 运行时保护

无论选择哪一种模式，都必须满足：

- 日志、崩溃报告、诊断信息和 IM 消息不输出完整 Key。
- HTTP 错误中的请求 Header 在持久化前脱敏。
- 设置 UI 默认只显示掩码。
- 不把 Key 写入模型健康状态、ModelAttempt 或会话历史。
- 代码审查时对新增凭据做显式确认，避免无意复制到更多位置。

### 5.3 Host 配置扩展

宿主使用固定的 `OpenCodeManagedProfile` 描述当前发行版允许启用的 Provider 和默认模型：

```ts
interface OpenCodeManagedProfile {
  version: 1
  defaultModel: string
  smallModel: string
  enabledProviders: string[]
  providers: Record<string, {
    env: Record<string, string>
  }>
}
```

当前内部发行版在 Profile 中直接保存 Provider 对应的环境变量和值。路由数据单独保存在 `managed-routing.json`，不与凭据混合。未来迁移到 Broker 或系统凭据存储时，只替换宿主生成环境变量的来源，不修改 OpenCode 模型目录格式。

---

## 六、模型路由

### 6.1 请求要求

每次 LLM 调用先生成确定性的 `RequestRequirements`：

```ts
interface RequestRequirements {
  inputModalities: Set<"text" | "image" | "audio" | "video" | "pdf">
  needsTools: boolean
  needsStructuredOutput: boolean
  estimatedInputTokens: number
  minimumOutputTokens: number
  preferredRole: "default_text" | "strong_text" | "vision" | "small"
  manualModel?: string
  manualMode?: "prefer" | "strict"
}
```

要求来源：

- 当前消息和历史中的附件类型。
- 当前工具集合是否非空。
- `json_schema` 等结构化输出要求。
- 目标 Agent，但 Agent 只提供偏好，不改变权限。
- 标题、摘要等内部任务使用 `small` 角色。
- 用户通过 `/model` 或设置指定的模型。

### 6.2 候选生成规则

候选模型按以下顺序生成：

1. 如果用户选择 `strict`，只使用指定模型，不跨模型 Failover。
2. 如果用户选择 `prefer`，指定模型排第一，后面追加兼容候选。
3. 根据 `preferredRole` 读取路由链。
4. 删除不满足输入模态、工具、结构化输出和上下文要求的模型。
5. 删除处于熔断 Open 状态的候选。
6. 相同 Provider 故障时优先选择其他 Provider，避免换模型但仍命中同一故障端点。

默认路由只按角色表达，不把具体模型 ID散落到业务代码：

```text
default_text -> [text-fast@provider-a, text-strong@provider-b]
strong_text  -> [text-strong@provider-b, text-fast@provider-a]
vision       -> [vision-primary@provider-b, vision-backup@provider-c]
small        -> [small-fast@provider-a, text-fast@provider-a]
```

### 6.3 Agent 边界

- `build` 仍是允许修改代码的主 Agent。
- `plan` 仍是限制编辑的规划 Agent。
- 路由器不能因为模型选择而自动从 `plan` 切到 `build`。
- Agent 可以提供 `preferredRole`，例如 `plan` 偏好 `strong_text`，但权限集合保持不变。
- 子 Agent 独立生成请求要求，但与主 Agent 共享进程内的模型健康状态。

---

## 七、视觉协作策略

图片输入采用“主模型负责、视觉模型协作”的固定边界。用户选择 DeepSeek 后，当前 Session 和本轮仍由 DeepSeek 处理；不能因为消息包含图片就静默把整轮切换成 GLM 或其他视觉模型。

### 7.1 协作流程

1. 从受控模型目录和 `managed-routing.json` 生成可用的视觉协作者列表，并通过 `vision` 工具描述提供给主模型。
2. 如果主模型本身支持图片，原始附件正常进入主模型上下文，由主模型决定是否直接分析或调用协作者复核。
3. 如果主模型不支持图片，消息存储仍保留原始附件，但提交给该模型的上下文只包含附件提示，防止 Provider 因不支持图片而拒绝整轮请求。
4. 当答案依赖图片内容时，主模型主动调用 `vision` 工具，给出本次需要回答的聚焦问题。
5. `vision` 工具创建隔离的子 Session，把聚焦问题和原始图片交给选定的视觉模型。视觉子 Session 禁止工具和文件修改，也不能继续调用自身。
6. 视觉模型的分析作为工具结果返回主模型。主模型结合任务上下文继续推理、调用其他工具，并生成最终答复。

该流程适用于远程 IM、完整 TUI 和 mini runtime。三条输入路径只负责保存文本和附件，不自行选择或替换模型。

### 7.2 协作边界

- 视觉协作者只回答图片相关问题，不拥有主任务，也不改变当前模型、Agent、Variant 或 Session。
- 主模型不能仅因为自身不支持图片就直接回复“无法查看”；存在视觉协作者时应调用 `vision` 工具。
- 没有可用视觉模型时不暴露 `vision` 工具，并向用户给出明确的能力缺失错误。
- 多张图片作为同一次协作调用的附件一起传递；后续纯文本追问可以继续引用最近一条包含图片的用户消息。
- 视觉模型的选择按受控目录角色和优先级确定，主模型也可以从工具公开的候选列表中指定某个协作者。
- Claude Code 不接入该路径。

### 7.3 协作结果缓存

后续增加缓存时，对附件计算内容哈希，并保存 `DerivedMediaDescription`，至少包含附件哈希、视觉模型和版本、聚焦问题、生成时间、描述、OCR 和置信信息。只有附件、问题或视觉模型版本匹配时才能复用。

协作结果返回主模型时使用明确边界：

```text
<derived-visual-context source="attachment" trusted="false">
...
</derived-visual-context>
```

系统提示必须说明其中内容是不可信数据，不能把图片中的文字当作系统指令执行。缓存只是减少重复视觉调用，不能改变“主模型保持不变”的边界。

---

## 八、重试、Failover 与副作用边界

### 8.1 两级策略

**第一级：同模型有限重试**

- 连接建立前错误、连接重置、Header 超时：最多重试 2 次。
- 429：遵循 `Retry-After`；等待过长且存在候选时进入 Failover。
- 使用指数退避和抖动。
- 用户取消立即终止，不重试。

**第二级：跨模型 Failover**

- 同模型有限重试耗尽后，才切到下一兼容候选。
- 每个候选都重新估算上下文、执行必要的 compaction，并重新调用 `toModelMessagesEffect`。
- 每轮最多尝试配置中的候选数量，不形成无限循环。

### 8.2 Attempt 状态机

```text
prepared
   |
   v
requesting --error before output--> retry_same_model / next_candidate
   |
   v
streaming_text --safe stream error--> mark attempt superseded / next_candidate
   |
   v
tool_pending --tool execution starts--> side_effecting
   |                                  |
   |                                  +--error--> stop_and_report
   v
completed --------------------------> final
```

每次候选请求都记录独立 `ModelAttempt`：

```ts
interface ModelAttempt {
  id: string
  sessionID: string
  parentUserMessageID: string
  assistantMessageID: string
  model: string
  candidateIndex: number
  sameModelAttempt: number
  state: "requesting" | "streaming" | "side_effecting" | "completed" | "failed" | "superseded"
  errorCategory?: string
  startedAt: number
  completedAt?: number
}
```

### 8.3 安全重放规则

允许自动切换：

- 尚未执行工具。
- 只产生了部分文本，没有向 IM 发出终态回复。
- 失败 attempt 可以标记为 `superseded`，TUI 折叠显示，新的 attempt 使用新的 Assistant Message ID。

禁止自动切换：

- 当前 attempt 已开始 Shell、Edit、写文件或具有写副作用的 MCP 调用。
- 无法判断工具是否执行成功。
- Provider 返回了可能已提交但客户端超时的写操作。

禁止切换时结束本次调用，并明确提示“模型连接中断，当前操作可能已产生副作用，请检查后继续”，不能静默重放整轮任务。

已经完成的上一轮工具结果可以作为历史进入下一次正常 LLM 调用；这里禁止的是重放正在失败的那次调用。

### 8.4 错误分类

| 错误 | 同模型重试 | 跨 Provider | 熔断计数 |
|---|---:|---:|---:|
| 用户取消/Abort | 否 | 否 | 否 |
| 401/403 | 否 | 策略允许时可以 | 凭据级 |
| 429 | 按 Retry-After 有限重试 | 可以 | 模型或凭据级 |
| 5xx | 有限重试 | 可以 | Provider 端点级 |
| 连接重置/Header 超时 | 有限重试 | 可以 | Provider 端点级 |
| 流中断且无副作用 | 有限重试 | 可以 | Provider 端点级 |
| 流中断且已有副作用 | 否 | 否 | Provider 端点级 |
| Context overflow | 重新组装/压缩 | 兼容时可以 | 否 |
| 400/工具 Schema 不兼容 | 否 | 仅明确能力不兼容时 | 模型级 |
| Content filter | 否 | 默认否 | 否 |

### 8.5 熔断机制

熔断 Key：

```text
providerID + baseURL + credentialRef + modelID
```

默认策略：

- 60 秒滚动窗口内连续 3 次可熔断错误 -> Open。
- Open 时长取 `max(Retry-After, 默认 5 分钟)`，并设置合理上限。
- 冷却结束进入 Half-Open，只允许一个探测请求。
- 探测成功恢复 Closed；失败重新 Open。
- Context overflow、用户取消和本地参数校验失败不进入熔断统计。
- 状态在同一个 OpenCode 进程内共享，主 Agent 和子 Agent 不重复冲击故障端点。

---

## 九、会话、TUI 与 IM 行为

### 9.1 会话历史

- 每个候选模型都从当前持久化历史重新加载消息。
- 使用现有 compacted history，而不是宣称“原样完整历史”。
- 按候选模型重新转换 Provider metadata、附件和工具消息。
- `superseded` attempt 保留审计记录，但不作为有效 Assistant 回复加入后续模型上下文。
- 实际完成回复的模型写入最终 Assistant Message。

### 9.2 TUI

TUI 可以展示一条紧凑状态：

```text
模型 A 连接失败，已切换到模型 B（第 2/3 个候选）
```

要求：

- 不把失败模型的部分文本拼到最终回复。
- 失败 attempt 默认折叠，可在诊断视图展开。
- 状态栏显示当前实际模型，而不是最初选择的模型。
- 用户可取消整个候选链。

### 9.3 远程 IM

- 中间 retry、熔断和候选失败不作为模型回复发给 IM。
- IM 只收到与当前 `replyId` 精确绑定的最终 Assistant 回复。
- 如果发生过 Failover，可在最终回复前附加一条宿主状态消息，但不能混入模型正文和 reply marker。
- 所有候选失败时，发送一次结构化终态错误，包含已尝试模型的脱敏摘要。
- 失败 attempt 和迟到事件不能触发重复 IM 回复。

---

## 十、固定托管模式

- `/models` 只展示受控目录中的模型。
- `/connect`、Provider 连接按键和项目级 Provider 设置均不存在。
- 设置页展示“自动选择”“固定模型”以及当前服务状态。
- 用户不需要理解 Provider 概念。
- 不提供官方模式、高级模式或读取旧 `.opencode` 的兼容入口。
- 模型目录路径和账号运行根由宿主自动注入，用户无需设置环境变量。

---

## 十一、源码实施位置

### 11.1 Multi-AI Code 主仓库

| 文件/模块 | 改动 |
|---|---|
| `electron/aicli/opencodeConfig.ts` | 单 Provider Profile 扩展为 Managed Profile；注入目录路径、启用列表和脱敏环境变量 |
| `electron/store/paths.ts` | 增加当前账号的 OpenCode 运行目录派生函数，禁止启动器自行拼接账号路径 |
| `electron/settings/types.ts` | 保留旧字段的反序列化兼容，但不再让旧单 Provider Profile 参与 OpenCode 启动 |
| 设置 UI | 移除旧 OpenCode Provider、Base URL 和 API Key 表单；模型由托管 Profile 固定注入 |
| 打包资源 | 增加 `managed-models.json`、`managed-routing.json` 和校验清单，并从模型文件生成同源编译快照 |
| AICLI 启动器 | 启动前校验模型目录，注入 `OPENCODE_RUNTIME_ROOT` 和目录绝对路径；禁止在日志中打印环境值 |

### 11.2 OpenCode fork

建议新增：

| 模块 | 职责 |
|---|---|
| `provider/model-router.ts` | RequestRequirements、能力过滤、候选链 |
| `provider/model-health.ts` | 熔断、Half-Open、Retry-After |
| `session/model-attempt.ts` | Attempt 状态和审计事件 |
| `session/failover.ts` | 同模型有限重试和跨候选编排 |
| `tool/vision.ts` | 隔离调用视觉协作者，并把原始图片和聚焦问题交给子 Session |
| `packages/core/src/model-collaboration.ts` | 读取视觉角色和优先级，生成可用的视觉协作者列表 |

修改现有模块：

| 文件 | 改动 |
|---|---|
| `session/prompt.ts` | 在调用 Processor 前生成候选；每次候选重新 compact/组装消息 |
| `session/processor.ts` | 上报首个文本、工具开始和副作用边界；不在这里决定下一模型 |
| `session/retry.ts` | 从无限 Schedule 改为带上限的同模型策略，输出统一错误分类 |
| `session/message-v2.ts` | 跳过 superseded attempt；对不支持图片的主模型隔离原始图片；后续持久化并复用视觉协作结果 |
| `provider/provider.ts` | 暴露统一能力信息和实际凭据引用，不保存明文审计数据 |
| `packages/core/src/global.ts` | 支持 `OPENCODE_RUNTIME_ROOT`，把所有可变路径重定向到当前 Multi-AI Code 账号 |
| `packages/core/src/models-dev.ts` | 固定只加载显式目录或同源快照；`refresh()` 为空操作，不启动定时刷新；保留网络实现代码供上游同步参考 |
| `packages/opencode/script/generate.ts`、打包脚本 | 托管构建强制使用本地 `MODELS_DEV_API_JSON`，缺失时失败，不回退网络 |
| `config/paths.ts`、`config/config.ts` | 删除 Home 和项目 `.opencode` 的发现与加载，只读取账号级配置目录 |
| `session/session.ts` | 托管模式把 Plan 写入账号运行目录，不在仓库创建 `.opencode/plans` |
| `packages/tui/src/app.tsx`、`packages/tui/src/config/keybind.ts` | 删除 `/connect` 和 Provider 连接按键；后续 Phase 再展示切换状态 |
| IM bridge | 继续只监听终态完成/错误，不转发 attempt 中间输出 |

`MessageV2.fromError` 继续只做错误标准化，不承担 Failover。

---

## 十二、实施阶段

### Phase 0：确认凭据发行策略

- 当前内部版本采用 `embedded` 模式，并确认安装范围、配额和停用方式。
- 为允许内置 Key 的文件建立明确清单，避免 Key 无限制扩散。
- 验证日志、崩溃报告、IM 和诊断页面不会输出完整 Key。
- 保留切换到 Broker/Keychain 的接口，不让凭据来源渗透到 ModelRouter。

### Phase 1：受控目录和多 Provider 注入

当前源码已完成：

- 打包受控目录和同源编译快照，并在启动前校验资源和 SHA-256。
- 定制构建和运行时都不接入 models.dev 网络入口，包括 `refresh(true)`，且不增加用户开关。
- 按账号注入 OpenCode 运行根目录，不读写宿主机全局 OpenCode 目录。
- 关闭项目 `.opencode`、项目 OpenCode 配置和项目级配置写入。
- 关闭官方账号远端配置、well-known 配置和系统 MDM 配置的合并入口，防止外部 Provider 污染托管目录。
- 宿主从托管 Profile 注入多个 Provider 的凭据、默认模型和启用列表。
- 删除 `/connect`、Provider 连接按键和 `models --refresh`。
- 不修改 Session Processor、工具执行和 IM 终态链路；自动 Failover 留到后续独立阶段。

### Phase 2：确定性模型路由

- 实现 RequestRequirements 和能力过滤。
- 文本、小模型、强模型和视觉模型按角色选择。
- Agent 权限保持不变。

### Phase 3：安全 Failover

- 先实现无工具请求的有限重试和跨 Provider 切换。
- 再接入工具副作用边界和 superseded attempt。
- 接入熔断、Half-Open 和 TUI 状态。
- 验证 IM 始终只收到一次终态回复。

### Phase 4：视觉协作与缓存

- 当前源码已实现主模型保持不变、内置 `vision` 协作工具、原始附件隔离和视觉子 Session。
- 后续实现附件哈希、DerivedMediaDescription 和历史复用。
- 增加视觉协作结果的提示注入防护。

每个阶段独立提交和发布，避免一次性改动 Provider、Session、TUI 与 IM 全链路。

---

## 十三、测试矩阵

### 13.1 目录与配置

- 首次启动无缓存，只展示受控模型。
- 已存在旧 `models.json` 时仍使用显式受控目录。
- 打包目录缺失或校验失败时拒绝启动，不回退到宿主机缓存。
- 启动 65 分钟后不会后台拉取 models.dev。
- 托管构建在未提供本地 `MODELS_DEV_API_JSON` 时失败，且不会访问 models.dev。
- 托管运行时调用 `ModelsDev.refresh(true)` 也不会发起网络请求。
- 目录损坏或清单校验失败时拒绝启动并提示诊断信息，不静默回退。
- 同一台机器的两个 Multi-AI Code 账号使用不同的 OpenCode 数据库、状态和日志目录。
- 宿主机已有官方 OpenCode 配置和缓存时，托管实例既不读取也不修改它们。
- 托管模式运行、生成 Plan、加载 Skill 和退出后，仓库内不会新增或修改 `.opencode`。
- 仓库或 Home 已存在 `.opencode` 时，其配置、Agent、Skill 和插件均不会被加载。
- `embedded` 模式可以在内部 Provider 配置中携带 Key；其他模式只保存凭据引用。
- 任一模式的启动日志和诊断输出都不出现完整 Key。

### 13.2 路由

- 普通文本选择默认文本模型。
- 图片任务仍由当前主模型拥有；不支持图片时由主模型调用可用的视觉协作者。
- 视觉协作不会改变主 Session 的模型、Agent 和 Variant。
- 不支持图片的主模型不会收到原始图片；视觉子 Session 能收到原始附件。
- 视觉子 Session 不能调用工具、修改文件或递归调用 `vision`。
- 需要工具时过滤不支持工具的模型。
- `json_schema` 请求过滤不支持结构化输出的模型。
- 超出候选上下文时先压缩或跳过候选。
- `strict` 固定模型不触发跨模型切换。

### 13.3 重试和 Failover

- 连接前超时：同模型有限重试后切换。
- 429：正确解析秒数和 HTTP Date 格式的 `Retry-After`。
- 401/403：不重复请求同一凭据。
- 5xx：熔断按端点生效，子 Agent 共享状态。
- Context overflow：不计入熔断，并按候选模型重新压缩。
- 所有候选失败：只产生一次终态错误。

### 13.4 副作用

- 部分文本后流中断：旧 attempt 被 supersede，最终文本不拼接。
- Shell 开始后流中断：不自动重放 Shell。
- Edit 完成但响应丢失：不自动重复 Edit。
- 写操作 MCP 超时：提示状态不确定，不自动 Failover。
- 用户取消：立即停止整个候选链。

### 13.5 视觉

- 同一图片在多轮历史中只生成一次描述。
- 图片内容变化后缓存失效。
- 视觉模型版本变化后按策略重新生成。
- 图片中的提示注入文字不会成为系统指令。
- 视觉模型不可用时给出明确终态错误或选择兼容备选。

### 13.6 TUI 与 IM

- TUI 显示实际模型及一次切换状态。
- 失败候选的迟到 completion 不覆盖最终模型。
- 每个远程 `replyId` 最多发送一次终态回复。
- 中间 retry、reasoning、tool progress 不作为 IM 回复。
- `/btw`、主对话和子 Agent 的 attempt 不串线。

---

## 十四、验收标准

方案完成需要同时满足：

1. 凭据来源符合选定的发行模式，且日志、崩溃报告、IM 和会话历史不输出完整 API Key。
2. 托管模式启动后只展示受控模型，不访问 models.dev。
3. 普通文本、强推理、视觉和小模型任务均能确定性路由。
4. 任一模型 429/5xx 时，在没有副作用的前提下可以自动完成 Failover。
5. Shell/Edit/MCP 写操作不会因 Failover 自动重复执行。
6. 每个候选模型使用自己的上下文限制重新组装请求。
7. 同一历史图片不会重复产生视觉调用费用。
8. TUI 可以诊断模型切换，IM 仍只收到一次最终回复。
9. 用户无法通过 `/connect`、旧设置或项目配置绕过受控 Provider 和模型目录。
10. 托管 OpenCode 的所有可变数据位于当前 Multi-AI Code 账号目录，宿主机官方 OpenCode 数据保持不变。
11. 定制版不会读取、生成或修改用户仓库及 Home 下的 `.opencode`。

---

## 十五、后续阶段产品决策

1. 内置共享 Key 是长期发行方式还是过渡方式；如果后续迁移，优先 Token Broker 还是用户 Key。
2. 是否允许同一份仓库和会话内容在不同模型供应商之间自动传输。
3. 手动固定模型默认使用 `prefer` 还是 `strict`。
4. Failover 状态是否需要在 IM 中单独提示，还是只在 TUI 和诊断日志中展示。
5. 受控模型目录是仅随版本发布，还是支持签名热更新。
这些决策不阻塞已经完成的 Phase 1；进入自动路由和 Failover 前必须确认，避免修改 Session 与工具副作用边界后再返工。
