// Shared UI state: the status (WebSocket /api/v1/events, five times a second and on every change),
// the configuration, the presets, the selected channel and the actions every page and widget uses.
import { computed, reactive } from "vue";
import { api } from "./api.js";

export function stored(key, fallback) {
  try {
    return localStorage.getItem(`mxl-color-corrector.${key}`) ?? fallback;
  } catch {
    return fallback;
  }
}
export function store(key, value) {
  try {
    localStorage.setItem(`mxl-color-corrector.${key}`, String(value));
  } catch {
    /* kept for this tab only */
  }
}

export const live = reactive({
  status: null, // GET /api/v1/status: version, mxl_revision, label, channels, nmos
  config: null, // GET /api/v1/config: every setting with value and source
  presets: [], // GET /api/v1/presets
  connected: false,
  everConnected: false,
  error: "", // API unreachable
  actionError: "", // the last failed action (banner)
  notice: "", // the last action's result worth showing (banner)
  selected: Number(stored("channel", 1)) || 1, // the channel the Channel tab edits
});

export const channels = computed(() => live.status?.channels || []);
export const channelOf = (n) => channels.value.find((c) => c.channel === n) || null;
export const selectedChannel = computed(() => channelOf(live.selected) || channels.value[0] || null);
export function selectChannel(n) {
  live.selected = n;
  store("channel", n);
}
export const settingValue = (key) => live.config?.settings?.find((s) => s.key === key)?.value ?? "";

/** Runs an action; a failure goes to the error banner. Returns the result (true for an empty answer), or undefined. */
export async function act(fn, notice = "") {
  try {
    const result = await fn();
    live.actionError = "";
    if (notice) live.notice = notice;
    return result ?? true;
  } catch (e) {
    live.actionError = e.message;
    return undefined;
  }
}

// ---- live control changes ------------------------------------------------------
// Changes go over the WebSocket, at most 30 messages a second (REST while it is down). Changes to
// one channel merge, so a fast second control does not drop the first: {white: {r}} and
// {white: {g}} become {white: {r, g}}.

const pending = new Map(); // channel -> controls
let sendTimer = null;
let lastSend = 0;
let socket = null;

function flush() {
  clearTimeout(sendTimer);
  sendTimer = null;
  lastSend = performance.now();
  for (const [channel, controls] of pending) {
    if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify({ type: "patch", channel, controls }));
    else act(() => api.patch(`/api/v1/channels/${channel}/controls`, controls));
  }
  pending.clear();
}

export function patchControls(channel, controls) {
  const merged = pending.get(channel) || {};
  for (const [key, value] of Object.entries(controls)) {
    merged[key] = value && typeof value === "object" && merged[key] ? { ...merged[key], ...value } : value;
  }
  pending.set(channel, merged);
  const wait = 33 - (performance.now() - lastSend);
  if (wait <= 0) flush();
  else if (!sendTimer) sendTimer = setTimeout(flush, wait);
}

/** POST /api/v1/channels/{n}/<action> (bypass, ab, reset) after the pending changes; the status follows on the WebSocket. */
export function channelAction(channel, action, body) {
  flush();
  return act(() => api.post(`/api/v1/channels/${channel}/${action}`, body));
}

// ---- presets --------------------------------------------------------------------

export async function refreshPresets() {
  try {
    live.presets = (await api.get("/api/v1/presets")).presets || [];
  } catch {
    /* keep the last list */
  }
}
const presetPath = (channel, name) => (channel ? `/api/v1/channels/${channel}/presets` : "/api/v1/presets") + (name ? `/${encodeURIComponent(name)}` : "");
export async function savePreset(channel, name) {
  if (await act(() => api.post(presetPath(channel), { name }), `Preset "${name}" saved.`)) refreshPresets();
}
export function recallPreset(channel, name) {
  flush();
  return act(() => api.post(`${presetPath(channel, name)}/recall`), `Preset "${name}" recalled.`);
}
export async function deletePreset(channel, name) {
  if (await act(() => api.del(presetPath(channel, name)))) refreshPresets();
}

// ---- live connection ------------------------------------------------------------

export async function refreshStatus() {
  try {
    live.status = await api.get("/api/v1/status");
    live.error = "";
  } catch (e) {
    live.error = `API unreachable: ${e.message}`;
  }
}

export async function refreshConfig() {
  try {
    live.config = await api.get("/api/v1/config");
  } catch {
    /* keep the last answer; the connection banner tells */
  }
}

let retry = 0;
let timer = 0;

function connect() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  socket = new WebSocket(`${proto}//${location.host}/api/v1/events`);
  socket.onopen = () => {
    live.connected = true;
    live.everConnected = true;
    live.error = "";
  };
  socket.onmessage = (ev) => {
    try {
      live.status = JSON.parse(ev.data);
    } catch {
      /* a broken frame is dropped; the next one replaces it */
    }
  };
  socket.onclose = () => {
    live.connected = false;
    retry = setTimeout(connect, 1000);
  };
}

/** Loads the status and the configuration, then follows the WebSocket; polls every 2 s while it is down. */
export async function startLive() {
  await refreshStatus();
  refreshConfig();
  connect();
  timer = setInterval(() => {
    if (!live.connected) refreshStatus();
  }, 2000);
}

export function stopLive() {
  clearInterval(timer);
  clearTimeout(retry);
  if (socket) {
    socket.onclose = null;
    socket.close();
  }
}
