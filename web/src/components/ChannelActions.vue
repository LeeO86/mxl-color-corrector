<script setup>
// Bypass on/off, the A/B settings slot and Reset (every control to its default) of one channel. Reset
// asks for a second press within 3 s. `grid` lays them out as big buttons (the bypass widget).
import { onUnmounted, ref } from "vue";
import Segmented from "./Segmented.vue";
import { channelAction } from "../store.js";

const props = defineProps({
  channel: { type: Object, required: true },
  grid: { type: Boolean, default: false },
});
const SLOTS = [
  { value: "a", label: "A", title: "Settings slot A" },
  { value: "b", label: "B", title: "Settings slot B" },
];

const armed = ref(false);
let disarm = 0;
function reset() {
  if (!armed.value) {
    armed.value = true;
    disarm = setTimeout(() => (armed.value = false), 3000);
    return;
  }
  clearTimeout(disarm);
  armed.value = false;
  channelAction(props.channel.channel, "reset", { control: "all" });
}
onUnmounted(() => clearTimeout(disarm));
const bypass = () => channelAction(props.channel.channel, "bypass", { enabled: !props.channel.bypass });
const slot = (value) => channelAction(props.channel.channel, "ab", { slot: value });
</script>

<template>
  <div :class="grid ? 'btn-grid three' : 'group'">
    <button class="btn bypass" :class="{ secondary: !channel.bypass }" :aria-pressed="channel.bypass" title="Pass the input through unchanged" @click="bypass">
      {{ channel.bypass ? "Bypassed" : "Bypass" }}
    </button>
    <Segmented :options="SLOTS" :model-value="channel.ab" :fullwidth="grid" label="A/B settings slot" @update:model-value="slot" />
    <button class="btn" :class="armed ? 'danger' : 'secondary'" :title="armed ? 'Press again to reset every control' : 'Every control to its default'" @click="reset">
      {{ armed ? "Confirm reset" : "Reset" }}
    </button>
  </div>
</template>
