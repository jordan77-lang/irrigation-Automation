#!/usr/bin/env python3
"""
Generate schedules/schedules.json from manual_events.json or device defaults.
"""
import argparse
import json
import datetime
import random
from pathlib import Path
from dateutil import tz


def parse_iso_utc(ts):
    if not isinstance(ts, str) or not ts:
        raise ValueError("Event time must be a non-empty string")
    if ts.endswith("Z"):
        ts = ts[:-1] + "+00:00"
    dt = datetime.datetime.fromisoformat(ts)
    if dt.tzinfo is None:
        raise ValueError("Event time must include timezone (use UTC Z)")
    return dt.astimezone(tz.UTC)


def iso(ts):
    return ts.replace(microsecond=0).astimezone(tz.UTC).isoformat().replace("+00:00", "Z")


def load_json(p):
    p = Path(p)
    if not p.exists():
        return None
    return json.loads(p.read_text())


def validate_and_normalize_manual(payload):
    if not isinstance(payload, dict):
        raise ValueError("manual_events.json must be a JSON object")

    devices = payload.get("devices")
    if not isinstance(devices, dict) or not devices:
        raise ValueError("manual_events.json must contain a non-empty devices object")

    normalized = {"devices": {}}

    for device_id, events in devices.items():
        if not isinstance(device_id, str) or not device_id:
            raise ValueError("Each device key must be a non-empty string")
        if not isinstance(events, list) or not events:
            raise ValueError(f"Device {device_id} must have a non-empty event list")

        seen_ids = set()
        parsed_events = []
        for i, evt in enumerate(events):
            if not isinstance(evt, dict):
                raise ValueError(f"Device {device_id} event #{i+1} must be an object")

            eid = evt.get("id")
            action = evt.get("action")
            when = evt.get("time")
            angle = evt.get("virtual_angle")
            dur = evt.get("expected_duration_s", 60)

            if not isinstance(eid, str) or not eid:
                raise ValueError(f"Device {device_id} event #{i+1} missing id")
            if eid in seen_ids:
                raise ValueError(f"Device {device_id} has duplicate event id: {eid}")
            seen_ids.add(eid)

            if action not in ("open", "close"):
                raise ValueError(f"Device {device_id} event {eid} has invalid action: {action}")

            dt = parse_iso_utc(when)

            try:
                angle_f = float(angle)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"Device {device_id} event {eid} has invalid virtual_angle") from exc

            try:
                dur_i = int(dur)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"Device {device_id} event {eid} has invalid expected_duration_s") from exc
            if dur_i <= 0:
                raise ValueError(f"Device {device_id} event {eid} expected_duration_s must be > 0")

            parsed_events.append(
                {
                    "id": eid,
                    "action": action,
                    "time": iso(dt),
                    "virtual_angle": angle_f,
                    "expected_duration_s": dur_i,
                    "_dt": dt,
                }
            )

        parsed_events.sort(key=lambda e: e["_dt"])
        last_dt = None
        for evt in parsed_events:
            if last_dt and evt["_dt"] <= last_dt:
                raise ValueError(f"Device {device_id} has non-increasing event times")
            last_dt = evt["_dt"]
            evt.pop("_dt", None)

        normalized["devices"][device_id] = parsed_events

    return normalized


def generate_for_device(dev, days_ahead=90):
    now = datetime.datetime.utcnow().replace(tzinfo=tz.UTC)
    end = now + datetime.timedelta(days=days_ahead)
    device_id = dev["device_id"]
    base_virtual = float(dev.get("base_virtual_deg", 0.0))
    open_turns = float(dev.get("open_turns", 1.0))
    open_delta = float(dev.get("open_delta_deg", 360.0 * open_turns))
    min_open_min = float(dev.get("min_open_min", 5))
    max_open_min = float(dev.get("max_open_min", 60))
    events = []
    start_dt = (now + datetime.timedelta(days=1)).replace(
        hour=random.randint(0, 23), minute=random.randint(0, 59), second=0
    )
    days_between = int(dev.get("days_between", 14))
    jitter_days = float(dev.get("jitter_days", 0))
    dt = start_dt
    while dt <= end:
        jitter = (
            datetime.timedelta(days=random.uniform(-jitter_days, jitter_days))
            if jitter_days
            else datetime.timedelta(0)
        )
        evt_time = dt + jitter
        open_target = base_virtual + open_delta
        duration_minutes = int(random.uniform(min_open_min, max_open_min))
        close_time = evt_time + datetime.timedelta(minutes=duration_minutes)
        close_target = base_virtual
        stamp = evt_time.strftime("%Y%m%dT%H%M%S")
        events.append(
            {
                "id": f"{device_id}-{stamp}-open",
                "action": "open",
                "time": iso(evt_time),
                "virtual_angle": open_target,
                "expected_duration_s": duration_minutes * 60,
            }
        )
        events.append(
            {
                "id": f"{device_id}-{stamp}-close",
                "action": "close",
                "time": iso(close_time),
                "virtual_angle": close_target,
                "expected_duration_s": 30,
            }
        )
        dt = dt + datetime.timedelta(days=days_between)
    return events


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--devices", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--days", type=int, default=90)
    args = parser.parse_args()

    manual = load_json("schedules/manual_events.json")
    if manual:
        payload = validate_and_normalize_manual(manual)
        payload["generated_from"] = "manual_events.json"
        payload["generated_at"] = iso(datetime.datetime.utcnow().replace(tzinfo=tz.UTC))
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(payload, indent=2))
        print("Used manual_events.json as authoritative schedule")
        return

    devices = load_json(args.devices) or []
    schedules = {
        "generated_at": iso(datetime.datetime.utcnow().replace(tzinfo=tz.UTC)),
        "devices": {},
    }
    for dev in devices:
        device_id = dev["device_id"]
        schedules["devices"][device_id] = generate_for_device(dev, days_ahead=args.days)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(schedules, indent=2))
    print("Wrote", args.out)


if __name__ == "__main__":
    main()
