# ADR 0027 — Main window: ribbon, command palette, standards in a side panel

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre (from the design canvas "Bimble UI options": main window A, Revit-like, with
  option C's command palette; standards option B, in a panel beside the drawing); details by Claude
- Reversal cost: medium (the frame of the window; tools and panels underneath are unchanged)

## Context / Problem
The top bar had grown into one row of toggles and buttons that ran off a 1280 px screen, and the
graphic style, plan export, placing and history each opened a floating window. The office works
in Revit: tools on tabs, a project browser, properties, one tab per open view.

## Decision
- **Title row**: the file (• when unsaved), Open, Save, Undo, Redo, and the palette button.
- **Ribbon**: tabs Model (place column, beam, wall, slab; measure), Annotate (sheet tools and
  their text and dimension types; what plans show: section labels, openings, section marks,
  levels), View (edges, hatching, orthographic, section box, fit, top), Sheets (new, close,
  on paper), Links (model, reference plan), Manage (graphic standards, history). Buttons in named
  groups, pressed when on, greyed when they can't run.
- **One `Action` per command**, run by one function: the ribbon, the palette and the keys can't
  disagree.
- **Command palette** (Ctrl+K or the title-row button): finds every action by typing (every word
  starts a word of its name, or the text is part of it), shows its tab and shortcut; arrows,
  Enter, Esc or a click outside.
- **Tabs of open views and sheets** over the main view; the active one is light like the canvas.
- **Standards** open in a panel on the right, beside the drawing, so changes are seen as they are
  made: a search over section titles and type names (matching sections open), hatch types as
  swatches drawn from their definition with the materials that use them on hover, then the tables.
- **Status bar** says what the pointer will do (placing, measuring, sheet tool), then the counts.
- **Theme** in `ui_theme.rs`: the canvas's colours, spacing and type sizes mapped onto egui; a
  test checks every text colour at 4.5:1 or better on every background.

## Alternatives considered
- B, Blender-like (tool shelf, outliner right): dense, but icon-only tools are a harder start for
  Revit users.
- C alone (modes and a palette): the calmest screen, the least familiar; its palette is kept.
- Standards A (one dialog with a page per kind and a preview): closer to Revit, but a modal
  dialog hides the drawing the changes are for.

## Evidence
Screenshots on the virtual display (1440 × 900) against the canvas: ribbon groups, Model → Column
pressed with the placing hint and the snap marker, view tabs, Standards panel with 18 hatch
swatches wrapping to its width, palette opened by Ctrl+K and its button, closed by Esc and a click
outside. Tests: palette matching, actions listed once, theme contrast.

## Revisit when
The office review (#26) finds a missing tool or a wrong place; icons for the ribbon (not yet:
text buttons); the Inter font is bundled (egui's default font for now).
