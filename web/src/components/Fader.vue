<script setup>
// One control as a fader (a "pot"): name, exact value, reset, and a slider between − and + buttons for
// fine steps. A held button repeats (mouse and touch). While the slider is dragged or the value typed,
// the fader shows its own value, so status pushes do not make it jump.
import { computed, onUnmounted, ref } from "vue";

const props = defineProps({
  label: { type: String, required: true },
  value: { type: Number, default: 0 },
  min: { type: Number, required: true },
  max: { type: Number, required: true },
  def: { type: Number, default: 0 }, // the reset value
  fine: { type: Number, default: 0.1 }, // one − / + step
  tint: { type: String, default: "" }, // r, g or b: the slider colour
});
const emit = defineEmits(["change"]);

const drag = ref(null);
const typed = ref(null);
const shown = computed(() => drag.value ?? props.value);
const round = (v) => Math.min(props.max, Math.max(props.min, Math.round(v * 10) / 10));

function onSlide(event) {
  drag.value = Number(event.target.value);
  emit("change", drag.value);
}
function onSlideEnd(event) {
  emit("change", Number(event.target.value));
  drag.value = null;
}
function onType(event) {
  const v = Number(event.target.value);
  typed.value = null;
  if (event.target.value !== "" && Number.isFinite(v)) emit("change", round(v));
}

// − / +: one step per press (Enter or Space too), repeated while held.
let target = 0;
let hold = 0;
function step(dir) {
  target = round(target + dir * props.fine);
  emit("change", target);
}
function press(dir, event) {
  if (event.button !== 0) return;
  event.currentTarget.setPointerCapture?.(event.pointerId);
  target = props.value;
  step(dir);
  hold = setTimeout(function again() {
    step(dir);
    hold = setTimeout(again, 60);
  }, 400);
}
function release() {
  clearTimeout(hold);
  hold = 0;
}
function key(dir, event) {
  if (event.detail !== 0) return; // a pointer press was handled by press()
  target = props.value;
  step(dir);
}
onUnmounted(release);
</script>

<template>
  <div class="fader" :class="tint">
    <span class="name">{{ label }}</span>
    <input
      class="value"
      type="number"
      :min="min"
      :max="max"
      step="0.1"
      :value="typed ?? shown.toFixed(1)"
      :aria-label="`${label} value`"
      @focus="typed = shown.toFixed(1)"
      @input="typed = $event.target.value"
      @change="onType"
      @blur="typed = null"
    />
    <button class="btn small secondary" title="Reset" :aria-label="`Reset ${label}`" :disabled="value === def" @click="emit('change', def)">↺</button>
    <div class="track">
      <button
        class="btn secondary step"
        :aria-label="`${label} down`"
        @pointerdown="press(-1, $event)"
        @pointerup="release"
        @pointercancel="release"
        @lostpointercapture="release"
        @click="key(-1, $event)"
        @contextmenu.prevent
      >
        −
      </button>
      <input type="range" :min="min" :max="max" step="0.1" :value="shown" :aria-label="label" @input="onSlide" @change="onSlideEnd" />
      <button
        class="btn secondary step"
        :aria-label="`${label} up`"
        @pointerdown="press(1, $event)"
        @pointerup="release"
        @pointercancel="release"
        @lostpointercapture="release"
        @click="key(1, $event)"
        @contextmenu.prevent
      >
        +
      </button>
    </div>
  </div>
</template>
