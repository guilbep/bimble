// The window's size class (ADR 0064): by width, 600 and 1 024 px. Below 1 024 the lab is the
// small-screen viewer (view, cut, measure, rename, material, note); from 1 024 the desktop frame.
// A phone on its side is wide but short: under 500 px of height it stays a phone.
import { computed, ref } from "vue";

export const PHONE_MAX = 600;
export const SMALL_MAX = 1024;
const SHORT = 500;

const size = ref({ w: window.innerWidth, h: window.innerHeight });
window.addEventListener("resize", () => { size.value = { w: window.innerWidth, h: window.innerHeight }; });

/** Under 1 024 px wide: the small-screen frame. */
export const small = computed(() => size.value.w < SMALL_MAX);
/** A phone: under 600 px wide, or on its side (short). */
export const phone = computed(() => size.value.w < PHONE_MAX || (small.value && size.value.h < SHORT));
/** Tools on a rail at the left and sheets as a panel at the right: a phone on its side, a tablet. */
export const wide = computed(() => small.value && size.value.w >= PHONE_MAX);

// Page zoom (pinch, double tap, or Safari zooming to a field): the small frame is laid out for the
// window, so a zoomed page shows only part of it (found on an iPhone 13 mini, 24 Nov: about 1.2,
// cause not known). Undo it: Safari snaps back to scale 1 when the viewport allows no more, then the
// previous rule is put back so a later pinch on the page still works where allowed.
const vv = window.visualViewport;
/** The page's zoom and the visible size, for `?debug=1`. */
export const zoom = ref({ scale: vv?.scale ?? 1, w: Math.round(vv?.width ?? size.value.w), h: Math.round(vv?.height ?? size.value.h), undone: 0 });
let undoing = false;
export function unzoom(): void {
  if (!vv) return;
  zoom.value = { ...zoom.value, scale: vv.scale, w: Math.round(vv.width), h: Math.round(vv.height) };
  if (!small.value || vv.scale <= 1.01 || undoing) return;
  const meta = document.querySelector<HTMLMetaElement>('meta[name="viewport"]');
  if (!meta) return;
  undoing = true;
  const old = meta.content;
  meta.content = `${old}, maximum-scale=1`;
  // The new rule applies when a frame is drawn; an idle view draws none, so ask for one with a style
  // change (and go back to the top left, where the frame starts).
  // 1 px of padding on <body>, which the fixed frame covers: Chrome reads the rule only after a
  // change of layout (a class with 0.01 px did nothing; e2e/small.mjs checks it).
  document.body.style.paddingBottom = document.body.style.paddingBottom ? "" : "1px";
  window.scrollTo(0, 0);
  window.setTimeout(() => { meta.content = old; undoing = false; zoom.value = { ...zoom.value, undone: zoom.value.undone + 1 }; }, 300);
}
vv?.addEventListener("resize", unzoom);
document.addEventListener("focusout", () => window.setTimeout(unzoom, 50));
/** `?debug=1`: the zoom readout in the top bar. */
export const debug = new URLSearchParams(location.search).get("debug") === "1";

export type SheetId = "views" | "objects" | "cut" | "measure" | "more";
/** The open sheet, or null. */
export const sheet = ref<SheetId | null>(null);
