<script setup lang="ts">
// The one "not available yet" state (ticket FAKE1): a page, dialog, panel or card that the core
// does not serve shows this, the same way everywhere, instead of an empty table or a dead button.
// `data-bb="not-available"` is what ccore/test/lab_routes.mjs looks for. Brand: the empty state
// (cream card, stone text, an eyebrow), no colour of action: nothing here can be done.
import { t } from "../i18n";

defineProps<{
  /** The screen's name, already translated. */
  what: string;
  /** "preview": not in this core yet; "readOnly": needs a model open for editing. */
  reason?: "preview" | "readOnly";
  /** What exists instead, already translated (optional). */
  instead?: string;
  /** Inside a card or a side panel: smaller. */
  compact?: boolean;
}>();
</script>

<template>
  <div class="na" :class="{ compact }" role="status" data-bb="not-available" :data-reason="reason ?? 'preview'">
    <div class="eyebrow">{{ t('na.eyebrow') }}</div>
    <div class="what">{{ what }}</div>
    <p class="say">{{ reason === 'readOnly' ? t('na.readOnly') : t('na.sentence') }}</p>
    <p v-if="instead" class="instead">{{ t('na.instead', { what: instead }) }}</p>
  </div>
</template>

<style scoped>
.na { display: flex; flex-direction: column; gap: var(--bb-space-2); max-width: 520px; margin: var(--bb-space-6) auto; padding: var(--bb-space-5);
  border: var(--bb-border-hairline); border-radius: var(--bb-radius-md); background: var(--bb-cream); color: var(--bb-ink); }
.na.compact { margin: 0; padding: var(--bb-space-3); gap: var(--bb-space-1); max-width: none; }
.eyebrow { font-family: var(--bb-font-data); font-size: var(--bb-size-eyebrow); letter-spacing: var(--bb-tracking-eyebrow); text-transform: uppercase; color: var(--bb-stone); }
.what { font-family: var(--bb-font-display); font-size: var(--bb-size-heading); line-height: var(--bb-lh-heading); }
.na.compact .what { font-size: var(--bb-size-ui); font-weight: 600; font-family: var(--bb-font-text); }
.say, .instead { margin: 0; font-size: var(--bb-size-ui); color: var(--bb-stone); }
.na.compact .say, .na.compact .instead { font-size: var(--bb-size-small); }
</style>
