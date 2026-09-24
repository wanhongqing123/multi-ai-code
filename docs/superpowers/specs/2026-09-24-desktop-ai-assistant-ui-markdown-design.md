# Desktop AI Assistant UI and Markdown Design

## Goal

Improve the MaiChat desktop AI assistant in two independent areas:

- Make the composer, approval menu, and model configuration dialog use MaiChat's own visual language instead of platform-native styling.
- Tell the model to return standards-compliant GitHub Flavored Markdown so structured content, especially tables, reaches the existing renderer in a valid form.

## Root Causes

The composer card uses a `QFrame` type selector. Because `QLabel` inherits from `QFrame`, the selector also draws a rounded border around the working-directory label. When the session directory is a root and `QDir::dirName()` is empty, the result is the narrow empty rectangle shown in the report.

The approval policy uses an unstyled `QMenu`, and the model settings use a stock `QDialog`, `QFormLayout`, `QLineEdit`, and `QDialogButtonBox`. Their native platform rendering does not match the rest of MaiChat.

MaiAgent currently creates `MaiContextBuilder` without a system prompt. The model therefore receives no output-format contract. The malformed table in the report is not valid GFM, so md4c correctly parses it as a paragraph.

## Design

### Markdown contract

Following Codex's separation between `base_instructions` and conversation `input`, add configurable `baseInstructions` to `MaiAgent::Options` and carry it separately on `MaiModelRequest`. The Chat Completions wire adapter serializes it as the first `role=system` message; a future Responses adapter can map it directly to the top-level `instructions` field. The desktop `AgentController` will supply an English prompt that requires user-facing text to use valid GFM where structure helps, with these concrete constraints:

- Keep normal prose as Markdown paragraphs.
- Use headings, lists, fenced code blocks, links, and tables only with valid GFM syntax.
- Put every table row on its own line.
- Give every table a delimiter row with one delimiter cell per column.
- Do not imitate tables with pipes in a single paragraph.

The renderer will remain standards-based. It will not rewrite malformed pipe text, because such heuristics can corrupt shell expressions, paths, and prose that legitimately contain `|`.

### Composer and approval menu

Give the outer composer card an object name and scope its QSS to that object. Child labels will no longer inherit the card border. When `QDir::dirName()` is empty, show the native root path so the working-directory boundary remains visible.

Style the model control, approval control, send button, and their hover/disabled states explicitly. Shorten approval menu rows to the three policy names and apply the same rounded white menu treatment used by message context menus. Detailed explanations remain in tooltips, avoiding the wide native one-line menu shown in the report.

### Model configuration dialog

Replace the stock form presentation with a frameless, translucent dialog containing a rounded white panel. Use a clear title and short subtitle, labels above full-width fields, explicit focus borders, a muted cancel button, and a blue primary save button. Keep the existing validation, masked API key, saved-value behavior, object names, and live agent rebuild behavior.

## Testing

- A MaiAgent test captures the first model request and verifies that base instructions remain separate from conversation messages.
- A wire-format test verifies that Chat Completions receives the base instructions as the first system message.
- An AgentController test verifies that its prompt contains the GFM table contract.
- Agent panel UI tests verify that the composer style is object-scoped, a root directory produces visible text, the policy menu has application styling, and the three policies remain selectable.
- Main window UI tests verify the custom model dialog structure, field behavior, disabled/enabled save states, and persistence.
- Existing Markdown document and layout tests continue to verify valid GFM table parsing; no malformed-table normalization is added.

## Non-goals

- Replacing md4c or the shared Markdown renderer.
- Restyling unrelated MaiChat dialogs.
- Modifying stored historical assistant messages.
