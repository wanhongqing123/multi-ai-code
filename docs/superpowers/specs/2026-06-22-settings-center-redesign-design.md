# Settings Center Redesign Design

## Goal

Redesign the existing settings dialog into a cleaner settings center without changing persistence behavior, IPC contracts, or field semantics.

## Visual Direction

- Use a wider modal so configuration is not squeezed into a narrow vertical form.
- Add a left sidebar that communicates the major setting groups: global shortcut, main AI, project build, and project runtime.
- Keep all existing settings visible in the main content area, using cards and grids to reduce visual clutter.
- Make the screenshot shortcut section visually prominent because it is the only global setting currently shown.
- Keep a sticky footer with cancel/save actions so users do not need to scroll to save.

## Functional Scope

- Preserve existing save flow in `AiSettingsDialog`.
- Preserve `ScreenshotSettingsSection`, `ProjectBuildSettingsSection`, and `ProjectRuntimeSettingsSection` data behavior.
- Add structural class names for the redesigned shell and cards so layout is testable.
- Update existing tests to assert the redesigned structure rather than fragile text-only expectations.

## Out Of Scope

- No new settings.
- No change to backend storage.
- No navigation/tab filtering in the first pass; the left sidebar is structural and visual.
- No commit unless explicitly requested.
