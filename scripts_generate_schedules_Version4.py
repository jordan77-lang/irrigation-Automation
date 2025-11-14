#!/usr/bin/env python3
"""
generate_schedules.py

Generates or composes schedules/schedules.json.

Behavior:
- If schedules/manual_events.json exists, use it as the schedules payload (developer/owner-controlled).
- Otherwise, generate randomized bi-weekly open/close events for devices listed in devices/devices.json.
- The output is written to the --out path in JSON with "generated_at" and "devices" fields.

Usage:
  python scripts/generate_schedules.py --devices devices/devices.json --out schedules/schedules.json --days 90
"""
import argparse
import json
import datetime
import random
from pathlib import Path
from dateutil import tz

def iso(ts):
    return ts.replace(microsecond=0).astimezone(tz.UTC).isoformat().replace('+00:00','Z')

def load_json(p):
    p = Path(p)
    if not p.exists():
        return None
    return json.loads(p.read_text())

def generate_for_device(dev, days_ahead=90):
    now = datetime.datetime.utcnow().replace(tzinfo=tz.UTC)
    end = now + datetime.timedelta(days=days_ahead)
    device_id = dev['device_id']
    base_virtual = float(dev.get('base_virtual_deg', 0.0))
    open_turns = float(dev.get('open_turns', 1.0))
    open_delta = float(dev.get('open_delta_deg', 360.0 * open_turns))
    min_open_min = float(dev.get('min_open_min', 5))
    max_open_min = float(dev.get('max_open_min', 60))
    events = []
    # Start scheduling from tomorrow at a random hour between 00-23 to spread events across time
    start_dt = (now + datetime.timedelta(days=1)).replace(hour=random.randint(0,23), minute=random.randint(0,59), second=0)
    # bi-weekly generator with optional day jitter +/- 2 days (configurable per device)
    days_between = int(dev.get('days_between', 14))
    jitter_days = float(dev.get('jitter_days', 0))
    dt = start_dt
    while dt <= end:
        jitter = datetime.timedelta(days=random.uniform(-jitter_days, jitter_days)) if jitter_days else datetime.timedelta(0)
        evt_time = dt + jitter
        open_target = base_virtual + open_delta
        duration_minutes = int(random.uniform(min_open_min, max_open_min))
        close_time = evt_time + datetime.timedelta(minutes=duration_minutes)
        close_target = base_virtual
        events.append({
            "id": f"{device_id}-{evt_time.strftime('%Y%m%dT%H%M%S')}-open",
            "action": "open",
            "time": iso(evt_time),
            "virtual_angle": open_target,
            "expected_duration_s": duration_minutes * 60
        })
        events.append({
            "id": f"{device_id}-{evt_time.strftime('%Y%m%dT%H%M%S')}-close",
            "action": "close",
            "time": iso(close_time),
            "virtual_angle": close_target,
            "expected_duration_s": 30
        })
        dt = dt + datetime.timedelta(days=days_between)
    return events

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--devices", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--days", type=int, default=90)
    args = parser.parse_args()

    manual = load_json('schedules/manual_events.json')
    if manual:
        payload = manual
        payload['generated_from'] = 'manual_events.json'
        payload['generated_at'] = iso(datetime.datetime.utcnow().replace(tzinfo=tz.UTC))
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(payload, indent=2))
        print("Used manual_events.json as authoritative schedule")
        return

    devices = load_json(args.devices) or []
    schedules = {
        "generated_at": iso(datetime.datetime.utcnow().replace(tzinfo=tz.UTC)),
        "devices": {}
    }
    for dev in devices:
        device_id = dev['device_id']
        schedules['devices'][device_id] = generate_for_device(dev, days_ahead=args.days)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(schedules, indent=2))
    print("Wrote", args.out)

if __name__ == "__main__":
    main()