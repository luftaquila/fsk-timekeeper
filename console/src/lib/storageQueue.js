// Run/result mutations share one order, including deletion, finalization and import.
let tail = Promise.resolve();
export function enqueue(task) {
  const result = tail.then(task);
  tail = result.catch(() => {});
  return result;
}
export async function flush() {
  let pending;
  do { pending = tail; await pending; } while (pending !== tail);
}
