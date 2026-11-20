<script setup lang="ts">
// EXAMPLE dialog: proves the registry (?dialog=blank). Copy it to add a dialog; delete it when a real dialog exists.
import { t } from "../i18n";
</script>

<template>
  <div class="bb-dialog" role="dialog" :aria-label="t('example.title')">
    <h2>{{ t('example.title') }}</h2>
  </div>
</template>
