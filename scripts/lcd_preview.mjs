#!/usr/bin/env node
/**
 * Render the site's 20x4 LCD pages in the terminal and assert every line fits.
 * Keeps docs/index.html's screen builders honest against real repo fixtures.
 *
 *   node scripts/lcd_preview.mjs
 *
 * Exits non-zero if any line would be truncated on the real HD44780.
 */
import fs from 'fs';

const COLS = 20;
const html = fs.readFileSync('docs/index.html', 'utf8');
const src = html.match(/<script>([\s\S]*)<\/script>/)?.[1];
if (!src) {
  console.error('Could not find <script> block in docs/index.html');
  process.exit(1);
}

const readJson = (p, fallback) => {
  try { return JSON.parse(fs.readFileSync(p, 'utf8')); } catch { return fallback; }
};
const schedules = readJson('schedules/schedules.json', { devices: {} });
const status = readJson('schedules/device_status.json', {});

// Minimal DOM stub — enough for the page's init() to run headlessly.
const mkEl = (value = '') => ({
  value, textContent: '', innerHTML: '', hidden: false, dataset: {}, style: {},
  classList: { add() {}, remove() {}, toggle() {}, contains: () => false },
  addEventListener() {}, dispatchEvent() {}, appendChild() {}, setAttribute() {},
  focus() {}, querySelectorAll: () => [],
});
const els = { openTurns: mkEl('4'), deviceId: mkEl('pd01') };
const globals = {
  document: {
    getElementById: id => (els[id] ||= mkEl()),
    querySelectorAll: () => [], querySelector: () => mkEl(),
    addEventListener() {}, body: mkEl(), createElement: () => mkEl(),
  },
  localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
  sessionStorage: { getItem: () => null, setItem() {}, removeItem() {} },
  setInterval() {}, setTimeout() {}, clearTimeout() {},
  window: { addEventListener() {} }, navigator: {},
  fetch: () => Promise.reject(new Error('offline')),
  location: { href: '' }, Event: class { constructor() {} },
};

const factory = new Function(...Object.keys(globals), `${src}
  return {
    pages: {
      SCHEDULE: lcdPageSchedule, READY: lcdPageReady,
      'DRY RUN': lcdPageDryRun, POWER: lcdPagePower,
    },
    pad: lcdPad,
    seed: (s, d) => { liveScheduleData = s; deviceStatusData = d; },
  };`);

const api = factory(...Object.values(globals));
api.seed(schedules, status);

let overflow = 0;
for (const [name, build] of Object.entries(api.pages)) {
  console.log(`--- ${name} ---`);
  console.log(`    ${'1234567890'.repeat(COLS / 10)}`);
  for (const [i, raw] of build().entries()) {
    const line = raw ?? '';
    const truncated = line.length > COLS;
    if (truncated) overflow++;
    console.log(`${i} |${api.pad(line)}|${truncated ? `  <-- TRUNCATED (${line.length})` : ''}`);
  }
  console.log();
}

if (overflow) {
  console.error(`${overflow} line(s) exceed ${COLS} columns and would be cut on the device.`);
  process.exit(1);
}
console.log(`All lines fit ${COLS} columns.`);
