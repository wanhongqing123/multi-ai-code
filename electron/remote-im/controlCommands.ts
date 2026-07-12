export type RemoteImControlCommandName = 'status' | 'plan' | 'build' | 'help'

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
    return {
      type: 'command',
      command: normalizedName as RemoteImControlCommandName,
      raw: trimmed
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
