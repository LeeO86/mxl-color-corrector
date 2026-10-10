<script setup>
// Status (SPECIFICATION.md §8, §9): the probes, versions, and per channel the state and the counters of
// /metrics (grains, late grains, clipping, added latency, processing time, setting changes).
import { computed, onMounted, onUnmounted, ref } from "vue";
import Pill from "./Pill.vue";
import { STATE_TEXT, api, fmtFormat, modeText, parseMetrics, probe, stateKind } from "../api.js";
import { channels, live } from "../store.js";

const probes = ref({ livez: null, readyz: null });
const metrics = ref({});
let timer = 0;

async function load() {
  const [livez, readyz] = await Promise.all([probe("/livez"), probe("/readyz")]);
  probes.value = { livez, readyz };
  try {
    metrics.value = parseMetrics(await api.text("/metrics"));
  } catch {
    /* keep the last values */
  }
}
onMounted(() => {
  load();
  timer = setInterval(load, 2000);
});
onUnmounted(() => clearInterval(timer));

const code = (status) => ({ text: status ? String(status) : "no answer", kind: status === 200 ? "ok" : status ? "warn" : "bad" });
const m = (c) => metrics.value[String(c.channel)] || {};
const mean = (c, name) => {
  const x = m(c);
  return x[`${name}_count`] > 0 ? ((x[`${name}_sum`] / x[`${name}_count`]) * 1000).toFixed(2) : "–";
};
const num = (v) => (Number.isFinite(v) ? String(v) : "–");
const nmos = computed(() => live.status?.nmos || {});
</script>

<template>
  <div class="grid two">
    <div class="panel">
      <h3>Health</h3>
      <dl class="kv">
        <dt>/livez</dt>
        <dd><Pill v-bind="code(probes.livez)" /></dd>
        <dt>/readyz</dt>
        <dd><Pill v-bind="code(probes.readyz)" /> <span class="muted small">200 when serving and, with a registry, listed on its Query API</span></dd>
        <dt>NMOS registered</dt>
        <dd><Pill :text="nmos.registry ? (nmos.registered ? 'yes' : 'no') : 'no registry'" :kind="nmos.registered ? 'ok' : nmos.registry ? 'warn' : 'neutral'" /></dd>
        <dt>Live updates</dt>
        <dd><Pill :text="live.connected ? 'connected' : 'reconnecting'" :kind="live.connected ? 'ok' : 'warn'" /></dd>
      </dl>
    </div>
    <div class="panel">
      <h3>Versions</h3>
      <dl class="kv">
        <dt>Corrector</dt>
        <dd>{{ live.status?.version || "–" }}</dd>
        <dt>MXL</dt>
        <dd><code>{{ live.status?.mxl_revision || "–" }}</code> (release/v1.1)</dd>
        <dt>Processing</dt>
        <dd>CPU, one 3×4 fixed-point matrix in Y′CbCr (AVX2 when the CPU has it)</dd>
      </dl>
    </div>
  </div>

  <div class="panel">
    <h3>Channels</h3>
    <table>
      <thead>
        <tr>
          <th>Channel</th>
          <th>State</th>
          <th>Processing</th>
          <th>Format</th>
          <th>Source</th>
          <th class="num">Grains</th>
          <th class="num" title="grains skipped because the reader fell behind">Late</th>
          <th class="num" title="clipped samples in the last second">Clipped</th>
          <th class="num" title="input slice visible to output slice commit, mean since start">Added ms</th>
          <th class="num" title="colour processing per grain, mean since start">Processing ms</th>
          <th class="num">Changes</th>
          <th>Bypass</th>
          <th>A/B</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="c in channels" :key="c.channel">
          <td><strong>{{ c.channel }}</strong> <span class="muted small">{{ c.label }}</span></td>
          <td><Pill :text="STATE_TEXT[c.state] || c.state" :kind="stateKind(c.state)" /></td>
          <td>{{ modeText(c) }}</td>
          <td class="nowrap">{{ fmtFormat(c) }}</td>
          <td>{{ c.source_label || "–" }}</td>
          <td class="num">{{ num(m(c).grains_processed_total) }}</td>
          <td class="num">{{ num(m(c).late_grains_total) }}</td>
          <td class="num">{{ (c.clipped_ratio * 100).toFixed(2) }} %</td>
          <td class="num">{{ mean(c, "added_latency_seconds") }}</td>
          <td class="num">{{ mean(c, "processing_seconds") }}</td>
          <td class="num">{{ num(m(c).settings_changes_total) }}</td>
          <td><Pill :text="c.bypass ? 'on' : 'off'" :kind="c.bypass ? 'warn' : 'neutral'" /></td>
          <td>{{ c.ab.toUpperCase() }}</td>
        </tr>
      </tbody>
    </table>
    <p class="note">
      Counters since the corrector started; all of them, with the histograms: <a href="/metrics" target="_blank">/metrics</a> (Prometheus, prefix
      <code>mxl_color_corrector_</code>). The full status: <a href="/statusz" target="_blank">/statusz</a>.
    </p>
  </div>
</template>
