/* A role (start / finish) combines its sensors' independently debounced crossings. The
 * role is certain through T when every member is: identified through T and no significant
 * hole below T. */
import { acceptCrossings } from "./sensor-stream";

// A sensor whose evidence has ended (reboot, master session end, capture order fault): an
// open-ended hole from its known tick on. Nothing it could still report is missing.
const ended = (stream) => stream.holes.some((h) => h.hi === Infinity && h.lo <= stream.known);

// streams: sensorStream() results of the role's members. -> { accepted: [{node_id, tick}], certain,
// finalCut, cuts: [{node_id, cut, final, hole}], settled }; settled: every member is certain through
// finalCut or has ended, so no later report can move a needed crossing before it.
export function roleStream(streams, { after, debounceTicks }) {
  const accepted = [];
  const cuts = [];
  const members = [];
  let certain = null;
  for (const stream of streams) {
    const r = acceptCrossings(stream, { after, debounceTicks });
    for (const tick of r.accepted) accepted.push({ node_id: stream.node_id, tick });
    if (r.cut != null) cuts.push({ node_id: stream.node_id, role: stream.role, cut: r.cut, final: r.final, hole: r.cutHole });
    if (certain == null || r.certain < certain) certain = r.certain;
    members.push({ certain: r.certain, ended: ended(stream) });
  }
  accepted.sort((a, b) => (a.tick < b.tick ? -1 : a.tick > b.tick ? 1 : a.node_id.localeCompare(b.node_id)));
  const finals = cuts.filter((c) => c.final).sort((a, b) => (a.cut < b.cut ? -1 : a.cut > b.cut ? 1 : 0));
  const finalCut = finals.length ? finals[0].cut : null;
  const settled = finalCut != null && members.every((m) => m.certain >= finalCut || m.ended);
  return { accepted, certain, finalCut, finalCuts: finals, cuts, settled };
}
