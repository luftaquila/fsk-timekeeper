// Radio v9 uses the existing reliable EVENT delivery for capture-loss ranges
// and checkpoints. A checkpoint covers only captures already handed off ahead
// of it. Latest-value diagnostics cannot establish this completeness contract.
export const CAPTURE_HEALTH = 15;
export const CAPTURE_LOSS = 16;
export const CAPTURE_CHECKPOINT = 32;
export const CAPTURE_TIME_UNKNOWN = 64;
export const WIRELESS_PROTOCOL_VERSION = 10; // 10: beacons carry a checkpoint request (`CP`)
const distance = (seq, baseline) => (seq - baseline) >>> 0;
const tick = (row) => BigInt(row.master_tick);

// run = { boundaryTick, masterBootId, nodes: { [node_id]: { boot, seq, role } } }
// rows = every stored event since the run cursor (any node, any order).
export function verifyCaptures(run, rows) {
  const boundary = BigInt(run.boundaryTick);
  const captures = [];
  let through = null;
  let fault = null;
  let faultTick = null;
  let sessionEnded = false;
  function fail(node, at, reason) {
    if (faultTick == null || at < faultTick || (at === faultTick && node < fault.node_id)) {
      faultTick = at;
      fault = { node_id: node, reason };
    }
  }
  for (const row of rows) {
    if (row.node_id === "0" && row.master_boot_id === run.masterBootId && (row.flags & CAPTURE_LOSS) && tick(row) >= boundary) {
      sessionEnded = true;
      fail("0", tick(row), "The master timebase changed during the run.");
    }
  }
  for (const [node, source] of Object.entries(run.nodes)) {
    const events = rows.filter((row) => row.node_id === node && row.master_boot_id === run.masterBootId);
    const ordered = events
      .filter((row) => row.sensor_boot_id === source.boot && distance(row.capture_seq, source.seq) < 0x80000000)
      .sort(
        (a, b) =>
          distance(a.capture_seq, source.seq) - distance(b.capture_seq, source.seq) ||
          Number(!!(a.flags & CAPTURE_CHECKPOINT)) - Number(!!(b.flags & CAPTURE_CHECKPOINT)) ||
          Number(!!(a.flags & CAPTURE_LOSS)) - Number(!!(b.flags & CAPTURE_LOSS)) ||
          (tick(a) < tick(b) ? -1 : tick(a) > tick(b) ? 1 : 0),
      );
    const delivered = ordered.filter((row) => !(row.flags & (CAPTURE_LOSS | CAPTURE_CHECKPOINT)));
    let pendingFault = null;
    let previousTick = null;
    const seen = new Set();
    let next = 1;
    let confirmed = boundary - 1n;
    for (const row of ordered) {
      const seq = distance(row.capture_seq, source.seq);
      const at = tick(row);
      if (row.flags & CAPTURE_CHECKPOINT) {
        if (seq === next - 1 && (row.flags & CAPTURE_HEALTH) === CAPTURE_HEALTH && row.sync_age_ms <= 7000) {
          if (at > confirmed) confirmed = at;
          if (pendingFault != null) fail(node, pendingFault, `Sensor ${node} lost a capture or its capture time cannot be verified.`);
        }
        continue;
      }
      const loss = !!(row.flags & CAPTURE_LOSS);
      const last = loss ? distance(row.end_seq, source.seq) : seq;
      if (loss ? last < next : seq === 0 || seen.has(seq)) continue;
      if (seq > next) break;
      next = Math.max(next, last + 1);
      if (!loss) seen.add(seq);
      const covered =
        loss &&
        new Set(
          delivered
            .filter((item) => {
              const position = distance(item.capture_seq, source.seq);
              return position >= seq && position <= last && item.flags === CAPTURE_HEALTH && item.sync_age_ms <= 7000;
            })
            .map((item) => item.capture_seq),
        ).size ===
          last - seq + 1;
      if (loss && covered) continue;
      const backwards = !loss && previousTick != null && at <= previousTick;
      // A reversed timestamp cannot classify its own fault as pre-START.
      const reversalAffectsRun = backwards && previousTick >= boundary;
      if (!loss) previousTick = at;
      if (loss || (row.flags & CAPTURE_HEALTH) !== CAPTURE_HEALTH || row.sync_age_ms > 7000 || backwards) {
        const unknown = !!(row.flags & CAPTURE_TIME_UNKNOWN);
        if (unknown || reversalAffectsRun || BigInt(row.end_tick) >= boundary) {
          const from = unknown ? (confirmed > boundary ? confirmed : boundary) : at < boundary ? boundary : at;
          if (pendingFault == null || from < pendingFault) pendingFault = from;
        }
      } else if (!loss) {
        // A consecutive healthy capture proves this stream complete through its own tick (nothing
        // can sit between consecutive seqs); a pending loss still waits for the checkpoint to settle.
        if (pendingFault == null && at > confirmed) confirmed = at;
        if (at >= boundary) captures.push({ ...row, role: source.role });
      }
    }
    if (events.some((row) => row.sensor_boot_id !== source.boot && (row.flags & CAPTURE_CHECKPOINT) && tick(row) >= boundary)) {
      sessionEnded = true;
      fail(node, confirmed > boundary ? confirmed : boundary, `Sensor ${node} rebooted during the run.`);
    }
    if (through == null || confirmed < through) through = confirmed;
  }
  const events = captures
    .filter((row) => tick(row) <= through && (faultTick == null || tick(row) < faultTick))
    .sort((a, b) => (tick(a) < tick(b) ? -1 : tick(a) > tick(b) ? 1 : a.node_id.localeCompare(b.node_id)));
  const faultStands = sessionEnded || (through != null && faultTick != null && through >= faultTick);
  return {
    events,
    throughTick: String(through ?? boundary - 1n),
    fault: faultStands ? fault : null,
    faultTick: faultStands ? String(faultTick) : null,
    sessionEnded, // master timebase change or sensor reboot: the run's timebase no longer exists
  };
}
