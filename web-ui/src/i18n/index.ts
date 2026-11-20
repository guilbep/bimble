// The language setting: one ref, one lookup. No i18n library (a few lines do it).
import { ref } from "vue";
import { fr, type MessageKey } from "./fr";
import { en } from "./en";
// Every ./parts/*.ts registers itself: each part types its `en` from its `fr`, so a missing key fails typecheck there.
const parts = Object.values(import.meta.glob<{ fr: Record<string, string>; en: Record<string, string> }>("./parts/*.ts", { eager: true }));

export type Lang = "fr" | "en";
const KEY = "bimble.lang";
// ponytail: a glob has no module types, so part keys are plain strings; a missing key is logged in dev (below), not a type error.
type Key = MessageKey | (string & {});
const merge = (base: Record<string, string>, l: Lang) => Object.assign({}, base, ...parts.map((p) => p[l])) as Record<Key, string>;
const messages = { fr: merge(fr, "fr"), en: merge(en, "en") };

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
export function t(key: Key, vars: Record<string, string | number> = {}): string {
  const msg = messages[lang.value][key];
  if (msg === undefined) {
    if (import.meta.env.DEV) console.error(`i18n: missing key "${key}" (${lang.value})`);
    return key;
  }
  return msg.replace(/\{(\w+)\}/g, (m, k: string) => String(vars[k] ?? m));
}

document.documentElement.lang = lang.value;
