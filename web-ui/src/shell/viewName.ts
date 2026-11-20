// A view's display name: said in the page's language while the default name is kept (`auto_name`).
import type { ViewSummary } from "../bimble";
import { t } from "../i18n";

const KEY = { "3d": "view.three_d", plan: "view.plan", ceiling: "view.ceiling" } as const;

export function viewName(v: ViewSummary): string {
  const a = v.auto_name;
  if (!a) return v.name;
  const word = t(KEY[a.kind]);
  return a.storey ? `${word} ${a.storey}` : word;
}
