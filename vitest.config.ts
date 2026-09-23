import { configDefaults, defineConfig } from 'vitest/config'

export default defineConfig({
  test: {
    // Provide minimal browser-API polyfills (localStorage, document) for
    // src/utils/theme.test.ts which runs in the default node environment.
    // Electron tests don't use these globals, so the polyfills are harmless.
    setupFiles: ['src/utils/theme.setup.ts'],
    restoreMocks: true,
    // 只跑本仓库自己的测试：排除 vendored 的 AICLI 子模块、MaiAgent / Qt 桌面端
    //（C++，各自走 CMake/CTest）与构建产物。
    exclude: [
      ...configDefaults.exclude,
      'third_party/**',
      'MaiAgent/**',
      'desktop/**',
      'MaiChat/**',
      'release/**',
      '.worktrees/**',
      'out/**'
    ],
  },
})
