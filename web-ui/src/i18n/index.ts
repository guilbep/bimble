// The language setting: one ref, one lookup. No i18n library (a few lines do it).
import { ref } from "vue";
import { fr, type MessageKey } from "./fr";
import { en } from "./en";

export type Lang = "fr" | "en";
const KEY = "bimble.lang";
const messages = { fr, en } as const;

function initial(): Lang {
  try {
    const saved = localStorage.getItem(KEY);
    if (saved === "fr" || saved === "en") return saved;
  } catch { /* no storage: fall back to the browser */ }
  return navigator.language.toLowerCase().startsWith("fr") ? "fr" : "en";
}

export const lang = ref<Lang>(initial());

export function setLang(l: Lang): void {
  lang.value = l;
  document.documentElement.lang = l;
  try { localStorage.setItem(KEY, l); } catch { /* works without it */ }
}

/** The message for `key` in the current language; `{name}` takes vars.name. Reactive: reads `lang`. */
export function t(key: MessageKey, vars: Record<string, string | number> = {}): string {
  return messages[lang.value][key].replace(/\{(\w+)\}/g, (m, k: string) => String(vars[k] ?? m));
}

document.documentElement.lang = lang.value;
