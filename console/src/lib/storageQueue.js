// Run saves, deletion, Clear and backup snapshot/import share one order.
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
