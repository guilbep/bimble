<script setup lang="ts">
// ponytail: hidden until the Modifier ticket decides when it shows (B2: a selection being modified).
const open = false;
</script>

<template>
  <!-- Reference: .vp + div (B2). -->
  <section v-if="open" class="mod" data-bb="modifier" />
</template>
