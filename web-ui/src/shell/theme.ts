// The interface's look from the user's settings (ticket SET1): Rust's six themes (crates/bimble-viewer/
// src/ui_theme.rs:57-163), its two fonts (:187) and its text size, 10–16 pt (:217-218), on the lab's own
// design tokens (src/brand/tokens.css). Plain TypeScript, no import: node reads it as it is
// (scripts/contrast.mjs checks every theme's text against its grounds, WCAG AA).
//
// - `grey`, Rust's default, is the lab's own default look (the Bimble design: cream, ink, the five
//   discs): an app opened with no settings looks as it always did (DEVIATIONS 23). The five others
//   take Rust's grounds and text (panel_bg, field_bg, hover_bg, divider, text, text_muted) and keep
//   Bimble's jobs for the disc colours (clay: do, pink: choose, moss: done, ochre: look, slate:
//   other), as fills that pass 4.5:1 with that theme's text and as text shades that pass on its grounds.
// - Applied as `data-theme` on the root (a token set each, `:root[data-theme=…]`), so every colour of
//   the lab, which all go through `--bb-*`, follows. Rust has no "system" theme: `prefers-color-scheme`
//   is not followed.
// - The font: `roboto` (Rust's default) is the lab's own text font (Source Sans 3: the lab ships no
//   Roboto); `ubuntu` asks the system for Ubuntu. The text size scales the root font size and the
//   lab's UI sizes from Rust's 12 pt default (12 pt: the sizes of the design).

/** The tokens a theme sets (tokens.css names, without `--bb-`). */
export type Token = "cream" | "sheet" | "sunk" | "ink" | "stone" | "line" | "on-ink"
  | "clay" | "clay-text" | "pink" | "pink-text" | "moss" | "moss-text" | "ochre" | "ochre-text" | "slate" | "slate-text";

/** The lab's default tokens (tokens.css), for the contrast check: the `grey` theme. */
export const BASE: Readonly<Record<Token, string>> = {
  cream: "#F5F2EB", sheet: "#FFFFFF", sunk: "#EDE7DB", ink: "#141414", stone: "#5C574D", line: "#DDD6C8", "on-ink": "#F5F2EB",
  clay: "#C06A41", "clay-text": "#A35A37", pink: "#D7ACB6", "pink-text": "#80676D", moss: "#6A8757", "moss-text": "#5B744A",
  ochre: "#D2B347", "ochre-text": "#7D6B2A", slate: "#6E88A3", "slate-text": "#5A6F85",
};

/** Rust's theme ids (ui_theme.rs THEMES, the default first) and the tokens each one changes. */
export const THEMES: Readonly<Record<string, Partial<Record<Token, string>>>> = {
  grey: {},
  dark: { // ui_theme.rs:79 Dark: panel 1e1e1e, ribbon 2a2a2a, hover 313131, border 3a3a3a, text e6e6e6, muted a8a8a8
    cream: "#1E1E1E", sheet: "#2A2A2A", sunk: "#313131", ink: "#E6E6E6", stone: "#A8A8A8", line: "#3A3A3A", "on-ink": "#1E1E1E",
    clay: "#8C4A2C", "clay-text": "#E8A07A", pink: "#74505A", "pink-text": "#E0B4BF", moss: "#4A6339", "moss-text": "#9DC08A",
    ochre: "#6E5A1C", "ochre-text": "#E0C764", slate: "#465C74", "slate-text": "#9DB6D0",
  },
  light: { // ui_theme.rs:100 Light: panel f7f7f7, field ffffff, widget e9e9e9, divider d6d6d6, text 1a1a1a, muted 555555
    cream: "#F7F7F7", sheet: "#FFFFFF", sunk: "#E9E9E9", ink: "#1A1A1A", stone: "#555555", line: "#D6D6D6", "on-ink": "#F7F7F7",
    clay: "#C8784F", moss: "#7A9466", // Bimble's clay and moss one step lighter: 4.5:1 with Rust's text 1a1a1a
  },
  "solarized-dark": { // ui_theme.rs:122: panel 002b36, ribbon 073642, divider 0d4150, border 2a5560, text eee8d5, muted 93a1a1
    cream: "#002B36", sheet: "#073642", sunk: "#04303C", ink: "#EEE8D5", stone: "#93A1A1", line: "#2A5560", "on-ink": "#002B36",
    clay: "#8A3A14", "clay-text": "#F4A27A", pink: "#7E2A55", "pink-text": "#F09AC0", moss: "#4F5C00", "moss-text": "#B5C94A",
    ochre: "#6B5200", "ochre-text": "#E0B83A", slate: "#1A5A88", "slate-text": "#8CC8F0",
  },
  "solarized-light": { // ui_theme.rs:143: panel fdf6e3, field ffffff, title eee8d5, border d3cbb7, text 073642, muted 4c5f66
    cream: "#FDF6E3", sheet: "#FFFFFF", sunk: "#EEE8D5", ink: "#073642", stone: "#4C5F66", line: "#D3CBB7", "on-ink": "#FDF6E3",
    clay: "#E89A6E", "clay-text": "#9A3A10", pink: "#E8B0C8", "pink-text": "#A32566", moss: "#B5C35A", "moss-text": "#5A6800",
    ochre: "#E3C35A", "ochre-text": "#735800", slate: "#9CC4E8", "slate-text": "#155A8A",
  },
  "high-contrast": { // ui_theme.rs:164: panel 000000, ribbon 101010, hover 333333, divider 808080, text ffffff, muted d0d0d0
    cream: "#000000", sheet: "#101010", sunk: "#333333", ink: "#FFFFFF", stone: "#D0D0D0", line: "#808080", "on-ink": "#000000",
    clay: "#8A3D1A", "clay-text": "#FFB08A", pink: "#7A3A4C", "pink-text": "#FFC0D0", moss: "#2F5A1E", "moss-text": "#A8E08A",
    ochre: "#6A5500", "ochre-text": "#FFD800", slate: "#2A4A70", "slate-text": "#A8D0FF",
  },
};

/** The tokens of theme `id` (an unknown id: the default, as ui_theme.rs by_id). */
export const tokensOf = (id: string): Record<Token, string> => ({ ...BASE, ...(THEMES[id] ?? {}) });

/** Rust's font ids (ui_theme.rs UiFont::ALL) and the CSS font family each one sets (null: the lab's). */
export const FONTS: Readonly<Record<string, string | null>> = {
  roboto: null,
  ubuntu: "'Ubuntu', 'Ubuntu Light', 'Source Sans 3', 'Helvetica Neue', Arial, sans-serif",
};

/** DEAD1: the faces a font choice needs from this computer (none shipped with the lab: ADR 0071);
 *  a choice not listed here is the lab's own fonts (shipped, brand/fonts.css). */
const SYSTEM_FACES: Readonly<Record<string, readonly string[]>> = { ubuntu: ["Ubuntu", "Ubuntu Light"] };
/** DEAD1: whether the font choice `id` can show on this computer: the lab's own fonts always; a
 *  system font when one of its faces is installed (text measured with it differs from the generic
 *  fallbacks). Without a canvas: assumed installed. */
export function fontInstalled(id: string): boolean {
  const faces = SYSTEM_FACES[id];
  if (!faces) return id in FONTS;
  const c = document.createElement("canvas").getContext("2d");
  if (!c) return true;
  const text = "mmmmmmmmmmlli1WQ@#ąé";
  for (const face of faces)
    for (const base of ["monospace", "serif"]) {
      c.font = `72px ${base}`;
      const w0 = c.measureText(text).width;
      c.font = `72px '${face}', ${base}`;
      if (c.measureText(text).width !== w0) return true;
    }
  return false;
}

/** Rust's text sizes (ui_theme.rs TEXT_SIZE, TEXT_SIZES). */
export const TEXT_SIZE = 12;
export const TEXT_SIZES: readonly [number, number] = [10, 16];

/** The look of the settings: their `theme`, `ui_font`, `text_size` (settings.rs names). */
export interface Look { theme: string; ui_font: string; text_size: number }

/** The CSS of every theme but the default: one `:root[data-theme=…]` rule each. */
export function themeCss(): string {
  return Object.entries(THEMES).filter(([, t]) => Object.keys(t).length > 0)
    .map(([id, t]) => `:root[data-theme="${id}"] { ${Object.entries(t).map(([k, v]) => `--bb-${k}: ${v};`).join(" ")} color-scheme: ${["dark", "solarized-dark", "high-contrast"].includes(id) ? "dark" : "light"}; }`)
    .join("\n");
}

/** Gives the page this look: `data-theme`, the text font, the root font size and the UI sizes. */
export function applyLook(look: Look, root: HTMLElement = document.documentElement): void {
  if (!document.getElementById("bb-themes")) {
    const style = document.createElement("style");
    style.id = "bb-themes";
    style.textContent = themeCss();
    document.head.appendChild(style);
  }
  root.dataset.theme = look.theme in THEMES ? look.theme : "grey";
  const font = FONTS[look.ui_font] ?? null;
  if (font) root.style.setProperty("--bb-font-text", font);
  else root.style.removeProperty("--bb-font-text");
  const size = Math.min(TEXT_SIZES[1], Math.max(TEXT_SIZES[0], Number.isFinite(look.text_size) ? look.text_size : TEXT_SIZE));
  if (size === TEXT_SIZE) {
    root.style.removeProperty("font-size");
    for (const k of ["ui", "small", "body"]) root.style.removeProperty(`--bb-size-${k}`);
  } else {
    const f = size / TEXT_SIZE;
    root.style.fontSize = `${(16 * f).toFixed(2)}px`;
    root.style.setProperty("--bb-size-ui", `${(14 * f).toFixed(2)}px`);
    root.style.setProperty("--bb-size-small", `${(13 * f).toFixed(2)}px`);
    root.style.setProperty("--bb-size-body", `${(16 * f).toFixed(2)}px`);
  }
}
