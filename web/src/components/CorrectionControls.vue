<script setup>
// The correction of one channel (SPECIFICATION.md §4): white and black colour (wheel or RGB pots), then
// gain, pedestal, brightness and saturation. Compact (the controls widget) leaves out the clip settings.
import ColourSection from "./ColourSection.vue";
import Fader from "./Fader.vue";
import { patchControls } from "../store.js";

const props = defineProps({
  channel: { type: Object, required: true },
  compact: { type: Boolean, default: false },
});

// Ranges and defaults of SPECIFICATION.md §4.1, in percent.
const MASTER = [
  { key: "gain", label: "Gain", min: 0, max: 200, def: 100 },
  { key: "pedestal", label: "Pedestal", min: -100, max: 100, def: 0 },
  { key: "brightness", label: "Brightness", min: -100, max: 100, def: 0 },
  { key: "saturation", label: "Saturation", min: 0, max: 200, def: 100 },
];
const send = (controls) => patchControls(props.channel.channel, controls);
</script>

<template>
  <div class="correction" :class="{ compact }">
    <ColourSection :channel="channel" which="white" :compact="compact" />
    <ColourSection :channel="channel" which="black" :compact="compact" />
    <div class="panel">
      <h3>{{ compact ? "Master" : "Master and clipping" }}</h3>
      <Fader
        v-for="m in MASTER"
        :key="m.key"
        :label="m.label"
        :value="channel[m.key]"
        :min="m.min"
        :max="m.max"
        :def="m.def"
        @change="(v) => send({ [m.key]: v })"
      />
      <template v-if="!compact">
        <label>Clip</label>
        <select :value="channel.clip" aria-label="Clip" @change="send({ clip: $event.target.value })">
          <option value="legal">legal (Y 64–940, C 64–960)</option>
          <option value="extended">extended (4–1019)</option>
          <option value="off">off (0–1023)</option>
        </select>
        <label class="check"><input type="checkbox" :checked="channel.rgb_clip" @change="send({ rgb_clip: $event.target.checked })" /> RGB gamut clip</label>
        <div v-if="channel.color_warning" class="warnbox">{{ channel.color_warning }}</div>
      </template>
    </div>
  </div>
</template>
