<script setup>
// The channel a page edits, with each channel's state. Kept in this browser.
import { computed } from "vue";
import Segmented from "./Segmented.vue";
import { STATE_TEXT } from "../api.js";
import { channels, live, selectChannel } from "../store.js";

const options = computed(() => channels.value.map((c) => ({ value: c.channel, label: c.label, state: c.state, bypass: c.bypass })));
</script>

<template>
  <div class="group">
    <span class="caption">Channel</span>
    <Segmented :options="options" :model-value="live.selected" label="Channel" @update:model-value="selectChannel">
      <template #default="{ option }">
        {{ option.label }}<span class="state-tag" :class="option.state">{{ STATE_TEXT[option.state] }}</span
        ><span v-if="option.bypass" class="state-tag bypass">bypass</span>
      </template>
    </Segmented>
  </div>
</template>
