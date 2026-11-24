# Bimble UI wireframes v01 (reference)

The reference for the Vue lab screens (ADR 0048), in the brand of ADR 0049. Imported on 20 Nov 2026
from `bimble-wireframes-v01.zip`. All sample data is synthetic.

- `fr/*.html`, `en/*.html`: each screen at 1440 × 900. The HTML is the source of truth.
- Row E (added 24 Nov 2026, ADR 0064): the small screens, each at its own size. `Phone*.html` at
  375 × 812, `PhoneLandscape.html` at 812 × 375, `Tablet.html` at 820 × 1180, `Laptop1280.html` at
  1280 × 720, `SmallRules.html` at 880 × 812. They are not in `web-ui/design-check/screens.json` yet.
- `DESIGN-SPEC.md`: tokens, type, measurements, components, screens, decisions.
- `PROMPT.md`: the implementation brief.
- `fonts/`: Sora, Source Sans 3, DM Mono (SIL OFL 1.1), for the pages to render alone.

The zip also held PNG renders of each page. They are not committed (9.6 MB): the comparison check
renders the HTML in Chrome at 1440 × 900, scale 1, the same way they were made (Pierre, 20 Nov).
