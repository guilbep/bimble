# ADR 0049 — Brand: two discs, one job per colour

- Status: Accepted (20 Nov 2026)
- Decided by: Pierre ("direction C for the artistic choices, and add colours from the palettes of
  those images": the five logo studies). Palette roles and contrast by Claude, on the design canvas
  "Bimble brand directions".
- Reversal cost: low for the Vue page (one tokens file), medium for the egui themes (ADR 0041)
- Touches: ADR 0041 (themes and Roboto), ADR 0043 (logo and icons, unchanged)

## Context / Problem

The Vue prototyping lab (ADR 0048) needs a tokens file before its first screen. Three directions
were drawn from the logo (Drawing sheet, Concrete, Two discs). The logo studies use five disc
colours: clay, pink, slate, ochre, moss.

## Decision

- Direction C, "Two discs": cream ground, round controls (pills, discs), square drawings, one large
  disc per screen at most (Home, empty states), never over work. Drawings print black and white.
- Type: Sora for headings, Source Sans 3 for text, DM Mono for data (ids, sizes). The Vue page
  loads them. The egui app keeps Roboto (ADR 0041) until a screen is ported.
- One job per colour. Fill, text shade (≥ 4.5:1 on cream and on white), and black on the fill:

| Colour | Job | Fill | Text shade | Black on fill |
|---|---|---|---|---|
| Clay | do: the main action | #C06A41 | #A35A37 | 4.7:1 |
| Pink | choose: selection | #D7ACB6 | #80676D | 9.2:1 |
| Moss | done: saved, in sync | #6A8757 | #5B744A | 4.6:1 |
| Ochre | look: not saved, borrowed, warnings | #D2B347 | #7D6B2A | 9.0:1 |
| Slate | other: linked models, references, "theirs" | #6E88A3 | #5A6F85 | 5.0:1 |

- Neutrals: cream #F5F2EB (ground), sheet #FFFFFF (cards, paper), ink #141414 (16.5:1 on cream),
  stone #5C574D (quiet text, 6.4:1), line #DDD6C8 (borders).
- Series order (charts, linked models): clay, slate, moss, ochre, pink. Errors have no logo colour:
  the word "Error" in ink on an ochre band.
- Marks: study 6 (clay and pink on cream) is the main mark, as in `packaging/logo.svg`. Proposed:
  study 7 (moss) marks the Vue lab, so a build under test never looks like a release.

## Alternatives considered

A (white paper, IBM Plex, square) and B (dark charcoal, Outfit with Roboto) are on the same canvas.

## Evidence

Contrast is computed with the WCAG 2 formula from the hex values (20 Nov). Disc colours sampled as
the median of a 45 × 40 px patch of each study (JPEG): clay read #BF6A41, kept as #C06A41 from
`packaging/logo.svg`.

## Revisit when

- The first Vue screens are seen on an old laptop screen.
- A screen is ported to egui.
