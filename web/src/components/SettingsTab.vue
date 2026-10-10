<script setup>
// Settings (SPECIFICATION.md §8): every setting with its value and origin (read-only: the corrector reads
// them at start), and channels, presets and routes as one JSON document (export, import).
import { computed, onMounted, ref } from "vue";
import { api, copyText, download } from "../api.js";
import { live, refreshConfig, refreshPresets, settingValue } from "../store.js";

const exportDoc = ref("");
const exportMsg = ref("");
const importText = ref("");
const importMsg = ref({ kind: "", text: "" });

const GROUPS = [
  { title: "Correction", test: /^CC_(CHANNELS|CLIP|RGB_CLIP|READ_OFFSET_GRAINS|PREVIEW_FPS)$/ },
  { title: "MXL", test: /^MXL_/ },
  { title: "NMOS", test: /^(NMOS_|HOST_ID)/ },
  { title: "Web, widgets, files and process", test: /./ },
];
// One line per key, from the README settings table.
const DESCRIPTIONS = {
  CC_CHANNELS: "Channels, 1–16.",
  CC_CLIP: "Default clip of a new channel: legal, extended or off.",
  CC_RGB_CLIP: "Default RGB gamut clip of a new channel.",
  CC_READ_OFFSET_GRAINS: "Whole-grain read offset. Slice mode always follows the writer.",
  CC_PREVIEW_FPS: "Thumbnail rate, 1–30.",
  CC_LOG_LEVEL: "error, warn, info.",
  CC_CONFIG_FILE: "JSON object of these keys (environment only).",
  CC_STATE_DIR: "Writable state: state.json (controls, presets) and routes.json.",
  MXL_DOMAIN_SCAN_PATH: "Parent of the domain directories, mirrors included.",
  MXL_OUTPUT_DOMAIN_DIR: "This corrector's domain directory.",
  MXL_OUTPUT_DOMAIN_ID: "This corrector's domain id.",
  MXL_HISTORY_DURATION_NS: "Written into a new domain's options.json only.",
  MXL_CLEANUP_ON_EXIT: "On shutdown, delete only this corrector's output domain.",
  NMOS_REGISTRY_ADDRESS: "Static registry (empty: do not register).",
  NMOS_REGISTRY_PORT: "Registration API port.",
  NMOS_QUERY_ADDRESS: "Query API host (/readyz).",
  NMOS_QUERY_PORT: "Query API port.",
  NMOS_DNS_SD: "Must stay false: this build has no DNS-SD.",
  NMOS_PORT: "IS-04 node and IS-05 connection API (also on WEB_PORT).",
  NMOS_SEED: "UUIDv5 seed of node, device, flows, senders, receivers and domain id.",
  NMOS_LABEL: "Node label; device label <label> Color Corrector.",
  NMOS_TAGS: "Tags on the node and device.",
  NMOS_HOST_ADDRESS: "IP announced in the node href and API endpoints.",
  HOST_ID: "Label alias; address alias only when it is an IP literal.",
  WEB_PORT: "This UI, the API, probes, metrics, widgets and the WebSocket.",
  WIDGET_FRAME_ANCESTORS: "Who may frame /widget pages (CSP frame-ancestors); /widgets answers them with CORS.",
  SHUTDOWN_TIMEOUT_S: "Budget for SIGTERM.",
};
const ORIGIN = {
  environment: { text: "ENV", cls: "env" },
  file: { text: "FILE", cls: "file" },
  default: { text: "DEFAULT", cls: "default" },
};
const groups = computed(() => {
  const left = [...(live.config?.settings || [])];
  return GROUPS.map((g) => {
    const items = left.filter((s) => g.test.test(s.key));
    items.forEach((s) => left.splice(left.indexOf(s), 1));
    return { title: g.title, items };
  }).filter((g) => g.items.length);
});

async function loadExport() {
  try {
    exportDoc.value = JSON.stringify(await api.get("/api/v1/config/export"), null, 2);
  } catch (e) {
    exportMsg.value = e.message;
  }
}
onMounted(() => {
  refreshConfig();
  loadExport();
});

async function copy() {
  exportMsg.value = (await copyText(exportDoc.value)) ? "Copied." : "Copy failed; select the text and copy it by hand.";
}
function onFile(ev) {
  ev.target.files?.[0]?.text().then((t) => (importText.value = t));
}
async function doImport() {
  try {
    JSON.parse(importText.value);
  } catch (e) {
    importMsg.value = { kind: "err", text: `Not JSON: ${e.message}` };
    return;
  }
  if (!confirm("Import? Every channel's A/B controls, the presets and the IS-05 routes are replaced.")) return;
  try {
    await api.post("/api/v1/config/import", importText.value);
    importMsg.value = { kind: "ok", text: "Imported. Channels, presets and routes are restored; ports and identity are unchanged." };
    importText.value = "";
    refreshPresets();
    loadExport();
  } catch (e) {
    importMsg.value = { kind: "err", text: e.message };
  }
}
const seed = computed(() => settingValue("NMOS_SEED") || "cc");
</script>

<template>
  <div class="panel">
    <h3>Configuration</h3>
    <p class="note" style="margin: 0">
      Precedence: environment (ENV), then the configuration file <code>CC_CONFIG_FILE</code> (FILE), then the default. The corrector reads them at
      start: change them in its deployment and restart it. The controls, presets and routes are kept in <code>{{ settingValue("CC_STATE_DIR") || "CC_STATE_DIR" }}</code>.
    </p>
  </div>
  <div v-for="g in groups" :key="g.title" class="panel">
    <h3>{{ g.title }}</h3>
    <table>
      <thead>
        <tr><th style="width: 34%">Key</th><th>Value</th><th style="width: 6rem">Origin</th></tr>
      </thead>
      <tbody>
        <tr v-for="s in g.items" :key="s.key">
          <td>
            <code>{{ s.key }}</code>
            <div class="desc">{{ DESCRIPTIONS[s.key] || "" }}</div>
          </td>
          <td style="overflow-wrap: anywhere">{{ s.value === "" ? "–" : s.value }}</td>
          <td><span class="badge" :class="ORIGIN[s.source]?.cls">{{ ORIGIN[s.source]?.text || s.source }}</span></td>
        </tr>
      </tbody>
    </table>
  </div>

  <div class="grid two">
    <div class="panel">
      <h3>Export</h3>
      <p class="note">One JSON document: both A/B slots of every channel, the presets, the IS-05 routes, and the settings (for information). No secrets.</p>
      <div class="actions" style="margin-top: 0">
        <span class="msg ok" style="margin: 0">{{ exportMsg }}</span>
        <span class="spacer"></span>
        <button class="btn secondary" @click="loadExport">Reload</button>
        <button class="btn secondary" :disabled="!exportDoc" @click="copy">Copy</button>
        <button class="btn" :disabled="!exportDoc" @click="download(`mxl-color-corrector-${seed}.json`, exportDoc + '\n')">Download</button>
      </div>
      <pre style="max-height: 320px">{{ exportDoc }}</pre>
    </div>
    <div class="panel">
      <h3>Import</h3>
      <p class="note">An exported document. Its channel count must match <code>CC_CHANNELS</code>; ports and identity are not changed.</p>
      <input type="file" accept="application/json,.json" aria-label="Open an exported JSON file" @change="onFile" />
      <textarea v-model="importText" aria-label="Exported JSON" placeholder='{"version": "1.0.0", "channels": [...]}' style="margin-top: 0.5rem"></textarea>
      <div class="actions">
        <span class="msg" :class="importMsg.kind" style="margin: 0">{{ importMsg.text }}</span>
        <span class="spacer"></span>
        <button class="btn" :disabled="!importText.trim()" @click="doImport">Import</button>
      </div>
    </div>
  </div>
</template>
