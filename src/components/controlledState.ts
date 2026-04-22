import type { Dispatch, SetStateAction } from 'react'

export function applyControlledStateUpdate<T>(
  setter: Dispatch<SetStateAction<T>>,
  update: SetStateAction<T>
): void {
  if (typeof update === 'function') {
    setter((prev) => (update as (prev: T) => T)(prev))
    return
  }
  setter(update)
}
