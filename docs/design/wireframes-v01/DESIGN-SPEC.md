# Bimble UI — wireframes v01, implementation spec

Reference: `png/fr/*.png` and `png/en/*.png` (1440 × 900, scale 1.0), rendered from `fr/*.html` / `en/*.html`.
The HTML is the source of truth for every size, colour and font; this file summarises it. Where they disagree, the HTML wins.
Design system: "Two discs" (ADR 0049; guilbep/bimble-design-skill, `main@750848d`). Canvas: https://claude.ai/artifact/UH6xEBDjwPfhS87AcsrB3V
Target: the Vue lab `web-ui/` (ADR 0048). The 3D and paper views stay the egui canvas (`#bimble`, `?viewport=1`); everything around it is Vue.
The model drawings inside the viewports of the wireframes are illustrations: the canvas draws the real model.

All sample data (projects, people, files, quantities, CO₂) is synthetic.

## Tokens (exact hex; no other colours)

| Token | Hex | Job |
|---|---|---|
| cream | `#F5F2EB` | window ground, panels, dialogs |
| sheet | `#FFFFFF` | top rows, ribbon, viewport, cards, inputs, status bar |
| sunk | `#EDE7DB` | hover, pressed tool, inner dividers, neutral chips |
| ink | `#141414` | text, icons, focus ring, active tab pill |
| stone | `#5C574D` | quiet text, input and control borders |
| line | `#DDD6C8` | decorative borders only |
| clay `#C06A41` / clay-text `#A35A37` | Do | the one primary button per view; links |
| pink `#D7ACB6` | Choose | selection only (viewport 75 % + 1.6 px ink outline, list rows, table rows) |
| moss `#6A8757` | Done | saved, in sync |
| ochre `#D2B347` | Look | not saved, borrowed, warnings, errors (word "Error"/"Erreur" first) |
| slate `#6E88A3` / slate-text `#5A6F85` | Other | linked models (1.4 px dashed 6 4), "theirs" |

Text on any disc fill is ink, never white. No red, no gradients, no shadows.

## Type

| Use | Font | Size / weight |
|---|---|---|
| Display (Home title) | Sora | 52 / 700, line-height 1.02 |
| Page headings (h2 in pages and dialogs) | Sora | 20–22 / 700 |
| Panel names ("Navigateur de modèle", "Propriétés") | Sora | 14 / 500 |
| UI text, labels, values | Source Sans 3 | 14 (13–13.5 in panels and tables), 400; 600 for buttons and table headers |
| Ribbon tool labels | Source Sans 3 | 12.5, line-height 1.15 |
| Data: numbers with units, IFC classes, GUIDs, keys `{client}`, shortcuts | DM Mono | 12–13 / 400 |
| Eyebrows | DM Mono | 12, uppercase, letter-spacing 0.08 em, stone |

The lab already self-hosts these fonts in `web-ui/src/brand/` (same files as `fonts/` here).

## Window (1440 × 900)

From top to bottom:

1. **Quick-access row**, sheet, min-height 46, padding 6 12, gap 6:
   logo disc 26 (in the lab: the lab mark `bimble-icon-lab.svg`, ADR 0049) → Home icon button → **Fichier ▾** (quiet pill, 600) → 1 × 18 line separator →
   icon buttons Open, Save, Undo, Redo, Print and export, Synchronize (32 × 32 discs, icon 20) →
   centred: file name (14) + schema (DM Mono 12, stone) + state chip →
   right: command field (pill, 32 high, 1 px stone border, search icon 16, placeholder 13.5 stone, `Ctrl K` kbd) →
   user trigram disc 30 (sunk, DM Mono 11) → primary clay button when the view has one.
2. **Tabs row**, sheet, padding 2 12 6, gap 2: pill tabs (padding 4 12, 14, stone; hover sunk; active = ink pill, cream text).
   Order, with 1 × 18 separators between groups:
   Projet · Collaborer · Implantation · Terrain | Béton · Armature · Acier · Bois | Annoter · Vue · Feuilles · Graphisme · ACV · Métré
   (EN: Project · Collaborate · Setting out · Site | Concrete · Rebar · Steel · Timber | Annotate · View · Sheets · Graphics · LCA · Quantities).
3. **Ribbon**, sheet, 1 px sunk top border, 1 px line bottom border, padding 6 6 4, min-height 88.
   Groups: padding 0 8, 1 px sunk right border; group name under the tools (12, stone, centred).
   Tool: column, icon 22 (stroke 1.5, round caps), label 12.5, min-width 58, max-width 92, padding 6 6 5, radius 12;
   hover sunk; pressed = sunk fill + 1 px ink border; unavailable = stone, 55 % opacity.
4. **Work area**: left panel 296 wide (1 px line right border) + main column.
   - Navigator (top, natural height): padding 12 12 8; title Sora 14/500 with a + icon button 26;
     rows 13.5 / line 20, padding 2 6, radius 8, icon 16; section rows 600; counts DM Mono 12 stone at the right;
     selected row pink. 1 px line under it.
   - Properties (rest): padding 12 14, gap 7; rows label stone / value ink (DM Mono for measures);
     inputs 30 high, 1 px stone, radius 8, numbers right-aligned in DM Mono 13 with the unit after them (DM Mono 12 stone);
     selects same with a chevron 14; section eyebrows; status dots (12 disc, 1 px ink outline) pinned to the bottom.
   - Main: view tabs strip (cream, padding 6 10, 1 px line bottom border; pills 13 with × in a 20 disc;
     active = sheet fill + 1 px line border), then the viewport (sheet, square corners) or a page (sheet, padding 24 32).
     Model / On paper switch (segmented pill, `P` kbd) at the right of the strip on views.
   - Optional **Modifier panel** under the viewport when something is selected: cream, 1 px stone top border, padding 8 12.
   - Optional right panel (Standards): 400 wide, cream, 1 px stone left border, padding 14.
5. **Status bar**, sheet, 1 px line top border, min-height 34, padding 4 12, 13, one line (hint ellipsised):
   hint · coordinates (DM Mono 12 stone) · flex space · state chips · counts (DM Mono 12 stone) · FR/EN switch.

## Components

- **Chips**: pill, padding 2 4 2 10, 13, ink text, × in an 18 disc when it can be undone. Neutral modes on sunk; ochre / moss / slate / pink by job.
- **Buttons**: primary clay fill, ink 600 text, pill, min-height 32, padding 5 16. Secondary sheet + 1 px ink, 400. Disabled sunk + stone. One primary per view.
- **Segmented control**: sunk pill track, padding 2; items padding 2 12, 13; active ink pill, cream text.
- **Tables**: header 13/600 stone, 1 px stone bottom border, padding 6 8; rows padding 5 8, 1 px sunk border; group rows cream 600; selected row pink; numbers right-aligned DM Mono.
- **Cards**: sheet, 1 px line border, radius 16, padding 14–18. Selected card: 2 px ink border.
- **Dialogs**: cream, radius 16, padding 24, gap 14, no shadow; backdrop ink at 30 %. Menus: sheet, 1 px stone border, radius 16, padding 6.
- **Banners**: full width, padding 10 16, 14, ochre or moss, first word says what it is.
- **Home**: one decorative disc (clay, 420) bled off the bottom-left corner; nowhere else.

## Screens

| Code | File | Name (FR / EN) | State |
|---|---|---|---|
| A1 | Home | Accueil / Home | exists |
| A2 | FileMenu | Menu Fichier / File menu | exists |
| A3 | ProjectInfo | Informations sur le projet / Project information | exists |
| A4 | TeamMember | Intervenant / Team member | new |
| A5 | Phases | Étapes de projet / Project phases | new |
| A6 | Implantation | Implantation / Setting out | partly (north, LV95 exist) |
| A7 | LevelsGrids | Niveaux et quadrillages / Levels and grids | new |
| A8 | Terrain | Terrain / Site | new |
| B1 | PlaceSlab | Placer une dalle / Place a slab | exists |
| B2 | SelectModify | Modifier / Modify | partly (move, copy, delete, hide, isolate exist) |
| B3 | Rebar | Armature / Rebar | new |
| B4 | Steel | Acier et bois / Steel and timber | new |
| C1 | SharedFolder | Dossier partagé / Shared folder | exists |
| C2 | Borrowed | Élément emprunté / Borrowed element | exists |
| C3 | Conflicts | Synchroniser / Synchronize | new (per-element keep mine/theirs) |
| C4 | Users | Utilisateurs / Users | new |
| C5 | Subprojects | Sous-projets / Sub-projects | new |
| C6 | History | Historique / History | exists |
| D1 | View3D | Vue 3D / 3D view | exists |
| D2 | PlanOnPaper | Sur papier / On paper | exists |
| D3 | SheetLayout | Feuilles / Sheets | exists |
| D4 | Export | Imprimer et exporter / Print and export | new (dialog) |
| D5 | Standards | Standards graphiques / Graphic standards | exists |
| D6 | Quantities | Métré / Quantities | exists |
| D7 | LCA | Bilan carbone / Carbon balance | new |
| D8 | Palette | Palette de commandes / Command palette | exists |

One interface = one name everywhere (ribbon, navigator, view tab, File menu, palette, window title). Use the names above.

## Decisions this design makes (each needs an ADR before its screen is built)

1. Two top rows (quick access, then tabs): 14 tabs don't fit one row at 1 280 px. Differs from ADR 0037 (egui's one row).
2. Trade tabs (Béton, Armature, Acier, Bois), Terrain, ACV and Métré as ribbon tabs, in the order above (ADR 0027 / 0037 describe egui's).
3. Sub-projects group, hide and control loading; borrowing (ADR 0038) still decides who edits.
4. No OK / Annuler: changes apply at once and undo with Ctrl+Z (as project information, ADR 0030).
5. The three phases Existant / Projet / Temporaire are fixed; view filters are free.
