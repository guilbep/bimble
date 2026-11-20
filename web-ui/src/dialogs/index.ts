// Dialog registry: every src/dialogs/*.vue, id = file name in kebab-case. Rendered over everything by App.vue.
// The component picks its framing: root class `bb-dialog` (cream card) or `bb-menu` (sheet menu). ADR 0052: no OK/Cancel.
// Optional export from the plain <script> block: `hint` (an i18n key shown in the status bar while the dialog is open).
import { ref } from "vue";
import { byId, registry, type ScreenModule } from "../pages";

export const dialogModules = byId(import.meta.glob<ScreenModule>("./*.vue", { eager: true }));
export const dialogs = registry(dialogModules);
/** The open dialog, or null. Esc closes it. */
export const currentDialog = ref<string | null>(null);
/** The open dialog's module, or null. */
export const dialog = (): ScreenModule | null => (currentDialog.value && dialogModules[currentDialog.value]) || null;
