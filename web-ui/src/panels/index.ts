// Right panel registry: every src/panels/*.vue, id = file name in kebab-case. 400 wide, right of the main column.
import { ref, type Component } from "vue";
import { registry } from "../pages";

export const panels = registry(import.meta.glob<{ default: Component }>("./*.vue", { eager: true }));
/** The right panel shown, or null. */
export const currentPanel = ref<string | null>(null);
