export interface MachineActivityClock {
  activityId: string
  sequence: number
  startedAtMs: number
  taskStartedAtMs: number
}

export function beginMachineActivityPhase(
  current: MachineActivityClock | undefined,
  startsNewTurn: boolean,
  now: number,
  createActivityId: () => string
): MachineActivityClock {
  if (startsNewTurn || !current) {
    return {
      activityId: createActivityId(),
      sequence: 0,
      startedAtMs: now,
      taskStartedAtMs: now
    }
  }
  return {
    activityId: current.activityId,
    sequence: current.sequence,
    startedAtMs: now,
    taskStartedAtMs: current.taskStartedAtMs
  }
}
