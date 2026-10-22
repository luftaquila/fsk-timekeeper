/* The camera Pi's HTTP API at https://<host> (tailscale serve; CORS allows the console's origin):
 *   GET  /api/info                                 { api, app, usbProto, camera, session, storage }
 *   GET  /api/clips/<session>/<seq>                clip metadata (below)
 *   GET  /api/clips/<session>/<seq>/tiles/<i>.png  full-resolution tile i: lines along x
 *   GET  /api/clips/<session>/<seq>/preview.webp   the whole clip downscaled
 *   GET  /api/config, PUT /api/config              settings kept on the Pi: { values, limits }
 *   PUT  /api/session                              { run }: a camera-mode run holds the camera
 *                                                  session; { run, session } once it runs
 *   WS   /api/live                                 setup view: downscaled recent lines + focus;
 *                                                  holds the camera session while open
 * Clip metadata: { session, seq, first, last, line, height, tileLines, tiles,
 *   preview: { width, height, linesPerPixel }, ocr: { car, conf, text } }; lines first..last,
 * line = the detected front (the crossing's line), tile i holds lines first + i * tileLines... */
export const CAMERA_API = 2;

export function cameraApi(host, { fetchImpl = globalThis.fetch?.bind(globalThis) } = {}) {
  const base = `https://${host}`;
  async function json(path, init) {
    const res = await fetchImpl(`${base}${path}`, init);
    if (!res.ok) {
      const body = await res.json().catch(() => null);
      throw new Error(body?.error || `Camera ${res.status}: ${path}`);
    }
    return res.json();
  }
  async function blob(path) {
    const res = await fetchImpl(`${base}${path}`);
    if (!res.ok) throw new Error(`Camera ${res.status}: ${path}`);
    return res.blob();
  }
  const clip = (session, seq) => `/api/clips/${session}/${seq}`;
  return {
    base,
    info: () => json("/api/info"),
    config: () => json("/api/config"),
    setConfig: (patch) => json("/api/config", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(patch) }),
    setSession: (run) => json("/api/session", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ run }) }),
    clip: (session, seq) => json(clip(session, seq)),
    tileUrl: (session, seq, i) => `${base}${clip(session, seq)}/tiles/${i}.png`,
    preview: (session, seq) => blob(`${clip(session, seq)}/preview.webp`),
    liveUrl: () => `wss://${host}/api/live`,
  };
}

// The Pi speaks this console's API and relays a master of its USB protocol.
export function cameraContract(info, usbProto) {
  if (!info) return { ok: false, reason: "Camera not reachable" };
  if (info.api !== CAMERA_API || info.usbProto !== usbProto) return { ok: false, reason: "Camera software and console do not match" };
  return { ok: true, reason: null };
}
