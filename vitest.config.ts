import { configDefaults, defineConfig } from 'vitest/config'

export default defineConfig({
  test: {
    // Provide minimal browser-API polyfills (localStorage, document) for
    // src/utils/theme.test.ts which runs in the default node environment.
    // Electron tests don't use these globals, so the polyfills are harmless.
    setupFiles: ['src/utils/theme.setup.ts'],
    restoreMocks: true,
    // 只跑本仓库自己的测试：排除 vendored 的 AICLI 子模块（有各自的测试框架/依赖，
    // 在这里会大量采集失败）以及 Qt 桌面端（C++ ctest，另一套）与构建产物。
    exclude: [...configDefaults.exclude, 'third_party/**', 'desktop/**', 'out/**'],
  },
})
