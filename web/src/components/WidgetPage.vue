<script setup>
// Operator-screen widget (SPECIFICATION.md §7.1): /widget/controls?channel=<n> (the correction) and
// /widget/bypass?channel=<n> (bypass, A/B, reset), with [&theme=dark|light|transparent], without the app
// around it, on this corrector's own API (same origin). It posts {type: "widget-ready"} and
// {type: "widget-size", w, h} to the page that frames it.
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import ChannelActions from "./ChannelActions.vue";
import CorrectionControls from "./CorrectionControls.vue";
import { STATE_TEXT } from "../api.js";
import { channelOf, live, startLive, stopLive } from "../store.js";

const params = new URLSearchParams(location.search);
const controls = location.pathname.replace(/\/$/, "") === "/widget/controls";
const n = Number(params.get("channel"));
const theme = params.get("theme");
if (theme) document.documentElement.dataset.theme = theme;
// A frame is see-through only when its color scheme matches its parent's; without the meta it is the default.
if (theme === "transparent") document.querySelector('meta[name="color-scheme"]')?.remove();

const ch = computed(() => channelOf(n));
const root = ref(null);
let ready = false;
let observer = null;

function post(message) {
  if (window.parent !== window) window.parent.postMessage(message, "*");
}
function postSize() {
  const rect = root.value.getBoundingClientRect();
  post({ type: "widget-size", w: Math.round(rect.width), h: Math.round(rect.height) });
}
watch(
  ch,
  (c) => {
    if (!c || ready) return;
    ready = true;
    post({ type: "widget-ready" });
    postSize();
  },
  { flush: "post" },
);
onMounted(() => {
  startLive();
  observer = new ResizeObserver(() => ready && postSize());
  observer.observe(root.value);
});
onUnmounted(() => {
  observer?.disconnect();
  stopLive();
});
</script>

<template>
  <div ref="root" class="widget-root">
    <template v-if="ch">
      <div class="widget-head">
        <span class="name">{{ ch.label }}</span>
        <span class="state-tag" :class="ch.state">{{ STATE_TEXT[ch.state] || ch.state }}</span>
        <span v-if="controls && ch.bypass" class="state-tag bypass">bypass</span>
        <span class="spacer"></span>
        <span class="muted small">{{ ch.source_label }}</span>
        <span v-if="controls" class="muted small">A/B {{ ch.ab.toUpperCase() }}</span>
      </div>
      <CorrectionControls v-if="controls" :channel="ch" compact />
      <ChannelActions v-else :channel="ch" grid />
    </template>
    <div v-else class="empty">{{ live.error || `Waiting for channel ${n}…` }}</div>
    <div v-if="live.actionError" class="widget-error" title="Dismiss" @click="live.actionError = ''">{{ live.actionError }}</div>
  </div>
</template>
