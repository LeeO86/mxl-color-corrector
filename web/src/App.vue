<script setup>
import { computed, onMounted, onUnmounted, ref } from "vue";
import Pill from "./components/Pill.vue";
import OverviewTab from "./components/OverviewTab.vue";
import ChannelTab from "./components/ChannelTab.vue";
import NmosTab from "./components/NmosTab.vue";
import StatusTab from "./components/StatusTab.vue";
import SettingsTab from "./components/SettingsTab.vue";
import { channels, live, startLive, stopLive } from "./store.js";

const tabs = [
  { id: "overview", label: "Overview", component: OverviewTab },
  { id: "channel", label: "Channel", component: ChannelTab },
  { id: "nmos", label: "NMOS", component: NmosTab },
  { id: "status", label: "Status", component: StatusTab },
  { id: "settings", label: "Settings", component: SettingsTab },
];

const current = ref("overview");
const tab = computed(() => tabs.find((t) => t.id === current.value));

function onHash() {
  const id = location.hash.slice(1);
  if (tabs.some((t) => t.id === id)) current.value = id;
}
function go(id) {
  current.value = id;
  location.hash = id;
}

const count = (pred) => channels.value.filter(pred).length;
const running = computed(() => count((c) => c.state !== "waiting"));
const wholeGrain = computed(() => count((c) => c.state === "whole_grain"));
const waiting = computed(() => count((c) => c.state === "waiting"));
const bypassed = computed(() => count((c) => c.bypass));
const registration = computed(() => {
  const n = live.status?.nmos;
  if (!n) return null;
  if (!n.registry) return { text: "no registry", kind: "neutral" };
  return n.registered ? { text: "registered", kind: "ok" } : { text: "not registered", kind: "warn" };
});

onMounted(() => {
  onHash();
  window.addEventListener("hashchange", onHash);
  startLive();
});
onUnmounted(() => {
  window.removeEventListener("hashchange", onHash);
  stopLive();
});
</script>

<template>
  <header>
    <h1>mxl-color-corrector</h1>
    <span v-if="live.status" class="node">{{ live.status.label }} · {{ channels.length }} channel{{ channels.length === 1 ? "" : "s" }}</span>
    <span class="spacer"></span>
    <Pill v-if="channels.length" :text="`${running} of ${channels.length} running`" :kind="running ? 'ok' : 'neutral'" title="channels correcting a picture (slice by slice or whole grains)" />
    <Pill v-if="wholeGrain" :text="`${wholeGrain} whole grain`" kind="warn" title="no slice commits, interlaced or an odd slice layout: up to one grain of delay" />
    <Pill v-if="waiting" :text="`${waiting} waiting`" kind="neutral" title="not routed, or the domain or flow is not there (yet)" />
    <Pill v-if="bypassed" :text="`${bypassed} bypassed`" kind="warn" title="channels passing their input through" />
    <Pill v-if="registration" :text="registration.text" :kind="registration.kind" title="NMOS registration" />
    <Pill :text="live.connected ? 'live' : 'offline'" :kind="live.connected ? 'ok' : 'bad'" title="/api/v1/events" />
    <span v-if="live.status" class="muted small">v{{ live.status.version }} · MXL {{ (live.status.mxl_revision || "").slice(0, 7) }}</span>
  </header>
  <div v-if="live.error" class="banner bad">{{ live.error }}</div>
  <div v-else-if="live.everConnected && !live.connected" class="banner warn">Live updates lost. Reconnecting; values refresh every 2 s meanwhile.</div>
  <div v-if="live.actionError" class="banner bad">
    {{ live.actionError }}
    <span class="spacer"></span>
    <button class="btn small secondary" @click="live.actionError = ''">Dismiss</button>
  </div>
  <div v-if="live.notice" class="banner info">
    {{ live.notice }}
    <span class="spacer"></span>
    <button class="btn small secondary" @click="live.notice = ''">Dismiss</button>
  </div>
  <nav>
    <button v-for="t in tabs" :key="t.id" :class="{ active: current === t.id }" :aria-current="current === t.id ? 'page' : undefined" @click="go(t.id)">
      {{ t.label }}
    </button>
  </nav>
  <main>
    <component :is="tab.component" />
  </main>
</template>
