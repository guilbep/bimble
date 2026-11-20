// Focus for the dialog backdrop (App.vue): on open, focus moves to the first input (else the first
// focusable); Tab and Shift+Tab cycle inside; on close, focus returns to the element that had it.
import { nextTick, watch, type Ref } from "vue";

const FOCUSABLE = 'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])';

const focusables = (root: HTMLElement) =>
  [...root.querySelectorAll<HTMLElement>(FOCUSABLE)].filter((e) => e.offsetParent !== null || e === document.activeElement);

/** Watches `open`; returns the keydown handler to put on the trap's root. */
export function useFocusTrap(root: Ref<HTMLElement | null>, open: Ref<unknown>) {
  let opener: HTMLElement | null = null;
  watch(open, async (now, before) => {
    if (now && !before) opener = document.activeElement as HTMLElement | null;
    if (now) {
      await nextTick();
      const r = root.value;
      if (!r || r.contains(document.activeElement)) return;
      (r.querySelector<HTMLElement>("input:not([disabled]), textarea:not([disabled])") ?? focusables(r)[0] ?? r).focus();
    } else if (before) {
      if (opener?.isConnected) opener.focus();
      opener = null;
    }
  });
  return (e: KeyboardEvent) => {
    const r = root.value;
    if (e.key !== "Tab" || !r) return;
    const all = focusables(r);
    if (!all.length) { e.preventDefault(); return; }
    const i = all.indexOf(document.activeElement as HTMLElement);
    const last = all.length - 1;
    if (e.shiftKey ? i <= 0 : i === last || i < 0) {
      e.preventDefault();
      all[e.shiftKey ? last : 0]?.focus();
    }
  };
}
