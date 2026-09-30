/* A role (start / finish) combines its sensors' independently debounced crossings
 * (PLAN §2.4 (2)). The role is certain through T when every member is: identified through T
 * and no significant hole below T. */
import { acceptCrossings } from "./sensor-stream";

// streams: sensorStream() results of the role's members.
// -> { accepted: [{node_id, tick}], certain, finalCut, cuts: [{node_id, cut, final, hole}] }
export function roleStream(streams, { after, debounceTicks }) {
  const accepted = [];
  const cuts = [];
  let certain = null;
  for (const stream of streams) {
    const r = acceptCrossings(stream, { after, debounceTicks });
    for (const tick of r.accepted) accepted.push({ node_id: stream.node_id, tick });
    if (r.cut != null) cuts.push({ node_id: stream.node_id, role: stream.role, cut: r.cut, final: r.final, hole: r.cutHole });
    if (certain == null || r.certain < certain) certain = r.certain;
  }
  accepted.sort((a, b) => (a.tick < b.tick ? -1 : a.tick > b.tick ? 1 : a.node_id.localeCompare(b.node_id)));
  const finals = cuts.filter((c) => c.final).sort((a, b) => (a.cut < b.cut ? -1 : a.cut > b.cut ? 1 : 0));
  return { accepted, certain, finalCut: finals.length ? finals[0].cut : null, finalCuts: finals, cuts };
}
