// Shared shareware-only browser probe harness. Owns every resource it starts and
// turns browser/runtime faults, OOS lines, and stopped simulation clocks into hard
// failures instead of warning-only console output.
import { createHash } from "node:crypto";
import { readFile, stat } from "node:fs/promises";
import { createServer } from "node:http";
import { dirname, extname, join, normalize, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { startRelay } from "./nip01-relay.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
export const REPO_ROOT = resolve(HERE, "../../../..");
export const WEB_ROOT = join(REPO_ROOT, "platform/emscripten");

const SHAREWARE_FILES = Object.freeze({
  "DUKE3D.GRP": Object.freeze({
    bytes: 11035779,
    sha256: "f943d0c2e2a0803a644a2107c81ea897dec87596d9dd1a6a432131ad6f5818d6",
  }),
  "LICENSE.TXT": Object.freeze({
    bytes: 9108,
    sha256: "38cd7edf73ba672db785a2c30ae0f2cceded7f94a6297397548ee8ce5312547d",
  }),
});

export class ProbeError extends Error {
  constructor(code, message, detail = undefined) {
    super(message);
    this.name = "ProbeError";
    this.code = code;
    this.detail = detail;
  }
}

export const sleep = (ms) => new Promise((done) => setTimeout(done, ms));

export function invariant(condition, code, message, detail = undefined) {
  if (!condition) throw new ProbeError(code, message, detail);
}

export async function withTimeout(work, timeoutMs, label) {
  let timer;
  try {
    return await Promise.race([
      Promise.resolve(work),
      new Promise((_, reject) => {
        timer = setTimeout(
          () => reject(new ProbeError("TIMEOUT", `${label} timed out after ${timeoutMs} ms`)),
          timeoutMs,
        );
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

export async function waitFor(label, probe, {
  timeoutMs = 60_000,
  intervalMs = 250,
} = {}) {
  const deadline = Date.now() + timeoutMs;
  let last;
  while (Date.now() < deadline) {
    try {
      last = await probe();
      if (last) return last;
    } catch (error) {
      if (error instanceof ProbeError && error.code !== "TIMEOUT") throw error;
      last = { error: String(error?.message ?? error) };
    }
    await sleep(intervalMs);
  }
  throw new ProbeError("TIMEOUT", `${label} timed out after ${timeoutMs} ms`, { last });
}

export async function verifySharewareBundle(repoRoot = REPO_ROOT) {
  const checked = [];
  for (const [name, expected] of Object.entries(SHAREWARE_FILES)) {
    const path = join(repoRoot, "assets/shareware", name);
    let info;
    let bytes;
    try {
      [info, bytes] = await Promise.all([stat(path), readFile(path)]);
    } catch (error) {
      throw new ProbeError("SHAREWARE_MISSING", `required shareware asset is missing: ${path}`, {
        cause: String(error?.message ?? error),
      });
    }
    const sha256 = createHash("sha256").update(bytes).digest("hex");
    invariant(info.isFile(), "SHAREWARE_TYPE", `${path} is not a regular file`);
    invariant(info.size === expected.bytes, "SHAREWARE_SIZE", `${name} has the wrong size`, {
      expected: expected.bytes,
      actual: info.size,
    });
    invariant(sha256 === expected.sha256, "SHAREWARE_HASH", `${name} failed SHA-256 verification`, {
      expected: expected.sha256,
      actual: sha256,
    });
    checked.push({ name, bytes: info.size, sha256 });
  }
  return checked;
}

export async function availableMemoryMB() {
  try {
    const text = await readFile("/proc/meminfo", "utf8");
    const match = /^MemAvailable:\s+(\d+)\s+kB$/m.exec(text);
    return match ? Math.floor(Number(match[1]) / 1024) : null;
  } catch {
    return null;
  }
}

export async function requireMemory(minimumMB) {
  const availableMB = await availableMemoryMB();
  if (availableMB != null) {
    invariant(availableMB >= minimumMB, "MEM_UNSAFE", "not enough free memory for this probe", {
      minimumMB,
      availableMB,
    });
  }
  return availableMB;
}

const MIME = Object.freeze({
  ".css": "text/css; charset=utf-8",
  ".data": "application/octet-stream",
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".map": "application/json; charset=utf-8",
  ".wasm": "application/wasm",
});

async function firstFile(candidates) {
  for (const path of candidates) {
    try {
      if ((await stat(path)).isFile()) return path;
    } catch {
      // Try the next root.
    }
  }
  return null;
}

export async function startWebServer({ repoRoot = REPO_ROOT, port = 0 } = {}) {
  const webRoot = join(repoRoot, "platform/emscripten");
  const required = [
    join(webRoot, "index.html"),
    join(webRoot, "eduke32-net.js"),
    join(repoRoot, "eduke32.js"),
    join(repoRoot, "eduke32.wasm"),
    join(repoRoot, "eduke32.data"),
  ];
  const missing = [];
  for (const path of required) {
    try {
      if (!(await stat(path)).isFile()) missing.push(path);
    } catch {
      missing.push(path);
    }
  }
  invariant(missing.length === 0, "WEB_BUILD_MISSING", "browser probe build artifacts are missing", {
    missing,
    build: "make PLATFORM=EMSCRIPTEN EM_SINGLE_FILE=0 NETDUKE32=1 obj=obj-em && npm run build:net",
  });

  const server = createServer(async (request, response) => {
    try {
      const url = new URL(request.url ?? "/", "http://127.0.0.1");
      const decoded = decodeURIComponent(url.pathname);
      const relative = normalize(decoded === "/" ? "index.html" : decoded.replace(/^\/+/, ""));
      if (relative === ".." || relative.startsWith(`..${process.platform === "win32" ? "\\" : "/"}`)) {
        response.writeHead(403).end("forbidden");
        return;
      }
      const path = await firstFile([join(webRoot, relative), join(repoRoot, relative)]);
      if (!path) {
        response.writeHead(404).end("not found");
        return;
      }
      const body = await readFile(path);
      response.writeHead(200, {
        "cache-control": "no-store",
        "content-type": MIME[extname(path)] ?? "application/octet-stream",
        "cross-origin-opener-policy": "same-origin",
        "cross-origin-resource-policy": "same-origin",
      });
      response.end(body);
    } catch (error) {
      response.writeHead(500).end(String(error?.message ?? error));
    }
  });

  await withTimeout(new Promise((resolveListen, reject) => {
    server.once("error", reject);
    server.listen(port, "127.0.0.1", resolveListen);
  }), 10_000, "local web server startup");
  const address = server.address();
  invariant(address && typeof address !== "string", "WEB_SERVER", "local web server has no TCP address");
  return {
    url: `http://127.0.0.1:${address.port}/`,
    close: () => withTimeout(new Promise((resolveClose, reject) => {
      server.close((error) => error ? reject(error) : resolveClose());
      server.closeAllConnections?.();
    }), 10_000, "local web server shutdown"),
  };
}

export function multiplayerConfig({
  matchName = "Bot Probe",
  maxPlayers,
  minPlayers,
  botSkill = 2,
  gametype = 0,
  episode = 0,
  level = 0,
  autoAim = 0,
  localOnly = 1,
} = {}) {
  invariant(Number.isInteger(maxPlayers) && maxPlayers >= 2 && maxPlayers <= 16,
    "BAD_CONFIG", "maxPlayers must be 2..16");
  invariant(Number.isInteger(minPlayers) && minPlayers >= 1 && minPlayers <= maxPlayers,
    "BAD_CONFIG", "minPlayers must be 1..maxPlayers");
  for (const [name, value, lo, hi] of [
    ["botSkill", botSkill, 0, 3], ["gametype", gametype, 0, 6],
    ["episode", episode, 0, 0], ["level", level, 0, 5],
  ]) invariant(Number.isInteger(value) && value >= lo && value <= hi,
    "BAD_CONFIG", `${name} must be ${lo}..${hi}`);

  return [
    "[Screen Setup]",
    "ScreenMode = 0",
    "ScreenWidth = 1024",
    "ScreenHeight = 768",
    "ScreenBPP = 8",
    "",
    "[Multiplayer]",
    `MatchName = ${matchName.replace(/[\r\n=]/g, " ")}`,
    `MaxPlayers = ${maxPlayers}`,
    `MinPlayers = ${minPlayers}`,
    `AutoAim = ${autoAim ? 1 : 0}`,
    `BotSkill = ${botSkill}`,
    `GameType = ${gametype}`,
    `Episode = ${episode}`,
    `Level = ${level}`,
    "GrpSharing = 0",
    `LocalOnly = ${localOnly ? 1 : 0}`,
    "",
  ].join("\n");
}

const HEADLESS_ARGS = Object.freeze([
  "--mute-audio",
  "--autoplay-policy=no-user-gesture-required",
  "--disable-background-timer-throttling",
  "--disable-renderer-backgrounding",
  "--disable-backgrounding-occluded-windows",
]);
const OOS_PATTERN = /Out.Of.Sync|\bOOS\b|\bDESYNC\b|\bMISMATCH\b/i;
const SOFTSNAP_PATTERN = /softsnap applied/i;
const KEY = Object.freeze({
  Up: "ArrowUp", Down: "ArrowDown", Left: "ArrowLeft", Right: "ArrowRight", Return: "Enter",
});

export class ProbeHarness {
  constructor(name, options = {}) {
    this.name = name;
    this.options = options;
    this.pages = [];
    this.faults = [];
    this.lines = [];
    this.browser = null;
    this.relay = null;
    this.web = null;
    this.baseUrl = null;
    this.availableMB = null;
  }

  async start() {
    await verifySharewareBundle(this.options.repoRoot ?? REPO_ROOT);
    this.availableMB = await requireMemory(this.options.minimumMB ?? 6144);

    const relayPort = Number(this.options.relayPort ?? 0);
    const relay = await startRelay(Number.isInteger(relayPort) ? relayPort : 0);
    this.relay = {
      ...relay,
      close: () => withTimeout(new Promise((resolveClose, reject) => {
        relay.wss.close((error) => error ? reject(error) : resolveClose());
        for (const client of relay.wss.clients) client.terminate();
      }), 10_000, "local relay shutdown"),
    };

    if (process.env.PROBE_URL) {
      this.baseUrl = new URL(process.env.PROBE_URL).href;
      invariant(/^https?:\/\/(127\.0\.0\.1|localhost)(:\d+)?\//.test(this.baseUrl),
        "NONLOCAL_SERVER", "PROBE_URL must be a loopback URL");
    } else {
      this.web = await startWebServer({ repoRoot: this.options.repoRoot ?? REPO_ROOT });
      this.baseUrl = this.web.url;
    }

    let playwright;
    try {
      playwright = await import("playwright");
    } catch (error) {
      throw new ProbeError("PLAYWRIGHT_MISSING", "repository-local Playwright is not installed; run npm ci", {
        cause: String(error?.message ?? error),
      });
    }
    this.browser = await withTimeout(playwright.chromium.launch({
      headless: process.env.HEADED !== "1",
      args: [...HEADLESS_ARGS],
    }), 60_000, "Chromium launch");
    return this;
  }

  async newPage(tag, configText = null) {
    invariant(this.browser, "HARNESS_STATE", "harness was not started");
    const context = await this.browser.newContext({ viewport: { width: 1024, height: 768 } });
    if (configText != null) {
      await context.addInitScript((cfg) => {
        localStorage.clear();
        sessionStorage.clear();
        localStorage.setItem("eduke32/cfg/eduke32.cfg", cfg);
      }, configText);
    }
    const page = await context.newPage();
    const record = {
      tag, context, page, crashed: false, closed: false, faults: [], ring: [],
      oos: [], softsnaps: [], probeLines: [], lastState: null,
    };
    this.pages.push(record);
    const fault = (kind, value) => {
      const item = { tag, kind, message: String(value?.message ?? value).slice(0, 500) };
      record.faults.push(item);
      this.faults.push(item);
    };
    page.on("console", (message) => {
      const text = message.text();
      const line = `[${tag}] ${text}`;
      record.ring.push(text.slice(0, 500));
      if (record.ring.length > 200) record.ring.shift();
      this.lines.push(line);
      if (this.lines.length > 2000) this.lines.shift();
      if (OOS_PATTERN.test(text)) record.oos.push(text);
      if (SOFTSNAP_PATTERN.test(text)) record.softsnaps.push(text);
      if (/^\[probe\]/.test(text)) record.probeLines.push(text);
      if (message.type() === "error" && !/favicon\.ico|Failed to load resource.*404/i.test(text))
        fault("console-error", text);
    });
    page.on("pageerror", (error) => fault("pageerror", error));
    page.on("crash", () => { record.crashed = true; fault("crash", "page crashed"); });
    page.on("close", () => { record.closed = true; });
    page.on("requestfailed", (request) => {
      const url = request.url();
      if (url.startsWith(this.baseUrl)) fault("requestfailed", `${url}: ${request.failure()?.errorText ?? "failed"}`);
    });

    const url = new URL(this.baseUrl);
    url.searchParams.set("relays", this.relay.url);
    await withTimeout(page.goto(url.href, { waitUntil: "commit", timeout: 60_000 }), 65_000, `${tag} navigation`);
    await this.waitForBoot(record);
    return record;
  }

  async waitForBoot(record, timeoutMs = 150_000) {
    await waitFor(`${record.tag} runtime boot`, async () => {
      this.assertHealthy(record);
      return withTimeout(record.page.evaluate(() => {
        const gear = document.getElementById("gear");
        return Boolean(gear && getComputedStyle(gear).display !== "none" && window.Module?.ccall);
      }), 5_000, `${record.tag} boot evaluate`);
    }, { timeoutMs, intervalMs: 500 });
    await record.page.bringToFront();
    await record.page.evaluate(() => {
      const canvas = document.querySelector("canvas");
      if (canvas) { canvas.setAttribute("tabindex", "0"); canvas.focus(); }
    });
    await this.waitForState(record, (state) => state.open === 1 && state.id === 0,
      "main menu", 30_000);
  }

  async state(record) {
    this.assertHealthy(record);
    const state = await withTimeout(record.page.evaluate(() => {
      const menu = window.__e32menu;
      if (!menu || typeof menu !== "object") return null;
      return JSON.parse(JSON.stringify(menu));
    }), 5_000, `${record.tag} state evaluate`);
    record.lastState = state;
    return state;
  }

  async waitForState(record, predicate, label, timeoutMs = 60_000) {
    return waitFor(`${record.tag} ${label}`, async () => {
      const state = await this.state(record);
      return state && predicate(state) ? state : false;
    }, { timeoutMs, intervalMs: 250 });
  }

  async tap(record, key) {
    this.assertHealthy(record);
    const actual = KEY[key] ?? key;
    await record.page.keyboard.down(actual);
    await sleep(120);
    await record.page.keyboard.up(actual);
    await sleep(180);
  }

  async selectRow(record, target, label) {
    invariant(Number.isInteger(target) && target >= 0, "BAD_ROW", `invalid row for ${label}`);
    for (let attempt = 0; attempt < 80; attempt++) {
      const menu = await this.state(record);
      invariant(menu?.open === 1 && Number.isInteger(menu.sel), "MENU_STATE", `${label}: menu row unavailable`, menu);
      if (menu.sel === target) return menu;
      await this.tap(record, menu.sel > target ? "Up" : "Down");
    }
    throw new ProbeError("MENU_SELECT", `${label}: could not select row ${target}`, {
      last: await this.state(record),
    });
  }

  async enterMenu(record, menuId, label) {
    for (let attempt = 0; attempt < 10; attempt++) {
      await this.tap(record, "Return");
      try {
        return await this.waitForState(record, (state) => state.open === 1 && state.id === menuId,
          label, 4_000);
      } catch (error) {
        if (!(error instanceof ProbeError) || error.code !== "TIMEOUT") throw error;
      }
    }
    throw new ProbeError("MENU_ENTER", `${label}: menu ${menuId} was not reached`, {
      last: await this.state(record),
    });
  }

  async setupPrivateHost(record) {
    await this.selectRow(record, 2, "main > multiplayer");
    await this.enterMenu(record, 20001, "multiplayer root");
    await this.selectRow(record, 2, "multiplayer > host private");
    await this.enterMenu(record, 20013, "host configuration");
    // Current HOSTCFG has rows 0..10. This explicit assertion catches the stale
    // row-8 assumption instead of accidentally changing CPU Skill.
    await this.selectRow(record, 10, "host configuration > start");
    const selected = await this.state(record);
    invariant(selected.sel === 10, "HOST_START_ROW", "Start is not selected at current row 10", selected);
    await this.enterMenu(record, 20016, "host lobby");
    return this.inviteCode(record);
  }

  async inviteCode(record) {
    return waitFor(`${record.tag} invite code`, () => withTimeout(record.page.evaluate(() => {
      const code = window.DukeNet?.match?.inviteCode?.();
      return typeof code === "string" && code.length > 0 ? code : false;
    }), 5_000, `${record.tag} invite evaluate`), { timeoutMs: 30_000, intervalMs: 250 });
  }

  async joinGuest(record, invite, name = "ProbeGuest") {
    invariant(typeof invite === "string" && invite.length > 0, "INVITE", "guest invite is empty");
    await withTimeout(record.page.evaluate(({ code, guestName }) => {
      if (!window.NetMenu?.joinCode) throw new Error("NetMenu.joinCode export is missing");
      window.NetMenu.joinCode(code, guestName);
    }, { code: invite, guestName: name }), 5_000, `${record.tag} join call`);
    const slot = await waitFor(`${record.tag} seat assignment`, () => withTimeout(record.page.evaluate(() => {
      const value = window.DukeNet?.getMyConnectIndex?.();
      return Number.isInteger(value) && value > 0 ? value : false;
    }), 5_000, `${record.tag} seat evaluate`), { timeoutMs: 60_000, intervalMs: 250 });
    return slot;
  }

  async launchHost(record) {
    await this.selectRow(record, 0, "lobby > launch");
    for (let attempt = 0; attempt < 10; attempt++) {
      await this.tap(record, "Return");
      try {
        return await this.waitForState(record, (state) => state.game === 1,
          "game launch", 5_000);
      } catch (error) {
        if (!(error instanceof ProbeError) || error.code !== "TIMEOUT") throw error;
      }
    }
    throw new ProbeError("LAUNCH", "host did not enter the game", { last: await this.state(record) });
  }

  async waitExactRoster(records, expected, timeoutMs = 90_000) {
    invariant(expected >= 2 && expected <= 16, "ROSTER_EXPECTATION", "expected roster must be 2..16");
    return waitFor(`exact ${expected}-seat roster`, async () => {
      const states = await Promise.all(records.map((record) => this.state(record)));
      return states.every((state) => state?.game === 1 && state.np === expected) ? states : false;
    }, { timeoutMs, intervalMs: 500 });
  }

  async ccall(record, name, resultType = null, argTypes = [], args = []) {
    return withTimeout(record.page.evaluate(({ fn, result, types, values }) => {
      const mod = window.Module;
      if (!mod?.ccall) throw new Error("Module.ccall is missing");
      if (typeof mod[`_${fn}`] !== "function") throw new Error(`${fn} export is missing`);
      return mod.ccall(fn, result, types, values);
    }, { fn: name, result: resultType, types: argTypes, values: args }), 5_000, `${record.tag} ${name}`);
  }

  async botMask(record) {
    const value = await this.ccall(record, "Net_GetBotMask", "number", [], []);
    invariant(Number.isInteger(value), "BOT_MASK", "Net_GetBotMask returned a non-integer", { value });
    return value >>> 0;
  }

  assertHealthy(record = null, { allowSoftsnap = false } = {}) {
    const records = record ? [record] : this.pages;
    for (const item of records) {
      invariant(!item.crashed && !item.closed, "PAGE_DEAD", `${item.tag} page is dead`, { ring: item.ring.slice(-20) });
      invariant(item.faults.length === 0, "PAGE_FAULT", `${item.tag} emitted a browser/runtime fault`, {
        faults: item.faults.slice(-10), ring: item.ring.slice(-20),
      });
      invariant(item.oos.length === 0, "OOS", `${item.tag} emitted an OOS/desync diagnostic`, {
        lines: item.oos.slice(-10),
      });
      if (!allowSoftsnap) invariant(item.softsnaps.length === 0, "SOFTSNAP", `${item.tag} required sync repair`, {
        lines: item.softsnaps.slice(-10),
      });
    }
  }

  async watchSimulation(records, durationMs, {
    expectedPlayers,
    stallMs = 15_000,
    intervalMs = 1000,
    allowSoftsnap = false,
    onSample = null,
  } = {}) {
    const progress = new Map(records.map((record) => [record, { plc: null, changedAt: Date.now() }]));
    const deadline = Date.now() + durationMs;
    let samples = 0;
    while (Date.now() < deadline) {
      await sleep(Math.min(intervalMs, Math.max(1, deadline - Date.now())));
      for (const record of records) {
        this.assertHealthy(record, { allowSoftsnap });
        const state = await this.state(record);
        invariant(state?.game === 1, "LEFT_GAME", `${record.tag} left the game during the probe`, state);
        if (expectedPlayers != null)
          invariant(state.np === expectedPlayers, "ROSTER_DRIFT", `${record.tag} roster changed`, {
            expected: expectedPlayers, actual: state.np, state,
          });
        const p = progress.get(record);
        if (p.plc !== state.plc) { p.plc = state.plc; p.changedAt = Date.now(); }
        invariant(Date.now() - p.changedAt < stallMs, "SIM_STALL", `${record.tag} simulation clock stalled`, {
          stallMs, state,
        });
        if (onSample) await onSample(record, state, samples);
      }
      samples++;
    }
    return samples;
  }

  diagnostics() {
    return {
      availableMB: this.availableMB,
      relay: this.relay ? { url: this.relay.url, stats: this.relay.stats } : null,
      pages: this.pages.map((record) => ({
        tag: record.tag,
        faults: record.faults,
        oos: record.oos.length,
        softsnaps: record.softsnaps.length,
        lastState: record.lastState,
        ring: record.ring.slice(-20),
      })),
    };
  }

  async close() {
    const errors = [];
    for (const record of [...this.pages].reverse()) {
      try { await record.context.close(); } catch (error) { errors.push(String(error?.message ?? error)); }
    }
    if (this.browser) {
      try { await this.browser.close(); } catch (error) { errors.push(String(error?.message ?? error)); }
      this.browser = null;
    }
    if (this.web) {
      try { await this.web.close(); } catch (error) { errors.push(String(error?.message ?? error)); }
      this.web = null;
    }
    if (this.relay) {
      try { await this.relay.close(); } catch (error) { errors.push(String(error?.message ?? error)); }
      this.relay = null;
    }
    if (errors.length) throw new ProbeError("CLEANUP", "probe cleanup failed", { errors });
  }
}

export async function runProbe(name, options, body) {
  const harness = new ProbeHarness(name, options);
  const started = Date.now();
  let result;
  let failure;
  try {
    await harness.start();
    result = await body(harness);
    harness.assertHealthy();
  } catch (error) {
    failure = {
      code: error?.code ?? "UNCAUGHT",
      message: String(error?.message ?? error),
      detail: error?.detail,
      stack: String(error?.stack ?? "").split("\n").slice(0, 8),
    };
  } finally {
    try {
      await harness.close();
    } catch (error) {
      failure ??= {
        code: error?.code ?? "CLEANUP",
        message: String(error?.message ?? error),
        detail: error?.detail,
      };
    }
  }
  const report = {
    probe: name,
    ok: !failure,
    elapsedMs: Date.now() - started,
    result: result ?? null,
    failure: failure ?? null,
    diagnostics: harness.diagnostics(),
  };
  console.log(`RESULT ${JSON.stringify(report)}`);
  process.exitCode = failure ? 1 : 0;
  return report;
}

export function parseWorldProbe(line) {
  const head = /^\[probe\]\s+plc=(\d+)\s+np=(\d+)/.exec(line);
  if (!head) return null;
  const players = new Map();
  const pattern = /p(\d+)\[x=(-?\d+) y=(-?\d+) s=(-?\d+) hp=(-?\d+) fr=(-?\d+) dd=(-?\d+) z=(-?\d+) cw=(-?\d+) gw=(-?\d+)\]/g;
  for (const match of line.matchAll(pattern)) {
    players.set(Number(match[1]), {
      x: Number(match[2]), y: Number(match[3]), sector: Number(match[4]), health: Number(match[5]),
      frags: Number(match[6]), dead: Number(match[7]), z: Number(match[8]),
      weapon: Number(match[9]), weapons: Number(match[10]),
    });
  }
  return { plc: Number(head[1]), players: Number(head[2]), seats: players };
}

export function bitSeats(mask) {
  const seats = [];
  for (let seat = 0; seat < 16; seat++) if (mask & (1 << seat)) seats.push(seat);
  return seats;
}
