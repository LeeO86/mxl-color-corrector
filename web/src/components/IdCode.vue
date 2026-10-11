<script setup>
// An id shortened to 8 characters (all of it on hover), or a dash when there is none.
import { shortId } from "../api.js";

defineProps({ id: { type: String, default: null } });
</script>

<template>
  <code v-if="id" :title="id">{{ shortId(id) }}</code>
  <span v-else class="muted">–</span>
</template>
