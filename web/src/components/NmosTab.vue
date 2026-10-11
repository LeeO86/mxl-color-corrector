<script setup>
// NMOS (SPECIFICATION.md §6): the node, its registration, every channel's receiver and sender with the
// active IS-05 parameters, and an IS-05 activation (or disable) of one channel's receiver.
import { computed, ref, watch } from "vue";
import IdCode from "./IdCode.vue";
import Pill from "./Pill.vue";
import { api } from "../api.js";
import { act, channelOf, channels, live, selectChannel, settingValue } from "../store.js";

const n = computed(() => live.status?.nmos);
const base = computed(() => (n.value ? `http://${settingValue("NMOS_HOST_ADDRESS") || location.hostname}:${n.value.port}` : ""));
const registration = computed(() => {
  if (!n.value?.registry) return { text: "no registry", kind: "neutral" };
  return n.value.registered ? { text: "registered", kind: "ok" } : { text: "not registered", kind: "warn" };
});
const rows = computed(() => (n.value?.channels || []).map((row) => ({ row, channel: channelOf(row.channel) })));

// IS-05 staged PATCH with activate_immediate on the selected channel's receiver.
const route = ref({ domain: "", flow: "" });
const receiver = computed(() => (n.value?.channels || []).find((r) => r.channel === live.selected));
watch(
  () => receiver.value?.receiver_id,
  () => (route.value = { domain: receiver.value?.mxl_domain_id || "", flow: receiver.value?.mxl_flow_id || "" }),
  { immediate: true },
);
function stage(enable) {
  const body = { master_enable: enable, activation: { mode: "activate_immediate" } };
  if (enable) body.transport_params = [{ mxl_domain_id: route.value.domain.trim(), mxl_flow_id: route.value.flow.trim() }];
  const path = `/x-nmos/connection/v1.1/single/receivers/${receiver.value.receiver_id}/staged`;
  act(() => api.patch(path, body), enable ? `Channel ${live.selected} activated.` : `Channel ${live.selected} disabled.`);
}
</script>

<template>
  <div v-if="!n" class="empty">Loading…</div>
  <template v-else>
    <div class="grid two">
      <div class="panel">
        <h3>Node <span class="spacer"></span><Pill :text="registration.text" :kind="registration.kind" /></h3>
        <dl class="kv">
          <dt>Label</dt>
          <dd>{{ live.status.label }}</dd>
          <dt>Node id</dt>
          <dd><code>{{ n.node_id }}</code></dd>
          <dt>Device id</dt>
          <dd><code>{{ n.device_id }}</code></dd>
          <dt>Address</dt>
          <dd>{{ settingValue("NMOS_HOST_ADDRESS") || "–" }}:{{ n.port }} (node and connection API)</dd>
          <dt>Registry</dt>
          <dd>{{ n.registry ? `${n.registry}:${settingValue("NMOS_REGISTRY_PORT")}` : "none: not registered" }}</dd>
          <dt>DNS-SD</dt>
          <dd>{{ n.dns_sd ? "on (not advertised: this build has none)" : "off" }}</dd>
          <dt>Seed</dt>
          <dd><code>{{ settingValue("NMOS_SEED") || "–" }}</code></dd>
        </dl>
        <div class="note">
          Raw resources: <a :href="`${base}/x-nmos/node/v1.3/self`" target="_blank" rel="noopener">self</a> ·
          <a :href="`${base}/x-nmos/node/v1.3/devices`" target="_blank" rel="noopener">devices</a> ·
          <a :href="`${base}/x-nmos/node/v1.3/senders`" target="_blank" rel="noopener">senders</a> ·
          <a :href="`${base}/x-nmos/node/v1.3/receivers`" target="_blank" rel="noopener">receivers</a> ·
          <a :href="`${base}/x-nmos/connection/v1.1/single/receivers`" target="_blank" rel="noopener">connection</a>
        </div>
      </div>
      <div class="panel">
        <h3>Activate a channel</h3>
        <p class="note" style="margin-top: 0">
          Each channel has one BCP-007-03 video receiver and one video sender in this corrector's domain. A controller activates a sender's
          <code>mxl_domain_id</code> and <code>mxl_flow_id</code> on the receiver with IS-05; the receiver accepts a flow that does not exist yet
          (state waiting). This form sends the same staged PATCH with <code>activate_immediate</code>.
        </p>
        <label>Channel</label>
        <select :value="live.selected" aria-label="Channel" @change="selectChannel(Number($event.target.value))">
          <option v-for="c in channels" :key="c.channel" :value="c.channel">{{ c.label }}</option>
        </select>
        <label>Domain id</label>
        <input v-model="route.domain" class="mono" placeholder="mxl_domain_id" aria-label="Domain id" />
        <label>Flow id</label>
        <input v-model="route.flow" class="mono" placeholder="mxl_flow_id" aria-label="Flow id" />
        <div class="actions">
          <button class="btn secondary" :disabled="!receiver" title="master_enable false: stop reading" @click="stage(false)">Disable</button>
          <button class="btn" :disabled="!receiver || !route.domain.trim() || !route.flow.trim()" @click="stage(true)">Activate</button>
        </div>
      </div>
    </div>

    <div class="panel">
      <h3>Receivers and senders</h3>
      <table>
        <thead>
          <tr>
            <th>Channel</th><th>Receiver</th><th>master_enable</th><th>Source domain</th><th>Source flow</th><th>Sender</th><th>Sender (subscription)</th>
            <th>Output flow</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="{ row, channel } in rows" :key="row.channel" :class="{ dim: !row.master_enable }">
            <td><strong>{{ row.channel }}</strong> <span class="muted small">{{ channel?.label }}</span></td>
            <td><IdCode :id="row.receiver_id" /></td>
            <td><Pill :text="row.master_enable ? 'true' : 'false'" :kind="row.master_enable ? 'ok' : 'neutral'" /></td>
            <td><IdCode :id="row.mxl_domain_id" /></td>
            <td><IdCode :id="row.mxl_flow_id" /></td>
            <td><IdCode :id="row.sender_id" /></td>
            <td><IdCode :id="row.subscription_sender" /></td>
            <td><IdCode :id="row.flow_id" /></td>
          </tr>
        </tbody>
      </table>
      <p class="note">Hover an id for all of it. The routes are kept in <code>CC_STATE_DIR/routes.json</code> across a restart.</p>
    </div>
  </template>
</template>
