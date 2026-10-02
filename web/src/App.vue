<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from "vue";

const page = ref("overview");
const channels = ref([]);
const nmos = ref({});
const settings = ref({});
const selected = ref(1);
const wipe = ref(50);
const presetName = ref("");
const route = reactive({ domain: "", flow: "" });
const status = ref("connecting");
let socket;
let sendTimer;
let pending = null;
let lastSend = 0;

const current = computed(() => channels.value.find((c) => c.channel === selected.value) || null);

function applyState(doc) {
  if (!doc) return;
  if (Array.isArray(doc.channels)) channels.value = doc.channels;
  if (doc.nmos) nmos.value = doc.nmos;
  if (!channels.value.find((c) => c.channel === selected.value) && channels.value.length) {
    selected.value = channels.value[0].channel;
  }
}

async function refresh() {
  const res = await fetch("/api/v1/status");
  if (res.ok) applyState(await res.json());
  const cfg = await fetch("/api/v1/settings");
  if (cfg.ok) settings.value = await cfg.json();
}

function connect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(`${proto}://${location.host}/api/v1/events`);
  socket.onopen = () => {
    status.value = "live";
  };
  socket.onclose = () => {
    status.value = "reconnecting";
    setTimeout(connect, 1000);
  };
  socket.onmessage = (ev) => {
    try {
      applyState(JSON.parse(ev.data));
    } catch {
      /* ignore malformed event */
    }
  };
}

function flush() {
  sendTimer = null;
  if (!pending || !socket || socket.readyState !== 1) return;
  socket.send(JSON.stringify(pending));
  pending = null;
  lastSend = performance.now();
}

function send(message) {
  const now = performance.now();
  pending = message;
  if (now - lastSend >= 33) flush();
  else if (!sendTimer) sendTimer = setTimeout(flush, 33 - (now - lastSend));
}

function patch(controls) {
  send({ type: "patch", channel: selected.value, controls });
}

function numeric(key, value) {
  patch({ [key]: Number(value) });
}

function rgb(group, axis, value) {
  const cur = current.value;
  if (!cur) return;
  patch({ [group]: { ...cur[group], [axis]: Number(value) } });
}

function wheel(which, x, y) {
  patch({ [`${which}_wheel`]: { x, y } });
}

async function post(path, body) {
  await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body || {}),
  });
  refresh();
}

async function savePreset(global) {
  const name = presetName.value.trim();
  if (!name) return;
  const path = global ? "/api/v1/presets" : `/api/v1/channels/${selected.value}/presets`;
  await post(path, { name });
}

async function activateRoute() {
  const summary = nmos.value.channels || [];
  const row = summary.find((c) => c.channel === selected.value);
  if (!row) return;
  await fetch(`/x-nmos/connection/v1.1/single/receivers/${row.receiver_id}/staged`, {
    method: "PATCH",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      master_enable: true,
      activation: { mode: "activate_immediate" },
      transport_params: [{ mxl_domain_id: route.domain, mxl_flow_id: route.flow }],
    }),
  });
  refresh();
}

function onWheel(which, event) {
  const rect = event.currentTarget.getBoundingClientRect();
  const x = Math.max(-1, Math.min(1, ((event.clientX - rect.left) / rect.width) * 2 - 1));
  const y = Math.max(-1, Math.min(1, ((event.clientY - rect.top) / rect.height) * 2 - 1));
  wheel(which, x, y);
}

const previewStamp = ref(0);
let previewTimer;
onMounted(() => {
  refresh();
  connect();
  previewTimer = setInterval(() => {
    previewStamp.value = Date.now();
  }, 250);
});
onBeforeUnmount(() => {
  if (socket) socket.close();
  clearInterval(previewTimer);
});
watch(selected, () => {
  const row = (nmos.value.channels || []).find((c) => c.channel === selected.value);
  if (row) {
    route.domain = row.mxl_domain_id || "";
    route.flow = row.mxl_flow_id || "";
  }
});

function pretty(value) {
  return JSON.stringify(value, null, 2);
}

function stateClass(state) {
  if (state === "running") return "ok";
  if (state === "whole_grain") return "warn";
  return "idle";
}
</script>

<template>
  <div class="app">
    <header>
      <div>
        <strong>MXL Color Corrector</strong>
        <span class="muted">{{ status }}</span>
      </div>
      <nav>
        <button :class="{ on: page === 'overview' }" @click="page = 'overview'">Overview</button>
        <button :class="{ on: page === 'channel' }" @click="page = 'channel'">Channel</button>
        <button :class="{ on: page === 'nmos' }" @click="page = 'nmos'">NMOS</button>
        <button :class="{ on: page === 'settings' }" @click="page = 'settings'">Settings</button>
      </nav>
    </header>

    <section v-if="page === 'overview'" class="grid">
      <article v-for="ch in channels" :key="ch.channel" class="card" @click="selected = ch.channel; page = 'channel'">
        <header>
          <b>{{ ch.label }}</b>
          <span :class="stateClass(ch.state)">{{ ch.state }}</span>
        </header>
        <p>{{ ch.source_label || "no source" }}</p>
        <p class="muted">{{ ch.slice_mode ? "slice" : ch.fallback_reason }} · A/B {{ ch.ab }} · {{ ch.bypass ? "bypass" : "active" }}</p>
        <p v-if="ch.color_warning" class="warn">{{ ch.color_warning }}</p>
      </article>
    </section>

    <section v-else-if="page === 'channel' && current" class="channel">
      <div class="toolbar">
        <label>Channel
          <select v-model.number="selected">
            <option v-for="ch in channels" :key="ch.channel" :value="ch.channel">{{ ch.label }}</option>
          </select>
        </label>
        <button @click="send({ type: 'bypass', channel: selected, enabled: !current.bypass })">{{ current.bypass ? "Bypass on" : "Bypass" }}</button>
        <button @click="send({ type: 'ab', channel: selected, slot: 'toggle' })">A/B {{ current.ab.toUpperCase() }}</button>
        <button @click="send({ type: 'reset', channel: selected, control: 'all' })">Reset</button>
        <span :class="stateClass(current.state)">{{ current.state }}</span>
        <span class="muted">clip {{ (current.clipped_ratio * 100).toFixed(2) }}%</span>
      </div>
      <div class="previews">
        <figure>
          <img :src="`/api/v1/channels/${selected}/preview/input.jpg?t=${previewStamp}`" alt="input" />
          <figcaption>Input</figcaption>
        </figure>
        <figure class="wipe" :style="{ '--wipe': wipe + '%' }">
          <img class="under" :src="`/api/v1/channels/${selected}/preview/input.jpg?t=${previewStamp}`" alt="input under wipe" />
          <img class="over" :src="`/api/v1/channels/${selected}/preview/output.jpg?t=${previewStamp}`" alt="output" />
          <figcaption>Wipe {{ wipe }}%</figcaption>
        </figure>
        <label class="wipe-slider">Wipe <input v-model.number="wipe" type="range" min="0" max="100" /></label>
      </div>
      <div class="panels">
        <div class="card">
          <h3>White colour</h3>
          <div class="wheel" @pointerdown="onWheel('white', $event)" @pointermove="(e) => e.buttons && onWheel('white', e)">
            <span class="dot" :style="{ left: ((current.white_wheel.x + 1) * 50) + '%', top: ((current.white_wheel.y + 1) * 50) + '%' }"></span>
          </div>
          <button @click="wheel('white', 0, 0)">Neutral</button>
          <div class="rgb">
            <label v-for="axis in ['r', 'g', 'b']" :key="axis">{{ axis.toUpperCase() }}
              <input :value="current.white[axis]" type="number" step="0.1" min="-20" max="20"
                @change="rgb('white', axis, $event.target.value)"
                @keydown.up.prevent="rgb('white', axis, Number(current.white[axis]) + ($event.shiftKey ? 1 : 0.1))"
                @keydown.down.prevent="rgb('white', axis, Number(current.white[axis]) - ($event.shiftKey ? 1 : 0.1))" />
            </label>
          </div>
        </div>
        <div class="card">
          <h3>Black colour</h3>
          <div class="wheel black" @pointerdown="onWheel('black', $event)" @pointermove="(e) => e.buttons && onWheel('black', e)">
            <span class="dot" :style="{ left: ((current.black_wheel.x + 1) * 50) + '%', top: ((current.black_wheel.y + 1) * 50) + '%' }"></span>
          </div>
          <button @click="wheel('black', 0, 0)">Neutral</button>
          <div class="rgb">
            <label v-for="axis in ['r', 'g', 'b']" :key="axis">{{ axis.toUpperCase() }}
              <input :value="current.black[axis]" type="number" step="0.1" min="-5" max="5"
                @change="rgb('black', axis, $event.target.value)"
                @keydown.up.prevent="rgb('black', axis, Number(current.black[axis]) + ($event.shiftKey ? 1 : 0.1))"
                @keydown.down.prevent="rgb('black', axis, Number(current.black[axis]) - ($event.shiftKey ? 1 : 0.1))" />
            </label>
          </div>
        </div>
        <div class="card sliders">
          <label>Gain {{ current.gain }}%
            <input :value="current.gain" type="range" min="0" max="200" step="0.1" @input="numeric('gain', $event.target.value)" />
          </label>
          <label>Pedestal {{ current.pedestal }}%
            <input :value="current.pedestal" type="range" min="-10" max="10" step="0.1" @input="numeric('pedestal', $event.target.value)" />
          </label>
          <label>Brightness {{ current.brightness }}%
            <input :value="current.brightness" type="range" min="-20" max="20" step="0.1" @input="numeric('brightness', $event.target.value)" />
          </label>
          <label>Saturation {{ current.saturation }}%
            <input :value="current.saturation" type="range" min="0" max="200" step="0.1" @input="numeric('saturation', $event.target.value)" />
          </label>
          <label>Clip
            <select :value="current.clip" @change="patch({ clip: $event.target.value })">
              <option>legal</option>
              <option>extended</option>
              <option>off</option>
            </select>
          </label>
          <label><input type="checkbox" :checked="current.rgb_clip" @change="patch({ rgb_clip: $event.target.checked })" /> RGB gamut clip</label>
          <p v-if="current.color_warning" class="warn">{{ current.color_warning }}</p>
          <div class="presets">
            <input v-model="presetName" placeholder="preset name" />
            <button @click="savePreset(false)">Save channel</button>
            <button @click="savePreset(true)">Save global</button>
          </div>
        </div>
      </div>
    </section>

    <section v-else-if="page === 'nmos'" class="card wide">
      <h3>Node {{ nmos.node_id }}</h3>
      <p>Registered: {{ nmos.registered }} · port {{ nmos.port }} · DNS-SD {{ nmos.dns_sd }}</p>
      <div v-for="row in nmos.channels || []" :key="row.channel" class="nmos-row">
        <b>CC {{ row.channel }}</b>
        <span>receiver {{ row.receiver_id }}</span>
        <span>sender {{ row.sender_id }}</span>
        <span>{{ row.master_enable ? "routed" : "idle" }} {{ row.mxl_flow_id || "" }}</span>
      </div>
      <h3>Activate channel {{ selected }}</h3>
      <label>Domain <input v-model="route.domain" /></label>
      <label>Flow <input v-model="route.flow" /></label>
      <button @click="activateRoute">Activate immediate</button>
    </section>

    <section v-else class="card wide">
      <h3>Settings</h3>
      <pre>{{ pretty(settings) }}</pre>
    </section>
  </div>
</template>

<style>
:root { color-scheme: dark; font-family: "Segoe UI", sans-serif; background: #121418; color: #e7e3d8; }
body { margin: 0; }
button, select, input { background: #1d2128; color: inherit; border: 1px solid #3a404a; border-radius: 6px; padding: 0.35rem 0.6rem; }
button.on, button:hover { border-color: #d7a15f; }
header { display: flex; justify-content: space-between; align-items: center; padding: 0.8rem 1rem; border-bottom: 1px solid #2a2f37; }
nav { display: flex; gap: 0.4rem; }
.muted { color: #9aa3b2; margin-left: 0.6rem; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(240px, 1fr)); gap: 0.8rem; padding: 1rem; }
.card { background: #1a1e25; border: 1px solid #2c323b; border-radius: 12px; padding: 0.8rem; }
.card header { display: flex; justify-content: space-between; padding: 0; border: 0; }
.ok { color: #8fd18a; } .warn { color: #e2b15a; } .idle { color: #9aa3b2; }
.channel, .wide { padding: 1rem; }
.toolbar, .presets, .rgb { display: flex; gap: 0.6rem; flex-wrap: wrap; align-items: center; }
.previews { display: grid; grid-template-columns: 1fr 1fr; gap: 0.8rem; margin: 0.8rem 0; }
figure { margin: 0; background: #000; min-height: 140px; position: relative; }
img { width: 100%; display: block; background: #000; }
.wipe { position: relative; }
.wipe .over { position: absolute; inset: 0; clip-path: inset(0 0 0 var(--wipe)); }
.panels { display: grid; grid-template-columns: 1fr 1fr 1.2fr; gap: 0.8rem; }
.wheel { width: 180px; height: 180px; border-radius: 50%; margin: 0.6rem auto; position: relative;
  background: conic-gradient(red, yellow, lime, cyan, blue, magenta, red); touch-action: none; }
.wheel.black { filter: brightness(0.45); }
.dot { position: absolute; width: 14px; height: 14px; border-radius: 50%; background: #fff; border: 2px solid #111; transform: translate(-50%, -50%); }
.sliders label, .wide label { display: flex; flex-direction: column; gap: 0.25rem; margin: 0.4rem 0; }
.nmos-row { display: grid; grid-template-columns: 80px 1fr 1fr 1fr; gap: 0.4rem; font-size: 0.85rem; margin: 0.3rem 0; }
pre { white-space: pre-wrap; }
@media (max-width: 900px) { .panels, .previews, .nmos-row { grid-template-columns: 1fr; } }
</style>
