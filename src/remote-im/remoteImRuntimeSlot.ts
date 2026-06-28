export interface DisconnectableRemoteImRuntime {
  disconnect(): Promise<void> | void
}

export function createRemoteImRuntimeSlot<
  Runtime extends DisconnectableRemoteImRuntime = DisconnectableRemoteImRuntime
>() {
  let current: Runtime | null = null
  const waiters = new Set<{
    resolve: (runtime: Runtime) => void
    reject: (error: Error) => void
    timer: ReturnType<typeof setTimeout>
  }>()

  async function disconnect(runtime: Runtime | null): Promise<void> {
    if (!runtime) return
    await runtime.disconnect()
  }

  function resolveWaiters(runtime: Runtime): void {
    for (const waiter of waiters) {
      clearTimeout(waiter.timer)
      waiter.resolve(runtime)
    }
    waiters.clear()
  }

  return {
    getCurrent(): Runtime | null {
      return current
    },

    setCurrent(runtime: Runtime): void {
      current = runtime
      resolveWaiters(runtime)
    },

    waitForCurrent(timeoutMs: number): Promise<Runtime> {
      if (current) return Promise.resolve(current)
      return new Promise<Runtime>((resolve, reject) => {
        const waiter = {
          resolve,
          reject,
          timer: setTimeout(() => {
            waiters.delete(waiter)
            reject(new Error('Tencent IM runtime is not connected'))
          }, timeoutMs)
        }
        waiters.add(waiter)
      })
    },

    async disconnectCurrent(): Promise<void> {
      const runtime = current
      current = null
      await disconnect(runtime)
    },

    async disconnectOwned(runtime: Runtime | null): Promise<void> {
      if (runtime && current === runtime) {
        current = null
      }
      await disconnect(runtime)
    }
  }
}
