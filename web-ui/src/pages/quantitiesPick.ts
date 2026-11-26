// DEAD1: the schedule the Quantities page shows when another surface chose one (the palette's
// "Go to: <schedule>"); null: "All elements". Page state only (ADR 0048), as levelsPick.ts.
import { ref } from "vue";
export const pickedSchedule = ref<string | null>(null);
