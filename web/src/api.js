// REST client for the corrector API (README "API") and display helpers.

async function request(path, { method = "GET", body, text = false } = {}) {
  const headers = {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  const resp = await fetch(path, {
    method,
    headers,
    body: body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body),
    cache: "no-store",
  });
  const raw = await resp.text();
  let data = raw;
  if (!text) {
    try {
      data = raw ? JSON.parse(raw) : null;
    } catch {
      data = raw;
    }
  }
  if (!resp.ok) {
    const reason = data && typeof data === "object" ? data.error : String(data || "").slice(0, 200);
    throw new Error(reason || `HTTP ${resp.status}`);
  }
  return data;
}

export const api = {
  get: (path) => request(path),
  text: (path) => request(path, { text: true }),
  post: (path, body) => request(path, { method: "POST", body: body ?? {} }),
  patch: (path, body) => request(path, { method: "PATCH", body }),
  del: (path) => request(path, { method: "DELETE" }),
};

/** HTTP status of a probe (/livez, /readyz) without throwing; 0 when it does not answer. */
export async function probe(path) {
  try {
    return (await fetch(path, { cache: "no-store" })).status;
  } catch {
    return 0;
  }
}

/** The first 8 characters of an id; the whole id goes into a title. */
export const shortId = (id) => (id ? String(id).slice(0, 8) : "–");

/** Copies text; falls back to a hidden textarea on plain http, where navigator.clipboard is missing. */
export async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    const area = document.createElement("textarea");
    area.value = text;
    area.style.position = "fixed";
    area.style.opacity = "0";
    document.body.appendChild(area);
    area.select();
    const ok = document.execCommand("copy");
    area.remove();
    return ok;
  }
}

export function download(name, text, type = "application/json") {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

/** Channel states (§5.1) as words, and their pill colour. */
export const STATE_TEXT = { running: "running", whole_grain: "whole grain", waiting: "waiting" };
export const stateKind = (state) => ({ running: "ok", whole_grain: "warn" })[state] || "neutral";
const REASON_TEXT = { interlaced: "interlaced input", no_slice_commits: "no slice commits", slice_layout: "slice layout" };
/** Slice mode, or why the channel processes whole grains. */
export const modeText = (c) => (c.slice_mode ? "slice by slice" : REASON_TEXT[c.fallback_reason] || "whole grain");

/** Signed percent with one decimal: +12.5 %, 0.0 %, −3.0 %. */
export function fmtPercent(value, signed = true) {
  const v = Number(value) || 0;
  const text = Math.abs(v).toFixed(1);
  return `${signed && v > 0.04 ? "+" : v < -0.04 ? "−" : ""}${text} %`;
}

export function fmtFormat(c) {
  if (!c.width) return "–";
  const rate = c.rate_den ? c.rate_num / c.rate_den : 0;
  return `${c.width}×${c.height}${c.interlaced ? "i" : "p"}${rate ? Number(rate.toFixed(2)) : ""}`;
}

/**
 * Per-channel numbers from /metrics (Prometheus text): `grains_processed_total`, `late_grains_total`,
 * `settings_changes_total`, `added_latency_seconds_sum` / `_count`, `processing_seconds_sum` / `_count`.
 * Keyed by channel number.
 */
export function parseMetrics(text) {
  const out = {};
  for (const line of text.split("\n")) {
    const m = /^mxl_color_corrector_(\w+)\{([^}]*)\} (\S+)$/.exec(line);
    if (!m) continue;
    const labels = Object.fromEntries([...m[2].matchAll(/(\w+)="([^"]*)"/g)].map((x) => [x[1], x[2]]));
    if (!labels.channel || labels.le || labels.state || labels.reason) continue;
    (out[labels.channel] ||= {})[m[1]] = Number(m[3]);
  }
  return out;
}
