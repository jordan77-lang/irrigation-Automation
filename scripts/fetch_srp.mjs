#!/usr/bin/env node
/**
 * Fetch upcoming SRP irrigation window(s) from water.gateway.srpnet.com
 * (same API used by https://github.com/mrbdahlem/irrigation)
 */
import fs from 'fs';
import path from 'path';

const SRP_API = 'https://water.gateway.srpnet.com/schedule/account';
const ARIZONA_OFFSET = '-07:00'; // America/Phoenix — no DST

const ACCOUNT_NAMES = {
  '0072499': '245 w 9th place',
};

const accounts = (process.env.SRP_ACCOUNTS || '')
  .split(',')
  .map((s) => s.trim())
  .filter(Boolean);

const valveAccount = (process.env.SRP_VALVE_ACCOUNT || accounts[0] || '').trim();
const mode = (process.env.SRP_MODE || 'preview').trim();
const deviceId = (process.env.DEVICE_ID || 'pd01').trim();
const openTurns = parseFloat(process.env.OPEN_TURNS || '4') || 4;
const outPreview = process.env.SRP_PREVIEW_PATH || 'schedules/srp_preview.json';
const outManual = process.env.SRP_MANUAL_PATH || 'schedules/manual_events.json';

if (!accounts.length) {
  console.error('SRP_ACCOUNTS is required (comma-separated account numbers)');
  process.exit(1);
}

async function fetchAccount(acct) {
  const url = `${SRP_API}/${acct}/quickview`;
  console.log(`Fetching ${acct} from ${url}`);
  const res = await fetch(url);
  if (!res.ok) {
    throw new Error(`HTTP ${res.status} for account ${acct}`);
  }
  return res.json();
}

function extractWindow(data) {
  const on =
    data.onDateTime ||
    data.displayFirstAccountScheduleDetail?.onDateTime ||
    null;
  const off =
    data.offDateTime ||
    data.displayFirstAccountScheduleDetail?.offDateTime ||
    null;
  if (!on || !off) return null;

  return {
    onDateTime: on,
    offDateTime: off,
    status: data.orderStatus || data.scheduleStatus || '',
    address: data.displayFirstAccountScheduleDetail?.address || '',
    notice: data.irrigationNotice || '',
  };
}

function toUtcIso(localNaive) {
  return new Date(`${localNaive}${ARIZONA_OFFSET}`).toISOString().replace('.000Z', 'Z');
}

function buildManualEvents(selected) {
  const openUtc = toUtcIso(selected.onDateTime);
  const closeUtc = toUtcIso(selected.offDateTime);
  const stamp = openUtc.slice(0, 19).replace(/[-:T]/g, (c) => (c === 'T' ? 'T' : ''));
  const openAngle = openTurns * 360;

  return {
    generated_at: new Date().toISOString().replace('.000Z', 'Z'),
    devices: {
      [deviceId]: [
        {
          id: `${deviceId}-${stamp}-open`,
          action: 'open',
          time: openUtc,
          virtual_angle: openAngle,
          expected_duration_s: 300,
        },
        {
          id: `${deviceId}-${stamp}-close`,
          action: 'close',
          time: closeUtc,
          virtual_angle: 0,
          expected_duration_s: 60,
        },
      ],
    },
  };
}

const results = [];

for (const acct of accounts) {
  try {
    const data = await fetchAccount(acct);
    const window = extractWindow(data);
    if (!window) {
      results.push({
        account: acct,
        name: ACCOUNT_NAMES[acct] || acct,
        error: 'No on/off times in SRP response',
      });
      continue;
    }
    results.push({
      account: acct,
      name: ACCOUNT_NAMES[acct] || acct,
      ...window,
      openUtc: toUtcIso(window.onDateTime),
      closeUtc: toUtcIso(window.offDateTime),
    });
  } catch (err) {
    results.push({
      account: acct,
      name: ACCOUNT_NAMES[acct] || acct,
      error: err.message,
    });
  }
}

const selected =
  results.find((r) => r.account === valveAccount && r.onDateTime) ||
  results.find((r) => r.onDateTime) ||
  null;

const preview = {
  fetched_at: new Date().toISOString(),
  source: 'srpnet.com',
  valve_account: valveAccount,
  accounts: results,
  selected,
};

fs.mkdirSync(path.dirname(outPreview), { recursive: true });
fs.writeFileSync(outPreview, JSON.stringify(preview, null, 2));
console.log(`Wrote ${outPreview}`);

if (mode === 'publish') {
  if (!selected) {
    console.error('No valid SRP window to publish');
    process.exit(1);
  }
  const manual = buildManualEvents(selected);
  fs.writeFileSync(outManual, JSON.stringify(manual, null, 2));
  console.log(`Wrote ${outManual} for ${selected.name} (${selected.account})`);
}
