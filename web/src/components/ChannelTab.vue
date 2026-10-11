<script setup>
// Channel (SPECIFICATION.md §7): input and output pictures with a wipe, bypass, A/B and reset, the
// correction (wheels or RGB pots, master faders, clipping) and the presets of the channel and of all channels.
import { computed, onMounted, onUnmounted, ref } from "vue";
import ChannelActions from "./ChannelActions.vue";
import ChannelPicker from "./ChannelPicker.vue";
import CorrectionControls from "./CorrectionControls.vue";
import IdCode from "./IdCode.vue";
import Pill from "./Pill.vue";
import { STATE_TEXT, fmtFormat, modeText, stateKind } from "../api.js";
import { deletePreset, live, recallPreset, refreshPresets, savePreset, selectedChannel } from "../store.js";

const ch = selectedChannel;
const wipe = ref(50);
const presetName = ref("");

// The JPEG thumbnails (CC_PREVIEW_FPS) are fetched four times a second while this page is open.
const stamp = ref(0);
let timer = 0;
onMounted(() => {
  refreshPresets();
  timer = setInterval(() => (stamp.value = Date.now()), 250);
});
onUnmounted(() => clearInterval(timer));
const picture = (kind) => `/api/v1/channels/${ch.value.channel}/preview/${kind}.jpg?t=${stamp.value}`;
const hide = (event) => (event.target.style.visibility = "hidden");
const show = (event) => (event.target.style.visibility = "");

const channelPresets = computed(() => live.presets.filter((p) => p.channel === ch.value?.channel));
const globalPresets = computed(() => live.presets.filter((p) => p.channel === 0));
async function save(global) {
  const name = presetName.value.trim();
  if (!name) return;
  await savePreset(global ? 0 : ch.value.channel, name);
}
function remove(channel, name) {
  if (confirm(`Delete preset "${name}"?`)) deletePreset(channel, name);
}
</script>

<template>
  <div v-if="!ch" class="empty">No channels.</div>
  <template v-else>
    <div class="toolbar">
      <ChannelPicker />
      <ChannelActions :channel="ch" />
      <span class="spacer"></span>
      <Pill :text="STATE_TEXT[ch.state] || ch.state" :kind="stateKind(ch.state)" title="channel state" />
      <Pill :text="`clip ${(ch.clipped_ratio * 100).toFixed(2)} %`" :kind="ch.clipped_ratio > 0.01 ? 'warn' : 'neutral'" title="clipped samples in the last second" />
    </div>

    <div class="grid two">
      <div class="panel">
        <h3>Pictures</h3>
        <div class="pictures">
          <figure class="picture">
            <img :src="picture('input')" alt="" @error="hide" @load="show" />
            <figcaption class="ov tl">Input</figcaption>
          </figure>
          <figure class="picture" :style="{ '--wipe': `${wipe}%` }">
            <img :src="picture('input')" alt="" @error="hide" @load="show" />
            <img class="over" :src="picture('output')" alt="" @error="hide" @load="show" />
            <figcaption class="ov tl">Output</figcaption>
            <figcaption v-if="wipe > 0" class="ov tr">Input</figcaption>
          </figure>
        </div>
        <label>Wipe: input left of {{ wipe }} %</label>
        <input v-model.number="wipe" type="range" min="0" max="100" aria-label="Wipe" />
      </div>
      <div class="panel">
        <h3>Source and output</h3>
        <dl class="kv">
          <dt>Source</dt>
          <dd>{{ ch.source_label || "not routed" }}</dd>
          <dt>Domain / flow</dt>
          <dd><IdCode :id="ch.source_domain" /> / <IdCode :id="ch.source_flow" /></dd>
          <dt>Format</dt>
          <dd>{{ fmtFormat(ch) }}</dd>
          <dt>Processing</dt>
          <dd>{{ modeText(ch) }}</dd>
          <dt>Output flow</dt>
          <dd><IdCode :id="ch.output_flow" /></dd>
          <dt>Grains</dt>
          <dd>{{ ch.grains }}</dd>
        </dl>
        <div v-if="ch.color_warning" class="warnbox">{{ ch.color_warning }}</div>
      </div>
    </div>

    <CorrectionControls :channel="ch" />

    <div class="panel">
      <h3>Presets</h3>
      <div class="row tight">
        <input v-model="presetName" placeholder="preset name" aria-label="Preset name" maxlength="64" style="width: 16rem" @keydown.enter="save(false)" />
        <button class="btn" :disabled="!presetName.trim()" title="This channel's settings" @click="save(false)">Save channel</button>
        <button class="btn secondary" :disabled="!presetName.trim()" title="The settings of every channel" @click="save(true)">Save global</button>
      </div>
      <table v-if="channelPresets.length || globalPresets.length" style="margin-top: 0.6rem">
        <thead>
          <tr><th>Name</th><th>Scope</th><th></th></tr>
        </thead>
        <tbody>
          <tr v-for="p in [...channelPresets, ...globalPresets]" :key="`${p.channel}/${p.name}`">
            <td>{{ p.name }}</td>
            <td class="muted">{{ p.channel ? ch.label : "all channels" }}</td>
            <td class="nowrap" style="text-align: right">
              <button class="btn small" @click="recallPreset(p.channel, p.name)">Recall</button>
              <button class="btn small secondary" style="margin-left: 0.3rem" @click="remove(p.channel, p.name)">Delete</button>
            </td>
          </tr>
        </tbody>
      </table>
      <p v-else class="note">No presets for this channel yet. A preset holds the controls of the active A/B slot.</p>
    </div>
  </template>
</template>
