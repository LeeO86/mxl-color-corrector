<script setup>
// White or black colour (SPECIFICATION.md §4.2): the colour wheel or the RGB pots, picked per section and
// kept in this browser. Both set the same R, G, B trims: the wheel their tint (its luma part stays), a pot
// one channel; the server reads the wheel back from the trims, so both views always agree. Exact values
// are typed into the pots.
import { computed, ref } from "vue";
import Fader from "./Fader.vue";
import Segmented from "./Segmented.vue";
import { fmtPercent } from "../api.js";
import { patchControls, store, stored } from "../store.js";

const props = defineProps({
  channel: { type: Object, required: true },
  which: { type: String, required: true }, // white or black
  compact: { type: Boolean, default: false },
});

// Trim at the wheel's rim (server kWhiteWheelSpan, kBlackWheelSpan) and the trim range.
const SPAN = { white: 20, black: 5 };
const LIMIT = 100;
const VIEWS = [
  { value: "wheel", label: "Wheel", title: "Colour wheel" },
  { value: "rgb", label: "RGB", title: "R, G and B pots" },
];
const AXES = [
  { key: "r", label: "R" },
  { key: "g", label: "G" },
  { key: "b", label: "B" },
];

const view = ref(stored(`view.${props.which}`, "wheel") === "rgb" ? "rgb" : "wheel");
function setView(value) {
  view.value = value;
  store(`view.${props.which}`, value);
}

const trims = computed(() => props.channel[props.which]);
const title = computed(() => (props.which === "white" ? "White colour" : "Black colour"));
const send = (controls) => patchControls(props.channel.channel, controls);

// The wheel: +x red (Cr), +y blue (Cb, downwards). The dot follows the pointer while it is dragged.
const drag = ref(null);
const dot = computed(() => drag.value || props.channel[`${props.which}_wheel`]);
// The tint is beyond the wheel's reach (the dot stays on the rim): the chroma radius of the trims.
const beyond = computed(() => {
  const { r, g, b } = trims.value;
  const luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  const scale = SPAN[props.which] / 1.8556;
  return Math.hypot((r - luma) / (1.5748 * scale), (b - luma) / (1.8556 * scale)) > 1.001;
});
function wheelAt(event) {
  const rect = event.currentTarget.getBoundingClientRect();
  let x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
  let y = ((event.clientY - rect.top) / rect.height) * 2 - 1;
  const radius = Math.hypot(x, y);
  if (radius > 1) {
    x /= radius;
    y /= radius;
  }
  drag.value = { x, y };
  send({ [`${props.which}_wheel`]: { x, y } });
}
function wheelDown(event) {
  if (event.button !== 0) return;
  event.currentTarget.setPointerCapture(event.pointerId);
  wheelAt(event);
}
function wheelMove(event) {
  if (drag.value) wheelAt(event);
}
const wheelUp = () => (drag.value = null);

const setAxis = (axis, value) => send({ [props.which]: { [axis]: value } });
const neutral = () => send({ [props.which]: { r: 0, g: 0, b: 0 } });
const isNeutral = computed(() => !trims.value.r && !trims.value.g && !trims.value.b);
</script>

<template>
  <div class="panel colour">
    <h3>
      {{ compact ? (which === "white" ? "White" : "Black") : title }}
      <span class="spacer"></span>
      <Segmented :options="VIEWS" :model-value="view" class="small" :label="`${title} control`" @update:model-value="setView" />
    </h3>
    <template v-if="view === 'wheel'">
      <div
        class="wheel"
        :class="[which, { beyond }]"
        role="slider"
        :aria-label="`${title} wheel`"
        :aria-valuetext="`R ${trims.r.toFixed(1)}, G ${trims.g.toFixed(1)}, B ${trims.b.toFixed(1)}`"
        :title="beyond ? 'The tint is beyond the wheel (RGB pots): the dot stays on the rim' : `Full deflection ±${SPAN[which]} %`"
        @pointerdown="wheelDown"
        @pointermove="wheelMove"
        @pointerup="wheelUp"
        @pointercancel="wheelUp"
        @lostpointercapture="wheelUp"
      >
        <span class="dot" :style="{ left: `${(dot.x + 1) * 50}%`, top: `${(dot.y + 1) * 50}%` }"></span>
      </div>
      <div class="readout num">
        <span v-for="a in AXES" :key="a.key" :class="a.key">{{ a.label }} {{ fmtPercent(trims[a.key]) }}</span>
      </div>
    </template>
    <template v-else>
      <Fader
        v-for="a in AXES"
        :key="a.key"
        :label="a.label"
        :tint="a.key"
        :value="trims[a.key]"
        :min="-LIMIT"
        :max="LIMIT"
        @change="(v) => setAxis(a.key, v)"
      />
    </template>
    <div class="actions">
      <span v-if="beyond && view === 'wheel'" class="muted small">beyond the wheel</span>
      <span class="spacer"></span>
      <button class="btn small secondary" :disabled="isNeutral" title="R, G and B to 0" @click="neutral">Neutral</button>
    </div>
  </div>
</template>
