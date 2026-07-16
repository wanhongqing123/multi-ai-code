export type RemoteImControlCommandName =
  | 'status'
  | 'plan'
  | 'build'
  | 'models'
  | 'model'
  | 'goal'
  | 'btw'
  | 'diff'
  | 'interrupt'
  | 'compact'
  | 'clear'
  | 'help'

export interface RemoteImControlCommandDefinition {
  name: RemoteImControlCommandName
  usage: string
  description: string
}

export type RemoteImControlCommandParseResult =
  | {
      type: 'text'
    }
  | {
      type: 'command'
      command: RemoteImControlCommandName
      raw: string
      args: string
    }
  | {
      type: 'unknown-command'
      commandText: string
    }

export const REMOTE_IM_CONTROL_COMMANDS: RemoteImControlCommandDefinition[] = [
  {
    name: 'status',
    usage: '/status',
    description: '查看当前 AICLI 运行状态'
  },
  {
    name: 'plan',
    usage: '/plan',
    description: '切换到计划模式'
  },
  {
    name: 'build',
    usage: '/build',
    description: '切换到执行模式'
  },
  {
    name: 'models',
    usage: '/models',
    description: '查看可用模型列表'
  },
  {
    name: 'model',
    usage: '/model <序号|模型ID|reasoning 档位>',
    description: '切换到指定模型或 Codex 推理档位'
  },
  {
    name: 'goal',
    usage: '/goal [目标|clear|pause|resume]',
    description: '查看、设置或管理当前 AICLI Goal'
  },
  {
    name: 'btw',
    usage: '/btw <任务>',
    description: '启动子 Agent 处理任务，完成后通过 IM 回传'
  },
  {
    name: 'diff',
    usage: '/diff [--stat] [文件或目录]',
    description: '查看当前仓库未提交改动并发送 Diff'
  },
  {
    name: 'interrupt',
    usage: '/interrupt',
    description: '中断当前正在执行的 AICLI 任务'
  },
  {
    name: 'compact',
    usage: '/compact',
    description: '压缩当前 AICLI 上下文'
  },
  {
    name: 'clear',
    usage: '/clear',
    description: '清空当前 AICLI 上下文并开启新会话'
  },
  {
    name: 'help',
    usage: '/help',
    description: '查看可用 IM 控制命令'
  }
]

const CONTROL_COMMAND_BY_NAME = new Map(
  REMOTE_IM_CONTROL_COMMANDS.map((command) => [command.name, command] as const)
)

export function parseRemoteImControlCommand(
  text: string
): RemoteImControlCommandParseResult {
  const trimmed = text.trim()
  if (!trimmed.startsWith('/')) {
    return { type: 'text' }
  }

  const [rawName] = trimmed.slice(1).split(/\s+/, 1)
  const normalizedName = rawName?.toLowerCase() ?? ''
  if (CONTROL_COMMAND_BY_NAME.has(normalizedName as RemoteImControlCommandName)) {
    const args = trimmed.slice((rawName ?? '').length + 1).trim()
    return {
      type: 'command',
      command: normalizedName as RemoteImControlCommandName,
      raw: trimmed,
      args
    }
  }

  // 只有“/ + 纯字母单词”才按敲错的控制命令拒收提示；首词含路径分隔符、
  // 点号、数字等形态（如 /etc/hosts、/tmp/a.log、/v2 接口）是日常开发消息，
  // 一律当普通文本放行给 AICLI，避免误杀。
  if (/^[a-zA-Z]+$/.test(normalizedName)) {
    return {
      type: 'unknown-command',
      commandText: trimmed
    }
  }

  return { type: 'text' }
}

export function formatRemoteImControlCommandHelp(): string {
  return [
    '可用 IM 控制命令：',
    ...REMOTE_IM_CONTROL_COMMANDS.map(
      (command) => `${command.usage} - ${command.description}`
    )
  ].join('\n')
}
