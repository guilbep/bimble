// Dialog registry: every src/dialogs/*.vue, id = file name in kebab-case. Rendered over everything by App.vue.
// The component picks its framing: root class `bb-dialog` (cream card) or `bb-menu` (sheet menu). ADR 0052: no OK/Cancel.
import { ref, type Component } from "vue";
import { registry } from "../pages";

export const dialogs = registry(import.meta.glob<{ default: Component }>("./*.vue", { eager: true }));
/** The open dialog, or null. Esc closes it. */
export const currentDialog = ref<string | null>(null);
