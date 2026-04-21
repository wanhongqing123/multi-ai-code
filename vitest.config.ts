import { defineConfig } from 'vitest/config'

export default defineConfig({
  test: {
    // Provide minimal browser-API polyfills (localStorage, document) for
    // src/utils/theme.test.ts which runs in the default node environment.
    // Electron tests don't use these globals, so the polyfills are harmless.
    setupFiles: ['src/utils/theme.setup.ts'],
    restoreMocks: true,
  },
})
