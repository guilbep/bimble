// The language setting: one ref, one lookup. No i18n library (a few lines do it).
import { ref } from "vue";
import { fr, type MessageKey } from "./fr";
import { en } from "./en";
// One part per shell component: each ticket adds keys only in its own part (en typed against fr, so a missing key fails typecheck).
import * as quickbar from "./parts/quickbar";
import * as tabsbar from "./parts/tabsbar";
import * as ribbon from "./parts/ribbon";
import * as leftpanel from "./parts/leftpanel";
import * as viewtabs from "./parts/viewtabs";
import * as statusbar from "./parts/statusbar";
import * as modifier from "./parts/modifier";
import * as home from "./parts/home";
import * as projectinfo from "./parts/projectinfo";

export type Lang = "fr" | "en";
const KEY = "bimble.lang";
const parts = [quickbar, tabsbar, ribbon, leftpanel, viewtabs, statusbar, modifier, home, projectinfo] as const;
type Key = MessageKey | keyof typeof quickbar.fr | keyof typeof tabsbar.fr | keyof typeof ribbon.fr
  | keyof typeof leftpanel.fr | keyof typeof viewtabs.fr | keyof typeof statusbar.fr | keyof typeof modifier.fr | keyof typeof home.fr | keyof typeof projectinfo.fr;
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
  return messages[lang.value][key].replace(/\{(\w+)\}/g, (m, k: string) => String(vars[k] ?? m));
}

document.documentElement.lang = lang.value;
