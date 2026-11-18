import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { expect, test } from "vitest";

import { CEmuHeadlessSession, normalizeKey, press, tokensToEvents } from "../src/testing/cemu-interaction.ts";

test("normalizeKey supports graph-row aliases", () => {
  expect(normalizeKey("y=")).toBe("yequ");
  expect(normalizeKey("YeQu")).toBe("yequ");
  expect(normalizeKey("window")).toBe("window");
  expect(normalizeKey("zoom")).toBe("zoom");
  expect(normalizeKey("trace")).toBe("trace");
  expect(normalizeKey("graph")).toBe("graph");
});

test("press emits normalized graph-row keys", () => {
  expect(press("y=").key).toBe("yequ");
  expect(press("window").key).toBe("window");
  expect(press("graph").key).toBe("graph");
});

test("tokensToEvents preserves the physical catalog combo and graph actions", () => {
  const events = tokensToEvents(["2nd", "0", "yequ", "graph"], {
    autoEnter: false,
    keyDelayMs: 100,
  });
  const keyEvents = events.filter((event) => event.kind === "key");
  const pauseEvents = events.filter((event) => event.kind === "pause");

  expect(keyEvents).toHaveLength(4);
  expect(keyEvents.map((event) => ("key" in event ? event.key : ""))).toEqual(["2nd", "0", "yequ", "graph"]);
  expect(pauseEvents).toHaveLength(4);
});


test("screen assertions fail closed without a CRC or nonempty text expectations", async () => {
  const evidenceDir = fs.mkdtempSync(path.join(os.tmpdir(), "cemu-assert-"));
  const session = Object.create(CEmuHeadlessSession.prototype);
  session.getScreen = async () => ({ crc: "17B2609A", output_excerpt: "= 255" });
  session.snapshot = async () => ({ autotester_exit: 0 });
  const base = { name: "known_failure", description: "hex must show FF", evidenceDir };
  try {
    expect((await session.captureScreenAssertion({ ...base, expectedCrcs: [] })).passed).toBe(false);
    expect((await session.captureScreenAssertion(base)).passed).toBe(false);
    expect((await session.captureScreenAssertion({ ...base, expectedOutputExcerptContains: ["= 255"] })).passed).toBe(true);
    expect((await session.captureScreenAssertion({ ...base, expectedCrcs: ["17B2609A"] })).passed).toBe(true);
    expect((await session.captureScreenAssertion({ ...base, expectedCrcs: ["00000000"] })).passed).toBe(false);
  } finally {
    fs.rmSync(evidenceDir, { recursive: true, force: true });
  }
});
