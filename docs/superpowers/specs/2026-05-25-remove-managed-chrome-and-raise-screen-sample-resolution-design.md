# Remove Managed Chrome And Raise Screen Sample Resolution Design

## Goal

Remove the entire managed Chrome feature set from the product and keep habit monitoring focused on app activity plus always-on screen sampling. At the same time, raise screen-sample capture fidelity so saved frames stay close to the real screen size and text is readable by downstream AI.

## Scope

- Remove all managed Chrome user-facing entry points.
- Remove managed Chrome runtime logic, IPC, preload contracts, persistence, and tests.
- Remove managed Chrome-specific habit settings and copy.
- Keep habit monitoring itself, screen sampler controls, and existing app activity capture.
- Change the background screen sampler to capture near the primary display's physical pixel size instead of forcing a 1280x720 thumbnail.

## Non-Goals

- Do not remove the screenshot hotkey / manual screenshot workflow.
- Do not redesign the habit monitor UX beyond removing managed Chrome sections and copy.
- Do not add new settings for capture quality or retention in this change.

## Design

### 1. Managed Chrome removal

- Delete the managed Chrome manager, collector, session persistence helpers, and their tests.
- Remove `habit:chrome:*` IPC handlers and the related `window.api.habit.chrome` preload surface.
- Remove topbar buttons, search actions, and habit monitor panels that mention managed Chrome.
- Simplify habit monitor settings by removing `collectManagedChrome`.
- Narrow habit-event source types to the app-owned source that remains after removal.

### 2. Screen sample fidelity

- Keep the existing `screen-samples/<date>/<timestamp>.jpg` storage layout so downstream consumers do not need migration.
- In the main-process sampler service, request desktop-capture frames at the primary display's physical pixel size using display `size * scaleFactor`.
- Treat the old sampler target size as a lower bound only; on normal displays the capture request should resolve to the full physical screen size.
- Keep JPEG output for now to avoid a format migration, but prioritize source resolution over the old fixed thumbnail size.

### 3. Testing

- Add or update regression tests that prove the renderer no longer exposes managed Chrome actions.
- Add a sampler-service regression test that proves capture size follows physical display dimensions instead of the old 1280x720 limit.
- Re-run habit, build-adjacent, and typecheck suites touched by the refactor.
