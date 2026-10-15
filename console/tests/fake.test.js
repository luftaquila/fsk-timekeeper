import { afterEach, describe, expect, it, vi } from "vitest";
import { createFakeTransport } from "../src/transport/fake";

afterEach(() => vi.useRealTimers());

describe("fake sensor removal", () => {
  it("stops pending sensor records, status reports and checkpoints while other sensors keep reporting", async () => {
    vi.useFakeTimers();
    const lines = [];
    const fake = createFakeTransport({ onLine: (line) => lines.push(line) });
    await fake.open();
    try {
      fake.addSensor("abcdef12");
      fake.crossing("ABCDEF12");
      await fake.write("CP");
      expect(fake.removeSensor("abcdef12")).toBe(true);
      expect(fake.removeSensor("ABCDEF12")).toBe(false);
      await fake.write("?STATUS");
      await vi.advanceTimersByTimeAsync(6000);
      expect(fake.sensors.has("ABCDEF12")).toBe(false);
      expect(lines.some((line) => /^(?:D ABCDEF12 |E \d+ ABCDEF12 )/.test(line))).toBe(false);
      expect(lines.some((line) => line.startsWith("D 0D2243B0 "))).toBe(true);
      const heartbeat = lines.filter((line) => line.startsWith("H ")).at(-1);
      expect(heartbeat.split(" ").at(-1)).toBe("2");
    } finally {
      await fake.close();
    }
  });

  it("preserves records already accepted by the master and supports deleting every sensor", async () => {
    vi.useFakeTimers();
    const lines = [];
    const fake = createFakeTransport({ onLine: (line) => lines.push(line) });
    await fake.open();
    try {
      fake.setSlotDelay(0);
      fake.crossing("ABCDEF12");
      await vi.advanceTimersByTimeAsync(25);
      expect(fake.queueLength).toBe(1);
      for (const id of [...fake.sensors.keys()]) fake.removeSensor(id);
      expect(fake.sensors.size).toBe(0);
      expect(fake.queueLength).toBe(1);
      await vi.advanceTimersByTimeAsync(6000);
      expect(fake.sensors.size).toBe(0);
      expect(lines.some((line) => /^E \d+ ABCDEF12 C /.test(line))).toBe(true);
      const heartbeat = lines.filter((line) => line.startsWith("H ")).at(-1);
      expect(heartbeat.split(" ").at(-1)).toBe("0");
      fake.addSensor("12345678");
      await fake.write("?STATUS");
      await vi.advanceTimersByTimeAsync(5);
      expect(lines.some((line) => line.startsWith("D 12345678 "))).toBe(true);
    } finally {
      await fake.close();
    }
  });
});
