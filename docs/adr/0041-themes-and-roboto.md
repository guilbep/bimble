# ADR 0041 — Interface themes, Roboto in the interface and on paper

- Status: Accepted (10 Nov 2026)
- Decided by: issue #53 (an engineer using Bimble): grey interface (RGB 105), Roboto for all
  text, smaller text. Pierre: these become the defaults for everyone; the old dark look stays
  as a preset; more presets with good contrast; light and dark modes; the font applies to the
  interface and to sheets; chosen in the UI or the config. Claude: the palettes, the contrast
  rule, the sheet font living in the graphic style, embedding in PDFs.
- Reversal cost: low (settings and one style field; old values still offered)

## Context / Problem

ADR 0027 fixed one dark look (panels #15–#2a, text #e6, 13 pt, egui's Ubuntu Light). Issue
#53 asked for a lighter grey, Roboto and smaller text. A mid grey is a hard background: on RGB
105 the old text colour reaches 4.4:1, secondary text 2.3:1 and the blue accent 2.0:1, below
WCAG AA (4.5:1), which the theme's test enforces.

Sheets and plans used the PDF's built-in Helvetica (not embedded; widths from its AFM table),
and the system's fonts for SVG and PNG, so a sheet's look depended on the machine.

## Decision

1. **Themes as data** (`ui_theme::THEMES`), picked in Settings → Appearance or as `theme` in
   `settings.json`, applied live: **Grey** (default: ribbon RGB 105, title row and panels in
   darker steps 75–95, white text, near-white secondary text, pale-blue accent, dark-blue
   selection), **Dark** (ADR 0027's), **Light**, **Solarized dark**, **Solarized light**
   (Solarized with its text one step stronger) and **High contrast**. A test checks every text
   colour (text, secondary, accent) on every surface (title, tabs, ribbon, panel, field,
   button, hovered button) and text on selection at ≥ 4.5:1 in every theme. The drawing area
   stays paper-coloured whatever the theme.
2. **Interface font**: Roboto (default) or Ubuntu Light; **text size** 10–16 pt, default 12
   (was 13). Settings `ui_font`, `text_size`.
3. **Roboto shipped**: Roboto 2.138 Regular and Bold (Apache-2.0), from Fedora's signed
   `google-roboto-fonts` package, in `crates/bimble-drawing/fonts/` with its licence.
4. **The sheet font is part of the graphic style** (`paper.font`: `roboto`, default, or
   `helvetica`), editable in the style editor. Not a per-user setting: a project's PDFs must
   look the same whoever prints them. Styles written before it get Roboto.
5. **Roboto on paper**: widths from Roboto's own advances (skrifa, already in the tree via
   resvg and egui); SVG names `Roboto, Helvetica, Arial, sans-serif`; PNG rendering always
   has Roboto loaded; PDFs embed each weight used as a Type 0 / CID TrueType font
   (Identity-H, 2-byte glyph ids, a ToUnicode map so text can be searched and copied), the
   whole file, flate-compressed.

## Alternatives considered

- **Exactly 105 on every surface**: flat, no separation between title, ribbon and panels.
  Shades around it instead.
- **Interface font = sheet font (one setting)**: a user's preference would change the
  project's PDFs. Rejected; the style holds the sheet font.
- **Any system font**: results differ per machine, nothing in the browser. Later, if asked.
- **Font subsetting in PDFs** (`subsetter` crate): smaller PDFs, a new dependency. Not now;
  see Evidence.

## Evidence

- Contrast (WCAG), worst pair per theme after tuning: all ≥ 4.5:1 (test
  `ui_theme::tests::text_contrast_meets_aa`); Grey's white text on 105: 5.5:1.
- Roboto measured with skrifa: digits 0.562 em (Helvetica 0.556); bold "362" at 2.5 mm differs
  from the Helvetica estimate, checked in `roboto_text_is_measured_named_and_embedded`.
- Demo plan (A3, 1:100) as PDF: 450 KB with Roboto Regular and Bold embedded whole (was tens
  of KB with Helvetica). poppler's `pdffonts` lists both as embedded CID TrueType with
  Unicode maps; `pdftotext` reads the text back; Ghostscript renders it without error.
- Binary and browser download: +0.7 MB (two 350 KB font files).

## Revisit when

- PDF size matters (many sheets per file, e-mail limits): subset the embedded fonts.
- Someone needs another font on paper: a font file in the style (embedded as above).
- Text types' own `font` field (#43, unused by the drawing code) gets wired up.
