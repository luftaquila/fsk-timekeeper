import { test, vi } from "vitest";
import assert from "node:assert/strict";
import { createWirelessClock } from "../src/lib/wireless-clock.js";

test("clock requests require a fresh correlated response and reject replay", async () => {
  const commands = [];
  const clock = createWirelessClock({ send: (command) => commands.push(command) });
  const first = clock.read();
  const second = clock.read();
  assert.match(commands[1].request_id, /^[0-9a-f]{32}$/);
  const reply = { ...commands[1], master_tick: "18446744073709551615", master_boot_id: 42 };
  assert.equal(clock.accept({ ...reply, request_id: "unknown" }), false);
  assert.equal(clock.accept(reply), true);
  assert.deepEqual(await second, { master_tick: reply.master_tick, master_boot_id: 42 });
  assert.equal(clock.accept(reply), false);
  const closed = assert.rejects(first, /closed/);
  clock.close();
  await closed;
});

test("clock timeout fails closed and a late response cannot revive the request", async () => {
  vi.useFakeTimers();
  try {
    let command;
    const clock = createWirelessClock({ send: (value) => { command = value; }, timeoutMs: 100 });
    const failed = assert.rejects(clock.read(), /did not confirm/);
    vi.advanceTimersByTime(100);
    await failed;
    assert.equal(clock.accept({ ...command, master_tick: "1", master_boot_id: 1 }), false);
  } finally {
    vi.useRealTimers();
  }
});

test("failed command delivery releases the request slot", async () => {
  const clock = createWirelessClock({ send: () => { throw new Error("disconnected"); } });
  for (let i = 0; i < 10; i++) await assert.rejects(clock.read(), /disconnected/);
  clock.close();
});
