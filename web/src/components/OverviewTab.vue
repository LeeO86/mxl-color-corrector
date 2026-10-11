<script setup>
// Overview (SPECIFICATION.md §7): every channel with its output picture (once a second), state, source,
// processing mode, A/B slot, bypass and clipping. A click opens the channel.
import { onMounted, onUnmounted, ref } from "vue";
import { STATE_TEXT, fmtFormat, modeText } from "../api.js";
import { channels, selectChannel } from "../store.js";

const stamp = ref(0);
let timer = 0;
onMounted(() => (timer = setInterval(() => (stamp.value = Date.now()), 1000)));
onUnmounted(() => clearInterval(timer));
function open(n) {
  selectChannel(n);
  location.hash = "channel";
}
const hide = (event) => (event.target.style.visibility = "hidden");
const show = (event) => (event.target.style.visibility = "");
</script>

<template>
  <div v-if="!channels.length" class="empty">No channels.</div>
  <div v-else class="grid cards">
    <article
      v-for="c in channels"
      :key="c.channel"
      class="panel card"
      role="button"
      tabindex="0"
      :aria-label="`Open ${c.label}`"
      @click="open(c.channel)"
      @keydown.enter="open(c.channel)"
    >
      <h3>
        <span class="name">{{ c.label }}</span>
        <span class="spacer"></span>
        <span v-if="c.bypass" class="state-tag bypass">bypass</span>
        <span class="state-tag" :class="c.state">{{ STATE_TEXT[c.state] || c.state }}</span>
      </h3>
      <figure class="picture">
        <img :src="`/api/v1/channels/${c.channel}/preview/output.jpg?t=${stamp}`" alt="" @error="hide" @load="show" />
        <figcaption class="ov br">A/B {{ c.ab.toUpperCase() }}</figcaption>
      </figure>
      <div class="srcline">{{ c.source_label || "not routed" }}</div>
      <div class="statusline">
        <span>{{ fmtFormat(c) }}</span>
        <span>{{ modeText(c) }}</span>
        <span :class="{ warn: c.clipped_ratio > 0.01 }">clip {{ (c.clipped_ratio * 100).toFixed(2) }} %</span>
      </div>
      <div v-if="c.color_warning" class="statusline"><span class="warn">{{ c.color_warning }}</span></div>
    </article>
  </div>
</template>
