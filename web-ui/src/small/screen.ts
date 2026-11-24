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

export type SheetId = "views" | "objects" | "cut" | "measure" | "more";
/** The open sheet, or null. */
export const sheet = ref<SheetId | null>(null);
